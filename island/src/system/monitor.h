#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{
bool is_foreground_fullscreen(HWND target_hwnd);

struct MonitorEnumData
{
    std::vector<HMONITOR> monitors;
};

inline BOOL CALLBACK monitor_enum_proc(HMONITOR h_monitor, HDC, LPRECT, LPARAM dw_data)
{
    auto* data = reinterpret_cast<MonitorEnumData*>(dw_data);
    data->monitors.push_back(h_monitor);
    return TRUE;
}

inline HMONITOR get_anchor_monitor_for(int target_monitor)
{
    HMONITOR selected_monitor = nullptr;

    if (target_monitor == -1)
    {
        POINT pt = {0, 0};
        GetCursorPos(&pt);
        selected_monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    else if (target_monitor > 0)
    {
        MonitorEnumData data;
        EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc, reinterpret_cast<LPARAM>(&data));

        int index = target_monitor - 1;
        if (index >= 0 && index < static_cast<int>(data.monitors.size()))
        {
            selected_monitor = data.monitors[index];
        }
    }

    if (!selected_monitor)
    {
        POINT pt = {0, 0};
        selected_monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    return selected_monitor;
}

inline HMONITOR get_anchor_monitor()
{
    return get_anchor_monitor_for(g_settings.target_monitor);
}

inline float get_monitor_dpi_scale(HMONITOR monitor)
{
    if (!monitor)
    {
        POINT pt = {0, 0};
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    UINT dpi_x = 96, dpi_y = 96;
    using GetDpiForMonitorT = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto p_get_dpi_for_monitor =
        reinterpret_cast<GetDpiForMonitorT>(GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (p_get_dpi_for_monitor && monitor)
    {
        p_get_dpi_for_monitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpi_x, &dpi_y);
    }
    return static_cast<float>(dpi_x) / 96.0f;
}

int get_monitor_refresh_rate(HWND hwnd);
D2D1_COLOR_F get_system_accent_color();

}
