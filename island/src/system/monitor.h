#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{

bool IsForegroundFullscreen(HWND targetHwnd);

struct MonitorEnumData {
    std::vector<HMONITOR> monitors;
};

inline BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
    auto* data = reinterpret_cast<MonitorEnumData*>(dwData);
    data->monitors.push_back(hMonitor);
    return TRUE;
}

inline HMONITOR GetAnchorMonitorFor(int targetMonitor) {
    HMONITOR selectedMonitor = nullptr;

    if (targetMonitor == -1) {
        POINT pt = {0, 0};
        GetCursorPos(&pt);
        selectedMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else if (targetMonitor > 0) {
        MonitorEnumData data;
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));

        int index = targetMonitor - 1;
        if (index >= 0 && index < static_cast<int>(data.monitors.size())) {
            selectedMonitor = data.monitors[index];
        }
    }

    if (!selectedMonitor) {
        POINT pt = {0, 0};
        selectedMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    return selectedMonitor;
}

inline HMONITOR GetAnchorMonitor() {
    return GetAnchorMonitorFor(g_settings.targetMonitor);
}

// Returns the DPI scale factor for a given monitor (1.0 = 96 DPI = 100%)
inline float GetMonitorDpiScale(HMONITOR monitor) {
    if (!monitor) {
        POINT pt = {0, 0};
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitor_t>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (pGetDpiForMonitor && monitor) {
        pGetDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    }
    return static_cast<float>(dpiX) / 96.0f;
}
int GetMonitorRefreshRate(HWND hwnd);
D2D1_COLOR_F GetSystemAccentColor();

}
