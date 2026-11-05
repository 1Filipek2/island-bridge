#include "core/settings.h"
#include "core/color.h"
#include "core/constants.h"
#include "system/input_hooks.h"
#include "core/localization.h"
#include "core/model.h"
#include "system/monitor.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{

std::wstring get_string_setting_copy(PCWSTR name)
{
    return host::string_setting(name);
}

namespace
{

std::wstring get_string_setting_with_fallback(PCWSTR primary, PCWSTR fallback, PCWSTR fallback2 = nullptr)
{
    std::wstring result = get_string_setting_copy(primary);
    if (!result.empty())
    {
        return result;
    }
    result = get_string_setting_copy(fallback);
    if (!result.empty())
    {
        return result;
    }
    if (fallback2)
    {
        return get_string_setting_copy(fallback2);
    }
    return L"";
}

}

void load_settings()
{
    Settings next;

    const std::wstring position = get_string_setting_copy(L"Appearance.Position");
    if (equals_no_case(position, L"top-left"))
    {
        next.position = Position::TopLeft;
    }
    else if (equals_no_case(position, L"top-right"))
    {
        next.position = Position::TopRight;
    }
    else if (equals_no_case(position, L"bottom-center"))
    {
        next.position = Position::BottomCenter;
    }
    else if (equals_no_case(position, L"bottom-left"))
    {
        next.position = Position::BottomLeft;
    }
    else if (equals_no_case(position, L"bottom-right"))
    {
        next.position = Position::BottomRight;
    }

    const std::wstring scale = get_string_setting_copy(L"Appearance.SizeScale");
    if (!scale.empty())
    {
        wchar_t* end;
        float parsed_scale = wcstof(scale.c_str(), &end);
        if (end != scale.c_str() && parsed_scale > 0.1f && parsed_scale < 10.0f)
        {
            next.size_scale = parsed_scale;
        }
    }
    next.base_size_scale = next.size_scale;

    std::wstring mon = get_string_setting_copy(L"Appearance.TargetMonitor");
    if (mon == L"primary")
    {
        next.target_monitor = 0;
    }
    else if (mon == L"follow")
    {
        next.target_monitor = -1;
    }
    else
    {
        next.target_monitor = _wtoi(mon.c_str());
    }

    // Auto DPI scaling: multiply sizeScale by monitor DPI factor for the target display.
    // On mixed-DPI setups or a 4K 200% display this scales the island to the right physical size.
    next.auto_dpi_scale = (host::int_setting(L"Appearance.AutoDpiScale") != 0);
    HMONITOR anchor_mon = get_anchor_monitor_for(next.target_monitor);
    const float dpi = next.auto_dpi_scale ? get_monitor_dpi_scale(anchor_mon) : 1.0f;
    next.size_scale = next.base_size_scale * dpi;

    std::wstring font_setting = get_string_setting_with_fallback(L"Themes.FontFamily", L"Appearance.FontFamily");
    size_t first_char = font_setting.find_first_not_of(L" \t\r\n\"'");
    if (first_char != std::wstring::npos)
    {
        size_t last_char = font_setting.find_last_not_of(L" \t\r\n\"'");
        next.font_family = font_setting.substr(first_char, last_char - first_char + 1);
    }
    else
    {
        next.font_family.clear();
    }

    const std::wstring accent_mode = get_string_setting_copy(L"Themes.AccentColorMode");
    if (equals_no_case(accent_mode, L"system"))
    {
        next.accent_mode = AccentMode::System;
    }
    else if (equals_no_case(accent_mode, L"custom"))
    {
        next.accent_mode = AccentMode::Custom;
    }

    next.custom_accent = color_from_hex(get_string_setting_copy(L"Themes.CustomAccentHex"), next.custom_accent);

    const std::wstring cal_accent = get_string_setting_with_fallback(
        L"Themes.CalendarAccent", L"CalendarWeather.CalendarAccent", L"Modules.CalendarAccent");
    if (equals_no_case(cal_accent, L"system"))
    {
        next.calendar_accent = CalendarAccentMode::System;
    }
    else
    {
        next.calendar_accent = CalendarAccentMode::Accent;
    }

    const std::wstring fps_str = get_string_setting_with_fallback(L"Animations.TargetFPS", L"Appearance.TargetFPS");
    if (equals_no_case(fps_str, L"auto") || fps_str.empty())
    {
        next.target_fps = 0;
    }
    else
    {
        next.target_fps = _wtoi(fps_str.c_str());
        if (next.target_fps < 0)
        {
            next.target_fps = 0;
        }
    }

    const std::wstring style_str =
        get_string_setting_with_fallback(L"Animations.AnimationStyle", L"Appearance.AnimationStyle");
    if (equals_no_case(style_str, L"smooth"))
    {
        next.animation_style = AnimationStyle::Smooth;
    }
    else if (equals_no_case(style_str, L"bouncy"))
    {
        next.animation_style = AnimationStyle::Bouncy;
    }
    else if (equals_no_case(style_str, L"snappy"))
    {
        next.animation_style = AnimationStyle::Snappy;
    }
    else
    {
        next.animation_style = AnimationStyle::Default;
    }

    const std::wstring progress_str = get_string_setting_copy(L"Appearance.ProgressStyle");
    if (equals_no_case(progress_str, L"wavy"))
    {
        next.progress_style = ProgressStyle::Wavy;
    }
    else if (equals_no_case(progress_str, L"squiggle"))
    {
        next.progress_style = ProgressStyle::Squiggle;
    }
    else if (equals_no_case(progress_str, L"bar"))
    {
        next.progress_style = ProgressStyle::Bar;
    }
    else
    {
        next.progress_style = ProgressStyle::Slim; // default
    }

    const std::wstring spectrum_str = get_string_setting_copy(L"Appearance.SpectrumStyle");
    if (equals_no_case(spectrum_str, L"orb"))
    {
        next.spectrum_style = SpectrumStyle::Orb;
    }
    else if (equals_no_case(spectrum_str, L"plasma"))
    {
        next.spectrum_style = SpectrumStyle::Plasma;
    }
    else if (equals_no_case(spectrum_str, L"led"))
    {
        next.spectrum_style = SpectrumStyle::Led;
    }
    else
    {
        next.spectrum_style = SpectrumStyle::Bars; // default
    }
    g_spectrum_bands_needed.store(next.spectrum_style == SpectrumStyle::Orb ||
                                      next.spectrum_style == SpectrumStyle::Plasma,
                                  std::memory_order_relaxed);
    next.expanded_media_anim = host::int_setting(L"Animations.ExpandedMediaTransitions") != 0;
    next.pill_cover_anim = host::int_setting(L"Animations.PillCoverSpin") != 0;

    const std::wstring speed = get_string_setting_with_fallback(
        L"Animations.AnimationSpeed", L"Appearance.AnimationSpeed", L"Behavior.AnimationSpeed");
    if (equals_no_case(speed, L"very-slow"))
    {
        next.animation_speed = 0.5f;
    }
    else if (equals_no_case(speed, L"slow"))
    {
        next.animation_speed = 0.75f;
    }
    else if (equals_no_case(speed, L"fast"))
    {
        next.animation_speed = 1.35f;
    }
    else if (equals_no_case(speed, L"very-fast"))
    {
        next.animation_speed = 1.65f;
    }
    else if (equals_no_case(speed, L"ultra-fast"))
    {
        next.animation_speed = 2.0f;
    }
    else
    {
        next.animation_speed = 1.0f;
    }

    next.media = host::int_setting(L"Modules.Media") != 0;
    next.media_auto_expand = host::int_setting(L"Modules.MediaAutoExpand") != 0;
    next.volume = host::int_setting(L"Modules.Volume") != 0;
    if (!next.volume)
    {
        std::lock_guard lock(g_state_mutex);
        g_state.volume.active = false;
    }
    next.brightness = host::int_setting(L"Modules.Brightness") != 0;
    if (!next.brightness)
    {
        std::lock_guard lock(g_state_mutex);
        g_state.brightness.active = false;
        g_brightness_initialized = false;
    }
    next.clipboard = host::int_setting(L"Modules.Clipboard") != 0;
    next.status_countdown_progress = host::int_setting(L"Modules.StatusCountdownProgress") != 0;

    next.battery = host::int_setting(L"Modules.Battery") != 0;
    next.progress = host::int_setting(L"Modules.Progress") != 0;
    next.caps_lock = host::int_setting(L"Modules.CapsLock") != 0;
    next.timer_enabled = host::int_setting(L"Modules.TimerModule") != 0;

    next.hide_show_hotkey_enabled = host::int_setting(L"Shortcuts.HideShowHotkeyEnabled") != 0;

    const std::wstring hotkey_mod_str = get_string_setting_copy(L"Shortcuts.HideShowModifiers");
    if (equals_no_case(hotkey_mod_str, L"ctrl_shift"))
    {
        next.hide_show_modifiers = MOD_CONTROL | MOD_SHIFT;
    }
    else if (equals_no_case(hotkey_mod_str, L"alt_shift"))
    {
        next.hide_show_modifiers = MOD_ALT | MOD_SHIFT;
    }
    else if (equals_no_case(hotkey_mod_str, L"win_alt"))
    {
        next.hide_show_modifiers = MOD_WIN | MOD_ALT;
    }
    else if (equals_no_case(hotkey_mod_str, L"ctrl_alt_shift"))
    {
        next.hide_show_modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    }
    else
    {
        next.hide_show_modifiers = MOD_CONTROL | MOD_ALT;
    }
    next.hide_show_modifiers |= MOD_NOREPEAT;

    // Single A-Z/0-9 key only; anything else (blank, multi-char, symbol) falls back to 'D'.
    const std::wstring hotkey_key_str = get_string_setting_copy(L"Shortcuts.HideShowKey");
    next.hide_show_vk = 'D';
    if (hotkey_key_str.size() == 1)
    {
        const wchar_t ch = towupper(hotkey_key_str[0]);
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
        {
            next.hide_show_vk = static_cast<UINT>(ch);
        }
    }

    // ── Quick Lookup ─────────────────────────────────────────────────────────
    next.quick_lookup = host::int_setting(L"Modules.QuickLookup") != 0;

    const std::wstring lookup_mod_str = get_string_setting_copy(L"Shortcuts.LookupModifiers");
    if (equals_no_case(lookup_mod_str, L"ctrl_shift"))
    {
        next.lookup_modifiers = MOD_CONTROL | MOD_SHIFT;
    }
    else if (equals_no_case(lookup_mod_str, L"alt_shift"))
    {
        next.lookup_modifiers = MOD_ALT | MOD_SHIFT;
    }
    else if (equals_no_case(lookup_mod_str, L"win_alt"))
    {
        next.lookup_modifiers = MOD_WIN | MOD_ALT;
    }
    else if (equals_no_case(lookup_mod_str, L"ctrl_alt_shift"))
    {
        next.lookup_modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    }
    else
    {
        next.lookup_modifiers = MOD_CONTROL | MOD_ALT;
    }
    next.lookup_modifiers |= MOD_NOREPEAT;

    // "Space", or a single A-Z / 0-9 key; anything else falls back to Space.
    next.lookup_vk = VK_SPACE;
    {
        std::wstring key_str = get_string_setting_copy(L"Shortcuts.LookupKey");
        const size_t key_first = key_str.find_first_not_of(L" \t\r\n\"'");
        if (key_first != std::wstring::npos)
        {
            key_str = key_str.substr(key_first, key_str.find_last_not_of(L" \t\r\n\"'") - key_first + 1);
        }
        else
        {
            key_str.clear();
        }
        if (key_str.size() == 1)
        {
            const wchar_t ch = towupper(key_str[0]);
            if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
            {
                next.lookup_vk = static_cast<UINT>(ch);
            }
        }
    }

    next.bluetooth_indicator = host::int_setting(L"Modules.BluetoothIndicator") != 0;
    next.bluetooth_show_battery = host::int_setting(L"Modules.BluetoothShowBattery") != 0;
    next.do_not_disturb_indicator = host::int_setting(L"Modules.DoNotDisturbIndicator") != 0;
    next.notification_respect_dn_d = host::int_setting(L"Modules.NotificationRespectDnD") != 0;

    // ── Premium material / redesign ──────────────────────────────────────────
    {
        const std::wstring backdrop = get_string_setting_copy(L"Themes.BackdropMaterial");
        if (equals_no_case(backdrop, L"acrylic"))
        {
            next.backdrop_material = BackdropMaterial::Acrylic;
        }
        else if (equals_no_case(backdrop, L"blur"))
        {
            next.backdrop_material = BackdropMaterial::Blur;
        }
        else
        {
            next.backdrop_material = BackdropMaterial::None;
        }
    }
    next.backdrop_tint = clamp(host::int_setting(L"Themes.BackdropTint") / 100.0f, 0.0f, 1.0f);
    next.backdrop_fill_alpha = clamp(host::int_setting(L"Themes.BackdropFillOpacity") / 100.0f, 0.0f, 1.0f);

    next.material_depth = host::int_setting(L"Themes.MaterialDepth") != 0;
    next.drop_shadow = host::int_setting(L"Themes.DropShadow") != 0;
    next.accent_bloom = clamp(host::int_setting(L"Themes.AccentBloom") / 100.0f, 0.0f, 2.0f);
    next.text_scale = clamp(host::int_setting(L"Themes.TextScale") / 100.0f, 0.7f, 1.6f);

    // ── Clock / date presentation (#61) ──────────────────────────────────────
    next.show_seconds = host::int_setting(L"Modules.ShowSeconds") != 0;
    const std::wstring clock_mode = get_string_setting_copy(L"Modules.ClockFormat");
    next.clock_follow_system = clock_mode.empty() || equals_no_case(clock_mode, L"system");
    next.use24_hour_clock = equals_no_case(clock_mode, L"24h");
    next.date_format = get_string_setting_copy(L"Modules.DateFormat");
    next.date_first = host::int_setting(L"Modules.DateFirst") != 0;
    const std::wstring first_day = get_string_setting_copy(L"Modules.FirstDayOfWeek");
    if (equals_no_case(first_day, L"monday"))
    {
        next.first_day_of_week = 1;
    }
    else if (equals_no_case(first_day, L"sunday"))
    {
        next.first_day_of_week = 0;
    }
    else
    {
        next.first_day_of_week = -1;
    }
    next.media_pill_clock = host::int_setting(L"Modules.MediaPillClock") != 0;

    // ── Localization (#35) ───────────────────────────────────────────────────
    {
        std::wstring lang_tag = get_string_setting_copy(L"Modules.Language");
        if (lang_tag.empty())
        {
            lang_tag = L"auto";
        }
        next.language = lang_tag;
        const UiLanguage resolved =
            equals_no_case(lang_tag, L"auto") ? detect_system_language() : language_from_tag(lang_tag);
        g_ui_language.store(static_cast<int>(resolved), std::memory_order_relaxed);
    }

    // ── File tray (#33) ──────────────────────────────────────────────────────
    next.file_tray_module = host::int_setting(L"Modules.FileTrayModule") != 0;
    next.file_tray_max_items = clamp_int(host::int_setting(L"Modules.FileTrayMaxItems"), 1, 25);
    next.file_tray_persist = host::int_setting(L"Modules.FileTrayPersist") != 0;
    next.notification_center_module = host::int_setting(L"Modules.NotificationCenter") != 0;

    // ── Media auto-expand exclusions (#62) ───────────────────────────────────
    next.media_expand_blocklist.clear();
    {
        const std::wstring raw = get_string_setting_copy(L"Modules.MediaExpandBlocklist");
        size_t start = 0;
        while (start <= raw.size())
        {
            const size_t comma = raw.find(L',', start);
            const size_t end = (comma == std::wstring::npos) ? raw.size() : comma;
            std::wstring token = raw.substr(start, end - start);
            const size_t a = token.find_first_not_of(L" \t\r\n");
            if (a != std::wstring::npos)
            {
                const size_t b = token.find_last_not_of(L" \t\r\n");
                std::wstring entry = token.substr(a, b - a + 1);
                // Stored lowercase so matching against media source/title is
                // case-insensitive without re-lowering on every frame.
                std::transform(entry.begin(), entry.end(), entry.begin(),
                               [](wchar_t c)
                               {
                                   return static_cast<wchar_t>(towlower(c));
                               });
                next.media_expand_blocklist.push_back(std::move(entry));
            }
            if (comma == std::wstring::npos)
            {
                break;
            }
            start = comma + 1;
        }
    }

    // ── Game overlay options (#25) ───────────────────────────────────────────
    next.game_overlay_show_fps = host::int_setting(L"Modules.GameOverlayShowFps") != 0;
    next.game_overlay_show_cpu = host::int_setting(L"Modules.GameOverlayShowCpu") != 0;
    next.game_overlay_show_gpu = host::int_setting(L"Modules.GameOverlayShowGpu") != 0;
    next.game_overlay_show_ram = host::int_setting(L"Modules.GameOverlayShowRam") != 0;
    next.game_overlay_show_disk = host::int_setting(L"Modules.GameOverlayShowDisk") != 0;
    next.game_overlay_compact = host::int_setting(L"Modules.GameOverlayCompact") != 0;
    next.tint_opacity = clamp(host::int_setting(L"Themes.TintIntensity") / 100.0f, 0.0f, 1.0f);
    const int setting_opacity = host::int_setting(L"Themes.PillOpacity");
    const int local_opacity = host::int_value(L"PillOpacityOverride", -1);
    g_pill_opacity_override.store(local_opacity);
    g_game_overlay_pinned.store(host::int_value(L"GameOverlayPinned", 0) != 0);
    g_pinned_expanded.store(host::int_value(L"PinnedExpanded", 0) != 0);
    next.pill_opacity = clamp((local_opacity >= 0 ? local_opacity : setting_opacity) / 100.0f, 0.35f, 1.0f);
    next.game_overlay = host::int_setting(L"Modules.GameOverlay") != 0;
    next.show_metric_text = host::int_setting(L"Modules.ShowMetricText") != 0;
    next.weather = host::int_setting(L"Modules.Weather") != 0;
    next.weather_city = get_string_setting_with_fallback(L"Modules.WeatherCity", L"Weather.WeatherCity",
                                                         L"CalendarWeather.WeatherCity");
    next.weather_fahrenheit = host::int_setting(L"Modules.WeatherFahrenheit") != 0;
    next.lyrics_visualizer = host::int_setting(L"Modules.LyricsVisualizer") != 0;
    next.karaoke_lyrics = host::int_setting(L"Modules.KaraokeLyrics") != 0;
    next.karaoke_letter_glow = host::int_setting(L"Modules.KaraokeLetterGlow") != 0;
    next.lyrics_neighbor_lines_visible = host::int_setting(L"Modules.LyricsNeighborLinesVisible") != 0;
    next.lyrics_offset_ms =
        clamp_int(host::int_setting(L"Modules.LyricsOffsetMs"), -lyrics_offset_limit_ms, lyrics_offset_limit_ms);
    next.lyrics_offset_bt_ms = clamp_int(host::int_setting(L"Modules.LyricsOffsetBluetoothMs"), -lyrics_offset_limit_ms,
                                         lyrics_offset_limit_ms);
    publish_lyrics_offsets(next.lyrics_offset_ms, next.lyrics_offset_bt_ms);
    next.lyrics_cache = host::int_setting(L"Modules.LyricsCache") != 0;
    {
        const std::wstring cap_str = get_string_setting_copy(L"Modules.LyricsCacheMaxMB");
        int cap_mb = 25; // default, also used if the value is unparsable
        if (cap_str == L"0")
        {
            cap_mb = 0;
        }
        else if (!cap_str.empty())
        {
            const int parsed = _wtoi(cap_str.c_str());
            if (parsed > 0)
            {
                cap_mb = clamp_int(parsed, 1, 1024);
            }
        }
        next.lyrics_cache_max_mb = cap_mb;
    }
    const std::wstring hide_sec =
        get_string_setting_with_fallback(L"Behavior.AutoHideIdleSeconds", L"Appearance.AutoHideIdleSeconds");
    next.auto_hide_idle_seconds = hide_sec.empty() ? 0 : _wtoi(hide_sec.c_str());
    next.unhide_on_hover = host::int_setting(L"Behavior.UnhideOnHover") != 0;
    next.always_on_top = host::int_setting(L"Behavior.AlwaysOnTop") != 0;
    const int local_expand_on_hover = host::int_value(L"ExpandOnHoverOverride", -1);
    next.expand_on_hover =
        local_expand_on_hover >= 0 ? (local_expand_on_hover != 0) : (host::int_setting(L"Behavior.ExpandOnHover") != 0);
    // Right-click toggle; stored with Wh_SetIntValue so it survives restarts.
    next.collapsed_lyrics = host::int_value(L"CollapsedLyrics", 0) != 0;
    {
        // Right-click choice wins until the Mod Settings value is changed.
        const int media_tab_lyrics_setting = host::int_setting(L"Modules.MediaTabLyrics") != 0 ? 1 : 0;
        int media_tab_lyrics_override = host::int_value(L"MediaTabLyricsOverride", -1);
        if (media_tab_lyrics_override >= 0 &&
            host::int_value(L"MediaTabLyricsOverrideBase", -1) != media_tab_lyrics_setting)
        {
            host::set_int_value(L"MediaTabLyricsOverride", -1);
            media_tab_lyrics_override = -1;
        }
        next.media_tab_lyrics =
            media_tab_lyrics_override >= 0 ? (media_tab_lyrics_override != 0) : (media_tab_lyrics_setting != 0);
    }
    next.auto_dpi_scale = host::int_setting(L"Appearance.AutoDpiScale") != 0;
    next.offset_x = host::int_setting(L"Appearance.OffsetX");
    next.offset_y = host::int_setting(L"Appearance.OffsetY");
    next.separate_expanded_offset_y = host::int_setting(L"Appearance.SeparateExpandedOffsetY") != 0;
    next.offset_y_expanded = host::int_setting(L"Appearance.OffsetYExpanded");

    std::wstring shape_str = get_string_setting_copy(L"Appearance.ShapeStyle");
    static std::wstring s_last_configured_shape = L"";
    if (!shape_str.empty() && shape_str != s_last_configured_shape)
    {
        s_last_configured_shape = shape_str;
        host::set_int_value(L"W11StyleOverride", -1);
        host::set_int_value(L"NotchStyleOverride", -1);
    }
    bool base_w11 = equals_no_case(shape_str, L"w11");
    bool base_notch = equals_no_case(shape_str, L"notch");

    const int local_w11_style = host::int_value(L"W11StyleOverride", -1);
    next.w11_style = local_w11_style >= 0 ? (local_w11_style != 0) : base_w11;

    const int local_notch_style = host::int_value(L"NotchStyleOverride", -1);
    next.notch_style = local_notch_style >= 0 ? (local_notch_style != 0) : base_notch;

    // A bottom-anchored island cannot be a top-edge macOS notch.
    if (is_bottom_position(next.position))
    {
        next.notch_style = false;
    }

    // Color settings — check local theme override first, then settings YAML.
    // Palettes live in kThemePalettes at file scope so the right-click menu
    // resolves the same colors and labels this does.
    //
    // A custom hex field counts as an intentional override once it differs from
    // the value shipped as its default. Presets keep working untouched for
    // anyone who never edits these fields, but an edited hex now takes effect
    // immediately instead of being silently discarded unless the Theme preset
    // also happened to be switched to Custom. Clearing the field back to its
    // default hands control back to the preset.
    //
    // `defaultHexes` deliberately lists the *historical* defaults as well as the
    // current one. Retiring the old palettes moved these defaults, and Windhawk
    // keeps whatever value is already stored in a user's config -- so someone who
    // never touched the hex fields would still be carrying "#0D0D0F" from the old
    // OLED Black default. Matching only the current default would read that as a
    // deliberate override and pin every new palette back to the old background.
    auto resolve_color = [](const wchar_t* setting_key, std::initializer_list<const wchar_t*> default_hexes,
                            D2D1_COLOR_F preset_color) -> D2D1_COLOR_F
    {
        const std::wstring value = get_string_setting_copy(setting_key);
        if (value.empty())
        {
            return preset_color;
        }

        // Compare the parsed colors rather than the strings, so equivalent
        // spellings of the default ("0D0D0F" without the '#', "#0D0D0FFF",
        // different case) are all still recognised as "untouched" and leave the
        // preset in charge.
        const D2D1_COLOR_F sentinel = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
        const D2D1_COLOR_F parsed = color_from_hex(value, sentinel);
        if (parsed.r == sentinel.r && parsed.g == sentinel.g && parsed.b == sentinel.b && parsed.a == sentinel.a)
        {
            // Unparseable, so treat it as not set.
            return preset_color;
        }

        for (const wchar_t* default_hex : default_hexes)
        {
            const D2D1_COLOR_F defaults = color_from_hex(default_hex, sentinel);
            if (parsed.r == defaults.r && parsed.g == defaults.g && parsed.b == defaults.b && parsed.a == defaults.a)
            {
                return preset_color;
            }
        }
        return parsed;
    };

    auto apply_preset = [&](Settings& target, int idx)
    {
        const ThemePalette& p = theme_palettes[idx];
        target.pill_bg_color = resolve_color(L"Themes.PillBgColor", {theme_palettes[0].bg, L"#0D0D0F"},
                                             color_from_hex(p.bg, D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f)));
        target.text_primary_color = resolve_color(L"Themes.TextPrimaryColor", {L"#FFFFFF", L"#F7F7F7"},
                                                  color_from_hex(p.fg, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)));
        target.text_secondary_color =
            resolve_color(L"Themes.TextSecondaryColor", {theme_palettes[0].sec, L"#B0B0B8", L"#888888"},
                          color_from_hex(p.sec, D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f)));
        target.contour_border_color =
            resolve_color(L"Themes.ContourBorderHex", {theme_palettes[0].border, L"#333338"},
                          color_from_hex(p.border, D2D1::ColorF(0.118f, 0.118f, 0.133f, 1.0f)));
    };

    std::wstring theme_preset_str = get_string_setting_copy(L"Themes.ThemePreset");
    // The active palette is persisted as an integer so the right-click menu can
    // change it without rewriting the settings file. Migrate the old key across
    // once, since the integers were renumbered when the palettes changed.
    if (host::int_value(theme_value_name, -1) < 0)
    {
        const int legacy = host::int_value(legacy_theme_value_name, -1);
        if (legacy >= 0)
        {
            int migrated;
            switch (legacy)
            {
                case 0:
                    migrated = 0;
                    break; // OLED Black    -> Obsidian
                case 1:
                    migrated = 1;
                    break; // Fluent        -> Graphite
                case 2:
                    migrated = 2;
                    break; // Midnight Blue -> Slate
                case 3:
                    migrated = 6;
                    break; // Deep Purple   -> Plum
                case 4:
                    migrated = 1;
                    break; // Fluent Design -> Graphite
                default:
                    migrated = custom_theme_index;
                    break; // out of range meant Custom
            }
            host::set_int_value(theme_value_name, migrated);
        }
    }

    static std::wstring s_last_configured_preset = L"";
    if (!theme_preset_str.empty() && theme_preset_str != s_last_configured_preset)
    {
        s_last_configured_preset = theme_preset_str;
        const int from_settings = theme_index_from_id(theme_preset_str);
        if (from_settings >= 0)
        {
            host::set_int_value(theme_value_name, from_settings);
        }
    }

    const int theme = host::int_value(theme_value_name, -1);
    if (theme >= 0 && theme < custom_theme_index)
    {
        next.theme_preset = static_cast<ThemePreset>(theme);
        apply_preset(next, theme);
    }
    else if (theme >= custom_theme_index || equals_no_case(theme_preset_str, L"custom"))
    {
        // Explicit Custom: the hex fields are authoritative, defaults included.
        next.theme_preset = ThemePreset::Custom;
        next.pill_bg_color =
            color_from_hex(get_string_setting_copy(L"Themes.PillBgColor"), D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f));
        next.text_primary_color =
            color_from_hex(get_string_setting_copy(L"Themes.TextPrimaryColor"), D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        next.text_secondary_color = color_from_hex(get_string_setting_copy(L"Themes.TextSecondaryColor"),
                                                   D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f));
        next.contour_border_color = color_from_hex(get_string_setting_copy(L"Themes.ContourBorderHex"),
                                                   D2D1::ColorF(0.118f, 0.118f, 0.133f, 1.0f));
    }
    else
    {
        const int from_settings = theme_index_from_id(theme_preset_str);
        const int preset_idx = (from_settings >= 0 && from_settings < custom_theme_index) ? from_settings : 0;
        next.theme_preset = static_cast<ThemePreset>(preset_idx);
        apply_preset(next, preset_idx);
    }

    // Graphite inherits the old Fluent theme's auto-translucency: it is the
    // neutral Windows 11 grey, and it reads best when the desktop shows through
    // a little. The rest of the palettes are opaque unless the user says so.
    if (next.theme_preset == ThemePreset::Graphite && local_opacity < 0)
    {
        next.pill_opacity = 0.88f;
    }

    next.border_merged_mode = host::int_setting(L"Appearance.BorderMergedMode") != 0;
    next.auto_hide_fullscreen = host::int_setting(L"Behavior.AutoHideFullscreen") != 0;
    next.hardware_monitor_module = host::int_setting(L"Modules.HardwareMonitorModule") != 0;
    next.github_module = host::int_setting(L"Modules.GitHubModule") != 0;
    const std::wstring border_mode_str = get_string_setting_copy(L"Themes.ContourBorderMode");
    if (equals_no_case(border_mode_str, L"borderless"))
    {
        next.contour_border_mode = ContourBorderMode::Borderless;
        next.contour_border_enabled = false;
    }
    else if (equals_no_case(border_mode_str, L"auto"))
    {
        next.contour_border_mode = ContourBorderMode::Auto;
        next.contour_border_enabled = true;
    }
    else if (equals_no_case(border_mode_str, L"default"))
    {
        next.contour_border_mode = ContourBorderMode::Default;
        next.contour_border_enabled = true;
    }
    else
    {
        next.contour_border_enabled = true; // no such setting; the mode dropdown drives this
        next.contour_border_mode =
            next.contour_border_enabled ? ContourBorderMode::Default : ContourBorderMode::Borderless;
    }
    next.clock_accent_glow = host::int_setting(L"Themes.ClockAccentGlow") != 0;
    bool settings_changed_while_hidden = (g_auto_hidden_parked.load() || g_manually_hidden.load());
    bool unhide_requested = (next.auto_hide_idle_seconds == 0) || (next.unhide_on_hover && !g_settings.unhide_on_hover);
    if (unhide_requested || (settings_changed_while_hidden && next.auto_hide_idle_seconds == 0))
    {
        g_auto_hidden_parked = false;
        g_manually_hidden = false;
        host::set_int_value(L"ManuallyHidden", 0);
        g_hotkey_unhide_until.store(now_seconds() +
                                    (next.auto_hide_idle_seconds > 0 ? next.auto_hide_idle_seconds : 6.0));
        if (g_hwnd)
        {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            PostMessageW(g_hwnd, wm_app_new_event, 0, 0);
        }
    }
    else if (settings_changed_while_hidden && next.unhide_on_hover)
    {
        g_auto_hidden_parked = false;
        g_manually_hidden = false;
        host::set_int_value(L"ManuallyHidden", 0);
        g_hotkey_unhide_until.store(now_seconds() +
                                    (next.auto_hide_idle_seconds > 0 ? next.auto_hide_idle_seconds : 6.0));
        if (g_hwnd)
        {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            PostMessageW(g_hwnd, wm_app_new_event, 0, 0);
        }
    }

    // Privacy Indicators: unified reading with complete fallback to legacy keys
    next.privacy_dots = host::int_setting(L"Indicators.PrivacyDots") != 0;
    next.privacy_dots_mic = host::int_setting(L"Indicators.PrivacyDotsMic") != 0;
    next.privacy_dots_cam = host::int_setting(L"Indicators.PrivacyDotsCam") != 0;
    next.privacy_dots_pulse = true; // no such setting; pulsing is always on

    std::wstring mic_hex_str = get_string_setting_with_fallback(L"Indicators.PrivacyDotsMicHex",
                                                                L"Indicators.MicDotHex", L"Modules.PrivacyDotsMicHex");
    next.privacy_dots_mic_hex = color_from_hex(mic_hex_str, D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f));

    std::wstring cam_hex_str = get_string_setting_with_fallback(L"Indicators.PrivacyDotsCamHex",
                                                                L"Indicators.CamDotHex", L"Modules.PrivacyDotsCamHex");
    next.privacy_dots_cam_hex = color_from_hex(cam_hex_str, D2D1::ColorF(0.204f, 0.780f, 0.349f, 1.0f));

    // Compatibility aliases: always keep in sync
    next.privacy_dots_enabled = next.privacy_dots;
    next.privacy_dot_pulsing = next.privacy_dots_pulse;
    next.mic_dot_color = next.privacy_dots_mic_hex;
    next.cam_dot_color = next.privacy_dots_cam_hex;

    g_pinned_expanded.store(false);
    host::set_int_value(L"PinnedExpanded", 0);

    // Compare-then-publish under one lock so the change flags describe exactly
    // the transition we are about to commit. Two LoadSettings() calls can run
    // concurrently (settings thread vs. tray menu on the render thread); without
    // this, both could read the same "old" values and each decide a hotkey
    // re-registration was needed, or neither would.
    bool city_changed = false;
    bool hotkey_setting_changed = false;
    bool backdrop_changed = false;
    bool caps_lock_changed = false;
    {
        std::lock_guard lock(g_settings_mutex);
        city_changed = next.weather_city != g_settings.weather_city;
        caps_lock_changed = next.caps_lock != g_settings.caps_lock;
        hotkey_setting_changed =
            next.hide_show_hotkey_enabled != g_settings.hide_show_hotkey_enabled ||
            next.hide_show_modifiers != g_settings.hide_show_modifiers ||
            next.hide_show_vk != g_settings.hide_show_vk || next.quick_lookup != g_settings.quick_lookup ||
            next.lookup_modifiers != g_settings.lookup_modifiers || next.lookup_vk != g_settings.lookup_vk;
        backdrop_changed = next.backdrop_material != g_settings.backdrop_material ||
                           std::fabs(next.backdrop_tint - g_settings.backdrop_tint) > 0.001f ||
                           std::fabs(next.backdrop_fill_alpha - g_settings.backdrop_fill_alpha) > 0.001f ||
                           next.always_on_top != g_settings.always_on_top;
        g_settings = std::move(next);
    }
    g_layout_dirty = true;
    if (city_changed && g_settings_changed_event)
    {
        SetEvent(g_settings_changed_event);
    }
    // Installs or removes WH_KEYBOARD_LL to match. The keyboard thread would pick
    // this up on its next backstop tick anyway; this just makes it immediate.
    if (caps_lock_changed)
    {
        notify_keyboard_thread_setting_changed();
    }
    // g_hwnd only exists once RenderThreadProc has created the overlay window;
    // the very first LoadSettings() call (at mod init, before StartThreads())
    // runs with g_hwnd still null, so ApplyHideShowHotkey() no-ops there and
    // RenderThreadProc does the initial registration itself right after
    // CreateWindowExW. Every later call (e.g. from WhTool_ModSettingsChanged)
    // re-registers live so hotkey edits apply without a mod restart.
    // Posted, not called: UnregisterHotKey / RegisterHotKey fail with
    // ERROR_WINDOW_OF_OTHER_THREAD from this thread, which left the old
    // combination registered, the new one never registered, and the bookkeeping
    // flag claiming otherwise -- so switching the hotkey off never released it.
    if (hotkey_setting_changed)
    {
        if (g_hwnd)
        {
            PostMessageW(g_hwnd, wm_app_apply_hotkey, 0, 0);
        }
    }
    // Same story for the backdrop: no-ops before the window exists, and applies
    // live afterwards so switching blur/acrylic needs no mod restart.
    if (backdrop_changed && g_hwnd)
    {
        PostMessageW(g_hwnd, wm_app_apply_backdrop, 0, 0);
    }
}

}
