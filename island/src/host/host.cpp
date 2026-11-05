#include "host/host.h"

#include "host/defaults.h"

#include <shlobj.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace host
{

namespace
{

constexpr std::uintmax_t log_limit = 1024 * 1024;
constexpr DWORD max_string_bytes = 64 * 1024;
constexpr std::size_t log_line_chars = 1024;

HKEY settings_key = nullptr;
HKEY state_key = nullptr;
HANDLE settings_event = nullptr;
std::wstring storage_directory;
std::filesystem::path log_file;
std::filesystem::path log_backup;
std::atomic<bool> logging = false;
std::mutex log_mutex;

std::wstring local_app_data()
{
    PWSTR path = nullptr;
    std::wstring result;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path)))
    {
        result = path;
    }

    CoTaskMemFree(path);
    return result;
}

HKEY open_key(const wchar_t* subkey, REGSAM access)
{
    HKEY key = nullptr;
    const LSTATUS status =
        RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, access, nullptr, &key, nullptr);
    return status == ERROR_SUCCESS ? key : nullptr;
}

void close_key(HKEY& key)
{
    if (key)
    {
        RegCloseKey(key);
        key = nullptr;
    }
}

bool read_dword(HKEY key, const wchar_t* name, int& value)
{
    if (!key || !name)
    {
        return false;
    }

    DWORD data = 0;
    DWORD size = sizeof(data);

    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS)
    {
        return false;
    }

    value = static_cast<int>(data);
    return true;
}

bool read_string(HKEY key, const wchar_t* name, std::wstring& value)
{
    if (!key || !name)
    {
        return false;
    }

    DWORD size = 0;

    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size > max_string_bytes)
    {
        return false;
    }

    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));

    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size) != ERROR_SUCCESS)
    {
        return false;
    }

    buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
    value = std::move(buffer);
    return true;
}

std::string to_utf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = static_cast<int>(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);

    if (bytes <= 0)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), bytes, nullptr, nullptr);
    return result;
}

void sanitize(wchar_t* text)
{
    for (wchar_t* cursor = text; *cursor; ++cursor)
    {
        const wchar_t value = *cursor;
        const bool control = value < 0x20 || value == 0x7F || (value >= 0x80 && value < 0xA0);
        const bool bidi = (value >= 0x202A && value <= 0x202E) || (value >= 0x2066 && value <= 0x2069) ||
                          value == 0x200E || value == 0x200F;

        if (control || bidi)
        {
            *cursor = L' ';
        }
    }
}

void write_log_line(const wchar_t* message)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t line[log_line_chars + 32] = {};
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    const std::string bytes = to_utf8(line);

    std::lock_guard lock(log_mutex);
    std::error_code error;

    if (std::filesystem::file_size(log_file, error) >= log_limit && !error)
    {
        std::filesystem::rename(log_file, log_backup, error);
    }

    HANDLE file =
        CreateFileW(log_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

}

bool start()
{
    const std::wstring base = local_app_data();

    if (base.empty())
    {
        return false;
    }

    std::error_code error;
    const std::filesystem::path root = std::filesystem::path(base) / L"IslandBridge";
    const std::filesystem::path storage = root / L"Island";
    const std::filesystem::path logs = root / L"logs";
    std::filesystem::create_directories(storage, error);
    std::filesystem::create_directories(logs, error);
    storage_directory = storage.wstring();
    log_file = logs / L"island.log";
    log_backup = logs / L"island.1.log";

    settings_key = open_key(settings_subkey, KEY_READ | KEY_NOTIFY);
    state_key = open_key(state_subkey, KEY_READ | KEY_WRITE);
    settings_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!settings_key || !state_key || !settings_event)
    {
        stop();
        return false;
    }

    rearm_settings_watch();
    refresh_logging();
    return true;
}

void stop()
{
    close_key(settings_key);
    close_key(state_key);

    if (settings_event)
    {
        CloseHandle(settings_event);
        settings_event = nullptr;
    }
}

int int_setting(const wchar_t* name)
{
    int value = 0;

    if (read_dword(settings_key, name, value))
    {
        return value;
    }

    const SettingDefault* fallback = name ? find_setting_default(name) : nullptr;
    return fallback && fallback->kind == SettingKind::Number ? fallback->number : 0;
}

std::wstring string_setting(const wchar_t* name)
{
    std::wstring value;

    if (read_string(settings_key, name, value))
    {
        return value;
    }

    const SettingDefault* fallback = name ? find_setting_default(name) : nullptr;
    return fallback && fallback->kind == SettingKind::Text ? std::wstring(fallback->text) : std::wstring{};
}

int int_value(const wchar_t* name, int fallback)
{
    int value = 0;
    return read_dword(state_key, name, value) ? value : fallback;
}

bool set_int_value(const wchar_t* name, int value)
{
    if (!state_key || !name)
    {
        return false;
    }

    const DWORD data = static_cast<DWORD>(value);
    return RegSetValueExW(state_key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data)) ==
           ERROR_SUCCESS;
}

bool storage_path(wchar_t* buffer, std::size_t size)
{
    if (!buffer || size == 0 || storage_directory.empty())
    {
        return false;
    }

    return wcscpy_s(buffer, size, storage_directory.c_str()) == 0;
}

HANDLE settings_changed_event()
{
    return settings_event;
}

void rearm_settings_watch()
{
    if (settings_key && settings_event)
    {
        RegNotifyChangeKeyValue(settings_key, TRUE,
                                REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC,
                                settings_event, TRUE);
    }
}

void refresh_logging()
{
    int value = 0;
    logging = read_dword(settings_key, logging_setting, value) && value != 0;
}

void log(const wchar_t* format, ...)
{
    if (!logging.load(std::memory_order_relaxed) || !format || log_file.empty())
    {
        return;
    }

    wchar_t message[log_line_chars] = {};
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(message, _TRUNCATE, format, arguments);
    va_end(arguments);
    sanitize(message);
    write_log_line(message);
}

}
