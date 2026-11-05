#include "system/monitor.h"

namespace island
{
bool is_foreground_fullscreen(HWND target_hwnd)
{
    HWND fg = GetForegroundWindow();
    if (!fg || fg == target_hwnd || fg == GetDesktopWindow() || fg == GetShellWindow())
    {
        return false;
    }

    if (!IsWindowVisible(fg))
    {
        return false;
    }

    wchar_t class_name[256] = {};
    GetClassNameW(fg, class_name, 256);
    if (std::wcscmp(class_name, L"WorkerW") == 0 || std::wcscmp(class_name, L"Progman") == 0 ||
        std::wcscmp(class_name, L"Shell_TrayWnd") == 0)
    {
        return false;
    }

    RECT client_rect = {};
    if (!GetClientRect(fg, &client_rect))
    {
        return false;
    }
    POINT pt = {0, 0};
    ClientToScreen(fg, &pt);
    client_rect.left += pt.x;
    client_rect.right += pt.x;
    client_rect.top += pt.y;
    client_rect.bottom += pt.y;

    HMONITOR h_mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(MONITORINFO)};
    if (GetMonitorInfoW(h_mon, &mi))
    {
        if (client_rect.left <= mi.rcMonitor.left && client_rect.top <= mi.rcMonitor.top &&
            client_rect.right >= mi.rcMonitor.right && client_rect.bottom >= mi.rcMonitor.bottom)
        {
            return true;
        }
    }

    return false;
}

int get_monitor_refresh_rate(HWND hwnd)
{
    HMONITOR monitor = nullptr;
    if (hwnd)
    {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    else
    {
        POINT pt = {0, 0};
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi))
    {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        {
            if (dm.dmDisplayFrequency > 1)
            {
                return static_cast<int>(dm.dmDisplayFrequency);
            }
        }
    }
    HDC hdc = GetDC(nullptr);
    int rate = GetDeviceCaps(hdc, VREFRESH);
    ReleaseDC(nullptr, hdc);
    return (rate > 1) ? rate : 60;
}

D2D1_COLOR_F get_system_accent_color()
{
    DWORD color = 0;
    BOOL opaque = FALSE;
    using DwmGetColorizationColorT = HRESULT(WINAPI*)(DWORD*, BOOL*);
    auto proc = reinterpret_cast<DwmGetColorizationColorT>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmGetColorizationColor"));

    if (proc && SUCCEEDED(proc(&color, &opaque)))
    {
        return D2D1::ColorF(((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f, (color & 0xff) / 255.0f,
                            1.0f);
    }

    return D2D1::ColorF(0x4cc9f0);
}

}
