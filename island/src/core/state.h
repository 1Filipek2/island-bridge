#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{
// g_settings_mutex guards g_settings, which is replaced while other threads read it.
// Leaf lock: never take another mutex while holding it.
inline std::mutex g_settings_mutex;
Settings get_settings_copy();

inline std::mutex g_state_mutex;
inline SharedState g_state;
inline std::atomic<std::uint64_t> g_art_generation_counter = 0;

inline HWND g_hwnd = nullptr;
inline HANDLE g_stop_event = nullptr;
inline HANDLE g_settings_changed_event = nullptr;
inline HANDLE g_render_thread = nullptr;
inline HANDLE g_media_thread = nullptr;
inline HANDLE g_audio_thread = nullptr;
inline HANDLE g_weather_thread = nullptr;
inline HANDLE g_github_thread = nullptr;
inline HANDLE g_lyrics_thread = nullptr;
inline HANDLE g_bluetooth_thread = nullptr;
inline HANDLE g_island_pipe_thread = nullptr;
inline std::atomic<bool> g_running = false;
inline std::atomic<int> g_idle_tab = 0;

inline std::atomic<bool> g_default_output_is_bluetooth{false};
inline std::atomic<int> g_lyrics_effective_offset_ms{0};
inline std::atomic<int> g_lyrics_effective_offset_bt_ms{0};
inline std::atomic<bool> g_volume_endpoint_stale{true};
inline std::atomic<bool> g_layout_dirty = true;
inline std::atomic<bool> g_click_expanded = false;
inline std::atomic<int> g_pressed_media_button = -1;
inline std::atomic<int> g_hovered_media_button = -1;
inline std::atomic<unsigned> g_skip_trigger_prev{0};
inline std::atomic<unsigned> g_skip_trigger_next{0};

// Media source selection (source dock and context menu); g_media_source_mutex guards the strings.
inline std::mutex g_media_source_mutex;
inline std::wstring g_preferred_media_source;
inline std::wstring g_switch_target;
inline std::wstring g_dock_override_aumid;
inline std::atomic<unsigned> g_dock_override_rev{0};
inline std::atomic<int> g_switch_dir{1};
inline std::atomic<unsigned> g_switch_seq{0};
inline std::atomic<double> g_user_source_switch_at{-100.0};
inline HANDLE g_media_refresh_event = nullptr;
inline std::atomic<int> g_hovered_source_slot{-1};
inline std::atomic<int> g_pressed_source_slot{-1};
inline std::atomic<int> g_hovered_file_tray_row = -1;
inline std::atomic<int> g_hovered_file_tray_action = -1; // -2 = bin button, >=0 = cut button of that row
inline std::atomic<int> g_hovered_notification_row = -1;
inline std::atomic<int> g_hovered_notification_action = -1;
inline std::atomic<bool> g_notification_center_unread{false};
inline std::atomic<bool> g_notification_center_detail_open{false};
inline std::atomic<int> g_notification_center_detail_rows{1};
inline std::atomic<bool> g_tray_drag_over{false};
inline std::atomic<bool> g_tray_internal_drag{false};
inline std::atomic<int> g_tab_edge_bump{0}; // +1 / -1: wheel or swipe pushed past the last / first tab
inline std::atomic<double> g_tab_last_paint_at{0.0};
inline std::atomic<bool> g_tab_animating{false};
inline bool g_ole_initialized = false;
inline std::atomic<bool> g_scrubbing = false;
inline std::atomic<float> g_scrub_drag_fraction = 0.0f;
inline std::atomic<double> g_last_live_seek_time = 0.0;
inline std::atomic<bool> g_audio_capture_needed = false;
inline std::atomic<bool> g_spectrum_bands_needed = false;
inline std::atomic<bool> g_manually_hidden = false;
inline UINT g_registered_hotkey_modifiers = 0;
inline UINT g_registered_hotkey_vk = 0;
inline bool g_hotkey_registered = false;

// Zero-CPU parking while auto-hidden (idle or fullscreen)
inline std::atomic<bool> g_auto_hidden_parked = false;
inline std::atomic<bool> g_fullscreen_override_visible = false;
inline std::atomic<bool> g_is_fullscreen = false;
inline std::atomic<double> g_hotkey_unhide_until = 0.0;
inline FILETIME g_prev_idle_time = {};
inline FILETIME g_prev_kernel_time = {};
inline FILETIME g_prev_user_time = {};
inline UINT g_shell_hook_message = 0;
inline UINT g_taskbar_created_message = 0;
inline bool g_brightness_initialized = false;
inline int g_last_brightness = -1;
inline BYTE g_brightness_power_source = 255;
inline std::atomic<double> g_last_lyrics_view_time = 0.0;

inline std::mutex g_bluetooth_battery_cache_mutex;
inline std::unordered_map<std::wstring, int> g_bluetooth_battery_cache;
inline std::atomic<std::uint64_t> g_bluetooth_connect_generation = 0;
inline std::atomic<bool> g_is_dn_d_active = false;
inline void* g_wnf_dnd_subscription = nullptr;

inline std::atomic<bool> g_game_overlay_pinned{false};
inline std::atomic<bool> g_pinned_expanded{false};
inline std::atomic<int> g_pill_opacity_override{-1};
void trigger_nudge();

}
