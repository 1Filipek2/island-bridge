#pragma once

#include "core/common.h"
#include "core/pixels.h"

namespace island
{
namespace island_pipe
{

constexpr wchar_t pipe_name[] = L"\\\\.\\pipe\\DynamicIslandNotify";
constexpr DWORD max_message_bytes = 12288;
constexpr std::size_t max_icon_bytes = 8192;
constexpr UINT max_icon_dimension = 256;
constexpr std::uint64_t rate_window_ms = 1000;
constexpr int rate_limit = 10;
constexpr std::size_t max_app_chars = 64;
constexpr std::size_t max_key_chars = 128;
constexpr std::size_t max_title_chars = 120;
constexpr std::size_t max_body_chars = 512;
constexpr double display_seconds = 4.0;
constexpr double display_gap_seconds = 0.3;
constexpr std::size_t max_queued = 5;
constexpr DWORD retry_wait_ms = 5000;
constexpr wchar_t client_file_name[] = L"IslandBridge.exe";

struct PipeMessage
{
    std::uint8_t category = 0;
    std::uint8_t flags = 0;
    std::wstring app;
    std::wstring bundle;
    std::wstring title;
    std::wstring subtitle;
    std::wstring body;
    std::vector<std::uint8_t> icon;
};

class RateLimiter
{
public:
    bool allow(std::uint64_t now_ms)
    {
        if (now_ms - window_start_ > rate_window_ms)
        {
            window_start_ = now_ms;
            count_ = 0;
        }

        if (count_ >= rate_limit)
        {
            return false;
        }

        ++count_;
        return true;
    }

private:
    std::uint64_t window_start_ = 0;
    int count_ = 0;
};

class OwnedHandle
{
public:
    explicit OwnedHandle(HANDLE handle = nullptr) : handle_(handle)
    {
    }

    ~OwnedHandle()
    {
        reset();
    }

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    HANDLE get() const
    {
        return handle_;
    }

    bool valid() const
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE handle = nullptr)
    {
        if (valid())
        {
            CloseHandle(handle_);
        }

        handle_ = handle;
    }

private:
    HANDLE handle_;
};

std::wstring sanitize(const std::wstring& text, std::size_t max_chars);
std::optional<PipeMessage> parse_message(const std::uint8_t* data, std::size_t size);
PixelsRef decode_icon(const std::vector<std::uint8_t>& bytes);

}
}
