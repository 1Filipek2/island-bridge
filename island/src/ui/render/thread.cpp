#include "ui/render/renderer_impl.h"
#include "ui/renderer.h"

namespace island
{
namespace
{

Activity activity_for_kind(IslandKind kind, const Settings& settings, const SharedState& state)
{
    Activity activity;
    activity.kind = kind;

    switch (kind)
    {
        case IslandKind::Media:
            activity.width =
                media_pill_layout::base_width +
                (state.system.mic_active || state.system.camera_active ? media_pill_layout::privacy_extra_width : 0.0f);
            activity.height = 44.0f;
            break;
        case IslandKind::Progress:
            activity.width = 230.0f;
            activity.height = 48.0f;
            break;
        case IslandKind::Clipboard:
            activity.width = 340.0f;
            activity.height = 56.0f;
            break;
        case IslandKind::Notification:
            activity.width = 360.0f;
            activity.height = 58.0f;
            break;
        case IslandKind::Volume:
        case IslandKind::Brightness:
            activity.width = 300.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::BatteryLow:
            activity.width = 290.0f;
            activity.height = 52.0f;
            break;
        case IslandKind::CapsLock:
            activity.width = 180.0f;
            activity.height = 42.0f;
            break;
        case IslandKind::Device:
            activity.width = 240.0f;
            activity.height = 50.0f;
            break;
        case IslandKind::Bluetooth:
            activity.width = 280.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::Timer:
            activity.width = 260.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::DoNotDisturb:
            activity.width = 220.0f;
            activity.height = 42.0f;
            break;
        case IslandKind::Lookup:
            // Seed only: the render loop measures the real height (measure_lookup_card).
            activity.width = lookup_layout::width;
            activity.height = 42.0f;
            break;
        case IslandKind::Idle:
        default:
            if (settings.auto_hide_idle_seconds == -1 && !state.system.mic_active && !state.system.camera_active)
            {
                activity.width = 0.0f;
                activity.height = 0.0f;
            }
            else
            {
                // Seed only: the render loop measures the real collapsed width (measure_idle_strip).
                activity.width = settings.weather ? 170.0f : 96.0f;
                activity.height = idle_strip_layout::height;
            }
            break;
    }

    activity.width *= settings.size_scale;
    activity.height *= settings.size_scale;
    return activity;
}

}

std::vector<IslandKind> choose_activities(const SharedState& state, const Settings& settings, double now)
{
    std::vector<IslandKind> activities;

    if (settings.quick_lookup && state.lookup.active && now < state.lookup.expires_at)
    {
        activities.push_back(IslandKind::Lookup);
    }

    if (state.clipboard.active && now < state.clipboard.expires_at)
    {
        activities.push_back(IslandKind::Clipboard);
    }
    if (settings.caps_lock && state.caps_lock.active && now < state.caps_lock.expires_at)
    {
        activities.push_back(IslandKind::CapsLock);
    }
    if (state.device.active && now < state.device.expires_at)
    {
        activities.push_back(IslandKind::Device);
    }
    if (settings.bluetooth_indicator && state.bluetooth_device.active && now < state.bluetooth_device.expires_at)
    {
        activities.push_back(IslandKind::Bluetooth);
    }
    if (settings.do_not_disturb_indicator && state.do_not_disturb.active && now < state.do_not_disturb.expires_at)
    {
        activities.push_back(IslandKind::DoNotDisturb);
    }
    if (settings.volume && state.volume.active && now < state.volume.expires_at)
    {
        activities.push_back(IslandKind::Volume);
    }
    if (settings.brightness && state.brightness.active && now < state.brightness.expires_at)
    {
        activities.push_back(IslandKind::Brightness);
    }
    if (state.notification.active && now < state.notification.expires_at)
    {
        activities.push_back(IslandKind::Notification);
    }
    if (settings.battery && state.battery.active && now < state.battery.expires_at)
    {
        activities.push_back(IslandKind::BatteryLow);
    }
    if (settings.progress && state.progress.active)
    {
        activities.push_back(IslandKind::Progress);
    }
    if (settings.timer_enabled &&
        ((state.timer.just_finished && now < state.timer.finished_expires_at) || state.timer.active))
    {
        activities.push_back(IslandKind::Timer);
    }
    if (settings.media && state.media.available)
    {
        activities.push_back(IslandKind::Media);
    }

    if (activities.empty())
    {
        activities.push_back(IslandKind::Idle);
    }

    return activities;
}

DWORD WINAPI render_thread_proc(void*)
{
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = window_class;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                window_class, L"Dynamic Island for Windows", WS_POPUP, 0, 0, 520, 140, nullptr, nullptr,
                                wc.hInstance, nullptr);

    if (!hwnd)
    {
        host::log(L"Failed to create Dynamic Island overlay window.");
        if (SUCCEEDED(hr_co))
        {
            CoUninitialize();
        }
        return 0;
    }

    g_hwnd = hwnd;
    if (g_shell_hook_message == 0)
    {
        g_shell_hook_message = RegisterWindowMessageW(L"SHELLHOOK");
    }
    if (g_taskbar_created_message == 0)
    {
        g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    }
    using ChangeWindowMessageFilterExT = BOOL(WINAPI*)(HWND, UINT, DWORD, PVOID);
    static auto p_change_window_message_filter_ex = reinterpret_cast<ChangeWindowMessageFilterExT>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"));
    if (p_change_window_message_filter_ex)
    {
        if (g_shell_hook_message)
        {
            p_change_window_message_filter_ex(hwnd, g_shell_hook_message, 1 /*MSGFLT_ALLOW*/, nullptr);
        }
        if (g_taskbar_created_message)
        {
            p_change_window_message_filter_ex(hwnd, g_taskbar_created_message, 1 /*MSGFLT_ALLOW*/, nullptr);
        }
    }
    else
    {
        using ChangeWindowMessageFilterT = BOOL(WINAPI*)(UINT, DWORD);
        static auto p_change_window_message_filter = reinterpret_cast<ChangeWindowMessageFilterT>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilter"));
        if (p_change_window_message_filter)
        {
            if (g_shell_hook_message)
            {
                p_change_window_message_filter(g_shell_hook_message, 1 /*MSGFLT_ADD*/);
            }
            if (g_taskbar_created_message)
            {
                p_change_window_message_filter(g_taskbar_created_message, 1 /*MSGFLT_ADD*/);
            }
        }
    }
    enable_blur_behind(hwnd);
    apply_backdrop_material(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    apply_hide_show_hotkey();

    if (g_settings.auto_hide_idle_seconds == 0)
    {
        g_manually_hidden = false;
        host::set_int_value(L"ManuallyHidden", 0);
    }
    else
    {
        g_manually_hidden = host::int_value(L"ManuallyHidden", 0) != 0;
    }
    if (g_manually_hidden.load())
    {
        ShowWindow(hwnd, SW_HIDE);
    }

    Renderer renderer;
    if (!renderer.initialize(hwnd))
    {
        DestroyWindow(hwnd);
        g_hwnd = nullptr;
        if (SUCCEEDED(hr_co))
        {
            CoUninitialize();
        }
        return 0;
    }

    // Restore the File Tray here: the shell icons need this thread's COM apartment.
    load_file_tray();

    // 1 ms timer resolution costs power, so it is held only while the island animates. The flag is set
    // only when timeBeginPeriod succeeded, so begin/end stay balanced.
    bool high_res_timer = false;

    constexpr double high_res_timer_hold_sec = 0.5;
    double last_animating_at = -1.0;

    auto set_high_res_timer = [&](bool want)
    {
        if (want == high_res_timer)
        {
            return;
        }
        if (want)
        {
            if (timeBeginPeriod(1) == TIMERR_NOERROR)
            {
                high_res_timer = true;
            }
        }
        else
        {
            timeEndPeriod(1);
            high_res_timer = false;
        }
    };

    SpringValue width_spring;
    SpringValue height_spring;
    SpringValue nudge_spring;
    SpringValue volume_bar_spring;
    width_spring.reset((g_settings.auto_hide_idle_seconds == -1 ? 0.0f : 120.0f) * g_settings.size_scale);
    height_spring.reset((g_settings.auto_hide_idle_seconds == -1 ? 0.0f : 36.0f) * g_settings.size_scale);
    nudge_spring.reset(0.0f);

    IslandKind previous_primary = IslandKind::Idle;
    auto previous_frame = std::chrono::steady_clock::now();
    auto next_frame_target = previous_frame;
    double next_battery_poll = 0.0;
    double next_progress_poll = 0.0;
    double next_system_poll = 0.0;
    double next_brightness_poll = 0.0;
    double next_privacy_poll = 0.0;
    bool was_manually_hidden = false;
    bool was_auto_hidden_parked = false;
    bool parked_for_fullscreen = false;

    static double last_interaction_time = now_seconds();
    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == wm_app_new_event)
            {
                nudge_spring.value = -6.0f;
                nudge_spring.velocity = 0.0f;
                nudge_spring.target = 0.0f;
                last_interaction_time = now_seconds();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        bind_volume_endpoint();

        bool just_unhidden = false;

        if (g_manually_hidden.load())
        {
            // Manual hide owns the window state, so auto-park bookkeeping is cleared.
            g_auto_hidden_parked = false;
            was_auto_hidden_parked = false;

            if (!was_manually_hidden)
            {
                ShowWindow(hwnd, SW_HIDE);
                dwm_backdrop::hide_all();
                g_audio_capture_needed.store(false, std::memory_order_relaxed);
                was_manually_hidden = true;
            }
            previous_frame = std::chrono::steady_clock::now();
            // Fully parked: only the stop event or a message wakes this thread; no timers.
            set_high_res_timer(false);
            MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, INFINITE, QS_ALLINPUT);
            next_frame_target = std::chrono::steady_clock::now();
            continue;
        }

        // Before the parked branch on purpose: that branch can block for up to 1.5 s.
        {
            static bool prev_parked_for_hook = false;
            const bool parked_now = g_auto_hidden_parked.load(std::memory_order_relaxed);
            if (parked_now != prev_parked_for_hook)
            {
                prev_parked_for_hook = parked_now;
                notify_mouse_thread_parked_changed();
            }
        }

        if (g_auto_hidden_parked.load())
        {
            if (g_settings.auto_hide_idle_seconds == 0 && !parked_for_fullscreen)
            {
                g_auto_hidden_parked = false;
            }
            else
            {
                previous_frame = std::chrono::steady_clock::now();
                set_high_res_timer(false);
                if (parked_for_fullscreen)
                {
                    MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, 1500, QS_ALLINPUT);
                    const bool still_fullscreen = g_settings.auto_hide_fullscreen && is_foreground_fullscreen(hwnd);
                    g_is_fullscreen.store(still_fullscreen, std::memory_order_relaxed);
                    if (!still_fullscreen)
                    {
                        g_fullscreen_override_visible = false;
                        g_auto_hidden_parked = false;
                    }
                    else if (g_fullscreen_override_visible.load())
                    {
                        g_auto_hidden_parked = false;
                    }
                }
                else
                {
                    MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, INFINITE, QS_ALLINPUT);
                }
                next_frame_target = std::chrono::steady_clock::now();
                continue;
            }
        }

        if (was_manually_hidden)
        {
            was_manually_hidden = false;
            just_unhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            last_interaction_time = now_seconds();
            g_layout_dirty = true;
            previous_frame = std::chrono::steady_clock::now();
            next_frame_target = previous_frame;
        }

        if (was_auto_hidden_parked && !g_auto_hidden_parked.load())
        {
            was_auto_hidden_parked = false;
            just_unhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            last_interaction_time = now_seconds();
            g_layout_dirty = true;
            previous_frame = std::chrono::steady_clock::now();
            next_frame_target = previous_frame;
        }

        const double now = now_seconds();
        // The registry and device polls below can take several ms, so they wait for tab animations to settle.
        const bool tab_busy = g_tab_animating.load(std::memory_order_relaxed) &&
                              (now - g_tab_last_paint_at.load(std::memory_order_relaxed)) < 0.25;
        const bool spring_busy = std::fabs(width_spring.value - width_spring.target) > 0.5f ||
                                 std::fabs(height_spring.value - height_spring.target) > 0.5f ||
                                 std::fabs(width_spring.velocity) > 0.5f || std::fabs(height_spring.velocity) > 0.5f;
        const bool poll_busy = tab_busy || spring_busy;
        if (now >= next_battery_poll)
        {
            update_battery_snapshot();
            next_battery_poll = now + 15.0;
        }
        if (now >= next_progress_poll)
        {
            update_progress_snapshot();
            next_progress_poll = now + 0.25;
        }
        if (!poll_busy && now >= next_privacy_poll)
        {
            update_privacy_indicators();
            next_privacy_poll = now + 2.0; // poll every 2 s
        }

        bool timer_just_completed = false;
        SharedState snapshot;
        {
            std::lock_guard lock(g_state_mutex);
            snapshot = g_state;
            if (g_state.timer.active && g_state.timer.running && now >= g_state.timer.ends_at)
            {
                g_state.timer.active = false;
                g_state.timer.running = false;
                g_state.timer.just_finished = true;
                g_state.timer.finished_expires_at = now + 6.0;
                timer_just_completed = true;
            }
            if (g_state.timer.just_finished && now >= g_state.timer.finished_expires_at)
            {
                g_state.timer.just_finished = false;
            }
            snapshot.timer = g_state.timer;
            if (g_state.clipboard.active && now >= g_state.clipboard.expires_at)
            {
                g_state.clipboard.active = false;
                snapshot.clipboard.active = false;
            }
            if (g_state.notification.active && now >= g_state.notification.expires_at)
            {
                g_state.notification.active = false;
                snapshot.notification.active = false;
            }
            if (g_state.volume.active && now >= g_state.volume.expires_at)
            {
                g_state.volume.active = false;
                snapshot.volume.active = false;
            }
            if (g_state.brightness.active && now >= g_state.brightness.expires_at)
            {
                g_state.brightness.active = false;
                snapshot.brightness.active = false;
            }
            if (g_state.caps_lock.active && now >= g_state.caps_lock.expires_at)
            {
                g_state.caps_lock.active = false;
                snapshot.caps_lock.active = false;
            }
            if (g_state.battery.active && now >= g_state.battery.expires_at)
            {
                g_state.battery.active = false;
                snapshot.battery.active = false;
            }
            if (g_state.device.active && now >= g_state.device.expires_at)
            {
                g_state.device.active = false;
                snapshot.device.active = false;
            }
        }
        if (timer_just_completed)
        {
            trigger_nudge();
        }

        if (g_lookup_ui.open &&
            (!g_settings.quick_lookup || !snapshot.lookup.active || now >= snapshot.lookup.expires_at))
        {
            close_lookup_panel(hwnd);
        }

        const std::vector<IslandKind> kinds = choose_activities(snapshot, g_settings, now);
        Activity primary = activity_for_kind(kinds[0], g_settings, snapshot);
        std::optional<Activity> secondary;
        if (kinds.size() >= 2)
        {
            secondary = activity_for_kind(kinds[1], g_settings, snapshot);
        }

        const bool pinned = g_pinned_expanded.load();

        if (primary.kind != previous_primary)
        {
            if (primary.kind != IslandKind::Idle)
            {
                nudge_spring.value = -6.0f;
                nudge_spring.velocity = 0.0f;
                nudge_spring.target = 0.0f;
            }
            last_interaction_time = now;
        }
        previous_primary = primary.kind;

        POINT cursor = {};
        GetCursorPos(&cursor);

        // Pill rectangle in screen space, from the same layout render uses (the window is a fixed stage).
        const bool flush_top_now = g_settings.notch_style || g_settings.border_merged_mode;
        const StageLayout hover_stage = compute_stage_layout(g_settings, width_spring.value, height_spring.value);
        const float hover_top =
            hover_stage.pill_top + (flush_top_now ? std::max(0.0f, nudge_spring.value) : nudge_spring.value);
        RECT pill_rect = {hover_stage.origin.x + static_cast<int>(std::lround(hover_stage.pill_left)),
                          hover_stage.origin.y + static_cast<int>(std::lround(hover_top)),
                          hover_stage.origin.x +
                              static_cast<int>(std::lround(hover_stage.pill_left + width_spring.value)),
                          hover_stage.origin.y + static_cast<int>(std::lround(hover_top + height_spring.value))};

        bool hover = false;
        if (width_spring.value > 1.0f && height_spring.value > 1.0f)
        {
            hover = PtInRect(&pill_rect, cursor) != FALSE;
        }
        else if (g_settings.unhide_on_hover)
        {
            RECT dock_rect = get_island_dock_rect();
            hover = PtInRect(&dock_rect, cursor) != FALSE;
        }

        // A file drag over the island counts as hover, so it expands onto the File Tray.
        if (g_tray_drag_over.load(std::memory_order_relaxed))
        {
            hover = true;
        }

        bool needs_render = false;

        if (!hover && g_click_expanded.load())
        {
            g_click_expanded = false;
            needs_render = true;
        }
        if (!hover && g_notification_center_detail_open.load(std::memory_order_relaxed))
        {
            close_notification_center_detail();
            needs_render = true;
        }
        if (!hover && g_hovered_media_button.load() != -1)
        {
            g_hovered_media_button = -1;
            needs_render = true;
        }
        // Only the 5 s after a title change is transient, so the pill surfaces and then auto-hides again
        // while playback continues.
        const bool recent_track_change = g_settings.media_auto_expand && !media_expand_blocked(snapshot.media) &&
                                         primary.kind == IslandKind::Media && snapshot.media.playing &&
                                         !snapshot.media.title.empty() && (now - snapshot.media.title_changed_at < 5.0);

        bool is_transient_alert =
            (primary.kind == IslandKind::Clipboard || primary.kind == IslandKind::Notification ||
             primary.kind == IslandKind::Volume || primary.kind == IslandKind::Brightness ||
             primary.kind == IslandKind::BatteryLow || primary.kind == IslandKind::CapsLock ||
             primary.kind == IslandKind::Device || primary.kind == IslandKind::Bluetooth ||
             primary.kind == IslandKind::DoNotDisturb || primary.kind == IslandKind::Lookup || recent_track_change);

        const bool unhide_grace_active = (now < g_hotkey_unhide_until.load());
        const bool hover_unhides = g_settings.unhide_on_hover && hover;

        bool currently_hidden = false;
        if (!unhide_grace_active)
        {
            if (g_settings.auto_hide_idle_seconds == -1 && !is_transient_alert && !pinned)
            {
                currently_hidden = true;
            }
            else if (g_settings.auto_hide_idle_seconds > 0)
            {
                currently_hidden = (now - last_interaction_time > g_settings.auto_hide_idle_seconds);
            }
        }

        bool is_hover_expanded = g_settings.expand_on_hover ? hover : (hover && g_click_expanded.load());
        const bool game_metrics_present =
            primary.kind == IslandKind::Idle && (g_settings.game_overlay || g_game_overlay_pinned.load());
        if (game_metrics_present)
        {
            is_hover_expanded = false;
        }

        if (currently_hidden && !g_settings.unhide_on_hover)
        {
            is_hover_expanded = false;
        }
        else if (is_hover_expanded || hover_unhides || pinned || is_transient_alert || unhide_grace_active)
        {
            last_interaction_time = now;
        }

        bool is_hidden = false;
        if (!unhide_grace_active)
        {
            if (g_settings.auto_hide_idle_seconds == -1 && !is_transient_alert && !is_hover_expanded &&
                !hover_unhides && !pinned)
            {
                is_hidden = true;
            }
            else if (g_settings.auto_hide_idle_seconds > 0)
            {
                if (hover_unhides)
                {
                    is_hidden = false;
                }
                else
                {
                    is_hidden = (now - last_interaction_time > g_settings.auto_hide_idle_seconds);
                }
            }
        }

        static bool is_fullscreen = false;
        static double last_fullscreen_check = -1.0; // -1.0 guarantees the very first iteration checks
        if (now - last_fullscreen_check > 0.5)
        {
            const bool new_fullscreen = g_settings.auto_hide_fullscreen && is_foreground_fullscreen(hwnd);
            if (is_fullscreen && !new_fullscreen)
            {
                g_fullscreen_override_visible = false;
            }
            is_fullscreen = new_fullscreen;
            g_is_fullscreen.store(is_fullscreen, std::memory_order_relaxed);
            last_fullscreen_check = now;
        }

        // Reclaim the top of the z-order (rate-limited, skipped while hidden).
        static double last_topmost_check = 0.0;
        if (!is_fullscreen && !g_manually_hidden.load() && !g_auto_hidden_parked.load() &&
            now - last_topmost_check > 1.0)
        {
            ensure_topmost(hwnd, &pill_rect);
            last_topmost_check = now;
        }

        const bool mic_indicator_active =
            snapshot.system.mic_active && g_settings.privacy_dots && g_settings.privacy_dots_mic;
        const bool cam_indicator_active =
            snapshot.system.camera_active && g_settings.privacy_dots && g_settings.privacy_dots_cam;
        const bool privacy_active = mic_indicator_active || cam_indicator_active;

        // Any surface showing CPU, RAM, disk, GPU or network figures: decides both sampling the expensive
        // counters and repainting on their changes.
        const int metric_tab_idx = normalized_tab_index(g_settings);
        const bool on_hardware_monitor_tab = (metric_tab_idx == hardware_monitor_tab_index(g_settings));
        const bool hw_monitor_visible = (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) &&
                                        !is_fullscreen && !game_metrics_present && (pinned || is_hover_expanded) &&
                                        on_hardware_monitor_tab;
        const bool game_overlay_visible = game_metrics_present && !is_fullscreen;
        const bool system_metrics_visible = hw_monitor_visible || game_overlay_visible;

        if (g_settings.brightness && !poll_busy && now >= next_brightness_poll)
        {
            update_brightness_snapshot();
            next_brightness_poll = now + 0.25;
        }

        if (!poll_busy && now >= next_system_poll)
        {
            const bool need_gpu_stats = game_overlay_visible || hw_monitor_visible;
            const bool need_net_stats = hw_monitor_visible; // net is only ever drawn in the HW dashboard

            update_system_snapshot(need_gpu_stats, need_net_stats);
            if (!g_endpoint_volume)
            {
                g_volume_endpoint_stale = true;
            }
            next_system_poll = now + 1.0;
        }

        if (primary.kind == IslandKind::Idle)
        {
            if (!is_fullscreen && (pinned || is_hover_expanded))
            {
                primary.width = media_layout::expanded_width * g_settings.size_scale;
                primary.height = media_layout::expanded_height * g_settings.size_scale;
            }
            else if (primary.width > 0.0f)
            {
                primary.width =
                    renderer.measure_idle_strip(snapshot, g_settings, now).total_width * g_settings.size_scale;
            }
        }
        if (!is_fullscreen && primary.kind == IslandKind::Idle &&
            (g_settings.game_overlay || g_game_overlay_pinned.load()))
        {
            const int metric_count =
                (g_settings.game_overlay_show_cpu ? 1 : 0) + (g_settings.game_overlay_show_ram ? 1 : 0) +
                (g_settings.game_overlay_show_gpu ? 1 : 0) + (g_settings.game_overlay_show_disk ? 1 : 0);

            const bool compact = g_settings.game_overlay_compact;
            primary.width = game_overlay_layout::width(compact, g_settings.game_overlay_show_fps, metric_count) *
                            g_settings.size_scale;
            primary.height = game_overlay_layout::metrics_for(compact).height * g_settings.size_scale;
        }
        const auto collapsed_media_width = [&](float fallback, bool allow_lyrics)
        {
            const float clock_section =
                g_settings.media_pill_clock ? renderer.measure_media_pill(snapshot, g_settings).section_width : 0.0f;

            if (allow_lyrics && collapsed_lyrics_active(snapshot, g_settings))
            {
                return (collapsed_lyrics_width + clock_section) * g_settings.size_scale;
            }

            if (!g_settings.media_pill_clock)
            {
                return fallback;
            }

            const float privacy_extra = snapshot.system.mic_active || snapshot.system.camera_active
                                            ? media_pill_layout::privacy_extra_width
                                            : 0.0f;
            return (media_pill_layout::base_width + privacy_extra + clock_section) * g_settings.size_scale;
        };

        if (primary.kind == IslandKind::Media)
        {
            if (!is_fullscreen && (is_hover_expanded || pinned || recent_track_change))
            {
                primary.width = media_layout::expanded_width * g_settings.size_scale;
                primary.height = media_layout::expanded_height * g_settings.size_scale;
            }
            else
            {
                primary.width = collapsed_media_width(primary.width, true);
            }
        }

        if (secondary && secondary->kind == IslandKind::Media && secondary->width > 1.0f)
        {
            secondary->width = collapsed_media_width(secondary->width, false);
        }

        if (primary.kind == IslandKind::Lookup)
        {
            secondary.reset();
            if (primary.width > 1.0f)
            {
                const Settings lookup_settings = get_settings_copy();
                primary.height = renderer.measure_lookup_card(snapshot, lookup_settings) * lookup_settings.size_scale;
            }
        }

        const bool fullscreen_suppressed =
            is_fullscreen && !g_fullscreen_override_visible.load(std::memory_order_relaxed);
        if ((is_hidden || fullscreen_suppressed) && !privacy_active && !pinned && !is_hover_expanded &&
            !is_transient_alert)
        {
            primary.width = 0.0f;
            primary.height = 0.0f;
            secondary.reset();
        }

        const bool media_waveform_visible =
            g_settings.media && snapshot.media.playing && g_settings.spectrum_style != SpectrumStyle::Bars &&
            ((primary.kind == IslandKind::Media && primary.width > 1.0f && primary.height > 1.0f) ||
             (secondary && secondary->kind == IslandKind::Media && secondary->width > 1.0f &&
              secondary->height > 1.0f));
        g_audio_capture_needed.store(media_waveform_visible, std::memory_order_relaxed);

        float target_width = primary.width;
        float target_height = primary.height;
        if (secondary)
        {
            target_width = primary.width + secondary->width + 12.0f * g_settings.size_scale;
            target_height = std::max(primary.height, secondary->height);
        }

        width_spring.target = target_width;
        height_spring.target = target_height;

        if (just_unhidden || (unhide_grace_active && width_spring.value < 0.5f && target_width > 1.0f))
        {
            width_spring.reset(target_width);
            height_spring.reset(target_height);
            nudge_spring.reset(0.0f);
        }

        const auto current_frame = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(current_frame - previous_frame).count();
        previous_frame = current_frame;
        dt = clamp(dt, 0.001f, 0.050f);

        float style_stiffness_mult = 1.0f;
        float style_damping_mult = 1.0f;
        if (g_settings.animation_style == AnimationStyle::Smooth)
        {
            style_stiffness_mult = 1.0f;
            style_damping_mult = 1.35f;
        }
        else if (g_settings.animation_style == AnimationStyle::Bouncy)
        {
            style_stiffness_mult = 1.1f;
            style_damping_mult = 0.70f;
        }
        else if (g_settings.animation_style == AnimationStyle::Snappy)
        {
            style_stiffness_mult = 1.5f;
            style_damping_mult = 1.25f;
        }

        const float speed = g_settings.animation_speed;
        float width_stiffness = 280.0f * style_stiffness_mult;
        float width_damping = 24.0f * style_damping_mult;
        if (target_width > width_spring.value)
        {
            width_stiffness = 380.0f * style_stiffness_mult;
            width_damping = 26.0f * style_damping_mult;
        }
        else if (target_width < width_spring.value)
        {
            width_stiffness = 200.0f * style_stiffness_mult;
            width_damping = 28.0f * style_damping_mult;
        }

        float height_stiffness = 280.0f * style_stiffness_mult;
        float height_damping = 24.0f * style_damping_mult;
        if (target_height > height_spring.value)
        {
            height_stiffness = 380.0f * style_stiffness_mult;
            height_damping = 26.0f * style_damping_mult;
        }
        else if (target_height < height_spring.value)
        {
            height_stiffness = 200.0f * style_stiffness_mult;
            height_damping = 28.0f * style_damping_mult;
        }

        width_spring.step(dt * speed, width_stiffness, width_damping);
        if (width_spring.value < 0.0f)
        {
            width_spring.value = 0.0f;
            width_spring.velocity = 0.0f;
        }

        height_spring.step(dt * speed, height_stiffness, height_damping);
        if (height_spring.value < 0.0f)
        {
            height_spring.value = 0.0f;
            height_spring.velocity = 0.0f;
        }

        nudge_spring.step(dt * speed, 280.0f * style_stiffness_mult, 24.0f * style_damping_mult);

        if (snapshot.volume.active)
        {
            volume_bar_spring.target = static_cast<float>(snapshot.volume.percent);
        }
        else
        {
            volume_bar_spring.reset(static_cast<float>(snapshot.system.volume_percent));
        }
        volume_bar_spring.step(dt * speed, 520.0f, 46.0f);
        snapshot.volume.display_percent = volume_bar_spring.value;

        {
            std::lock_guard lock(g_state_mutex);
            g_state.system.render_fps = clamp_int(static_cast<int>(1.0f / std::max(dt, 0.001f) + 0.5f), 0, 1000);
        }

        const bool dragging_or_hover =
            hover || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && PtInRect(&pill_rect, cursor));

        // Ctrl+hover: the island turns see-through and click-through until Ctrl is released.
        const bool ctrl_held = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool ctrl_hover_ct = hover && ctrl_held && !g_lookup_ui.open;

        set_click_through(hwnd, (primary.kind == IslandKind::Idle && !dragging_or_hover && !pinned) || ctrl_hover_ct);

        if (std::abs(width_spring.velocity) > 0.01f || std::abs(width_spring.target - width_spring.value) > 0.01f ||
            std::abs(height_spring.velocity) > 0.01f || std::abs(height_spring.target - height_spring.value) > 0.01f ||
            std::abs(nudge_spring.velocity) > 0.01f || std::abs(nudge_spring.target - nudge_spring.value) > 0.01f ||
            std::abs(volume_bar_spring.velocity) > 0.01f ||
            std::abs(volume_bar_spring.target - volume_bar_spring.value) > 0.01f)
        {
            needs_render = true;
        }

        {
            HMONITOR current_anchor = nullptr;
            get_anchor_work_rect(&current_anchor);
            const float current_dpi = get_monitor_dpi_scale(current_anchor);
            static HMONITOR s_last_anchor_mon = nullptr;
            static float s_last_dpi = 0.0f;
            if (current_anchor != s_last_anchor_mon || current_dpi != s_last_dpi)
            {
                s_last_anchor_mon = current_anchor;
                s_last_dpi = current_dpi;
                std::lock_guard lock(g_settings_mutex);
                if (g_settings.auto_dpi_scale)
                {
                    g_settings.size_scale = g_settings.base_size_scale * current_dpi;
                }
                else
                {
                    g_settings.size_scale = g_settings.base_size_scale;
                }
                g_layout_dirty = true;
            }
        }

        if (g_lookup_ui.open && primary.kind == IslandKind::Lookup)
        {
            static double s_last_lookup_paint = 0.0;
            if (now - s_last_lookup_paint >= 0.2)
            {
                s_last_lookup_paint = now;
                needs_render = true;
            }
        }

        // A tab transition keeps painting until its spring settles (without layout upkeep every frame).
        if (g_tab_animating.load(std::memory_order_relaxed))
        {
            if (tab_busy)
            {
                needs_render = true;
            }
            else
            {
                g_tab_animating.store(false, std::memory_order_relaxed);
            }
        }

        if (g_layout_dirty.load())
        {
            needs_render = true;
        }

        static bool prev_hover = false;
        static bool prev_pinned = false;
        if (hover != prev_hover || pinned != prev_pinned)
        {
            needs_render = true;
            prev_hover = hover;
            prev_pinned = pinned;
        }

        // Continuous animation (playing media bars, marquees, battery pulse) wants ~60 Hz; the pacer at the
        // bottom enforces the rate. Paused media is not continuous. The secondary pill counts too.
        const bool media_shown =
            primary.kind == IslandKind::Media || (secondary && secondary->kind == IslandKind::Media);
        const bool media_animating = media_shown && (snapshot.media.playing || is_hover_expanded || pinned);

        // Transient alert pills expire within seconds. Device, Bluetooth and DND count only with the
        // countdown bar on.
        const auto is_transient_pill = [](IslandKind kind)
        {
            return kind == IslandKind::BatteryLow || kind == IslandKind::Clipboard || kind == IslandKind::Notification;
        };
        const auto has_countdown_bar = [](IslandKind kind)
        {
            return kind == IslandKind::Device || kind == IslandKind::Bluetooth || kind == IslandKind::DoNotDisturb;
        };
        const bool countdown_bars_on = g_settings.status_countdown_progress;

        const bool continuous_animation =
            media_animating || is_transient_pill(primary.kind) || (secondary && is_transient_pill(secondary->kind)) ||
            (countdown_bars_on &&
             (has_countdown_bar(primary.kind) || (secondary && has_countdown_bar(secondary->kind))));
        if (continuous_animation)
        {
            needs_render = true;
        }

        if (snapshot.system.mic_active || snapshot.system.camera_active)
        {
            needs_render = true;
        }

        static SYSTEMTIME prev_time = {};
        const bool clock_on_screen =
            primary.kind == IslandKind::Idle || (primary.kind == IslandKind::Media && g_settings.media_pill_clock);
        if (clock_on_screen && !is_hidden)
        {
            SYSTEMTIME local = {};
            GetLocalTime(&local);
            if (local.wMinute != prev_time.wMinute)
            {
                needs_render = true;
                prev_time = local;
            }
        }

        // Text that ticks once a second (focus timer, Clock with seconds): repaint exactly when the shown
        // value changes, without asking for continuous pacing.
        int shown_second = -1;
        const bool timer_shown =
            primary.kind == IslandKind::Timer || (secondary && secondary->kind == IslandKind::Timer);
        if (timer_shown && snapshot.timer.running)
        {
            shown_second = static_cast<int>(std::ceil(snapshot.timer.ends_at - now));
        }
        else if (clock_on_screen && g_settings.show_seconds && !is_hidden)
        {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            shown_second = st.wSecond;
        }
        static int s_prev_shown_second = -1;
        if (shown_second != s_prev_shown_second)
        {
            s_prev_shown_second = shown_second;
            needs_render = true;
        }

        static std::uint64_t prev_art_gen = 0;
        static std::uint64_t prev_src_icon_gen = 0;
        static std::uint64_t prev_notif_icon_gen = 0;
        static std::uint64_t prev_clip_icon_gen = 0;
        static int prev_cpu = -1;
        static int prev_ram = -1;
        static int prev_disk = -1;
        static int prev_vol = -1;
        static int prev_bright = -1;
        static bool prev_muted = false;
        static int prev_bat = -1;
        static bool prev_charging = false;
        static int prev_prog = -1;
        static std::wstring prev_media_title;
        static bool prev_playing = false;

        // CPU, RAM and disk are compared only while something shows them (CPU changes on every sample).
        const bool system_metrics_changed = system_metrics_visible && (snapshot.system.cpu_percent != prev_cpu ||
                                                                       snapshot.system.memory_percent != prev_ram ||
                                                                       snapshot.system.disk_free_percent != prev_disk);

        if (snapshot.media.art_generation != prev_art_gen ||
            snapshot.media.source_icon_generation != prev_src_icon_gen || snapshot.media.title != prev_media_title ||
            snapshot.media.playing != prev_playing || snapshot.notification.icon->generation != prev_notif_icon_gen ||
            snapshot.clipboard.app_icon->generation != prev_clip_icon_gen || system_metrics_changed ||
            snapshot.system.volume_percent != prev_vol || snapshot.system.volume_muted != prev_muted ||
            snapshot.brightness.percent != prev_bright || snapshot.battery.percent != prev_bat ||
            snapshot.battery.charging != prev_charging || snapshot.progress.percent != prev_prog)
        {
            needs_render = true;
            prev_art_gen = snapshot.media.art_generation;
            prev_src_icon_gen = snapshot.media.source_icon_generation;
            prev_media_title = snapshot.media.title;
            prev_playing = snapshot.media.playing;
            prev_notif_icon_gen = snapshot.notification.icon->generation;
            prev_clip_icon_gen = snapshot.clipboard.app_icon->generation;
            prev_cpu = snapshot.system.cpu_percent;
            prev_ram = snapshot.system.memory_percent;
            prev_disk = snapshot.system.disk_free_percent;
            prev_vol = snapshot.system.volume_percent;
            prev_muted = snapshot.system.volume_muted;
            prev_bright = snapshot.brightness.percent;
            prev_bat = snapshot.battery.percent;
            prev_charging = snapshot.battery.charging;
            prev_prog = snapshot.progress.percent;
        }

        static bool prev_ctrl_hover_ct = false;
        if (ctrl_hover_ct != prev_ctrl_hover_ct)
        {
            needs_render = true;
            prev_ctrl_hover_ct = ctrl_hover_ct;
        }

        if (needs_render)
        {
            Settings render_settings = get_settings_copy();
            if (ctrl_hover_ct)
            {
                render_settings.pill_opacity = clamp(render_settings.pill_opacity * 0.35f, 0.15f, 0.45f);
            }
            else if (render_settings.theme_preset == ThemePreset::Graphite && g_pill_opacity_override.load() < 0)
            {
                const bool is_expanded =
                    is_hover_expanded || pinned || is_transient_alert || (width_spring.value > 260.0f);
                render_settings.pill_opacity = is_expanded ? 0.98f : 0.88f;
            }
            if (render_settings.backdrop_material != BackdropMaterial::None && !dwm_backdrop::available())
            {
                render_settings.backdrop_material = BackdropMaterial::None;
            }
            renderer.render(snapshot, render_settings, primary, secondary, width_spring.value, height_spring.value,
                            nudge_spring.value, hover, pinned, now);
        }

        // Zero-CPU parking for auto-hide: only after the collapse animation settled at 0.
        const bool wants_auto_hidden_park =
            !g_manually_hidden.load() && !pinned && !is_hover_expanded && !is_transient_alert && !privacy_active &&
            (is_hidden || fullscreen_suppressed) && width_spring.value < 0.5f && height_spring.value < 0.5f &&
            std::fabs(width_spring.velocity) < 0.5f && std::fabs(height_spring.velocity) < 0.5f;

        if (wants_auto_hidden_park)
        {
            parked_for_fullscreen = fullscreen_suppressed;
            was_auto_hidden_parked = true;
            g_auto_hidden_parked = true;
            ShowWindow(hwnd, SW_HIDE);
            dwm_backdrop::hide_all();
            g_audio_capture_needed.store(false, std::memory_order_relaxed);
            set_high_res_timer(false);
            continue;
        }

        int target_fps = g_settings.target_fps;
        if (target_fps <= 0)
        {
            static int s_cached_rate = 60;
            static double s_rate_checked_at = -1e9;
            if (now - s_rate_checked_at > 2.0)
            {
                s_cached_rate = get_monitor_refresh_rate(hwnd);
                s_rate_checked_at = now;
            }
            target_fps = s_cached_rate;
        }
        target_fps = clamp_int(target_fps, 30, 1000);
        double target_frame_ms = 1000.0 / static_cast<double>(target_fps);

        // High Target FPS only for spring motion; continuous content relaxes to 60 Hz (never above the user's
        // lower setting).
        const bool springs_animating =
            std::fabs(width_spring.value - width_spring.target) > 0.5f ||
            std::fabs(height_spring.value - height_spring.target) > 0.5f || std::fabs(width_spring.velocity) > 0.5f ||
            std::fabs(height_spring.velocity) > 0.5f || std::fabs(nudge_spring.value) > 0.5f ||
            std::fabs(nudge_spring.velocity) > 0.5f ||
            std::fabs(volume_bar_spring.value - volume_bar_spring.target) > 0.5f ||
            std::fabs(volume_bar_spring.velocity) > 0.5f || g_tab_animating.load(std::memory_order_relaxed);

        if (continuous_animation && !springs_animating)
        {
            constexpr double continuous_frame_ms = 1000.0 / 60.0;
            target_frame_ms = std::max(target_frame_ms, continuous_frame_ms);
        }

        // 1 ms timer resolution follows actual animation (continuous content or springs), not single repaints.
        if (continuous_animation || springs_animating)
        {
            last_animating_at = now;
        }
        set_high_res_timer(last_animating_at >= 0.0 && now - last_animating_at < high_res_timer_hold_sec);

        if (!needs_render)
        {
            // Nothing changed: poll at ~60 Hz.
            WaitForSingleObject(g_stop_event, 16);
            next_frame_target = std::chrono::steady_clock::now();
        }
        else if (springs_animating && g_settings.target_fps <= 0 && SUCCEEDED(DwmFlush()))
        {
            // Auto FPS while resizing: wait for the next DWM composition so frames land on vsync.
            next_frame_target = std::chrono::steady_clock::now();
        }
        else
        {
            next_frame_target += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(target_frame_ms));

            auto now_time = std::chrono::steady_clock::now();
            if (now_time < next_frame_target)
            {
                double remaining_ms = std::chrono::duration<double, std::milli>(next_frame_target - now_time).count();
                if (remaining_ms >= 1.5)
                {
                    WaitForSingleObject(g_stop_event, static_cast<DWORD>(remaining_ms - 0.5));
                }
                // Spinning pays off only for fast springs on high refresh rates.
                if (springs_animating && target_frame_ms < 10.0)
                {
                    while (std::chrono::steady_clock::now() < next_frame_target &&
                           WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
                    {
                        std::this_thread::yield();
                    }
                }
            }
            else
            {
                next_frame_target = now_time;
            }
        }
    }

    set_high_res_timer(false);

    unbind_volume_endpoint();
    dwm_backdrop::destroy_all();
    renderer.shutdown();
    DestroyWindow(hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(window_class, wc.hInstance);

    if (SUCCEEDED(hr_co))
    {
        CoUninitialize();
    }

    return 0;
}

}
