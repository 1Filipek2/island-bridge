#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{

// Guards every access to g_settings. LoadSettings() replaces the whole struct
// from Windhawk's settings thread *and* from the tray context menu (which runs
// on the render thread), while the render, weather and media threads read it
// concurrently. Settings holds std::wstring / std::vector members, so an
// unsynchronised assignment can free a buffer a reader is still walking -- not
// a stale-value glitch but a genuine use-after-free.
//
// This is a strict LEAF lock: never acquire another mutex while holding it.
// That keeps it deadlock-free even where a caller already owns g_stateMutex
// (see WeatherThreadProc), because no path can ever take the two in the
// opposite order.
inline std::mutex g_settings_mutex;
Settings get_settings_copy();

inline std::mutex g_state_mutex;
inline SharedState g_state;
inline std::atomic<uint64_t> g_art_generation_counter = 0;

inline HWND g_hwnd = nullptr;
inline HANDLE g_stop_event = nullptr;
inline HANDLE g_settings_changed_event = nullptr;
inline HANDLE g_render_thread = nullptr;
inline HANDLE g_media_thread = nullptr;
inline HANDLE g_audio_thread = nullptr;
inline HANDLE g_weather_thread = nullptr;
inline HANDLE g_github_thread = nullptr;
inline HANDLE g_lyrics_thread = nullptr;
inline HANDLE g_notification_thread = nullptr;
inline HANDLE g_bluetooth_thread = nullptr;
inline HANDLE g_island_pipe_thread = nullptr;
inline std::atomic<bool> g_running = false;
inline std::atomic<int> g_idle_tab = 0;

// Lyrics sync offset. Written rarely (settings load, right-click nudge, default-output
// change) and only read by draw code, so no locks are needed.
inline std::atomic<bool> g_default_output_is_bluetooth{false}; // set by BindVolumeEndpoint(), event-driven
inline std::atomic<int> g_lyrics_effective_offset_ms{0};       // speakers / wired: setting + right-click nudge
inline std::atomic<int> g_lyrics_effective_offset_bt_ms{0};    // Bluetooth output: setting + right-click nudge
// Set when the default render endpoint must be re-read. Declared up here (not next to
// VolumeNotifyClient) so the Bluetooth watcher, which comes earlier in the file, can set it.
inline std::atomic<bool> g_volume_endpoint_stale{true};
inline std::atomic<bool> g_layout_dirty = true;
inline std::atomic<bool> g_click_expanded = false;
inline std::atomic<int> g_pressed_media_button = -1;
inline std::atomic<int> g_hovered_media_button = -1;
inline std::atomic<unsigned> g_skip_trigger_prev{
    0}; // bumped on click, the render thread plays the prev button's animation
inline std::atomic<unsigned> g_skip_trigger_next{0}; // same for the next button

// Media source selection (source dock + right-click submenu).
inline std::mutex g_media_source_mutex;              // leaf lock for the two strings below
inline std::wstring g_preferred_media_source;        // AUMID the user picked; empty = Auto
inline std::wstring g_switch_target;                 // pending animated switch
inline std::wstring g_dock_override_aumid;           // temporary pick from the dock; guarded by g_mediaSourceMutex
inline std::atomic<unsigned> g_dock_override_rev{0}; // bumped on every dock pick / Auto reset
inline std::atomic<int> g_switch_dir{1};
inline std::atomic<unsigned> g_switch_seq{0};
inline std::atomic<double> g_user_source_switch_at{-100.0}; // NowSeconds() of the last user switch
inline HANDLE g_media_refresh_event = nullptr;              // wakes MediaThreadProc
inline std::atomic<int> g_hovered_source_slot{-1};
inline std::atomic<int> g_pressed_source_slot{-1};
inline std::atomic<int> g_hovered_file_tray_row = -1;    // row index under the cursor on the File Tray card
inline std::atomic<int> g_hovered_file_tray_action = -1; // -2 = bin button, >=0 = cut button of that row
inline std::atomic<int> g_hovered_notification_row = -1;
inline std::atomic<int> g_hovered_notification_action = -1;
inline std::atomic<bool> g_notification_center_unread{false};
inline std::atomic<bool> g_notification_center_detail_open{false};
inline std::atomic<int> g_notification_center_detail_rows{1};
inline std::atomic<bool> g_tray_drag_over{false};     // a file drag from outside is hovering the island
inline std::atomic<bool> g_tray_internal_drag{false}; // a drag that started from the tray itself is running
inline std::atomic<int> g_tab_edge_bump{0};           // +1 / -1: wheel or swipe pushed past the last / first tab
inline std::atomic<double> g_tab_last_paint_at{0.0};  // last time the tab view painted a moving frame
inline std::atomic<bool> g_tab_animating{false};      // a tab transition is in motion (render loop paces frames for it)
inline bool g_ole_initialized = false;
inline std::atomic<bool> g_scrubbing = false;             // true while press-dragging the media timeline scrubber
inline std::atomic<float> g_scrub_drag_fraction = 0.0f;   // live 0..1 drag position while g_scrubbing is true
inline std::atomic<double> g_last_live_seek_time = 0.0;   // throttle gate for live seeks while dragging
inline std::atomic<bool> g_audio_capture_needed = false;  // gates the WASAPI loopback thread
inline std::atomic<bool> g_spectrum_bands_needed = false; // only Pulse Orb / Plasma Thread read the frequency bands
inline std::atomic<bool> g_manually_hidden = false;       // user-toggled hide, via menu or hotkey
inline UINT g_registered_hotkey_modifiers = 0;            // modifiers currently registered with the OS
inline UINT g_registered_hotkey_vk = 0;                   // vk currently registered with the OS
inline bool g_hotkey_registered = false;

// --- Zero-CPU parking for idle/fullscreen auto-hide (mirrors g_manuallyHidden) ---
inline std::atomic<bool> g_auto_hidden_parked = false; // true while OS-hidden due to idle/fullscreen auto-hide
inline std::atomic<bool> g_fullscreen_override_visible =
    false;                                        // user forced the island visible via hotkey while fullscreen
inline std::atomic<bool> g_is_fullscreen = false; // cached fullscreen state, shared with the hotkey handler
inline std::atomic<double> g_hotkey_unhide_until =
    0.0; // grace deadline to keep island visible after hotkey / settings unhide
inline FILETIME g_prev_idle_time = {};
inline FILETIME g_prev_kernel_time = {};
inline FILETIME g_prev_user_time = {};
inline UINT g_shell_hook_message = 0;
inline UINT g_taskbar_created_message = 0;
inline bool g_brightness_initialized = false;
inline int g_last_brightness = -1;
inline BYTE g_brightness_power_source = 255; // ACLineStatus of the previous sample
inline std::atomic<double> g_last_lyrics_view_time = 0.0;

inline std::mutex g_bluetooth_battery_cache_mutex;
inline std::unordered_map<std::wstring, int>
    g_bluetooth_battery_cache; // Bluetooth device Id -> last known battery percent (-1 = never learned)
inline std::atomic<uint64_t> g_bluetooth_connect_generation = 0;
inline std::atomic<bool> g_is_dn_d_active = false;
inline void* g_wnf_dnd_subscription = nullptr;

inline std::atomic<bool> g_game_overlay_pinned{false};
inline std::atomic<bool> g_pinned_expanded{false};
inline std::atomic<int> g_pill_opacity_override{-1};
void trigger_nudge();

}
