#include "host/defaults.h"

#include <algorithm>
#include <array>

namespace host
{

namespace
{

constexpr std::array<SettingDefault, 105> setting_table = {
    SettingDefault{L"Appearance.Position", SettingKind::Text, 0, L"top-center"},
    SettingDefault{L"Appearance.TargetMonitor", SettingKind::Text, 0, L"primary"},
    SettingDefault{L"Appearance.OffsetX", SettingKind::Number, 0, L""},
    SettingDefault{L"Appearance.OffsetY", SettingKind::Number, 0, L""},
    SettingDefault{L"Appearance.SeparateExpandedOffsetY", SettingKind::Number, 0, L""},
    SettingDefault{L"Appearance.OffsetYExpanded", SettingKind::Number, 0, L""},
    SettingDefault{L"Appearance.BorderMergedMode", SettingKind::Number, 0, L""},
    SettingDefault{L"Appearance.ShapeStyle", SettingKind::Text, 0, L"default"},
    SettingDefault{L"Appearance.ProgressStyle", SettingKind::Text, 0, L"slim"},
    SettingDefault{L"Appearance.SpectrumStyle", SettingKind::Text, 0, L"bars"},
    SettingDefault{L"Appearance.SizeScale", SettingKind::Text, 0, L"1.0"},
    SettingDefault{L"Appearance.AutoDpiScale", SettingKind::Number, 1, L""},
    SettingDefault{L"Behavior.AlwaysOnTop", SettingKind::Number, 1, L""},
    SettingDefault{L"Behavior.ExpandOnHover", SettingKind::Number, 1, L""},
    SettingDefault{L"Behavior.AutoHideIdleSeconds", SettingKind::Text, 0, L"0"},
    SettingDefault{L"Behavior.AutoHideFullscreen", SettingKind::Number, 1, L""},
    SettingDefault{L"Behavior.UnhideOnHover", SettingKind::Number, 1, L""},
    SettingDefault{L"Animations.TargetFPS", SettingKind::Text, 0, L"auto"},
    SettingDefault{L"Animations.AnimationStyle", SettingKind::Text, 0, L"default"},
    SettingDefault{L"Animations.AnimationSpeed", SettingKind::Text, 0, L"normal"},
    SettingDefault{L"Animations.ExpandedMediaTransitions", SettingKind::Number, 0, L""},
    SettingDefault{L"Animations.PillCoverSpin", SettingKind::Number, 0, L""},
    SettingDefault{L"Themes.ThemePreset", SettingKind::Text, 0, L"obsidian"},
    SettingDefault{L"Themes.PillOpacity", SettingKind::Number, 96, L""},
    SettingDefault{L"Themes.TintIntensity", SettingKind::Number, 72, L""},
    SettingDefault{L"Themes.BackdropMaterial", SettingKind::Text, 0, L"none"},
    SettingDefault{L"Themes.BackdropFillOpacity", SettingKind::Number, 45, L""},
    SettingDefault{L"Themes.BackdropTint", SettingKind::Number, 55, L""},
    SettingDefault{L"Themes.MaterialDepth", SettingKind::Number, 1, L""},
    SettingDefault{L"Themes.DropShadow", SettingKind::Number, 1, L""},
    SettingDefault{L"Themes.AccentBloom", SettingKind::Number, 100, L""},
    SettingDefault{L"Themes.TextScale", SettingKind::Number, 100, L""},
    SettingDefault{L"Themes.AccentColorMode", SettingKind::Text, 0, L"auto"},
    SettingDefault{L"Themes.CustomAccentHex", SettingKind::Text, 0, L"#4cc9f0"},
    SettingDefault{L"Themes.CalendarAccent", SettingKind::Text, 0, L"red"},
    SettingDefault{L"Themes.ClockAccentGlow", SettingKind::Number, 1, L""},
    SettingDefault{L"Themes.FontFamily", SettingKind::Text, 0, L""},
    SettingDefault{L"Themes.ContourBorderMode", SettingKind::Text, 0, L"default"},
    SettingDefault{L"Themes.ContourBorderHex", SettingKind::Text, 0, L"#1E1E22"},
    SettingDefault{L"Themes.PillBgColor", SettingKind::Text, 0, L"#08080A"},
    SettingDefault{L"Themes.TextPrimaryColor", SettingKind::Text, 0, L"#FFFFFF"},
    SettingDefault{L"Themes.TextSecondaryColor", SettingKind::Text, 0, L"#9B9BA5"},
    SettingDefault{L"Indicators.PrivacyDots", SettingKind::Number, 1, L""},
    SettingDefault{L"Indicators.PrivacyDotsMic", SettingKind::Number, 1, L""},
    SettingDefault{L"Indicators.PrivacyDotsMicHex", SettingKind::Text, 0, L"#FF9500"},
    SettingDefault{L"Indicators.PrivacyDotsCam", SettingKind::Number, 1, L""},
    SettingDefault{L"Indicators.PrivacyDotsCamHex", SettingKind::Text, 0, L"#34C759"},
    SettingDefault{L"Modules.Media", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.MediaAutoExpand", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.MediaPillClock", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.Volume", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.Brightness", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.CapsLock", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.Battery", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.BluetoothIndicator", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.BluetoothShowBattery", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.BluetoothAllowList", SettingKind::Text, 0, L"FreeBuds;DualSense"},
    SettingDefault{L"Modules.Progress", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.TimerModule", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.Clipboard", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.StatusCountdownProgress", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.DoNotDisturbIndicator", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.NotificationRespectDnD", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.WindowsToastListener", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.NotificationCenter", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GitHubModule", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GitHubUser", SettingKind::Text, 0, L"1Filipek2"},
    SettingDefault{L"Modules.HardwareMonitorModule", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GameOverlay", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.ShowMetricText", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.Weather", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.WeatherCities", SettingKind::Text, 0, L"London|51.50735|-0.12776;New York|40.71278|-74.00597"},
    SettingDefault{L"Modules.WeatherFahrenheit", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.MediaTabLyrics", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.Lyrics", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.LyricsVisualizer", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.KaraokeLyrics", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.KaraokeLetterGlow", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.LyricsOffsetMs", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.LyricsOffsetBluetoothMs", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.LyricsNeighborLinesVisible", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.LyricsCache", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.LyricsCacheMaxMB", SettingKind::Text, 0, L"25"},
    SettingDefault{L"Modules.Language", SettingKind::Text, 0, L"auto"},
    SettingDefault{L"Modules.ClockFormat", SettingKind::Text, 0, L"system"},
    SettingDefault{L"Modules.ShowSeconds", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.DateFormat", SettingKind::Text, 0, L""},
    SettingDefault{L"Modules.DateFirst", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.FirstDayOfWeek", SettingKind::Text, 0, L"auto"},
    SettingDefault{L"Modules.QuickLookup", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.FileTrayModule", SettingKind::Number, 0, L""},
    SettingDefault{L"Modules.FileTrayMaxItems", SettingKind::Number, 10, L""},
    SettingDefault{L"Modules.FileTrayPersist", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.MediaExpandBlocklist", SettingKind::Text, 0, L""},
    SettingDefault{L"Modules.GameOverlayShowFps", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GameOverlayShowCpu", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GameOverlayShowGpu", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GameOverlayShowRam", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GameOverlayShowDisk", SettingKind::Number, 1, L""},
    SettingDefault{L"Modules.GameOverlayCompact", SettingKind::Number, 0, L""},
    SettingDefault{L"Shortcuts.HideShowHotkeyEnabled", SettingKind::Number, 1, L""},
    SettingDefault{L"Shortcuts.HideShowModifiers", SettingKind::Text, 0, L"ctrl_alt"},
    SettingDefault{L"Shortcuts.HideShowKey", SettingKind::Text, 0, L"D"},
    SettingDefault{L"Shortcuts.LookupModifiers", SettingKind::Text, 0, L"ctrl_alt"},
    SettingDefault{L"Shortcuts.LookupKey", SettingKind::Text, 0, L"Space"},
};

}

std::span<const SettingDefault> setting_defaults()
{
    return setting_table;
}

const SettingDefault* find_setting_default(std::wstring_view name)
{
    const auto found = std::find_if(setting_table.begin(), setting_table.end(), [&](const SettingDefault& entry)
    {
        return entry.name == name;
    });
    return found == setting_table.end() ? nullptr : &*found;
}

}
