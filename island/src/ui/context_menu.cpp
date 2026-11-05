#include "ui/context_menu.h"
#include "ui/actions.h"
#include "ui/backdrop.h"
#include "ui/file_tray.h"
#include "core/localization.h"
#include "media/lyrics_cache.h"
#include "media/media_source.h"
#include "core/model.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/util.h"
#include "services/location.h"
#include "services/weather.h"

namespace island
{
namespace
{

// ── Media source menu (right-click submenu and the dock's "+N" list) ─────────
static constexpr UINT media_source_menu_id_base = 100; // 100 = Auto, 101.. = sources

// Shape style submenu: +0 = iPhone pill, +1 = Windows 11 flyout, +2 = macOS notch.
static constexpr UINT shape_menu_id_base = 50;

// Lyrics sync submenu: +0 header (disabled), +1..+3 earlier 10/50/250, +4..+6 later 10/50/250, +7 reset.
static constexpr UINT lyrics_sync_menu_id_base = 60;

// Right-click nudge for the profile that is active right now (Bluetooth or speakers).
// Stored separately from the Mod Settings value; effective offset = setting + nudge.
// Deliberately does not call LoadSettings(), which also resets the pinned state.
void adjust_lyrics_nudge(int delta_ms)
{
    const bool bt = g_default_output_is_bluetooth.load(std::memory_order_relaxed);
    const wchar_t* key = bt ? L"LyricsNudgeBtMs" : L"LyricsNudgeMs";
    const Settings s = get_settings_copy();
    const int setting = bt ? s.lyrics_offset_bt_ms : s.lyrics_offset_ms;
    // Clamp the nudge so setting + nudge stays in range; repeated presses never pile up invisibly.
    const int nudge = clamp_int(host::int_value(key, 0) + delta_ms, -lyrics_offset_limit_ms - setting,
                                lyrics_offset_limit_ms - setting);
    host::set_int_value(key, nudge);
    publish_lyrics_offsets(s.lyrics_offset_ms, s.lyrics_offset_bt_ms);
    g_layout_dirty = true;
}

void reset_lyrics_nudge()
{
    const bool bt = g_default_output_is_bluetooth.load(std::memory_order_relaxed);
    host::set_int_value(bt ? L"LyricsNudgeBtMs" : L"LyricsNudgeMs", 0);
    const Settings s = get_settings_copy();
    publish_lyrics_offsets(s.lyrics_offset_ms, s.lyrics_offset_bt_ms);
    g_layout_dirty = true;
}

struct MediaSourceMenuItem
{
    std::wstring aumid;
    std::wstring name;
};

// Ticks: "Auto" = automatic mode, an app = the one currently being controlled.
std::vector<MediaSourceMenuItem> append_media_source_items(HMENU menu)
{
    std::vector<MediaSourceMenuItem> items;
    std::wstring active;
    {
        std::lock_guard lock(g_state_mutex);
        active = g_state.media.source_app_user_model_id;
        for (const MediaSourceInfo& source : g_state.media_sources)
        {
            items.push_back(MediaSourceMenuItem{source.aumid, source.name});
        }
    }

    const bool auto_mode = get_preferred_media_source().empty();
    AppendMenuW(menu, MF_STRING | (auto_mode ? MF_CHECKED : 0), media_source_menu_id_base, loc(L"Auto"));
    if (!items.empty())
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    for (size_t i = 0; i < items.size(); ++i)
    {
        AppendMenuW(menu, MF_STRING | (items[i].aumid == active ? MF_CHECKED : 0),
                    media_source_menu_id_base + 1 + static_cast<UINT>(i), items[i].name.c_str());
    }
    return items;
}

void apply_media_source_menu_command(UINT cmd, const std::vector<MediaSourceMenuItem>& items)
{
    if (cmd < media_source_menu_id_base)
    {
        return;
    }
    if (cmd == media_source_menu_id_base)
    {
        lock_media_source(L"");
        return;
    }
    const size_t index = cmd - media_source_menu_id_base - 1;
    if (index < items.size())
    {
        lock_media_source(items[index].aumid);
    }
}

}

void show_media_source_popup(HWND hwnd, POINT screen_point)
{
    HMENU menu = CreatePopupMenu();
    const std::vector<MediaSourceMenuItem> items = append_media_source_items(menu);
    SetForegroundWindow(hwnd);
    dwm_backdrop::ensure_behind(hwnd);
    const UINT cmd =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_point.x, screen_point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    dwm_backdrop::ensure_behind(hwnd);
    g_layout_dirty = true;
    apply_media_source_menu_command(cmd, items);
}

namespace
{

void open_island_bridge_window()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = length > 0 && length < ARRAYSIZE(path) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash)
    {
        return;
    }
    slash[1] = L'\0';
    if (wcscat_s(path, ARRAYSIZE(path), L"IslandBridge.exe") != 0)
    {
        return;
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path, L"--show", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        host::log(L"Could not open IslandBridge.");
    }
}

}

void show_context_menu(HWND hwnd, POINT screen_point)
{
    bool timer_active = false;
    bool timer_running = false;
    {
        std::lock_guard lock(g_state_mutex);
        timer_active = g_state.timer.active;
        timer_running = g_state.timer.running;
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 40, g_manually_hidden.load() ? L"Show Island" : L"Hide Island");
    AppendMenuW(menu, MF_STRING, 1, L"Dismiss");

    HMENU timer_menu = CreatePopupMenu();
    AppendMenuW(timer_menu, MF_STRING, 30, L"Start 25 min Focus");
    AppendMenuW(timer_menu, MF_STRING, 31, L"Start 50 min Focus");
    AppendMenuW(timer_menu, MF_STRING, 32, L"Start 5 min Break");
    if (timer_active)
    {
        AppendMenuW(timer_menu, MF_STRING, 33, timer_running ? L"Pause Timer" : L"Resume Timer");
        AppendMenuW(timer_menu, MF_STRING, 34, L"Stop Timer");
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(timer_menu), L"Focus Timer");

    std::vector<MediaSourceMenuItem> media_source_items;
    {
        HMENU source_menu = CreatePopupMenu();
        media_source_items = append_media_source_items(source_menu);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(source_menu), loc(L"Media source"));
    }
    if (g_settings.file_tray_module)
    {
        size_t tray_count = 0;
        {
            std::lock_guard lock(g_state_mutex);
            tray_count = g_state.file_tray_items.size();
        }
        HMENU tray_menu = CreatePopupMenu();
        AppendMenuW(tray_menu, MF_STRING, 42, L"Paste from clipboard");
        const std::wstring clear_label =
            tray_count == 0 ? std::wstring(L"Clear tray") : L"Clear tray (" + std::to_wstring(tray_count) + L")";
        AppendMenuW(tray_menu, MF_STRING | (tray_count == 0 ? (MF_GRAYED | MF_DISABLED) : 0), 41, clear_label.c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tray_menu), loc(L"File Tray"));
    }
    AppendMenuW(menu, MF_STRING, 2, L"Pin expanded");
    AppendMenuW(menu, MF_STRING, 3, g_game_overlay_pinned.load() ? L"Hide game overlay" : L"Show game overlay");
    {
        // g_settings already reflects any right-click override, so it is the
        // source of truth for which style is active.
        const int current_shape = g_settings.notch_style ? 2 : (g_settings.w11_style ? 1 : 0);
        HMENU shape_menu = CreatePopupMenu();
        AppendMenuW(shape_menu, MF_STRING, shape_menu_id_base, L"iPhone Pill");
        AppendMenuW(shape_menu, MF_STRING, shape_menu_id_base + 1, L"Windows 11 Flyout");
        if (is_bottom_position(g_settings.position))
        {
            AppendMenuW(shape_menu, MF_STRING | MF_GRAYED | MF_DISABLED, shape_menu_id_base + 2,
                        L"macOS Notch (unavailable at bottom)");
        }
        else
        {
            AppendMenuW(shape_menu, MF_STRING, shape_menu_id_base + 2, L"macOS Notch");
        }
        CheckMenuRadioItem(shape_menu, shape_menu_id_base, shape_menu_id_base + 2,
                           shape_menu_id_base + static_cast<UINT>(current_shape), MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(shape_menu), L"Shape style");
    }
    const int active_expand_on_hover = host::int_value(L"ExpandOnHoverOverride", -1) >= 0
                                           ? host::int_value(L"ExpandOnHoverOverride", 0)
                                           : (host::int_setting(L"Behavior.ExpandOnHover") != 0);
    AppendMenuW(menu, MF_STRING, 11, active_expand_on_hover ? L"Expand on Click" : L"Expand on Hover");
    AppendMenuW(menu, MF_STRING | (host::int_value(L"CollapsedLyrics", 0) ? MF_CHECKED : 0), 13,
                L"Show lyrics on collapsed island");
    AppendMenuW(menu, MF_STRING | (g_settings.media_tab_lyrics ? MF_CHECKED : 0), 15, L"Show lyrics in media tab");
    {
        bool cache_on = false;
        {
            std::lock_guard lock(g_settings_mutex);
            cache_on = g_settings.lyrics_cache && g_settings.lyrics_cache_max_mb > 0;
        }
        // Counters are maintained by the lyrics thread; nothing is scanned here.
        std::wstring cache_label = L"Clear lyrics cache";
        if (g_lyrics_cache_counts_valid.load(std::memory_order_relaxed))
        {
            const int songs = g_lyrics_cache_file_count.load(std::memory_order_relaxed);
            const uint64_t bytes = g_lyrics_cache_bytes.load(std::memory_order_relaxed);
            wchar_t detail[64] = {};
            if (bytes >= 1048576ull)
            {
                swprintf_s(detail, L" (%d songs, %.1f MB)", songs, static_cast<double>(bytes) / 1048576.0);
            }
            else
            {
                swprintf_s(detail, L" (%d songs, %llu KB)", songs,
                           static_cast<unsigned long long>((bytes + 1023ull) / 1024ull));
            }
            cache_label += detail;
        }
        AppendMenuW(menu, MF_STRING | (cache_on ? 0 : (MF_GRAYED | MF_DISABLED)), 43, cache_label.c_str());

        {
            // Shows and adjusts whichever profile is active right now.
            const bool bt = g_default_output_is_bluetooth.load(std::memory_order_relaxed);
            const int effective = bt ? g_lyrics_effective_offset_bt_ms.load(std::memory_order_relaxed)
                                     : g_lyrics_effective_offset_ms.load(std::memory_order_relaxed);
            wchar_t header[96] = {};
            swprintf_s(header, L"%s: %+d ms", bt ? L"Bluetooth" : L"Speakers", effective);

            HMENU lyrics_sync_menu = CreatePopupMenu();
            AppendMenuW(lyrics_sync_menu, MF_STRING | MF_GRAYED | MF_DISABLED, lyrics_sync_menu_id_base, header);
            AppendMenuW(lyrics_sync_menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(lyrics_sync_menu, MF_STRING, lyrics_sync_menu_id_base + 1, L"Earlier +10 ms");
            AppendMenuW(lyrics_sync_menu, MF_STRING, lyrics_sync_menu_id_base + 2, L"Earlier +50 ms");
            AppendMenuW(lyrics_sync_menu, MF_STRING, lyrics_sync_menu_id_base + 3, L"Earlier +250 ms");
            AppendMenuW(lyrics_sync_menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(lyrics_sync_menu, MF_STRING, lyrics_sync_menu_id_base + 4, L"Later -10 ms");
            AppendMenuW(lyrics_sync_menu, MF_STRING, lyrics_sync_menu_id_base + 5, L"Later -50 ms");
            AppendMenuW(lyrics_sync_menu, MF_STRING, lyrics_sync_menu_id_base + 6, L"Later -250 ms");
            AppendMenuW(lyrics_sync_menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(lyrics_sync_menu, MF_STRING, lyrics_sync_menu_id_base + 7, L"Reset this profile");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(lyrics_sync_menu), L"Lyrics sync");
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const int active_opacity = g_pill_opacity_override.load();
    HMENU opacity_menu = CreatePopupMenu();
    AppendMenuW(opacity_menu, MF_STRING | (active_opacity == 100 ? MF_CHECKED : 0), 4, L"100% (Opaque)");
    AppendMenuW(opacity_menu, MF_STRING | (active_opacity == 85 ? MF_CHECKED : 0), 5, L"85%");
    AppendMenuW(opacity_menu, MF_STRING | (active_opacity == 70 ? MF_CHECKED : 0), 6, L"70%");
    AppendMenuW(opacity_menu, MF_STRING | (active_opacity == 55 ? MF_CHECKED : 0), 7, L"55%");
    AppendMenuW(opacity_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(opacity_menu, MF_STRING | (active_opacity < 0 ? MF_CHECKED : 0), 8, L"Theme default");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity_menu), L"Transparency");
    {
        const std::vector<WeatherCityEntry> weather_cities = load_weather_cities();
        if (!weather_cities.empty())
        {
            const bool auto_city = is_weather_city_auto();
            const int active_city = get_active_weather_city_index(weather_cities.size());
            HMENU weather_menu = CreatePopupMenu();
            std::wstring auto_label = L"Auto (by location)";
            if (auto_city)
            {
                std::wstring shown_city;
                {
                    std::lock_guard lock(g_state_mutex);
                    shown_city = g_state.weather.city;
                }
                if (!shown_city.empty())
                {
                    auto_label += L" \x2013 " + shown_city;
                }
            }
            AppendMenuW(weather_menu, MF_STRING | (auto_city ? MF_CHECKED : 0), weather_city_auto_menu_id,
                        auto_label.c_str());
            const location::Status location_status = location::status();
            if (auto_city && location::can_fix(location_status))
            {
                AppendMenuW(weather_menu, MF_STRING, weather_location_fix_menu_id,
                            location_status == location::Status::ServiceDisabled ? loc(L"Turn on precise location...")
                                                                                 : loc(L"Allow precise location..."));
            }
            AppendMenuW(weather_menu, MF_SEPARATOR, 0, nullptr);
            for (size_t i = 0; i < weather_cities.size(); ++i)
            {
                AppendMenuW(weather_menu,
                            MF_STRING | (!auto_city && static_cast<int>(i) == active_city ? MF_CHECKED : 0),
                            weather_city_menu_id_base + static_cast<UINT>(i), weather_cities[i].name.c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(weather_menu), L"Weather city");
        }
    }
    // Color theme presets. Built from kThemePalettes and nested in a submenu:
    // nine palettes listed flat would dominate the root menu.
    const int active_theme = host::int_value(theme_value_name, static_cast<int>(g_settings.theme_preset));
    HMENU theme_menu = CreatePopupMenu();
    for (int i = 0; i < custom_theme_index; ++i)
    {
        AppendMenuW(theme_menu, MF_STRING | (active_theme == i ? MF_CHECKED : 0),
                    theme_menu_id_base + static_cast<UINT>(i), theme_palettes[i].label);
    }
    AppendMenuW(theme_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(theme_menu, MF_STRING | (active_theme >= custom_theme_index ? MF_CHECKED : 0),
                theme_menu_id_base + static_cast<UINT>(custom_theme_index), L"Custom Colors");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme_menu), L"Theme");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 9, L"Open IslandBridge");

    SetForegroundWindow(hwnd);
    dwm_backdrop::ensure_behind(hwnd);
    const UINT cmd =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_point.x, screen_point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    dwm_backdrop::ensure_behind(hwnd);
    g_layout_dirty = true;

    // Theme ids are a contiguous range rather than individual cases, so adding a
    // palette to kThemePalettes needs no change here.
    if (cmd >= theme_menu_id_base && cmd <= theme_menu_id_base + static_cast<UINT>(custom_theme_index))
    {
        host::set_int_value(theme_value_name, static_cast<int>(cmd - theme_menu_id_base));
        load_settings();
        return;
    }

    if (cmd == weather_location_fix_menu_id)
    {
        location::fix_access(g_settings_changed_event);
        return;
    }

    if (cmd == weather_city_auto_menu_id ||
        (cmd >= weather_city_menu_id_base && cmd < weather_city_menu_id_base + weather_city_menu_max))
    {
        host::set_int_value(L"WeatherCityIndex", cmd == weather_city_auto_menu_id
                                                     ? weather_city_auto
                                                     : static_cast<int>(cmd - weather_city_menu_id_base));
        if (g_settings_changed_event)
        {
            SetEvent(g_settings_changed_event);
        }
        return;
    }

    if (cmd >= media_source_menu_id_base)
    {
        apply_media_source_menu_command(cmd, media_source_items);
        return;
    }

    switch (cmd)
    {
        case 1:
            dismiss_transient_state();
            g_click_expanded = false;
            g_layout_dirty = true;
            trigger_nudge();
            break;
        case 2:
        {
            const bool next_val = !g_pinned_expanded.load();
            g_pinned_expanded.store(next_val);
            host::set_int_value(L"PinnedExpanded", next_val ? 1 : 0);
            break;
        }
        case 3:
        {
            const bool next_val = !g_game_overlay_pinned.load();
            g_game_overlay_pinned.store(next_val);
            host::set_int_value(L"GameOverlayPinned", next_val ? 1 : 0);
            break;
        }
        case 4:
            g_pill_opacity_override.store(100);
            host::set_int_value(L"PillOpacityOverride", 100);
            load_settings();
            break;
        case 5:
            g_pill_opacity_override.store(85);
            host::set_int_value(L"PillOpacityOverride", 85);
            load_settings();
            break;
        case 6:
            g_pill_opacity_override.store(70);
            host::set_int_value(L"PillOpacityOverride", 70);
            load_settings();
            break;
        case 7:
            g_pill_opacity_override.store(55);
            host::set_int_value(L"PillOpacityOverride", 55);
            load_settings();
            break;
        case 8:
            g_pill_opacity_override.store(-1);
            host::set_int_value(L"PillOpacityOverride", -1);
            load_settings();
            break;
        case 9:
        {
            open_island_bridge_window();
            break;
        }
        case shape_menu_id_base:
        case shape_menu_id_base + 1:
        case shape_menu_id_base + 2:
        {
            const bool want_w11 = (cmd == shape_menu_id_base + 1);
            const bool want_notch = (cmd == shape_menu_id_base + 2);
            if (want_notch && is_bottom_position(g_settings.position))
            {
                break; // a bottom-anchored island can't be a top-edge notch
            }
            // Both overrides are written every time, so exactly one style is active.
            host::set_int_value(L"W11StyleOverride", want_w11 ? 1 : 0);
            host::set_int_value(L"NotchStyleOverride", want_notch ? 1 : 0);
            load_settings();
            g_layout_dirty = true;
            trigger_nudge();
            break;
        }
        case 11:
        {
            const int active_expand_on_hover = host::int_value(L"ExpandOnHoverOverride", -1) >= 0
                                                   ? host::int_value(L"ExpandOnHoverOverride", 0)
                                                   : (host::int_setting(L"Behavior.ExpandOnHover") != 0);
            host::set_int_value(L"ExpandOnHoverOverride", active_expand_on_hover ? 0 : 1);
            load_settings();
            g_layout_dirty = true;
            break;
        }
        case 13:
        {
            host::set_int_value(L"CollapsedLyrics", host::int_value(L"CollapsedLyrics", 0) ? 0 : 1);
            load_settings();
            g_layout_dirty = true;
            trigger_nudge();
            break;
        }
        case 15:
        {
            const int media_tab_lyrics_setting = host::int_setting(L"Modules.MediaTabLyrics") != 0 ? 1 : 0;
            host::set_int_value(L"MediaTabLyricsOverride", get_settings_copy().media_tab_lyrics ? 0 : 1);
            host::set_int_value(L"MediaTabLyricsOverrideBase", media_tab_lyrics_setting);
            load_settings();
            g_layout_dirty = true;
            trigger_nudge();
            break;
        }
        case 30:
            start_focus_timer(25, false);
            break;
        case 31:
            start_focus_timer(50, false);
            break;
        case 32:
            start_focus_timer(5, true);
            break;
        case 33:
            toggle_timer_pause();
            break;
        case 34:
            stop_focus_timer();
            break;
        case 40:
        {
            // Only reachable while visible (a hidden window can't be
            // right-clicked), so in practice this toggles hidden -> the
            // hotkey is the only way back. That's intentional; see readme.
            const bool now_hidden = !g_manually_hidden.load();
            g_manually_hidden = now_hidden;
            host::set_int_value(L"ManuallyHidden", now_hidden ? 1 : 0);
            if (now_hidden)
            {
                g_hotkey_unhide_until.store(0.0);
            }
            g_layout_dirty = true;
            break;
        }
        case lyrics_sync_menu_id_base + 1:
        case lyrics_sync_menu_id_base + 2:
        case lyrics_sync_menu_id_base + 3:
        case lyrics_sync_menu_id_base + 4:
        case lyrics_sync_menu_id_base + 5:
        case lyrics_sync_menu_id_base + 6:
        {
            static const int lyrics_steps[] = {10, 50, 250, -10, -50, -250};
            adjust_lyrics_nudge(lyrics_steps[cmd - lyrics_sync_menu_id_base - 1]);
            break;
        }
        case lyrics_sync_menu_id_base + 7:
            reset_lyrics_nudge();
            break;
        case 43:
            // Only sets a flag: the lyrics thread does the deletion (never the render thread)
            // and resets the counters. Lyrics currently on screen are left alone.
            g_lyrics_cache_clear_requested.store(true);
            break;
        case 42:
            paste_into_file_tray(hwnd);
            break;
        case 41:
        {
            // Clear the File Tray. Only the island's references are dropped --
            // nothing on disk is touched.
            {
                std::lock_guard lock(g_state_mutex);
                g_state.file_tray_items.clear();
            }
            save_file_tray();
            g_hovered_file_tray_row = -1;
            g_layout_dirty = true;
            break;
        }
    }
}

}
