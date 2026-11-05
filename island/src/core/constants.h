#pragma once

#include "core/common.h"
namespace island
{


constexpr wchar_t window_class[] = L"Windhawk.DynamicIslandForWindows";
constexpr UINT wm_app_layout_changed = WM_APP + 0x442;
constexpr UINT wm_app_new_event = WM_APP + 0x443;
constexpr UINT wm_app_mouse_wake = WM_APP + 0x446;
// RegisterHotKey cannot associate a hot key with a window created by another
// thread, and the same applies to tearing one down. LoadSettings runs on
// Windhawk's settings thread while the overlay window belongs to the render
// thread, so hotkey and backdrop changes are posted across and applied there.
constexpr UINT wm_app_apply_hotkey = WM_APP + 0x447;
constexpr UINT wm_app_apply_backdrop = WM_APP + 0x448;
constexpr int id_hide_show_hotkey = 1;
constexpr int id_lookup_hotkey = 2;
constexpr float render_pad_x = 28.0f;
constexpr float render_pad_y = 22.0f;
constexpr UINT clipboard_image_thumb_max_dim = 160;  // longer-side cap for the clipboard image thumbnail

}
