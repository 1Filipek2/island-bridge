#pragma once

#include "core/common.h"
namespace island
{


constexpr wchar_t kWindowClass[] = L"Windhawk.DynamicIslandForWindows";
constexpr UINT WM_APP_LAYOUT_CHANGED = WM_APP + 0x442;
constexpr UINT WM_APP_NEW_EVENT = WM_APP + 0x443;
constexpr UINT WM_APP_MOUSE_WAKE = WM_APP + 0x446;
// RegisterHotKey cannot associate a hot key with a window created by another
// thread, and the same applies to tearing one down. LoadSettings runs on
// Windhawk's settings thread while the overlay window belongs to the render
// thread, so hotkey and backdrop changes are posted across and applied there.
constexpr UINT WM_APP_APPLY_HOTKEY = WM_APP + 0x447;
constexpr UINT WM_APP_APPLY_BACKDROP = WM_APP + 0x448;
constexpr int ID_HIDE_SHOW_HOTKEY = 1;
constexpr int ID_LOOKUP_HOTKEY = 2;
constexpr float kRenderPadX = 28.0f;
constexpr float kRenderPadY = 22.0f;
constexpr UINT kClipboardImageThumbMaxDim = 160;  // longer-side cap for the clipboard image thumbnail

}
