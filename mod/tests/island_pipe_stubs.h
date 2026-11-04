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

struct PixelsRef
{
    explicit operator bool() const
    {
        return false;
    }
};

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
