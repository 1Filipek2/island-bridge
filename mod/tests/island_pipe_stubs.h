#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct BitmapPixels
{
    std::vector<std::uint8_t> bgra;
    UINT width = 0;
    UINT height = 0;
};

struct PixelsRef
{
    PixelsRef() = default;

    explicit PixelsRef(BitmapPixels&& pixels)
        : valid_(!pixels.bgra.empty())
    {
    }

    explicit operator bool() const
    {
        return valid_;
    }

    bool valid_ = false;
};

inline bool DecodeImageBytesToPixels(const std::vector<std::uint8_t>& bytes, BitmapPixels* pixels)
{
    if (bytes.size() < 2)
    {
        return false;
    }

    pixels->width = bytes[0];
    pixels->height = bytes[1];
    pixels->bgra.assign(static_cast<std::size_t>(pixels->width) * pixels->height * 4, 0);
    return true;
}

struct NotificationSnapshot
{
    bool active = false;
    std::wstring app;
    std::wstring title;
    std::wstring body;
    PixelsRef icon;
    double expiresAt = 0.0;
};

struct SharedState
{
    NotificationSnapshot notification;
};

struct Settings
{
    bool notificationRespectDnD = true;
};

inline Settings GetSettingsCopy()
{
    return {};
}

inline std::wstring GetStringSettingCopy(PCWSTR)
{
    return L"";
}

inline PixelsRef FindAppIconByName(const std::wstring&, UINT)
{
    return {};
}

inline double NowSeconds()
{
    return 0.0;
}

inline void TriggerNudge()
{
}

inline void Wh_Log(const wchar_t*, ...)
{
}

inline std::atomic<bool> g_isDnDActive = false;
inline std::mutex g_stateMutex;
inline SharedState g_state;
inline HANDLE g_stopEvent = nullptr;
