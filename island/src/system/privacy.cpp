#include "system/privacy.h"
#include "ui/icons.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{
std::wstring clean_active_app_name(const std::wstring& raw)
{
    std::wstring name;
    size_t hash_pos = raw.rfind(L'#');
    if (hash_pos != std::wstring::npos)
    {
        name = raw.substr(hash_pos + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
        {
            name.resize(name.size() - 4);
        }
    }
    else
    {
        size_t under_pos = raw.find(L'_');
        name = (under_pos != std::wstring::npos) ? raw.substr(0, under_pos) : raw;
        if (name.rfind(L"Microsoft.", 0) == 0)
        {
            name = name.substr(10);
        }
        if (name.rfind(L"Windows.", 0) == 0)
        {
            name = name.substr(8);
        }
        else if (name.rfind(L"Windows", 0) == 0 && name.size() > 7)
        {
            name = name.substr(7);
        }
    }
    std::wstring lower = to_lower_copy(name);
    if (lower == L"msedge")
    {
        return L"Microsoft Edge";
    }
    if (lower == L"chrome")
    {
        return L"Google Chrome";
    }
    if (lower == L"brave")
    {
        return L"Brave";
    }
    if (lower == L"firefox")
    {
        return L"Firefox";
    }
    if (lower == L"discord")
    {
        return L"Discord";
    }
    if (lower == L"zoom")
    {
        return L"Zoom";
    }
    if (lower == L"obs64" || lower == L"obs32" || lower == L"obs")
    {
        return L"OBS Studio";
    }
    if (lower == L"teams")
    {
        return L"Microsoft Teams";
    }
    if (lower == L"skype")
    {
        return L"Skype";
    }
    if (lower == L"audacity")
    {
        return L"Audacity";
    }
    if (lower == L"fl64" || lower == L"fl")
    {
        return L"FL Studio";
    }
    if (lower == L"ciscocollabhost")
    {
        return L"Webex";
    }
    return name;
}

bool read_reg_qword(HKEY key, const wchar_t* name, uint64_t* out)
{
    DWORD type = 0;
    uint64_t value = 0;
    DWORD data_size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &data_size) != ERROR_SUCCESS)
    {
        return false;
    }
    if (type != REG_QWORD || data_size != sizeof(value))
    {
        return false;
    }
    *out = value;
    return true;
}

// In use only when Windows recorded a start and no stop (never-used entries have both at zero).
bool consent_store_entry_in_use(HKEY key)
{
    uint64_t stop_time = 0;
    if (!read_reg_qword(key, L"LastUsedTimeStop", &stop_time) || stop_time != 0)
    {
        return false;
    }

    uint64_t start_time = 0;
    if (read_reg_qword(key, L"LastUsedTimeStart", &start_time))
    {
        return start_time != 0;
    }

    static std::atomic<bool> logged_missing_start{false};
    if (!logged_missing_start.exchange(true))
    {
        host::log(L"Privacy indicator: a ConsentStore entry has LastUsedTimeStop=0 but no "
                  L"readable REG_QWORD LastUsedTimeStart; treating it as not in use.");
    }
    return false;
}

bool is_device_active_via_registry(const wchar_t* capability, std::wstring* out_app_name = nullptr)
{
    bool is_active = false;
    std::wstring base_path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    base_path += capability;

    auto check_subkeys = [&](HKEY h_key_parent) -> bool
    {
        constexpr REGSAM sam = KEY_READ | KEY_WOW64_64KEY;
        DWORD index = 0;
        wchar_t sub_key_name[256];
        DWORD name_len = ARRAYSIZE(sub_key_name);
        while (RegEnumKeyExW(h_key_parent, index, sub_key_name, &name_len, nullptr, nullptr, nullptr, nullptr) ==
               ERROR_SUCCESS)
        {
            HKEY h_sub;
            if (RegOpenKeyExW(h_key_parent, sub_key_name, 0, sam, &h_sub) == ERROR_SUCCESS)
            {
                if (_wcsicmp(sub_key_name, L"NonPackaged") == 0)
                {
                    DWORD np_index = 0;
                    wchar_t np_sub_key_name[256];
                    DWORD np_name_len = ARRAYSIZE(np_sub_key_name);
                    while (RegEnumKeyExW(h_sub, np_index, np_sub_key_name, &np_name_len, nullptr, nullptr, nullptr,
                                         nullptr) == ERROR_SUCCESS)
                    {
                        HKEY h_np_sub;
                        if (RegOpenKeyExW(h_sub, np_sub_key_name, 0, sam, &h_np_sub) == ERROR_SUCCESS)
                        {
                            if (consent_store_entry_in_use(h_np_sub))
                            {
                                if (out_app_name && out_app_name->empty())
                                {
                                    *out_app_name = clean_active_app_name(np_sub_key_name);
                                }
                                RegCloseKey(h_np_sub);
                                RegCloseKey(h_sub);
                                return true;
                            }
                            RegCloseKey(h_np_sub);
                        }
                        np_index++;
                        np_name_len = ARRAYSIZE(np_sub_key_name);
                    }
                }
                else
                {
                    if (consent_store_entry_in_use(h_sub))
                    {
                        if (out_app_name && out_app_name->empty())
                        {
                            *out_app_name = clean_active_app_name(sub_key_name);
                        }
                        RegCloseKey(h_sub);
                        return true;
                    }
                }
                RegCloseKey(h_sub);
            }
            index++;
            name_len = ARRAYSIZE(sub_key_name);
        }
        return false;
    };

    HKEY h_key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, base_path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &h_key) == ERROR_SUCCESS)
    {
        is_active = check_subkeys(h_key);
        RegCloseKey(h_key);
    }

    if (!is_active)
    {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base_path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &h_key) ==
            ERROR_SUCCESS)
        {
            is_active = check_subkeys(h_key);
            RegCloseKey(h_key);
        }
    }

    return is_active;
}

bool is_microphone_active(std::wstring* out_app_name = nullptr)
{
    return is_device_active_via_registry(L"microphone", out_app_name);
}

bool is_camera_active(std::wstring* out_app_name = nullptr)
{
    return is_device_active_via_registry(L"webcam", out_app_name);
}

}

void update_progress_snapshot()
{
    const int progress = host::int_value(L"ProgressPercent", -1);
    std::lock_guard lock(g_state_mutex);
    g_state.progress.active = progress >= 0 && progress <= 100;
    g_state.progress.percent = clamp_int(progress, 0, 100);
}

void update_privacy_indicators()
{
    std::wstring mic_app;
    std::wstring cam_app;
    const bool mic = (g_settings.privacy_dots && g_settings.privacy_dots_mic) ? is_microphone_active(&mic_app) : false;
    const bool cam = (g_settings.privacy_dots && g_settings.privacy_dots_cam) ? is_camera_active(&cam_app) : false;
    std::lock_guard lock(g_state_mutex);
    g_state.system.mic_active = mic;
    g_state.system.camera_active = cam;
    g_state.system.mic_app = mic_app;
    g_state.system.camera_app = cam_app;
}

}
