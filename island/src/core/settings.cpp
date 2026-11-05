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


std::wstring GetStringSettingCopy(PCWSTR name) {
    return host::string_setting(name);
}


namespace
{


std::wstring GetStringSettingWithFallback(PCWSTR primary, PCWSTR fallback, PCWSTR fallback2 = nullptr) {
    std::wstring result = GetStringSettingCopy(primary);
    if (!result.empty()) {
        return result;
    }
    result = GetStringSettingCopy(fallback);
    if (!result.empty()) {
        return result;
    }
    if (fallback2) {
        return GetStringSettingCopy(fallback2);
    }
    return L"";
}

}



void LoadSettings() {
    Settings next;

    const std::wstring position = GetStringSettingCopy(L"Appearance.Position");
    if (EqualsNoCase(position, L"top-left")) {
        next.position = Position::TopLeft;
    } else if (EqualsNoCase(position, L"top-right")) {
        next.position = Position::TopRight;
    } else if (EqualsNoCase(position, L"bottom-center")) {
        next.position = Position::BottomCenter;
    } else if (EqualsNoCase(position, L"bottom-left")) {
        next.position = Position::BottomLeft;
    } else if (EqualsNoCase(position, L"bottom-right")) {
        next.position = Position::BottomRight;
    }

    const std::wstring scale = GetStringSettingCopy(L"Appearance.SizeScale");
    if (!scale.empty()) {
        wchar_t* end;
        float parsedScale = wcstof(scale.c_str(), &end);
        if (end != scale.c_str() && parsedScale > 0.1f && parsedScale < 10.0f) {
            next.sizeScale = parsedScale;
        }
    }
    next.baseSizeScale = next.sizeScale;

    std::wstring mon = GetStringSettingCopy(L"Appearance.TargetMonitor");
    if (mon == L"primary") next.targetMonitor = 0;
    else if (mon == L"follow") next.targetMonitor = -1;
    else next.targetMonitor = _wtoi(mon.c_str());

    // Auto DPI scaling: multiply sizeScale by monitor DPI factor for the target display.
    // On mixed-DPI setups or a 4K 200% display this scales the island to the right physical size.
    next.autoDpiScale = (host::int_setting(L"Appearance.AutoDpiScale") != 0);
    HMONITOR anchorMon = GetAnchorMonitorFor(next.targetMonitor);
    const float dpi = next.autoDpiScale ? GetMonitorDpiScale(anchorMon) : 1.0f;
    next.sizeScale = next.baseSizeScale * dpi;

    std::wstring fontSetting = GetStringSettingWithFallback(L"Themes.FontFamily", L"Appearance.FontFamily");
    size_t firstChar = fontSetting.find_first_not_of(L" \t\r\n\"'");
    if (firstChar != std::wstring::npos) {
        size_t lastChar = fontSetting.find_last_not_of(L" \t\r\n\"'");
        next.fontFamily = fontSetting.substr(firstChar, lastChar - firstChar + 1);
    } else {
        next.fontFamily.clear();
    }

    const std::wstring accentMode = GetStringSettingCopy(L"Themes.AccentColorMode");
    if (EqualsNoCase(accentMode, L"system")) {
        next.accentMode = AccentMode::System;
    } else if (EqualsNoCase(accentMode, L"custom")) {
        next.accentMode = AccentMode::Custom;
    }

    next.customAccent = ColorFromHex(GetStringSettingCopy(L"Themes.CustomAccentHex"), next.customAccent);

    const std::wstring calAccent = GetStringSettingWithFallback(L"Themes.CalendarAccent", L"CalendarWeather.CalendarAccent", L"Modules.CalendarAccent");
    if (EqualsNoCase(calAccent, L"system")) {
        next.calendarAccent = CalendarAccentMode::System;
    } else {
        next.calendarAccent = CalendarAccentMode::Accent;
    }

    const std::wstring fpsStr = GetStringSettingWithFallback(L"Animations.TargetFPS", L"Appearance.TargetFPS");
    if (EqualsNoCase(fpsStr, L"auto") || fpsStr.empty()) {
        next.targetFps = 0;
    } else {
        next.targetFps = _wtoi(fpsStr.c_str());
        if (next.targetFps < 0) next.targetFps = 0;
    }

    const std::wstring styleStr = GetStringSettingWithFallback(L"Animations.AnimationStyle", L"Appearance.AnimationStyle");
    if (EqualsNoCase(styleStr, L"smooth")) {
        next.animationStyle = AnimationStyle::Smooth;
    } else if (EqualsNoCase(styleStr, L"bouncy")) {
        next.animationStyle = AnimationStyle::Bouncy;
    } else if (EqualsNoCase(styleStr, L"snappy")) {
        next.animationStyle = AnimationStyle::Snappy;
    } else {
        next.animationStyle = AnimationStyle::Default;
    }

    const std::wstring progressStr = GetStringSettingCopy(L"Appearance.ProgressStyle");
    if (EqualsNoCase(progressStr, L"wavy")) {
        next.progressStyle = ProgressStyle::Wavy;
    } else if (EqualsNoCase(progressStr, L"squiggle")) {
        next.progressStyle = ProgressStyle::Squiggle;
    } else if (EqualsNoCase(progressStr, L"bar")) {
        next.progressStyle = ProgressStyle::Bar;
    } else {
        next.progressStyle = ProgressStyle::Slim;  // default
    }

    const std::wstring spectrumStr = GetStringSettingCopy(L"Appearance.SpectrumStyle");
    if (EqualsNoCase(spectrumStr, L"orb")) {
        next.spectrumStyle = SpectrumStyle::Orb;
    } else if (EqualsNoCase(spectrumStr, L"plasma")) {
        next.spectrumStyle = SpectrumStyle::Plasma;
    } else if (EqualsNoCase(spectrumStr, L"led")) {
        next.spectrumStyle = SpectrumStyle::Led;
    } else {
        next.spectrumStyle = SpectrumStyle::Bars;  // default
    }
    g_spectrumBandsNeeded.store(next.spectrumStyle == SpectrumStyle::Orb ||
                                    next.spectrumStyle == SpectrumStyle::Plasma,
                                std::memory_order_relaxed);
    next.expandedMediaAnim = host::int_setting(L"Animations.ExpandedMediaTransitions") != 0;
    next.pillCoverAnim = host::int_setting(L"Animations.PillCoverSpin") != 0;

    const std::wstring speed = GetStringSettingWithFallback(L"Animations.AnimationSpeed", L"Appearance.AnimationSpeed", L"Behavior.AnimationSpeed");
    if (EqualsNoCase(speed, L"very-slow")) {
        next.animationSpeed = 0.5f;
    } else if (EqualsNoCase(speed, L"slow")) {
        next.animationSpeed = 0.75f;
    } else if (EqualsNoCase(speed, L"fast")) {
        next.animationSpeed = 1.35f;
    } else if (EqualsNoCase(speed, L"very-fast")) {
        next.animationSpeed = 1.65f;
    } else if (EqualsNoCase(speed, L"ultra-fast")) {
        next.animationSpeed = 2.0f;
    } else {
        next.animationSpeed = 1.0f;
    }

    next.media = host::int_setting(L"Modules.Media") != 0;
    next.mediaAutoExpand = host::int_setting(L"Modules.MediaAutoExpand") != 0;
    next.volume = host::int_setting(L"Modules.Volume") != 0;
    if (!next.volume) {
        std::lock_guard lock(g_stateMutex);
        g_state.volume.active = false;
    }
    next.brightness = host::int_setting(L"Modules.Brightness") != 0;
    if (!next.brightness) {
        std::lock_guard lock(g_stateMutex);
        g_state.brightness.active = false;
        g_brightnessInitialized = false;
    }
    next.clipboard = host::int_setting(L"Modules.Clipboard") != 0;
    next.statusCountdownProgress = host::int_setting(L"Modules.StatusCountdownProgress") != 0;

    next.battery = host::int_setting(L"Modules.Battery") != 0;
    next.progress = host::int_setting(L"Modules.Progress") != 0;
    next.capsLock = host::int_setting(L"Modules.CapsLock") != 0;
    next.timerEnabled = host::int_setting(L"Modules.TimerModule") != 0;

    next.hideShowHotkeyEnabled = host::int_setting(L"Shortcuts.HideShowHotkeyEnabled") != 0;

    const std::wstring hotkeyModStr = GetStringSettingCopy(L"Shortcuts.HideShowModifiers");
    if (EqualsNoCase(hotkeyModStr, L"ctrl_shift")) {
        next.hideShowModifiers = MOD_CONTROL | MOD_SHIFT;
    } else if (EqualsNoCase(hotkeyModStr, L"alt_shift")) {
        next.hideShowModifiers = MOD_ALT | MOD_SHIFT;
    } else if (EqualsNoCase(hotkeyModStr, L"win_alt")) {
        next.hideShowModifiers = MOD_WIN | MOD_ALT;
    } else if (EqualsNoCase(hotkeyModStr, L"ctrl_alt_shift")) {
        next.hideShowModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    } else {
        next.hideShowModifiers = MOD_CONTROL | MOD_ALT;
    }
    next.hideShowModifiers |= MOD_NOREPEAT;

    // Single A-Z/0-9 key only; anything else (blank, multi-char, symbol) falls back to 'D'.
    const std::wstring hotkeyKeyStr = GetStringSettingCopy(L"Shortcuts.HideShowKey");
    next.hideShowVk = 'D';
    if (hotkeyKeyStr.size() == 1) {
        const wchar_t ch = towupper(hotkeyKeyStr[0]);
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            next.hideShowVk = static_cast<UINT>(ch);
        }
    }

    // ── Quick Lookup ─────────────────────────────────────────────────────────
    next.quickLookup = host::int_setting(L"Modules.QuickLookup") != 0;

    const std::wstring lookupModStr = GetStringSettingCopy(L"Shortcuts.LookupModifiers");
    if (EqualsNoCase(lookupModStr, L"ctrl_shift")) {
        next.lookupModifiers = MOD_CONTROL | MOD_SHIFT;
    } else if (EqualsNoCase(lookupModStr, L"alt_shift")) {
        next.lookupModifiers = MOD_ALT | MOD_SHIFT;
    } else if (EqualsNoCase(lookupModStr, L"win_alt")) {
        next.lookupModifiers = MOD_WIN | MOD_ALT;
    } else if (EqualsNoCase(lookupModStr, L"ctrl_alt_shift")) {
        next.lookupModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    } else {
        next.lookupModifiers = MOD_CONTROL | MOD_ALT;
    }
    next.lookupModifiers |= MOD_NOREPEAT;

    // "Space", or a single A-Z / 0-9 key; anything else falls back to Space.
    next.lookupVk = VK_SPACE;
    {
        std::wstring keyStr = GetStringSettingCopy(L"Shortcuts.LookupKey");
        const size_t keyFirst = keyStr.find_first_not_of(L" \t\r\n\"'");
        if (keyFirst != std::wstring::npos) {
            keyStr = keyStr.substr(keyFirst, keyStr.find_last_not_of(L" \t\r\n\"'") - keyFirst + 1);
        } else {
            keyStr.clear();
        }
        if (keyStr.size() == 1) {
            const wchar_t ch = towupper(keyStr[0]);
            if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
                next.lookupVk = static_cast<UINT>(ch);
            }
        }
    }

    next.bluetoothIndicator = host::int_setting(L"Modules.BluetoothIndicator") != 0;
    next.bluetoothShowBattery = host::int_setting(L"Modules.BluetoothShowBattery") != 0;
    next.doNotDisturbIndicator = host::int_setting(L"Modules.DoNotDisturbIndicator") != 0;
    next.notificationRespectDnD = host::int_setting(L"Modules.NotificationRespectDnD") != 0;

    // ── Premium material / redesign ──────────────────────────────────────────
    {
        const std::wstring backdrop = GetStringSettingCopy(L"Themes.BackdropMaterial");
        if (EqualsNoCase(backdrop, L"acrylic")) {
            next.backdropMaterial = BackdropMaterial::Acrylic;
        } else if (EqualsNoCase(backdrop, L"blur")) {
            next.backdropMaterial = BackdropMaterial::Blur;
        } else {
            next.backdropMaterial = BackdropMaterial::None;
        }
    }
    next.backdropTint = Clamp(host::int_setting(L"Themes.BackdropTint") / 100.0f, 0.0f, 1.0f);
    next.backdropFillAlpha = Clamp(host::int_setting(L"Themes.BackdropFillOpacity") / 100.0f, 0.0f, 1.0f);

    next.materialDepth = host::int_setting(L"Themes.MaterialDepth") != 0;
    next.dropShadow = host::int_setting(L"Themes.DropShadow") != 0;
    next.accentBloom = Clamp(host::int_setting(L"Themes.AccentBloom") / 100.0f, 0.0f, 2.0f);
    next.textScale = Clamp(host::int_setting(L"Themes.TextScale") / 100.0f, 0.7f, 1.6f);

    // ── Clock / date presentation (#61) ──────────────────────────────────────
    next.showSeconds = host::int_setting(L"Modules.ShowSeconds") != 0;
    const std::wstring clockMode = GetStringSettingCopy(L"Modules.ClockFormat");
    next.clockFollowSystem = clockMode.empty() || EqualsNoCase(clockMode, L"system");
    next.use24HourClock = EqualsNoCase(clockMode, L"24h");
    next.dateFormat = GetStringSettingCopy(L"Modules.DateFormat");
    next.dateFirst = host::int_setting(L"Modules.DateFirst") != 0;
    const std::wstring firstDay = GetStringSettingCopy(L"Modules.FirstDayOfWeek");
    if (EqualsNoCase(firstDay, L"monday")) {
        next.firstDayOfWeek = 1;
    } else if (EqualsNoCase(firstDay, L"sunday")) {
        next.firstDayOfWeek = 0;
    } else {
        next.firstDayOfWeek = -1;
    }
    next.mediaPillClock = host::int_setting(L"Modules.MediaPillClock") != 0;

    // ── Localization (#35) ───────────────────────────────────────────────────
    {
        std::wstring langTag = GetStringSettingCopy(L"Modules.Language");
        if (langTag.empty()) {
            langTag = L"auto";
        }
        next.language = langTag;
        const UiLanguage resolved = EqualsNoCase(langTag, L"auto") ? DetectSystemLanguage()
                                                                  : LanguageFromTag(langTag);
        g_uiLanguage.store(static_cast<int>(resolved), std::memory_order_relaxed);
    }

    // ── File tray (#33) ──────────────────────────────────────────────────────
    next.fileTrayModule = host::int_setting(L"Modules.FileTrayModule") != 0;
    next.fileTrayMaxItems = ClampInt(host::int_setting(L"Modules.FileTrayMaxItems"), 1, 25);
    next.fileTrayPersist = host::int_setting(L"Modules.FileTrayPersist") != 0;
    next.notificationCenterModule = host::int_setting(L"Modules.NotificationCenter") != 0;

    // ── Media auto-expand exclusions (#62) ───────────────────────────────────
    next.mediaExpandBlocklist.clear();
    {
        const std::wstring raw = GetStringSettingCopy(L"Modules.MediaExpandBlocklist");
        size_t start = 0;
        while (start <= raw.size()) {
            const size_t comma = raw.find(L',', start);
            const size_t end = (comma == std::wstring::npos) ? raw.size() : comma;
            std::wstring token = raw.substr(start, end - start);
            const size_t a = token.find_first_not_of(L" \t\r\n");
            if (a != std::wstring::npos) {
                const size_t b = token.find_last_not_of(L" \t\r\n");
                std::wstring entry = token.substr(a, b - a + 1);
                // Stored lowercase so matching against media source/title is
                // case-insensitive without re-lowering on every frame.
                std::transform(entry.begin(), entry.end(), entry.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                next.mediaExpandBlocklist.push_back(std::move(entry));
            }
            if (comma == std::wstring::npos) {
                break;
            }
            start = comma + 1;
        }
    }

    // ── Game overlay options (#25) ───────────────────────────────────────────
    next.gameOverlayShowFps = host::int_setting(L"Modules.GameOverlayShowFps") != 0;
    next.gameOverlayShowCpu = host::int_setting(L"Modules.GameOverlayShowCpu") != 0;
    next.gameOverlayShowGpu = host::int_setting(L"Modules.GameOverlayShowGpu") != 0;
    next.gameOverlayShowRam = host::int_setting(L"Modules.GameOverlayShowRam") != 0;
    next.gameOverlayShowDisk = host::int_setting(L"Modules.GameOverlayShowDisk") != 0;
    next.gameOverlayCompact = host::int_setting(L"Modules.GameOverlayCompact") != 0;
    next.tintOpacity = Clamp(host::int_setting(L"Themes.TintIntensity") / 100.0f, 0.0f, 1.0f);
    const int settingOpacity = host::int_setting(L"Themes.PillOpacity");
    const int localOpacity = host::int_value(L"PillOpacityOverride", -1);
    g_pillOpacityOverride.store(localOpacity);
    g_gameOverlayPinned.store(host::int_value(L"GameOverlayPinned", 0) != 0);
    g_pinnedExpanded.store(host::int_value(L"PinnedExpanded", 0) != 0);
    next.pillOpacity = Clamp((localOpacity >= 0 ? localOpacity : settingOpacity) / 100.0f,
                             0.35f, 1.0f);
    next.gameOverlay = host::int_setting(L"Modules.GameOverlay") != 0;
    next.showMetricText = host::int_setting(L"Modules.ShowMetricText") != 0;
    next.weather = host::int_setting(L"Modules.Weather") != 0;
    next.weatherCity = GetStringSettingWithFallback(L"Modules.WeatherCity", L"Weather.WeatherCity", L"CalendarWeather.WeatherCity");
    next.weatherFahrenheit = host::int_setting(L"Modules.WeatherFahrenheit") != 0;
    next.lyricsVisualizer = host::int_setting(L"Modules.LyricsVisualizer") != 0;
    next.karaokeLyrics = host::int_setting(L"Modules.KaraokeLyrics") != 0;
    next.karaokeLetterGlow = host::int_setting(L"Modules.KaraokeLetterGlow") != 0;
    next.lyricsNeighborLinesVisible = host::int_setting(L"Modules.LyricsNeighborLinesVisible") != 0;
    next.lyricsOffsetMs = ClampInt(host::int_setting(L"Modules.LyricsOffsetMs"),
                                   -kLyricsOffsetLimitMs, kLyricsOffsetLimitMs);
    next.lyricsOffsetBtMs = ClampInt(host::int_setting(L"Modules.LyricsOffsetBluetoothMs"),
                                     -kLyricsOffsetLimitMs, kLyricsOffsetLimitMs);
    PublishLyricsOffsets(next.lyricsOffsetMs, next.lyricsOffsetBtMs);
    next.lyricsCache = host::int_setting(L"Modules.LyricsCache") != 0;
    {
        const std::wstring capStr = GetStringSettingCopy(L"Modules.LyricsCacheMaxMB");
        int capMb = 25;  // default, also used if the value is unparsable
        if (capStr == L"0") {
            capMb = 0;
        } else if (!capStr.empty()) {
            const int parsed = _wtoi(capStr.c_str());
            if (parsed > 0) capMb = ClampInt(parsed, 1, 1024);
        }
        next.lyricsCacheMaxMB = capMb;
    }
    const std::wstring hideSec = GetStringSettingWithFallback(L"Behavior.AutoHideIdleSeconds", L"Appearance.AutoHideIdleSeconds");
    next.autoHideIdleSeconds = hideSec.empty() ? 0 : _wtoi(hideSec.c_str());
    next.unhideOnHover = host::int_setting(L"Behavior.UnhideOnHover") != 0;
    next.alwaysOnTop = host::int_setting(L"Behavior.AlwaysOnTop") != 0;
    const int localExpandOnHover = host::int_value(L"ExpandOnHoverOverride", -1);
    next.expandOnHover = localExpandOnHover >= 0 ? (localExpandOnHover != 0) : (host::int_setting(L"Behavior.ExpandOnHover") != 0);
    // Right-click toggle; stored with Wh_SetIntValue so it survives restarts.
    next.collapsedLyrics = host::int_value(L"CollapsedLyrics", 0) != 0;
    {
        // Right-click choice wins until the Mod Settings value is changed.
        const int mediaTabLyricsSetting = host::int_setting(L"Modules.MediaTabLyrics") != 0 ? 1 : 0;
        int mediaTabLyricsOverride = host::int_value(L"MediaTabLyricsOverride", -1);
        if (mediaTabLyricsOverride >= 0 &&
            host::int_value(L"MediaTabLyricsOverrideBase", -1) != mediaTabLyricsSetting) {
            host::set_int_value(L"MediaTabLyricsOverride", -1);
            mediaTabLyricsOverride = -1;
        }
        next.mediaTabLyrics = mediaTabLyricsOverride >= 0 ? (mediaTabLyricsOverride != 0)
                                                          : (mediaTabLyricsSetting != 0);
    }
    next.autoDpiScale = host::int_setting(L"Appearance.AutoDpiScale") != 0;
    next.offsetX = host::int_setting(L"Appearance.OffsetX");
    next.offsetY = host::int_setting(L"Appearance.OffsetY");
    next.separateExpandedOffsetY = host::int_setting(L"Appearance.SeparateExpandedOffsetY") != 0;
    next.offsetYExpanded = host::int_setting(L"Appearance.OffsetYExpanded");


    std::wstring shapeStr = GetStringSettingCopy(L"Appearance.ShapeStyle");
    static std::wstring s_lastConfiguredShape = L"";
    if (!shapeStr.empty() && shapeStr != s_lastConfiguredShape) {
        s_lastConfiguredShape = shapeStr;
        host::set_int_value(L"W11StyleOverride", -1);
        host::set_int_value(L"NotchStyleOverride", -1);
    }
    bool baseW11 = EqualsNoCase(shapeStr, L"w11");
    bool baseNotch = EqualsNoCase(shapeStr, L"notch");

    const int localW11Style = host::int_value(L"W11StyleOverride", -1);
    next.w11Style = localW11Style >= 0 ? (localW11Style != 0) : baseW11;

    const int localNotchStyle = host::int_value(L"NotchStyleOverride", -1);
    next.notchStyle = localNotchStyle >= 0 ? (localNotchStyle != 0) : baseNotch;

    // A bottom-anchored island cannot be a top-edge macOS notch.
    if (IsBottomPosition(next.position)) {
        next.notchStyle = false;
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
    auto resolveColor = [](const wchar_t* settingKey,
                           std::initializer_list<const wchar_t*> defaultHexes,
                           D2D1_COLOR_F presetColor) -> D2D1_COLOR_F {
        const std::wstring value = GetStringSettingCopy(settingKey);
        if (value.empty()) {
            return presetColor;
        }

        // Compare the parsed colors rather than the strings, so equivalent
        // spellings of the default ("0D0D0F" without the '#', "#0D0D0FFF",
        // different case) are all still recognised as "untouched" and leave the
        // preset in charge.
        const D2D1_COLOR_F sentinel = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
        const D2D1_COLOR_F parsed = ColorFromHex(value, sentinel);
        if (parsed.r == sentinel.r && parsed.g == sentinel.g &&
            parsed.b == sentinel.b && parsed.a == sentinel.a) {
            // Unparseable, so treat it as not set.
            return presetColor;
        }

        for (const wchar_t* defaultHex : defaultHexes) {
            const D2D1_COLOR_F defaults = ColorFromHex(defaultHex, sentinel);
            if (parsed.r == defaults.r && parsed.g == defaults.g &&
                parsed.b == defaults.b && parsed.a == defaults.a) {
                return presetColor;
            }
        }
        return parsed;
    };

    auto applyPreset = [&](Settings& target, int idx) {
        const ThemePalette& p = kThemePalettes[idx];
        target.pillBgColor = resolveColor(
            L"Themes.PillBgColor", {kThemePalettes[0].bg, L"#0D0D0F"},
            ColorFromHex(p.bg, D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f)));
        target.textPrimaryColor = resolveColor(
            L"Themes.TextPrimaryColor", {L"#FFFFFF", L"#F7F7F7"},
            ColorFromHex(p.fg, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)));
        target.textSecondaryColor = resolveColor(
            L"Themes.TextSecondaryColor", {kThemePalettes[0].sec, L"#B0B0B8", L"#888888"},
            ColorFromHex(p.sec, D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f)));
        target.contourBorderColor = resolveColor(
            L"Themes.ContourBorderHex", {kThemePalettes[0].border, L"#333338"},
            ColorFromHex(p.border, D2D1::ColorF(0.118f, 0.118f, 0.133f, 1.0f)));
    };

    std::wstring themePresetStr = GetStringSettingCopy(L"Themes.ThemePreset");
    // The active palette is persisted as an integer so the right-click menu can
    // change it without rewriting the settings file. Migrate the old key across
    // once, since the integers were renumbered when the palettes changed.
    if (host::int_value(kThemeValueName, -1) < 0) {
        const int legacy = host::int_value(kLegacyThemeValueName, -1);
        if (legacy >= 0) {
            int migrated;
            switch (legacy) {
                case 0:  migrated = 0; break;                  // OLED Black    -> Obsidian
                case 1:  migrated = 1; break;                  // Fluent        -> Graphite
                case 2:  migrated = 2; break;                  // Midnight Blue -> Slate
                case 3:  migrated = 6; break;                  // Deep Purple   -> Plum
                case 4:  migrated = 1; break;                  // Fluent Design -> Graphite
                default: migrated = kCustomThemeIndex; break;   // out of range meant Custom
            }
            host::set_int_value(kThemeValueName, migrated);
        }
    }

    static std::wstring s_lastConfiguredPreset = L"";
    if (!themePresetStr.empty() && themePresetStr != s_lastConfiguredPreset) {
        s_lastConfiguredPreset = themePresetStr;
        const int fromSettings = ThemeIndexFromId(themePresetStr);
        if (fromSettings >= 0) {
            host::set_int_value(kThemeValueName, fromSettings);
        }
    }

    const int theme = host::int_value(kThemeValueName, -1);
    if (theme >= 0 && theme < kCustomThemeIndex) {
        next.themePreset = static_cast<ThemePreset>(theme);
        applyPreset(next, theme);
    } else if (theme >= kCustomThemeIndex || EqualsNoCase(themePresetStr, L"custom")) {
        // Explicit Custom: the hex fields are authoritative, defaults included.
        next.themePreset = ThemePreset::Custom;
        next.pillBgColor = ColorFromHex(GetStringSettingCopy(L"Themes.PillBgColor"),
                                        D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f));
        next.textPrimaryColor = ColorFromHex(GetStringSettingCopy(L"Themes.TextPrimaryColor"),
                                             D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        next.textSecondaryColor = ColorFromHex(GetStringSettingCopy(L"Themes.TextSecondaryColor"),
                                               D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f));
        next.contourBorderColor = ColorFromHex(GetStringSettingCopy(L"Themes.ContourBorderHex"),
                                               D2D1::ColorF(0.118f, 0.118f, 0.133f, 1.0f));
    } else {
        const int fromSettings = ThemeIndexFromId(themePresetStr);
        const int presetIdx = (fromSettings >= 0 && fromSettings < kCustomThemeIndex) ? fromSettings : 0;
        next.themePreset = static_cast<ThemePreset>(presetIdx);
        applyPreset(next, presetIdx);
    }

    // Graphite inherits the old Fluent theme's auto-translucency: it is the
    // neutral Windows 11 grey, and it reads best when the desktop shows through
    // a little. The rest of the palettes are opaque unless the user says so.
    if (next.themePreset == ThemePreset::Graphite && localOpacity < 0) {
        next.pillOpacity = 0.88f;
    }

    next.borderMergedMode = host::int_setting(L"Appearance.BorderMergedMode") != 0;
    next.autoHideFullscreen = host::int_setting(L"Behavior.AutoHideFullscreen") != 0;
    next.hardwareMonitorModule = host::int_setting(L"Modules.HardwareMonitorModule") != 0;
    next.githubModule = host::int_setting(L"Modules.GitHubModule") != 0;
    const std::wstring borderModeStr = GetStringSettingCopy(L"Themes.ContourBorderMode");
    if (EqualsNoCase(borderModeStr, L"borderless")) {
        next.contourBorderMode = ContourBorderMode::Borderless;
        next.contourBorderEnabled = false;
    } else if (EqualsNoCase(borderModeStr, L"auto")) {
        next.contourBorderMode = ContourBorderMode::Auto;
        next.contourBorderEnabled = true;
    } else if (EqualsNoCase(borderModeStr, L"default")) {
        next.contourBorderMode = ContourBorderMode::Default;
        next.contourBorderEnabled = true;
    } else {
        next.contourBorderEnabled = true;  // no such setting; the mode dropdown drives this
        next.contourBorderMode = next.contourBorderEnabled ? ContourBorderMode::Default : ContourBorderMode::Borderless;
    }
    next.clockAccentGlow = host::int_setting(L"Themes.ClockAccentGlow") != 0;
    bool settingsChangedWhileHidden = (g_autoHiddenParked.load() || g_manuallyHidden.load());
    bool unhideRequested = (next.autoHideIdleSeconds == 0) ||
                           (next.unhideOnHover && !g_settings.unhideOnHover);
    if (unhideRequested || (settingsChangedWhileHidden && next.autoHideIdleSeconds == 0)) {
        g_autoHiddenParked = false;
        g_manuallyHidden = false;
        host::set_int_value(L"ManuallyHidden", 0);
        g_hotkeyUnhideUntil.store(NowSeconds() + (next.autoHideIdleSeconds > 0 ? next.autoHideIdleSeconds : 6.0));
        if (g_hwnd) {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            PostMessageW(g_hwnd, WM_APP_NEW_EVENT, 0, 0);
        }
    } else if (settingsChangedWhileHidden && next.unhideOnHover) {
        g_autoHiddenParked = false;
        g_manuallyHidden = false;
        host::set_int_value(L"ManuallyHidden", 0);
        g_hotkeyUnhideUntil.store(NowSeconds() + (next.autoHideIdleSeconds > 0 ? next.autoHideIdleSeconds : 6.0));
        if (g_hwnd) {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            PostMessageW(g_hwnd, WM_APP_NEW_EVENT, 0, 0);
        }
    }

    // Privacy Indicators: unified reading with complete fallback to legacy keys
    next.privacyDots = host::int_setting(L"Indicators.PrivacyDots") != 0;
    next.privacyDotsMic = host::int_setting(L"Indicators.PrivacyDotsMic") != 0;
    next.privacyDotsCam = host::int_setting(L"Indicators.PrivacyDotsCam") != 0;
    next.privacyDotsPulse = true;  // no such setting; pulsing is always on

    std::wstring micHexStr = GetStringSettingWithFallback(L"Indicators.PrivacyDotsMicHex", L"Indicators.MicDotHex", L"Modules.PrivacyDotsMicHex");
    next.privacyDotsMicHex = ColorFromHex(micHexStr, D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f));

    std::wstring camHexStr = GetStringSettingWithFallback(L"Indicators.PrivacyDotsCamHex", L"Indicators.CamDotHex", L"Modules.PrivacyDotsCamHex");
    next.privacyDotsCamHex = ColorFromHex(camHexStr, D2D1::ColorF(0.204f, 0.780f, 0.349f, 1.0f));

    // Compatibility aliases: always keep in sync
    next.privacyDotsEnabled = next.privacyDots;
    next.privacyDotPulsing = next.privacyDotsPulse;
    next.micDotColor = next.privacyDotsMicHex;
    next.camDotColor = next.privacyDotsCamHex;

    g_pinnedExpanded.store(false);
    host::set_int_value(L"PinnedExpanded", 0);

    // Compare-then-publish under one lock so the change flags describe exactly
    // the transition we are about to commit. Two LoadSettings() calls can run
    // concurrently (settings thread vs. tray menu on the render thread); without
    // this, both could read the same "old" values and each decide a hotkey
    // re-registration was needed, or neither would.
    bool cityChanged = false;
    bool hotkeySettingChanged = false;
    bool backdropChanged = false;
    bool capsLockChanged = false;
    {
        std::lock_guard lock(g_settingsMutex);
        cityChanged = next.weatherCity != g_settings.weatherCity;
        capsLockChanged = next.capsLock != g_settings.capsLock;
        hotkeySettingChanged =
            next.hideShowHotkeyEnabled != g_settings.hideShowHotkeyEnabled ||
            next.hideShowModifiers != g_settings.hideShowModifiers ||
            next.hideShowVk != g_settings.hideShowVk ||
            next.quickLookup != g_settings.quickLookup ||
            next.lookupModifiers != g_settings.lookupModifiers ||
            next.lookupVk != g_settings.lookupVk;
        backdropChanged =
            next.backdropMaterial != g_settings.backdropMaterial ||
            std::fabs(next.backdropTint - g_settings.backdropTint) > 0.001f ||
            std::fabs(next.backdropFillAlpha - g_settings.backdropFillAlpha) > 0.001f ||
            next.alwaysOnTop != g_settings.alwaysOnTop;
        g_settings = std::move(next);
    }
    g_layoutDirty = true;
    if (cityChanged && g_settingsChangedEvent) {
        SetEvent(g_settingsChangedEvent);
    }
    // Installs or removes WH_KEYBOARD_LL to match. The keyboard thread would pick
    // this up on its next backstop tick anyway; this just makes it immediate.
    if (capsLockChanged) {
        NotifyKeyboardThreadSettingChanged();
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
    if (hotkeySettingChanged) {
        if (g_hwnd) {
            PostMessageW(g_hwnd, WM_APP_APPLY_HOTKEY, 0, 0);
        }
    }
    // Same story for the backdrop: no-ops before the window exists, and applies
    // live afterwards so switching blur/acrylic needs no mod restart.
    if (backdropChanged && g_hwnd) {
        PostMessageW(g_hwnd, WM_APP_APPLY_BACKDROP, 0, 0);
    }
}


}
