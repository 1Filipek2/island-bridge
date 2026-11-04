#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace dbgcap
{

namespace
{

constexpr DWORD buffer_size = 4096;
constexpr std::size_t message_capacity = buffer_size - sizeof(DWORD);
constexpr DWORD default_seconds = 60;

class Handle
{
public:
    explicit Handle(HANDLE handle) : handle_(handle)
    {
    }

    ~Handle()
    {
        if (handle_)
        {
            CloseHandle(handle_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator = (const Handle&) = delete;

    HANDLE get() const
    {
        return handle_;
    }

private:
    HANDLE handle_;
};

std::vector<std::string_view> parse_filters(int argc, char** argv)
{
    if (argc <= 2)
    {
        return {"openmeteo", "otif", "DnD"};
    }

    if (argc == 3 && std::string_view(argv[2]) == "*")
    {
        return {};
    }

    return std::vector<std::string_view>(argv + 2, argv + argc);
}

bool matches(std::string_view message, const std::vector<std::string_view>& filters)
{
    if (filters.empty())
    {
        return true;
    }

    for (const auto& filter : filters)
    {
        if (message.find(filter) != std::string_view::npos)
        {
            return true;
        }
    }

    return false;
}

std::string_view read_message(const char* view)
{
    const char* text = view + sizeof(DWORD);
    std::string_view message(text, strnlen(text, message_capacity));

    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
    {
        message.remove_suffix(1);
    }

    return message;
}

int run(DWORD seconds, const std::vector<std::string_view>& filters)
{
    const Handle buffer_ready(CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY"));
    const Handle data_ready(CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY"));
    const Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, buffer_size, L"DBWIN_BUFFER"));

    if (!buffer_ready.get() || !data_ready.get() || !mapping.get())
    {
        std::fprintf(stderr, "Error: Cannot open the debug buffer: %lu\n", GetLastError());
        return 1;
    }

    const auto* view = static_cast<const char*>(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, buffer_size));

    if (!view)
    {
        std::fprintf(stderr, "Error: Cannot map the debug buffer: %lu\n", GetLastError());
        return 1;
    }

    const ULONGLONG end = GetTickCount64() + seconds * 1000ULL;
    SetEvent(buffer_ready.get());

    for (ULONGLONG now = GetTickCount64(); now < end; now = GetTickCount64())
    {
        if (WaitForSingleObject(data_ready.get(), static_cast<DWORD>(end - now)) != WAIT_OBJECT_0)
        {
            continue;
        }

        DWORD pid = 0;
        std::memcpy(&pid, view, sizeof(pid));
        const std::string_view message = read_message(view);

        if (matches(message, filters))
        {
            std::printf("[%lu] %.*s\n", pid, static_cast<int>(message.size()), message.data());
            std::fflush(stdout);
        }

        SetEvent(buffer_ready.get());
    }

    UnmapViewOfFile(view);
    return 0;
}

}

}

int main(int argc, char** argv)
{
    const DWORD seconds = argc > 1
        ? static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10))
        : dbgcap::default_seconds;

    return dbgcap::run(seconds, dbgcap::parse_filters(argc, argv));
}
