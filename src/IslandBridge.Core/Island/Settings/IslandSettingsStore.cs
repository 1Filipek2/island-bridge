using System.Globalization;
using Microsoft.Win32;

namespace IslandBridge.Core.Island.Settings;

// Reads and writes HKCU\Software\IslandBridge\Island. The island watches the Settings key and reloads
// live; context menu choices live in the State key and can override a setting until it changes.
public sealed class IslandSettingsStore
{
    public const string DefaultSettingsKey = @"Software\IslandBridge\Island\Settings";
    public const string DefaultStateKey = @"Software\IslandBridge\Island\State";

    private const string ThemeState = "ColorThemeV2";
    private const string OpacityState = "PillOpacityOverride";
    private const string ExpandOnHoverState = "ExpandOnHoverOverride";
    private const string W11State = "W11StyleOverride";
    private const string NotchState = "NotchStyleOverride";
    private const string MediaTabLyricsState = "MediaTabLyricsOverride";
    private const string MediaTabLyricsBaseState = "MediaTabLyricsOverrideBase";

    private readonly RegistryKey _root;
    private readonly string _settingsKey;
    private readonly string _stateKey;

    public IslandSettingsStore(RegistryKey? root = null, string settingsKey = DefaultSettingsKey, string stateKey = DefaultStateKey)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(settingsKey);
        ArgumentException.ThrowIfNullOrWhiteSpace(stateKey);

        _root = root ?? Registry.CurrentUser;
        _settingsKey = settingsKey;
        _stateKey = stateKey;
    }

    public string Get(IslandSetting setting)
    {
        ArgumentNullException.ThrowIfNull(setting);

        using var settings = _root.OpenSubKey(_settingsKey);
        using var state = _root.OpenSubKey(_stateKey);
        var stored = ReadStored(settings, setting);
        return Effective(setting, stored, state) ?? stored;
    }

    public bool Set(IslandSetting setting, string value)
    {
        ArgumentNullException.ThrowIfNull(setting);
        ArgumentNullException.ThrowIfNull(value);

        if (setting.IsNumeric)
        {
            if (!int.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number))
            {
                return false;
            }

            var normalizedNumber = setting.NormalizeNumber(number);
            using var settings = _root.CreateSubKey(_settingsKey);
            ReleaseOverrides(setting, normalizedNumber.ToString(CultureInfo.InvariantCulture));
            settings.SetValue(setting.Name, normalizedNumber, RegistryValueKind.DWord);
            return true;
        }

        var normalized = setting.NormalizeText(value);

        if (normalized is null)
        {
            return false;
        }

        using (var settings = _root.CreateSubKey(_settingsKey))
        {
            ReleaseOverrides(setting, normalized);
            settings.SetValue(setting.Name, normalized, RegistryValueKind.String);
        }

        return true;
    }

    public void Reset(IEnumerable<IslandSetting> settings)
    {
        ArgumentNullException.ThrowIfNull(settings);

        using var key = _root.CreateSubKey(_settingsKey);

        foreach (var setting in settings)
        {
            ReleaseOverrides(setting, setting.DefaultValue);
            key.DeleteValue(setting.Name, throwOnMissingValue: false);
        }
    }

    private static string ReadStored(RegistryKey? key, IslandSetting setting)
    {
        var raw = key?.GetValue(setting.Name);

        if (setting.IsNumeric)
        {
            return raw is int number ? number.ToString(CultureInfo.InvariantCulture) : setting.DefaultValue;
        }

        return raw is string text ? text : setting.DefaultValue;
    }

    private static int? ReadState(RegistryKey? state, string name) => state?.GetValue(name) is int value && value >= 0 ? value : null;

    private static string? Effective(IslandSetting setting, string stored, RegistryKey? state)
    {
        switch (setting.Name)
        {
            case "Themes.ThemePreset" when ReadState(state, ThemeState) is { } index && index < setting.Choices.Count:
                return setting.Choices[index].Value;
            case "Themes.PillOpacity" when ReadState(state, OpacityState) is { } opacity:
                return setting.NormalizeNumber(opacity).ToString(CultureInfo.InvariantCulture);
            case "Behavior.ExpandOnHover" when ReadState(state, ExpandOnHoverState) is { } expand:
                return expand != 0 ? "1" : "0";
            case "Modules.MediaTabLyrics" when ReadState(state, MediaTabLyricsState) is { } lyrics && ReadState(state, MediaTabLyricsBaseState)?.ToString(CultureInfo.InvariantCulture) == stored:
                return lyrics != 0 ? "1" : "0";
            case "Appearance.ShapeStyle" when ReadState(state, W11State) == 1:
                return "w11";
            case "Appearance.ShapeStyle" when ReadState(state, NotchState) == 1:
                return "notch";
            case "Appearance.ShapeStyle" when ReadState(state, W11State) == 0 || ReadState(state, NotchState) == 0:
                return "default";
            default:
                return null;
        }
    }

    private void ReleaseOverrides(IslandSetting setting, string value)
    {
        string[] names = setting.Name switch
        {
            "Themes.ThemePreset" => [ThemeState],
            "Themes.PillOpacity" => [OpacityState],
            "Behavior.ExpandOnHover" => [ExpandOnHoverState],
            "Appearance.ShapeStyle" => [W11State, NotchState],
            "Modules.MediaTabLyrics" => [MediaTabLyricsState, MediaTabLyricsBaseState],
            _ => [],
        };

        if (names.Length == 0)
        {
            return;
        }

        using var state = _root.CreateSubKey(_stateKey);

        foreach (var name in names)
        {
            state.DeleteValue(name, throwOnMissingValue: false);
        }

        if (setting.Name == "Themes.ThemePreset")
        {
            var index = setting.Choices.ToList().FindIndex(choice => choice.Value == value);

            if (index >= 0)
            {
                state.SetValue(ThemeState, index, RegistryValueKind.DWord);
            }
        }
    }
}
