#include "ui/window.h"
#include "ui/actions.h"
#include "ui/backdrop.h"
#include "notifications/capture.h"
#include "core/constants.h"
#include "ui/context_menu.h"
#include "ui/file_tray.h"
#include "ui/hit_test.h"
#include "ui/icons.h"
#include "system/input_hooks.h"
#include "core/layout.h"
#include "media/media_source.h"
#include "core/model.h"
#include "ui/placement.h"
#include "services/quick_lookup.h"
#include "ui/renderer.h"
#include "core/state.h"
#include "core/tabs.h"
#include "core/util.h"

namespace island
{
LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    static POINT s_touch_start = {0, 0};
    static ULONGLONG s_touch_start_time = 0;
    static bool s_tray_press_active = false;
    static POINT s_tray_press_pt = {0, 0};
    static std::wstring s_tray_press_path;
    switch (msg)
    {
        case WM_CREATE:
            AddClipboardFormatListener(hwnd);
            if (g_shell_hook_message == 0)
            {
                g_shell_hook_message = RegisterWindowMessageW(L"SHELLHOOK");
            }
            if (g_taskbar_created_message == 0)
            {
                g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
            }
            RegisterShellHookWindow(hwnd);
            // File Tray: an OLE drop target (not DragAcceptFiles) so the island reacts while a drag hovers.
            {
                g_ole_initialized = SUCCEEDED(OleInitialize(nullptr));
                ComPtr<IDropTarget> drop_target;
                drop_target.Attach(new TrayDropTarget());
                const HRESULT drop_hr = RegisterDragDrop(hwnd, drop_target.Get());
                if (FAILED(drop_hr))
                {
                    host::log(L"RegisterDragDrop failed (0x%08X).", static_cast<unsigned>(drop_hr));
                }
            }
            return 0;

        case WM_DROPFILES:
        {
            HDROP drop = reinterpret_cast<HDROP>(w_param);
            if (!g_settings.file_tray_module)
            {
                DragFinish(drop);
                return 0;
            }

            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<FileTrayItem> added;
            added.reserve(count);

            for (UINT i = 0; i < count; ++i)
            {
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path)) == 0)
                {
                    continue;
                }

                FileTrayItem item;
                item.path = path;

                item.name = base_name_from_path(path);
                if (item.name.empty())
                {
                    item.name = path;
                }

                WIN32_FILE_ATTRIBUTE_DATA attr = {};
                if (GetFileAttributesExW(path, GetFileExInfoStandard, &attr))
                {
                    item.is_directory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    item.size_bytes = (static_cast<std::uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
                }

                SHFILEINFOW info = {};
                if (SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON) && info.hIcon)
                {
                    item.icon = icon_to_pixels_ref(info.hIcon, 32);
                    DestroyIcon(info.hIcon);
                }

                added.push_back(std::move(item));
            }
            DragFinish(drop);

            if (!added.empty())
            {
                std::lock_guard lock(g_state_mutex);
                for (auto& item : added)
                {
                    auto existing = std::find_if(g_state.file_tray_items.begin(), g_state.file_tray_items.end(),
                                                 [&](const FileTrayItem& other)
                                                 {
                                                     return _wcsicmp(other.path.c_str(), item.path.c_str()) == 0;
                                                 });
                    if (existing != g_state.file_tray_items.end())
                    {
                        g_state.file_tray_items.erase(existing);
                    }
                    g_state.file_tray_items.push_back(std::move(item));
                }

                const std::size_t cap = static_cast<std::size_t>(std::max(1, g_settings.file_tray_max_items));
                while (g_state.file_tray_items.size() > cap)
                {
                    g_state.file_tray_items.erase(g_state.file_tray_items.begin());
                }
            }

            const int tray_tab = file_tray_tab_index(g_settings);
            if (tray_tab >= 0)
            {
                g_idle_tab = tray_tab;
            }
            g_layout_dirty = true;
            return 0;
        }

        case WM_DESTROY:
            RevokeDragDrop(hwnd);
            if (g_ole_initialized)
            {
                OleUninitialize();
                g_ole_initialized = false;
            }
            RemoveClipboardFormatListener(hwnd);
            DeregisterShellHookWindow(hwnd);
            return 0;

        // Hotkeys and the window shape must be changed from the thread that owns the window.
        case wm_app_apply_hotkey:
            apply_hide_show_hotkey();
            return 0;

        case wm_app_apply_backdrop:
            apply_backdrop_material(hwnd);
            return 0;

        case wm_app_mouse_wake:
            return 0;

        case wm_app_capslock:
        {
            if (!g_settings.caps_lock)
            {
                return 0;
            }
            bool is_num = (w_param == VK_NUMLOCK);
            bool caps_on = (l_param & 1) != 0;
            bool num_on = (l_param & 2) != 0;
            {
                std::lock_guard lock(g_state_mutex);
                g_state.caps_lock.active = true;
                g_state.caps_lock.caps_on = caps_on;
                g_state.caps_lock.num_on = num_on;
                g_state.caps_lock.is_num_event = is_num;
                g_state.caps_lock.expires_at = now_seconds() + 2.5;
            }
            trigger_nudge();
            return 0;
        }

        case WM_DEVICECHANGE:
        {
            // DBT_DEVICEARRIVAL = 0x8000, DBT_DEVICEREMOVECOMPLETE = 0x8004
            if (w_param == 0x8000 || w_param == 0x8004)
            {
                bool arrived = (w_param == 0x8000);
                std::wstring dev_name;
                bool is_bt = false;

                if (l_param)
                {
                    auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(l_param);
                    if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME)
                    {
                        dev_name = L"USB Drive";
                    }
                    else if (hdr->dbch_devicetype == DBT_DEVTYP_PORT)
                    {
                        dev_name = L"COM Device";
                    }
                    else
                    {
                        is_bt = true;
                        dev_name = L"Bluetooth Device";
                    }
                }

                {
                    std::lock_guard lock(g_state_mutex);
                    g_state.device.active = true;
                    g_state.device.event_type = arrived ? DeviceEventType::Connected : DeviceEventType::Disconnected;
                    g_state.device.device_name = dev_name;
                    g_state.device.is_bluetooth_like = is_bt;
                    g_state.device.expires_at = now_seconds() + 3.0;
                }
                trigger_nudge();
            }
            return 0;
        }

        case WM_POWERBROADCAST:
        {
            if (w_param == PBT_APMRESUMESUSPEND || w_param == PBT_APMRESUMEAUTOMATIC ||
                w_param == PBT_APMRESUMECRITICAL)
            {
                if (g_settings_changed_event)
                {
                    SetEvent(g_settings_changed_event);
                }
                {
                    std::lock_guard lock(g_state_mutex);
                    g_state.weather.last_updated = 0.0;
                }
                trigger_nudge();
            }
            return TRUE;
        }

        case WM_CLIPBOARDUPDATE:
            if (g_settings.clipboard)
            {
                capture_clipboard(hwnd);
            }
            return 0;

        case WM_NCHITTEST:
        {
            POINT pt = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ScreenToClient(hwnd, &pt);

            // The window is a large transparent stage: only the pill is clickable.
            if (g_pill_client_rect.valid)
            {
                const PillClientRect& r = g_pill_client_rect;
                if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom)
                {
                    return HTTRANSPARENT;
                }
                return HTCLIENT;
            }

            RECT client_rect = {};
            GetClientRect(hwnd, &client_rect);

            const int min_x = static_cast<int>(render_pad_x);
            const int max_x = client_rect.right - static_cast<int>(render_pad_x);
            const int min_y =
                (g_settings.notch_style || g_settings.border_merged_mode) ? 0 : static_cast<int>(render_pad_y);
            const int max_y = client_rect.bottom - static_cast<int>(render_pad_y);

            if (max_x > min_x && max_y > min_y)
            {
                if (pt.x < min_x || pt.x > max_x || pt.y < min_y || pt.y > max_y)
                {
                    return HTTRANSPARENT;
                }
            }
            return HTCLIENT;
        }

        case WM_KEYDOWN:
            if (handle_lookup_key_down(hwnd, w_param))
            {
                return 0;
            }
            break;

        case WM_CHAR:
            if (handle_lookup_char(static_cast<wchar_t>(w_param)))
            {
                return 0;
            }
            break;

        case WM_KILLFOCUS:
            if (g_lookup_ui.open)
            {
                close_lookup_panel(hwnd);
            }
            return 0;

        case wm_app_layout_changed:
            g_layout_dirty = true;
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(l_param) == HTCLIENT)
            {
                if (g_lookup_ui.open)
                {
                    POINT cp;
                    GetCursorPos(&cp);
                    ScreenToClient(hwnd, &cp);
                    SetCursor(LoadCursorW(nullptr, lookup_cursor_at(cp.x, cp.y)));
                    return TRUE;
                }
                if (g_scrubbing.load())
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }

                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);

                bool media_active = false;
                {
                    std::lock_guard lock(g_state_mutex);
                    media_active = g_settings.media && g_state.media.available;
                }

                const int current_tab = normalized_tab_index(g_settings);
                if (media_active && current_tab == 0)
                {
                    const MediaContentPoint cp = media_content_from_client(pt.x, pt.y);
                    const bool hover_clickable = media_art_hit_test(cp) || media_transport_hit_test(cp) != -1 ||
                                                 media_scrub_fraction_from_content(cp) >= 0.0f ||
                                                 g_hovered_source_slot.load(std::memory_order_relaxed) >= 0 ||
                                                 (g_settings.media_tab_lyrics && media_lyrics_button_hit_test(cp));
                    if (hover_clickable)
                    {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }

                if (g_settings.file_tray_module && current_tab == file_tray_tab_index(g_settings) &&
                    (g_hovered_file_tray_row.load(std::memory_order_relaxed) >= 0 ||
                     g_hovered_file_tray_action.load(std::memory_order_relaxed) != -1))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }

                if (current_tab == notification_center_tab_index(g_settings) &&
                    (g_hovered_notification_action.load(std::memory_order_relaxed) != -1 ||
                     g_hovered_notification_row.load(std::memory_order_relaxed) >= 0))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;

        case WM_LBUTTONDOWN:
        {
            int x_pos = GET_X_LPARAM(l_param);
            int y_pos = GET_Y_LPARAM(l_param);

            s_touch_start.x = x_pos;
            s_touch_start.y = y_pos;
            s_touch_start_time = GetTickCount64();

            if (g_lookup_ui.open)
            {
                s_tray_press_active = false;
                return 0;
            }

            s_tray_press_active = false;
            if (g_settings.file_tray_module && normalized_tab_index(g_settings) == file_tray_tab_index(g_settings))
            {
                const MediaContentPoint tcp = media_content_from_client(x_pos, y_pos);
                std::lock_guard lock(g_state_mutex);
                const int count = static_cast<int>(g_state.file_tray_items.size());
                const int row = file_tray_row_at_content_point(tcp, count);
                if (row >= 0 && !file_tray_remove_hit_test(tcp, row))
                {
                    s_tray_press_active = true;
                    s_tray_press_pt.x = x_pos;
                    s_tray_press_pt.y = y_pos;
                    s_tray_press_path = g_state.file_tray_items[static_cast<std::size_t>(count - 1 - row)].path;
                }
            }

            bool media_active = false;
            {
                std::lock_guard lock(g_state_mutex);
                media_active = g_settings.media && g_state.media.available;
            }

            const int current_tab = normalized_tab_index(g_settings);
            if (media_active && current_tab == 0)
            {
                const MediaContentPoint cp = media_content_from_client(x_pos, y_pos);

                {
                    const DockHit dock = resolve_source_dock_hit(cp);
                    if (dock.slot >= 0)
                    {
                        g_pressed_source_slot = dock.slot;
                        g_layout_dirty = true;
                        return 0;
                    }
                }

                const int cmd = media_transport_hit_test(cp);
                if (cmd != -1)
                {
                    g_pressed_media_button = cmd;
                    SetCapture(hwnd);
                    g_layout_dirty = true;
                    return 0;
                }

                const float fraction = media_scrub_fraction_from_content(cp);
                if (fraction >= 0.0f)
                {
                    g_scrub_drag_fraction.store(fraction, std::memory_order_relaxed);
                    g_scrubbing = true;
                    g_last_live_seek_time = 0.0;
                    SetCapture(hwnd);
                    g_layout_dirty = true;
                    return 0;
                }
            }
        }
        break;

        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            const int x_pos = GET_X_LPARAM(l_param);
            const int y_pos = GET_Y_LPARAM(l_param);

            if (g_lookup_ui.open)
            {
                handle_lookup_mouse_move(x_pos, y_pos);
                return 0;
            }

            if (g_scrubbing.load())
            {
                const MediaContentPoint cp = media_content_from_client(x_pos, y_pos);
                const float fraction = media_scrub_fraction_unbounded(cp);
                if (fraction < 0.0f)
                {
                    return 0;
                }
                g_scrub_drag_fraction.store(fraction, std::memory_order_relaxed);
                g_layout_dirty = true;

                const double now = now_seconds();
                if (now - g_last_live_seek_time.load(std::memory_order_relaxed) >= 0.15)
                {
                    g_last_live_seek_time.store(now, std::memory_order_relaxed);
                    std::int64_t end_ticks = 0;
                    {
                        std::lock_guard lock(g_state_mutex);
                        end_ticks = g_state.media.end_ticks;
                    }
                    if (end_ticks > 0)
                    {
                        seek_media_to_ticks(static_cast<std::int64_t>(fraction * end_ticks));
                    }
                }
                return 0;
            }

            int hovered = -1;
            bool media_active = false;
            {
                std::lock_guard lock(g_state_mutex);
                media_active = g_settings.media && g_state.media.available;
            }
            const int current_tab = normalized_tab_index(g_settings);

            int hovered_slot = -1;
            if (media_active && current_tab == 0)
            {
                const MediaContentPoint media_pt = media_content_from_client(x_pos, y_pos);
                hovered_slot = resolve_source_dock_hit(media_pt).slot;
                hovered = hovered_slot >= 0 ? -1 : media_transport_hit_test(media_pt);
            }

            if (g_hovered_media_button.exchange(hovered) != hovered)
            {
                g_layout_dirty = true;
            }
            if (g_hovered_source_slot.exchange(hovered_slot) != hovered_slot)
            {
                g_layout_dirty = true;
            }
            {
                bool on_lyrics_btn = false;
                if (g_settings.media_tab_lyrics && media_active && current_tab == 0)
                {
                    on_lyrics_btn = media_lyrics_button_hit_test(media_content_from_client(x_pos, y_pos));
                }
                if (g_hovered_lyrics_btn.exchange(on_lyrics_btn) != on_lyrics_btn)
                {
                    g_layout_dirty = true;
                }
            }

            int hovered_row = -1;
            int hovered_action = -1;
            if (g_settings.file_tray_module && current_tab == file_tray_tab_index(g_settings))
            {
                std::size_t tray_count = 0;
                {
                    std::lock_guard lock(g_state_mutex);
                    tray_count = g_state.file_tray_items.size();
                }
                const MediaContentPoint tray_pt = media_content_from_client(x_pos, y_pos);
                hovered_row = file_tray_row_at_content_point(tray_pt, static_cast<int>(tray_count));
                if (tray_count > 0 && file_tray_clear_hit_test(tray_pt))
                {
                    hovered_action = -2;
                }
                else if (file_tray_paste_hit_test(tray_pt))
                {
                    hovered_action = -3;
                }
                else if (hovered_row >= 0 && file_tray_remove_hit_test(tray_pt, hovered_row))
                {
                    hovered_action = hovered_row;
                }
            }
            bool tray_hover_changed = false;
            if (g_hovered_file_tray_row.exchange(hovered_row) != hovered_row)
            {
                tray_hover_changed = true;
            }
            if (g_hovered_file_tray_action.exchange(hovered_action) != hovered_action)
            {
                tray_hover_changed = true;
            }
            if (tray_hover_changed)
            {
                g_layout_dirty = true;
            }

            NotificationCenterHit center_hit;
            if (current_tab == notification_center_tab_index(g_settings))
            {
                center_hit = notification_center_hit_test(media_content_from_client(x_pos, y_pos));
            }
            const bool center_row_changed = g_hovered_notification_row.exchange(center_hit.row) != center_hit.row;
            const bool center_action_changed =
                g_hovered_notification_action.exchange(center_hit.action) != center_hit.action;
            if (center_row_changed || center_action_changed)
            {
                g_layout_dirty = true;
            }

            if (s_tray_press_active)
            {
                if (!(w_param & MK_LBUTTON))
                {
                    s_tray_press_active = false;
                }
                else if (std::abs(x_pos - s_tray_press_pt.x) >= GetSystemMetrics(SM_CXDRAG) ||
                         std::abs(y_pos - s_tray_press_pt.y) >= GetSystemMetrics(SM_CYDRAG))
                {
                    s_tray_press_active = false;
                    const std::wstring drag_path = s_tray_press_path;
                    start_file_tray_drag(drag_path);
                    return 0;
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_hovered_source_slot.exchange(-1) != -1 || g_pressed_source_slot.exchange(-1) != -1)
            {
                g_layout_dirty = true;
            }
            if (g_hovered_media_button.exchange(-1) != -1)
            {
                g_layout_dirty = true;
            }
            if (g_hovered_lyrics_btn.exchange(false))
            {
                g_layout_dirty = true;
            }
            s_tray_press_active = false;
            if (g_hovered_file_tray_row.exchange(-1) != -1)
            {
                g_layout_dirty = true;
            }
            if (g_hovered_file_tray_action.exchange(-1) != -1)
            {
                g_layout_dirty = true;
            }
            if (g_hovered_notification_row.exchange(-1) != -1)
            {
                g_layout_dirty = true;
            }
            if (g_hovered_notification_action.exchange(-1) != -1)
            {
                g_layout_dirty = true;
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(l_param) != hwnd)
            {
                if (g_scrubbing.exchange(false))
                {
                    g_layout_dirty = true;
                }
                if (g_pressed_media_button.exchange(-1) != -1)
                {
                    g_layout_dirty = true;
                }
                if (g_hovered_media_button.exchange(-1) != -1)
                {
                    g_layout_dirty = true;
                }
            }
            return 0;

        case WM_LBUTTONUP:
        {
            if (g_scrubbing.load())
            {
                g_scrubbing = false;
                ReleaseCapture();

                const float final_fraction = clamp(g_scrub_drag_fraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
                std::int64_t end_ticks = 0;
                {
                    std::lock_guard lock(g_state_mutex);
                    end_ticks = g_state.media.end_ticks;
                }
                if (end_ticks > 0)
                {
                    const std::int64_t target_ticks = static_cast<std::int64_t>(final_fraction * end_ticks);
                    seek_media_to_ticks(target_ticks);
                    std::lock_guard lock(g_state_mutex);
                    g_state.media.position_ticks = target_ticks;
                    g_state.media.last_updated_ticks = GetTickCount64();
                }
                g_layout_dirty = true;
                return 0;
            }

            if (g_pressed_media_button.load() != -1)
            {
                g_pressed_media_button = -1;
                ReleaseCapture();
                g_layout_dirty = true;
            }
            if (g_pressed_source_slot.exchange(-1) != -1)
            {
                g_layout_dirty = true;
            }

            int x_pos = GET_X_LPARAM(l_param);
            int y_pos = GET_Y_LPARAM(l_param);

            ULONGLONG now = GetTickCount64();
            if (s_touch_start_time > 0 && (now - s_touch_start_time) < 500)
            {
                const int dx = x_pos - s_touch_start.x;
                const int dy = y_pos - s_touch_start.y;
                const int travel = std::abs(dy) > std::abs(dx) ? dy : dx;
                if (std::abs(travel) > 40)
                {
                    if (active_tab_count(get_settings_copy()) > 1)
                    {
                        step_idle_tab(travel > 0 ? -1 : 1);
                    }
                    s_touch_start_time = 0;
                    return 0;
                }
            }
            s_touch_start_time = 0;

            bool media_active = false;
            if (handle_lookup_click(x_pos, y_pos))
            {
                return 0;
            }
            std::vector<IslandKind> kinds;
            {
                std::lock_guard lock(g_state_mutex);
                media_active = g_settings.media && g_state.media.available;
                kinds = choose_activities(g_state, g_settings, now_seconds());
            }

            const bool on_secondary_split =
                (kinds.size() > 1 && g_pill_client_rect.valid &&
                 x_pos > (g_pill_client_rect.left + (g_pill_client_rect.right - g_pill_client_rect.left) * 0.5f));
            if (!kinds.empty() && !on_secondary_split &&
                (kinds[0] == IslandKind::Notification || kinds[0] == IslandKind::Clipboard))
            {
                {
                    std::lock_guard lock(g_state_mutex);
                    if (kinds[0] == IslandKind::Notification)
                    {
                        g_state.notification.active = false;
                        g_state.notification.expires_at = 0.0;
                    }
                    else if (kinds[0] == IslandKind::Clipboard)
                    {
                        g_state.clipboard.active = false;
                        g_state.clipboard.expires_at = 0.0;
                    }
                }
                g_click_expanded = false;
                g_layout_dirty = true;
                trigger_nudge();
                return 0;
            }
            const bool game_metrics_present = !kinds.empty() && kinds[0] == IslandKind::Idle &&
                                              (g_settings.game_overlay || g_game_overlay_pinned.load());

            bool expanded = g_pinned_expanded.load() || g_click_expanded.load();
            if (!game_metrics_present && !g_settings.expand_on_hover && !expanded)
            {
                g_click_expanded = true;
                g_layout_dirty = true;
                return 0;
            }

            RECT client_rect;
            GetClientRect(hwnd, &client_rect);
            const float height = static_cast<float>(client_rect.bottom - client_rect.top);

            const int current_tab = normalized_tab_index(g_settings);

            const MediaContentPoint cp = media_content_from_client(x_pos, y_pos);

            if (current_tab == notification_center_tab_index(g_settings))
            {
                const NotificationCenterHit hit = notification_center_hit_test(cp);
                if (hit.action != -1 || hit.row >= 0)
                {
                    handle_notification_center_click(hit);
                    g_hovered_notification_row = -1;
                    g_hovered_notification_action = -1;
                    g_layout_dirty = true;
                    return 0;
                }
            }

            if (g_settings.file_tray_module && current_tab == file_tray_tab_index(g_settings))
            {
                const bool was_pressed = s_tray_press_active;
                s_tray_press_active = false;

                if (file_tray_paste_hit_test(cp))
                {
                    paste_into_file_tray(hwnd);
                    g_layout_dirty = true;
                    return 0;
                }

                std::wstring to_open;
                std::wstring to_copy;
                bool changed = false;
                {
                    std::lock_guard lock(g_state_mutex);
                    const int count = static_cast<int>(g_state.file_tray_items.size());
                    if (count > 0 && file_tray_clear_hit_test(cp))
                    {
                        g_state.file_tray_items.clear();
                        changed = true;
                    }
                    else
                    {
                        const int row = file_tray_row_at_content_point(cp, count);
                        if (row >= 0)
                        {
                            const std::size_t index = static_cast<std::size_t>(count - 1 - row);
                            if (file_tray_remove_hit_test(cp, row))
                            {
                                g_state.file_tray_items.erase(g_state.file_tray_items.begin() +
                                                              static_cast<std::ptrdiff_t>(index));
                                changed = true;
                            }
                            else if (was_pressed)
                            {
                                const FileTrayItem& clicked = g_state.file_tray_items[index];
                                if (clicked.is_text)
                                {
                                    to_copy = clicked.text;
                                }
                                else
                                {
                                    to_open = clicked.path;
                                }
                            }
                        }
                    }
                }
                if (changed)
                {
                    save_file_tray();
                    g_hovered_file_tray_row = -1;
                    g_hovered_file_tray_action = -1;
                    g_layout_dirty = true;
                    return 0;
                }
                if (!to_copy.empty())
                {
                    copy_text_to_clipboard(hwnd, to_copy);
                    return 0;
                }
                if (!to_open.empty())
                {
                    SHELLEXECUTEINFOW sei = {sizeof(sei)};
                    sei.fMask = SEE_MASK_FLAG_NO_UI;
                    sei.lpFile = to_open.c_str();
                    sei.nShow = SW_SHOWNORMAL;
                    ShellExecuteExW(&sei);
                    return 0;
                }
                return 0;
            }

            if (media_active && current_tab == 0)
            {
                {
                    const DockHit dock = resolve_source_dock_hit(cp);
                    if (dock.slot >= 0)
                    {
                        if (dock.overflow)
                        {
                            POINT sp = {x_pos, y_pos};
                            ClientToScreen(hwnd, &sp);
                            show_media_source_popup(hwnd, sp);
                        }
                        else if (!dock.is_active)
                        {
                            request_media_source_switch(dock.aumid);
                        }
                        g_layout_dirty = true;
                        return 0;
                    }
                }

                if (g_settings.media_tab_lyrics && media_lyrics_button_hit_test(cp))
                {
                    g_immersive_lyrics.store(!g_immersive_lyrics.load());
                    g_layout_dirty = true;
                    return 0;
                }

                const int cmd = media_transport_hit_test(cp);
                if (cmd != -1)
                {
                    if (cmd == 0)
                    {
                        g_skip_trigger_prev.fetch_add(1);
                    }
                    else if (cmd == 2)
                    {
                        g_skip_trigger_next.fetch_add(1);
                    }
                    g_layout_dirty = true;
                    send_media_transport_command(cmd);
                    return 0;
                }

                // A release over the scrubber that did not start a drag is swallowed (no "open the app").
                if (media_scrub_fraction_from_content(cp) >= 0.0f)
                {
                    return 0;
                }
            }

            if (height > 50.0f)
            {
                if (media_active)
                {
                    if (current_tab == 0)
                    {
                        open_relevant_app();
                    }
                }
                else if (kinds.empty() || kinds[0] != IslandKind::Idle)
                {
                    handle_status_click_at_point(hwnd, l_param);
                }
            }
            else
            {
                if (media_active)
                {
                    open_relevant_app();
                }
                else
                {
                    handle_status_click_at_point(hwnd, l_param);
                }
            }
        }
            return 0;

        case WM_MBUTTONUP:
            toggle_endpoint_mute();
            return 0;

        case WM_LBUTTONDBLCLK:
        {
            const bool next_val = !g_pinned_expanded.load();
            g_pinned_expanded.store(next_val);
            host::set_int_value(L"PinnedExpanded", next_val ? 1 : 0);
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            // Wheel delta is accumulated, so every notch counts and touchpads work.
            static int s_wheel_accum = 0;
            static ULONGLONG s_last_wheel_event = 0;
            static ULONGLONG s_last_wheel_step = 0;

            const ULONGLONG wheel_now = GetTickCount64();
            const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
            if (delta == 0)
            {
                return 0;
            }

            if (wheel_now - s_last_wheel_event > 220 || (s_wheel_accum != 0 && ((delta > 0) != (s_wheel_accum > 0))))
            {
                s_wheel_accum = 0;
            }
            s_last_wheel_event = wheel_now;
            s_wheel_accum += delta;

            if (std::abs(s_wheel_accum) < WHEEL_DELTA)
            {
                return 0;
            }

            // At most one notch is held pending, so inertia cannot race through every tab.
            const bool precise = std::abs(delta) < WHEEL_DELTA;
            const ULONGLONG cooldown = precise ? 200 : 85;
            if (wheel_now - s_last_wheel_step < cooldown)
            {
                s_wheel_accum = s_wheel_accum > 0 ? WHEEL_DELTA : -WHEEL_DELTA;
                return 0;
            }
            s_last_wheel_step = wheel_now;
            const int wheel_step = s_wheel_accum > 0 ? -1 : 1;
            const bool detail_scrolled =
                normalized_tab_index(g_settings) == notification_center_tab_index(g_settings) &&
                scroll_notification_center_detail(wheel_step);
            if (!detail_scrolled)
            {
                step_idle_tab(wheel_step); // wheel up = previous tab
            }
            s_wheel_accum = 0;
            return 0;
        }

        case WM_RBUTTONUP:
        {
            POINT pt = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ClientToScreen(hwnd, &pt);
            show_context_menu(hwnd, pt);
            return 0;
        }

        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
        {
            g_layout_dirty = true;
            PostMessageW(hwnd, wm_app_new_event, 0, 0);
            return 0;
        }

        case WM_HOTKEY:
        {
            if (w_param == id_lookup_hotkey)
            {
                start_quick_lookup(hwnd);
                return 0;
            }
            if (w_param == id_hide_show_hotkey)
            {
                if (g_is_fullscreen.load(std::memory_order_relaxed))
                {
                    g_fullscreen_override_visible = !g_fullscreen_override_visible.load();
                    g_auto_hidden_parked = false;
                    g_layout_dirty = true;
                    if (g_fullscreen_override_visible.load())
                    {
                        g_manually_hidden = false;
                        host::set_int_value(L"ManuallyHidden", 0);
                        g_hotkey_unhide_until.store(now_seconds() + (g_settings.auto_hide_idle_seconds > 0
                                                                         ? g_settings.auto_hide_idle_seconds
                                                                         : 6.0));
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                        PostMessageW(hwnd, wm_app_new_event, 0, 0);
                    }
                }
                else
                {
                    bool is_currently_hidden = g_manually_hidden.load() || g_auto_hidden_parked.load();
                    if (hwnd && !IsWindowVisible(hwnd))
                    {
                        is_currently_hidden = true;
                    }
                    if (is_currently_hidden)
                    {
                        g_manually_hidden = false;
                        host::set_int_value(L"ManuallyHidden", 0);
                        g_auto_hidden_parked = false;
                        g_fullscreen_override_visible = true;
                        g_hotkey_unhide_until.store(now_seconds() + (g_settings.auto_hide_idle_seconds > 0
                                                                         ? g_settings.auto_hide_idle_seconds
                                                                         : 6.0));
                        g_layout_dirty = true;
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                        PostMessageW(hwnd, wm_app_new_event, 0, 0);
                    }
                    else
                    {
                        g_manually_hidden = true;
                        host::set_int_value(L"ManuallyHidden", 1);
                        g_hotkey_unhide_until.store(0.0);
                        g_fullscreen_override_visible = false;
                        g_layout_dirty = true;
                        PostMessageW(hwnd, wm_app_new_event, 0, 0);
                    }
                }
            }
            return 0;
        }
    }

    if (msg == g_shell_hook_message && g_shell_hook_message != 0)
    {
        if (w_param == HSHELL_WINDOWCREATED)
        {
            capture_shell_notification(reinterpret_cast<HWND>(l_param));
        }
        return 0;
    }

    if (msg == g_taskbar_created_message && g_taskbar_created_message != 0)
    {
        host::log(L"TaskbarCreated received; re-registering shell hook window.");
        DeregisterShellHookWindow(hwnd);
        RegisterShellHookWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

}
