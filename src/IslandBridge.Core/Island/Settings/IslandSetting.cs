using System.Globalization;
using IslandBridge.Core.Text;

namespace IslandBridge.Core.Island.Settings;

public enum IslandSettingGroup
{
    Appearance,
    Behavior,
    Animations,
    Theme,
    Media,
    Lyrics,
    ClockWeather,
    Alerts,
    Privacy,
    Tabs,
    GameOverlay,
    Shortcuts,
    Diagnostics,
}

public enum IslandSettingKind
{
    Toggle,
    Number,
    Choice,
    Text,
    Color,
    Key,
    Cities,
}

public enum NumberStyle
{
    Box,
    Slider,
}

public sealed record IslandChoice(string Value, string? LabelKey = null, string? Literal = null, object? Argument = null);

public sealed record IslandSettingCondition(string Name, Func<string, bool> IsMet);

public sealed class IslandSetting
{
    private IslandSetting(string name, IslandSettingGroup group, IslandSettingKind kind)
    {
        Name = name;
        Group = group;
        Kind = kind;
    }

    public string Name { get; }

    public IslandSettingGroup Group { get; }

    public IslandSettingKind Kind { get; }

    public bool IsNumeric => Kind is IslandSettingKind.Toggle or IslandSettingKind.Number;

    public int DefaultNumber { get; private init; }

    public string DefaultText { get; private init; } = string.Empty;

    public int Minimum { get; private init; }

    public int Maximum { get; private init; }

    public int Step { get; private init; } = 1;

    public NumberStyle Style { get; private init; }

    public string? Unit { get; private init; }

    public int MaxLength { get; private init; }

    public IReadOnlyList<IslandChoice> Choices { get; private init; } = [];

    public Func<string, string?>? TextRule { get; private init; }

    public IReadOnlyList<string> KeyChoices { get; private init; } = [];

    public IslandSettingCondition? EnabledWhen { get; private init; }

    public string ResourceKey => "Island_" + Name.Replace('.', '_');

    public string DefaultValue => IsNumeric ? DefaultNumber.ToString(CultureInfo.InvariantCulture) : DefaultText;

    public static IslandSetting Toggle(string name, IslandSettingGroup group, bool value, IslandSettingCondition? enabledWhen = null) =>
        new(name, group, IslandSettingKind.Toggle) { DefaultNumber = value ? 1 : 0, Minimum = 0, Maximum = 1, EnabledWhen = enabledWhen };

    public static IslandSetting Number(
        string name,
        IslandSettingGroup group,
        int value,
        int minimum,
        int maximum,
        NumberStyle style,
        int step = 1,
        string? unit = null,
        IslandSettingCondition? enabledWhen = null) =>
        new(name, group, IslandSettingKind.Number)
        {
            DefaultNumber = value,
            Minimum = minimum,
            Maximum = maximum,
            Style = style,
            Step = step,
            Unit = unit,
            EnabledWhen = enabledWhen,
        };

    public static IslandSetting Choice(
        string name,
        IslandSettingGroup group,
        string value,
        IReadOnlyList<IslandChoice> choices,
        IslandSettingCondition? enabledWhen = null) =>
        new(name, group, IslandSettingKind.Choice) { DefaultText = value, Choices = choices, EnabledWhen = enabledWhen };

    public static IslandSetting Text(
        string name,
        IslandSettingGroup group,
        string value,
        int maxLength,
        Func<string, string?>? rule = null,
        IslandSettingCondition? enabledWhen = null) =>
        new(name, group, IslandSettingKind.Text) { DefaultText = value, MaxLength = maxLength, TextRule = rule, EnabledWhen = enabledWhen };

    public static IslandSetting Color(string name, IslandSettingGroup group, string value, IslandSettingCondition? enabledWhen = null) =>
        new(name, group, IslandSettingKind.Color) { DefaultText = value, MaxLength = 9, EnabledWhen = enabledWhen };

    public static IslandSetting Key(
        string name,
        IslandSettingGroup group,
        string value,
        IReadOnlyList<string> extraKeys,
        IslandSettingCondition? enabledWhen = null) =>
        new(name, group, IslandSettingKind.Key) { DefaultText = value, MaxLength = 5, KeyChoices = extraKeys, EnabledWhen = enabledWhen };

    public static IslandSetting Cities(string name, IslandSettingGroup group, string value, IslandSettingCondition? enabledWhen = null) =>
        new(name, group, IslandSettingKind.Cities) { DefaultText = value, MaxLength = 2048, EnabledWhen = enabledWhen };

    public int NormalizeNumber(int value) => Kind == IslandSettingKind.Toggle ? (value != 0 ? 1 : 0) : Math.Clamp(value, Minimum, Maximum);

    public string? NormalizeText(string value)
    {
        ArgumentNullException.ThrowIfNull(value);

        return Kind switch
        {
            IslandSettingKind.Choice => Choices.FirstOrDefault(choice => string.Equals(choice.Value, value.Trim(), StringComparison.OrdinalIgnoreCase))?.Value,
            IslandSettingKind.Color => IslandColor.Normalize(value),
            IslandSettingKind.Key => NormalizeKey(value),
            IslandSettingKind.Cities => WeatherCityList.TryParse(value, out var cities) ? WeatherCityList.Format(cities) : null,
            IslandSettingKind.Text => NormalizeFreeText(value),
            _ => null,
        };
    }

    private string? NormalizeFreeText(string value)
    {
        var clean = TextSanitizer.Sanitize(value, MaxLength).Trim();
        return TextRule is null ? clean : TextRule(clean);
    }

    private string? NormalizeKey(string value)
    {
        var trimmed = value.Trim();
        var named = KeyChoices.FirstOrDefault(key => string.Equals(key, trimmed, StringComparison.OrdinalIgnoreCase));

        if (named is not null)
        {
            return named;
        }

        return trimmed.Length == 1 && char.IsAsciiLetterOrDigit(trimmed[0]) ? trimmed.ToUpperInvariant() : null;
    }
}
