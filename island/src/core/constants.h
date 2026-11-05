#pragma once

#include "core/common.h"

namespace island
{
constexpr wchar_t window_class[] = L"Windhawk.DynamicIslandForWindows";
constexpr UINT wm_app_layout_changed = WM_APP + 0x442;
constexpr UINT wm_app_new_event = WM_APP + 0x443;
constexpr UINT wm_app_mouse_wake = WM_APP + 0x446;
// Hotkeys and backdrops belong to the render thread, so settings changes are posted to it.
constexpr UINT wm_app_apply_hotkey = WM_APP + 0x447;
constexpr UINT wm_app_apply_backdrop = WM_APP + 0x448;
constexpr int id_hide_show_hotkey = 1;
constexpr int id_lookup_hotkey = 2;
constexpr float render_pad_x = 28.0f;
constexpr float render_pad_y = 22.0f;
constexpr UINT clipboard_image_thumb_max_dim = 160;

}
