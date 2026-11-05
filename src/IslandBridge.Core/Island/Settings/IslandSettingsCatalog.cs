using System.Text.RegularExpressions;
using static IslandBridge.Core.Island.Settings.IslandSettingGroup;

namespace IslandBridge.Core.Island.Settings;

// Mirrors island/src/host/defaults.cpp (the source of truth for defaults); a test keeps both in sync.
public static partial class IslandSettingsCatalog
{
    public const string LoggingName = "Diagnostics.Logging";

    private static readonly string[] Modifiers = ["ctrl_alt", "ctrl_shift", "alt_shift", "win_alt", "ctrl_alt_shift"];

    public static IReadOnlyList<IslandSetting> All { get; } = Build();

    public static IReadOnlyList<string> Retired { get; } = ["Modules.WindowsToastListener"];

    public static IslandSetting? Find(string name) => All.FirstOrDefault(setting => setting.Name == name);

    public static IEnumerable<IslandSetting> InGroup(IslandSettingGroup group) => All.Where(setting => setting.Group == group);

    private static IslandChoice[] Plain(params string[] values) => [.. values.Select(value => new IslandChoice(value))];

    private static IslandChoice[] Formatted(string labelKey, params string[] values) =>
        [.. values.Select(value => new IslandChoice(value, labelKey, Argument: value))];

    private static IslandSettingCondition On(string name) => new(name, value => value != "0");

    private static IslandSettingCondition Is(string name, string expected) =>
        new(name, value => string.Equals(value, expected, StringComparison.OrdinalIgnoreCase));

    private static IslandSettingCondition IsNot(string name, string excluded) =>
        new(name, value => !string.Equals(value, excluded, StringComparison.OrdinalIgnoreCase));

    private static string? GitHubUser(string value) => value.Length == 0 || GitHubUserPattern().IsMatch(value) ? value : null;

    [GeneratedRegex("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})$", RegexOptions.CultureInvariant)]
    private static partial Regex GitHubUserPattern();

    private static IslandSetting[] Build()
    {
        IslandChoice[] monitors =
        [
            new("primary"),
            .. Enumerable.Range(1, 5).Select(index => new IslandChoice(index.ToString(System.Globalization.CultureInfo.InvariantCulture), "IslandFormat_Display", Argument: index)),
            new("follow"),
        ];

        IslandChoice[] languages =
        [
            new("auto"),
            new("en", Literal: "English"),
            new("fr", Literal: "Français"),
            new("es", Literal: "Español"),
            new("de", Literal: "Deutsch"),
            new("pt", Literal: "Português"),
            new("it", Literal: "Italiano"),
            new("ru", Literal: "Русский"),
            new("tr", Literal: "Türkçe"),
            new("hi", Literal: "हिन्दी"),
            new("zh", Literal: "简体中文"),
            new("ja", Literal: "日本語"),
            new("ko", Literal: "한국어"),
        ];

        var modifiers = Plain(Modifiers);

        return
        [
            IslandSetting.Choice("Appearance.Position", Appearance, "top-center", Plain("top-center", "top-left", "top-right", "bottom-center", "bottom-left", "bottom-right")),
            IslandSetting.Choice("Appearance.TargetMonitor", Appearance, "primary", monitors),
            IslandSetting.Choice("Appearance.ShapeStyle", Appearance, "default", Plain("default", "w11", "notch")),
            IslandSetting.Choice("Appearance.SizeScale", Appearance, "1.0", Formatted("IslandFormat_Scale", "0.8", "1.0", "1.2", "1.5", "1.8", "2.0", "2.5")),
            IslandSetting.Toggle("Appearance.AutoDpiScale", Appearance, true),
            IslandSetting.Toggle("Appearance.BorderMergedMode", Appearance, false),
            IslandSetting.Number("Appearance.OffsetX", Appearance, 0, -2000, 2000, NumberStyle.Box, unit: "px"),
            IslandSetting.Number("Appearance.OffsetY", Appearance, 0, -2000, 2000, NumberStyle.Box, unit: "px"),
            IslandSetting.Toggle("Appearance.SeparateExpandedOffsetY", Appearance, false),
            IslandSetting.Number("Appearance.OffsetYExpanded", Appearance, 0, -2000, 2000, NumberStyle.Box, unit: "px", enabledWhen: On("Appearance.SeparateExpandedOffsetY")),

            IslandSetting.Toggle("Behavior.AlwaysOnTop", Behavior, true),
            IslandSetting.Toggle("Behavior.ExpandOnHover", Behavior, true),
            IslandSetting.Choice("Behavior.AutoHideIdleSeconds", Behavior, "0", Plain("0", "-1", "5", "10", "30", "60")),
            IslandSetting.Toggle("Behavior.AutoHideFullscreen", Behavior, true),
            IslandSetting.Toggle("Behavior.UnhideOnHover", Behavior, true),

            IslandSetting.Choice("Animations.TargetFPS", Animations, "auto", [new("auto"), .. Formatted("IslandFormat_Fps", "60", "90", "120", "144", "165", "240", "360", "500")]),
            IslandSetting.Choice("Animations.AnimationStyle", Animations, "default", Plain("default", "smooth", "bouncy", "snappy")),
            IslandSetting.Choice("Animations.AnimationSpeed", Animations, "normal", Plain("very-slow", "slow", "normal", "fast", "very-fast", "ultra-fast")),
            IslandSetting.Toggle("Animations.ExpandedMediaTransitions", Animations, false),
            IslandSetting.Toggle("Animations.PillCoverSpin", Animations, false),

            IslandSetting.Choice("Themes.ThemePreset", Theme, "obsidian", Plain("obsidian", "graphite", "slate", "nord", "evergreen", "espresso", "plum", "porcelain", "custom")),
            IslandSetting.Color("Themes.PillBgColor", Theme, "#08080A"),
            IslandSetting.Color("Themes.TextPrimaryColor", Theme, "#FFFFFF"),
            IslandSetting.Color("Themes.TextSecondaryColor", Theme, "#9B9BA5"),
            IslandSetting.Choice("Themes.ContourBorderMode", Theme, "default", Plain("default", "auto", "borderless")),
            IslandSetting.Color("Themes.ContourBorderHex", Theme, "#1E1E22", Is("Themes.ContourBorderMode", "default")),
            IslandSetting.Choice("Themes.AccentColorMode", Theme, "auto", Plain("auto", "system", "custom")),
            IslandSetting.Color("Themes.CustomAccentHex", Theme, "#4cc9f0", Is("Themes.AccentColorMode", "custom")),
            IslandSetting.Choice("Themes.CalendarAccent", Theme, "red", Plain("red", "system")),
            IslandSetting.Toggle("Themes.ClockAccentGlow", Theme, true),
            IslandSetting.Number("Themes.PillOpacity", Theme, 96, 35, 100, NumberStyle.Slider, unit: "%"),
            IslandSetting.Number("Themes.TintIntensity", Theme, 72, 0, 100, NumberStyle.Slider, unit: "%"),
            IslandSetting.Choice("Themes.BackdropMaterial", Theme, "none", Plain("none", "blur", "acrylic")),
            IslandSetting.Number("Themes.BackdropFillOpacity", Theme, 45, 0, 100, NumberStyle.Slider, unit: "%", enabledWhen: IsNot("Themes.BackdropMaterial", "none")),
            IslandSetting.Number("Themes.BackdropTint", Theme, 55, 0, 100, NumberStyle.Slider, unit: "%", enabledWhen: IsNot("Themes.BackdropMaterial", "none")),
            IslandSetting.Toggle("Themes.MaterialDepth", Theme, true),
            IslandSetting.Toggle("Themes.DropShadow", Theme, true),
            IslandSetting.Number("Themes.AccentBloom", Theme, 100, 0, 200, NumberStyle.Slider, unit: "%"),
            IslandSetting.Number("Themes.TextScale", Theme, 100, 70, 160, NumberStyle.Slider, unit: "%"),
            IslandSetting.Text("Themes.FontFamily", Theme, string.Empty, 64, value => value.Trim('"', '\'')),

            IslandSetting.Toggle("Modules.Media", Media, true),
            IslandSetting.Toggle("Modules.MediaAutoExpand", Media, false, On("Modules.Media")),
            IslandSetting.Text("Modules.MediaExpandBlocklist", Media, string.Empty, 512, enabledWhen: On("Modules.Media")),
            IslandSetting.Toggle("Modules.MediaPillClock", Media, false, On("Modules.Media")),
            IslandSetting.Choice("Appearance.ProgressStyle", Media, "slim", Plain("slim", "wavy", "squiggle", "bar"), On("Modules.Media")),
            IslandSetting.Choice("Appearance.SpectrumStyle", Media, "bars", Plain("bars", "led", "plasma", "orb"), On("Modules.Media")),

            IslandSetting.Toggle("Modules.MediaTabLyrics", Lyrics, false),
            IslandSetting.Toggle("Modules.LyricsVisualizer", Lyrics, true),
            IslandSetting.Toggle("Modules.KaraokeLyrics", Lyrics, true),
            IslandSetting.Toggle("Modules.KaraokeLetterGlow", Lyrics, true, On("Modules.KaraokeLyrics")),
            IslandSetting.Toggle("Modules.LyricsNeighborLinesVisible", Lyrics, false),
            IslandSetting.Number("Modules.LyricsOffsetMs", Lyrics, 0, -3000, 3000, NumberStyle.Box, 10, "ms"),
            IslandSetting.Number("Modules.LyricsOffsetBluetoothMs", Lyrics, 0, -3000, 3000, NumberStyle.Box, 10, "ms"),
            IslandSetting.Toggle("Modules.LyricsCache", Lyrics, true),
            IslandSetting.Choice("Modules.LyricsCacheMaxMB", Lyrics, "25", [new("0"), .. Formatted("IslandFormat_Megabytes", "10", "25", "100")], On("Modules.LyricsCache")),

            IslandSetting.Choice("Modules.Language", ClockWeather, "auto", languages),
            IslandSetting.Choice("Modules.ClockFormat", ClockWeather, "system", Plain("system", "12h", "24h")),
            IslandSetting.Toggle("Modules.ShowSeconds", ClockWeather, false),
            IslandSetting.Text("Modules.DateFormat", ClockWeather, string.Empty, 32),
            IslandSetting.Toggle("Modules.DateFirst", ClockWeather, false),
            IslandSetting.Choice("Modules.FirstDayOfWeek", ClockWeather, "auto", Plain("auto", "monday", "sunday")),
            IslandSetting.Toggle("Modules.Weather", ClockWeather, true),
            IslandSetting.Toggle("Modules.WeatherFahrenheit", ClockWeather, false, On("Modules.Weather")),
            IslandSetting.Cities("Modules.WeatherCities", ClockWeather, "London|51.50735|-0.12776;New York|40.71278|-74.00597", On("Modules.Weather")),

            IslandSetting.Toggle("Modules.NotificationCenter", Alerts, true),
            IslandSetting.Toggle("Modules.NotificationRespectDnD", Alerts, true),
            IslandSetting.Toggle("Modules.DoNotDisturbIndicator", Alerts, true),
            IslandSetting.Toggle("Modules.StatusCountdownProgress", Alerts, false),
            IslandSetting.Toggle("Modules.Volume", Alerts, true),
            IslandSetting.Toggle("Modules.Brightness", Alerts, true),
            IslandSetting.Toggle("Modules.CapsLock", Alerts, true),
            IslandSetting.Toggle("Modules.Battery", Alerts, true),
            IslandSetting.Toggle("Modules.Progress", Alerts, true),
            IslandSetting.Toggle("Modules.Clipboard", Alerts, true),
            IslandSetting.Toggle("Modules.BluetoothIndicator", Alerts, true),
            IslandSetting.Toggle("Modules.BluetoothShowBattery", Alerts, true, On("Modules.BluetoothIndicator")),
            IslandSetting.Text("Modules.BluetoothAllowList", Alerts, "FreeBuds;DualSense", 512, enabledWhen: On("Modules.BluetoothIndicator")),

            IslandSetting.Toggle("Indicators.PrivacyDots", Privacy, true),
            IslandSetting.Toggle("Indicators.PrivacyDotsMic", Privacy, true, On("Indicators.PrivacyDots")),
            IslandSetting.Color("Indicators.PrivacyDotsMicHex", Privacy, "#FF9500", On("Indicators.PrivacyDotsMic")),
            IslandSetting.Toggle("Indicators.PrivacyDotsCam", Privacy, true, On("Indicators.PrivacyDots")),
            IslandSetting.Color("Indicators.PrivacyDotsCamHex", Privacy, "#34C759", On("Indicators.PrivacyDotsCam")),

            IslandSetting.Toggle("Modules.GitHubModule", Tabs, true),
            IslandSetting.Text("Modules.GitHubUser", Tabs, "1Filipek2", 39, GitHubUser, On("Modules.GitHubModule")),
            IslandSetting.Toggle("Modules.HardwareMonitorModule", Tabs, true),
            IslandSetting.Toggle("Modules.TimerModule", Tabs, true),
            IslandSetting.Toggle("Modules.FileTrayModule", Tabs, false),
            IslandSetting.Number("Modules.FileTrayMaxItems", Tabs, 10, 1, 25, NumberStyle.Box, enabledWhen: On("Modules.FileTrayModule")),
            IslandSetting.Toggle("Modules.FileTrayPersist", Tabs, true, On("Modules.FileTrayModule")),
            IslandSetting.Toggle("Modules.QuickLookup", Tabs, false),

            IslandSetting.Toggle("Modules.GameOverlay", GameOverlay, false),
            IslandSetting.Toggle("Modules.GameOverlayCompact", GameOverlay, false, On("Modules.GameOverlay")),
            IslandSetting.Toggle("Modules.ShowMetricText", GameOverlay, false, On("Modules.GameOverlay")),
            IslandSetting.Toggle("Modules.GameOverlayShowFps", GameOverlay, true, On("Modules.GameOverlay")),
            IslandSetting.Toggle("Modules.GameOverlayShowCpu", GameOverlay, true, On("Modules.GameOverlay")),
            IslandSetting.Toggle("Modules.GameOverlayShowGpu", GameOverlay, true, On("Modules.GameOverlay")),
            IslandSetting.Toggle("Modules.GameOverlayShowRam", GameOverlay, true, On("Modules.GameOverlay")),
            IslandSetting.Toggle("Modules.GameOverlayShowDisk", GameOverlay, true, On("Modules.GameOverlay")),

            IslandSetting.Toggle("Shortcuts.HideShowHotkeyEnabled", Shortcuts, true),
            IslandSetting.Choice("Shortcuts.HideShowModifiers", Shortcuts, "ctrl_alt", modifiers, On("Shortcuts.HideShowHotkeyEnabled")),
            IslandSetting.Key("Shortcuts.HideShowKey", Shortcuts, "D", [], On("Shortcuts.HideShowHotkeyEnabled")),
            IslandSetting.Choice("Shortcuts.LookupModifiers", Shortcuts, "ctrl_alt", modifiers, On("Modules.QuickLookup")),
            IslandSetting.Key("Shortcuts.LookupKey", Shortcuts, "Space", ["Space"], On("Modules.QuickLookup")),

            IslandSetting.Toggle(LoggingName, IslandSettingGroup.Diagnostics, false),
        ];
    }
}
