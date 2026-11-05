using System.Globalization;
using System.Text.RegularExpressions;
using IslandBridge.Core.Island.Settings;
using Microsoft.Win32;

namespace IslandBridge.Tests.Island;

public sealed partial class IslandSettingsTests : IDisposable
{
    private readonly string _testRoot = @"Software\IslandBridge.Tests\" + Guid.NewGuid().ToString("N");

    private string SettingsKey => _testRoot + @"\Settings";

    private string StateKey => _testRoot + @"\State";

    [Fact]
    public void Catalog_matches_the_island_defaults()
    {
        var defaults = ReadIslandDefaults();
        var catalog = IslandSettingsCatalog.All.Where(setting => setting.Name != IslandSettingsCatalog.LoggingName).ToDictionary(setting => setting.Name);

        foreach (var (name, (numeric, value)) in defaults)
        {
            if (IslandSettingsCatalog.Retired.Contains(name))
            {
                Assert.False(catalog.ContainsKey(name), name);
                continue;
            }

            Assert.True(catalog.TryGetValue(name, out var setting), $"missing {name}");
            Assert.Equal(numeric, setting.IsNumeric);
            Assert.Equal(value, setting.DefaultValue);
        }

        Assert.Empty(catalog.Keys.Except(defaults.Keys));
    }

    [Fact]
    public void Catalog_is_consistent()
    {
        var names = IslandSettingsCatalog.All.Select(setting => setting.Name).ToList();
        Assert.Equal(names.Count, names.Distinct(StringComparer.Ordinal).Count());

        foreach (var setting in IslandSettingsCatalog.All)
        {
            if (setting.EnabledWhen is { } condition)
            {
                Assert.NotNull(IslandSettingsCatalog.Find(condition.Name));
            }

            if (setting.Kind == IslandSettingKind.Choice)
            {
                Assert.Contains(setting.Choices, choice => choice.Value == setting.DefaultText);
            }

            if (setting.Kind == IslandSettingKind.Number)
            {
                Assert.InRange(setting.DefaultNumber, setting.Minimum, setting.Maximum);
            }

            if (!setting.IsNumeric && setting.Kind != IslandSettingKind.Choice)
            {
                Assert.Equal(setting.DefaultText.Length == 0 ? string.Empty : setting.NormalizeText(setting.DefaultText)?.ToUpperInvariant(), setting.DefaultText.ToUpperInvariant());
            }
        }
    }

    [Theory]
    [InlineData("en-US")]
    [InlineData("sk-SK")]
    public void Every_label_has_a_string(string language)
    {
        var keys = ReadResourceKeys(language);
        var missing = new List<string>();

        void Require(string key)
        {
            if (!keys.Contains(key))
            {
                missing.Add(key);
            }
        }

        foreach (var group in Enum.GetValues<IslandSettingGroup>())
        {
            Require($"IslandGroup_{group}");
        }

        foreach (var setting in IslandSettingsCatalog.All)
        {
            Require(setting.ResourceKey);

            foreach (var choice in setting.Choices.Where(choice => choice.Literal is null))
            {
                Require(choice.LabelKey ?? $"{setting.ResourceKey}_{string.Concat(choice.Value.Select(c => char.IsAsciiLetterOrDigit(c) ? c : '_'))}");
            }
        }

        Assert.Empty(missing);
    }

    [Theory]
    [InlineData("#4cc9f0", "#4CC9F0")]
    [InlineData("fff", "#FFF")]
    [InlineData(" 0D0D0FB0 ", "#0D0D0FB0")]
    [InlineData("#12345", null)]
    [InlineData("#GG0000", null)]
    [InlineData("", null)]
    public void Colors_are_normalized(string input, string? expected)
    {
        Assert.Equal(expected, IslandColor.Normalize(input));
    }

    [Fact]
    public void Short_colors_expand_when_parsed()
    {
        Assert.True(IslandColor.TryParse("#F80", out var red, out var green, out var blue, out var alpha));
        Assert.Equal((255, 136, 0, 255), (red, green, blue, alpha));
        Assert.Equal("#FF8800", IslandColor.Format(red, green, blue, alpha, withAlpha: true));
        Assert.Equal("#FF880080", IslandColor.Format(red, green, blue, 128, withAlpha: true));
    }

    [Theory]
    [InlineData("Shortcuts.HideShowKey", "k", "K")]
    [InlineData("Shortcuts.HideShowKey", "7", "7")]
    [InlineData("Shortcuts.HideShowKey", "Space", null)]
    [InlineData("Shortcuts.HideShowKey", "ab", null)]
    [InlineData("Shortcuts.LookupKey", "space", "Space")]
    [InlineData("Shortcuts.LookupKey", "č", null)]
    [InlineData("Appearance.Position", "TOP-LEFT", "top-left")]
    [InlineData("Appearance.Position", "middle", null)]
    [InlineData("Modules.GitHubUser", "octo-cat", "octo-cat")]
    [InlineData("Modules.GitHubUser", "-bad", null)]
    [InlineData("Modules.GitHubUser", "", "")]
    [InlineData("Themes.FontFamily", " \"Segoe UI\" ", "Segoe UI")]
    public void Text_values_are_validated(string name, string input, string? expected)
    {
        Assert.Equal(expected, Setting(name).NormalizeText(input));
    }

    [Fact]
    public void Free_text_drops_control_and_bidi_characters()
    {
        var input = "dd." + (char)0x202E + "MM" + (char)0x07 + ".yyyy";
        Assert.Equal("dd.MM.yyyy", Setting("Modules.DateFormat").NormalizeText(input));
    }

    [Fact]
    public void Numbers_are_clamped()
    {
        Assert.Equal(35, Setting("Themes.PillOpacity").NormalizeNumber(5));
        Assert.Equal(3000, Setting("Modules.LyricsOffsetMs").NormalizeNumber(99999));
        Assert.Equal(1, Setting("Modules.Media").NormalizeNumber(42));
    }

    [Fact]
    public void Cities_round_trip_in_the_island_format()
    {
        Assert.True(WeatherCityList.TryParse("Trnava|48.37773|17.58603; New York|40.71278|-74.00597", out var cities));
        Assert.Equal(2, cities.Count);
        Assert.Equal(new WeatherCity("New York", 40.71278, -74.00597), cities[1]);
        Assert.Equal("Trnava|48.37773|17.58603;New York|40.71278|-74.00597", WeatherCityList.Format(cities));
        Assert.True(WeatherCityList.TryParse(string.Empty, out var none));
        Assert.Empty(none);
    }

    [Theory]
    [InlineData("Nowhere|91|0")]
    [InlineData("Nowhere|0|181")]
    [InlineData("Nowhere|0")]
    [InlineData("|1|2")]
    [InlineData("A|1|2|3")]
    [InlineData("A|NaN|2")]
    [InlineData("A|1e3|2")]
    public void Invalid_cities_are_rejected(string input)
    {
        Assert.False(WeatherCityList.TryParse(input, out _));
    }

    [Fact]
    public void City_input_accepts_a_decimal_comma_and_cleans_the_name()
    {
        var city = WeatherCityList.Create(" Ban;ská|Bystrica ", "48,736", "19,146");

        Assert.NotNull(city);
        Assert.Equal("Ban ská Bystrica", city.Name);
        Assert.Equal("Ban ská Bystrica|48.736|19.146", WeatherCityList.Format([city]));
    }

    [Fact]
    public void City_list_is_capped_like_the_island()
    {
        var input = string.Join(';', Enumerable.Range(0, WeatherCityList.MaxCities + 1).Select(i => string.Create(CultureInfo.InvariantCulture, $"C{i}|1|{i}")));
        Assert.False(WeatherCityList.TryParse(input, out _));
    }

    [Fact]
    public void Store_returns_defaults_when_nothing_is_stored()
    {
        var store = CreateStore();

        Assert.Equal("top-center", store.Get(Setting("Appearance.Position")));
        Assert.Equal("96", store.Get(Setting("Themes.PillOpacity")));
        Assert.Equal("1", store.Get(Setting("Modules.Media")));
    }

    [Fact]
    public void Store_writes_typed_values()
    {
        var store = CreateStore();

        Assert.True(store.Set(Setting("Appearance.OffsetY"), "-12"));
        Assert.True(store.Set(Setting("Appearance.Position"), "Bottom-Right"));
        Assert.True(store.Set(Setting("Themes.CustomAccentHex"), "ff0000"));

        using var key = Registry.CurrentUser.OpenSubKey(SettingsKey);
        Assert.NotNull(key);
        Assert.Equal(RegistryValueKind.DWord, key.GetValueKind("Appearance.OffsetY"));
        Assert.Equal(-12, key.GetValue("Appearance.OffsetY"));
        Assert.Equal("bottom-right", key.GetValue("Appearance.Position"));
        Assert.Equal("#FF0000", key.GetValue("Themes.CustomAccentHex"));
        Assert.Equal("-12", store.Get(Setting("Appearance.OffsetY")));
    }

    [Fact]
    public void Store_rejects_invalid_values_without_writing()
    {
        var store = CreateStore();

        Assert.False(store.Set(Setting("Appearance.Position"), "middle"));
        Assert.False(store.Set(Setting("Appearance.OffsetY"), "abc"));
        Assert.False(store.Set(Setting("Modules.WeatherCities"), "Bad|100|0"));

        using var key = Registry.CurrentUser.OpenSubKey(SettingsKey);
        Assert.True(key is null || key.ValueCount == 0);
    }

    [Fact]
    public void Store_ignores_a_value_of_the_wrong_type()
    {
        using (var key = Registry.CurrentUser.CreateSubKey(SettingsKey))
        {
            key.SetValue("Modules.Media", "yes", RegistryValueKind.String);
            key.SetValue("Appearance.Position", 3, RegistryValueKind.DWord);
        }

        var store = CreateStore();
        Assert.Equal("1", store.Get(Setting("Modules.Media")));
        Assert.Equal("top-center", store.Get(Setting("Appearance.Position")));
    }

    [Fact]
    public void Context_menu_overrides_are_shown_and_released_on_write()
    {
        using (var state = Registry.CurrentUser.CreateSubKey(StateKey))
        {
            state.SetValue("PillOpacityOverride", 70, RegistryValueKind.DWord);
            state.SetValue("ExpandOnHoverOverride", 0, RegistryValueKind.DWord);
            state.SetValue("ColorThemeV2", 3, RegistryValueKind.DWord);
        }

        var store = CreateStore();
        Assert.Equal("70", store.Get(Setting("Themes.PillOpacity")));
        Assert.Equal("0", store.Get(Setting("Behavior.ExpandOnHover")));
        Assert.Equal("nord", store.Get(Setting("Themes.ThemePreset")));

        Assert.True(store.Set(Setting("Themes.PillOpacity"), "90"));
        Assert.True(store.Set(Setting("Behavior.ExpandOnHover"), "1"));
        Assert.True(store.Set(Setting("Themes.ThemePreset"), "plum"));

        using var after = Registry.CurrentUser.OpenSubKey(StateKey);
        Assert.NotNull(after);
        Assert.Null(after.GetValue("PillOpacityOverride"));
        Assert.Null(after.GetValue("ExpandOnHoverOverride"));
        Assert.Equal(6, after.GetValue("ColorThemeV2"));
        Assert.Equal("90", store.Get(Setting("Themes.PillOpacity")));
        Assert.Equal("1", store.Get(Setting("Behavior.ExpandOnHover")));
        Assert.Equal("plum", store.Get(Setting("Themes.ThemePreset")));
    }

    [Fact]
    public void Shape_overrides_from_the_menu_are_shown()
    {
        using (var state = Registry.CurrentUser.CreateSubKey(StateKey))
        {
            state.SetValue("W11StyleOverride", -1, RegistryValueKind.DWord);
            state.SetValue("NotchStyleOverride", 1, RegistryValueKind.DWord);
        }

        var store = CreateStore();
        Assert.Equal("notch", store.Get(Setting("Appearance.ShapeStyle")));

        Assert.True(store.Set(Setting("Appearance.ShapeStyle"), "default"));
        Assert.Equal("default", store.Get(Setting("Appearance.ShapeStyle")));
    }

    [Fact]
    public void Reset_removes_the_stored_values()
    {
        var store = CreateStore();
        Assert.True(store.Set(Setting("Behavior.AlwaysOnTop"), "0"));
        Assert.True(store.Set(Setting("Behavior.AutoHideIdleSeconds"), "30"));
        Assert.True(store.Set(Setting("Modules.Media"), "0"));

        store.Reset(IslandSettingsCatalog.InGroup(IslandSettingGroup.Behavior));

        Assert.Equal("1", store.Get(Setting("Behavior.AlwaysOnTop")));
        Assert.Equal("0", store.Get(Setting("Behavior.AutoHideIdleSeconds")));
        Assert.Equal("0", store.Get(Setting("Modules.Media")));
    }

    public void Dispose()
    {
        Registry.CurrentUser.DeleteSubKeyTree(_testRoot, throwOnMissingSubKey: false);
    }

    private static IslandSetting Setting(string name) => IslandSettingsCatalog.Find(name) ?? throw new ArgumentException(name, nameof(name));

    private static Dictionary<string, (bool Numeric, string Value)> ReadIslandDefaults()
    {
        var source = File.ReadAllText(Path.Combine(RepositoryRoot(), "island", "src", "host", "defaults.cpp"));
        var result = new Dictionary<string, (bool, string)>(StringComparer.Ordinal);

        foreach (Match match in DefaultEntry().Matches(source))
        {
            var numeric = match.Groups["kind"].Value == "Number";
            result[match.Groups["name"].Value] = (numeric, numeric ? match.Groups["number"].Value : match.Groups["text"].Value);
        }

        Assert.NotEmpty(result);
        return result;
    }

    private static HashSet<string> ReadResourceKeys(string language)
    {
        var path = Path.Combine(RepositoryRoot(), "src", "IslandBridge", "Strings", language, "Resources.resw");
        var document = System.Xml.Linq.XDocument.Load(path);
        return [.. document.Root!.Elements("data").Select(data => (string)data.Attribute("name")!)];
    }

    private static string RepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);

        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "IslandBridge.slnx")))
        {
            directory = directory.Parent;
        }

        Assert.NotNull(directory);
        return directory.FullName;
    }

    [GeneratedRegex("""SettingDefault\{L"(?<name>[^"]+)",\s*SettingKind::(?<kind>Number|Text),\s*(?<number>-?\d+),\s*L"(?<text>[^"]*)"\}""")]
    private static partial Regex DefaultEntry();

    private IslandSettingsStore CreateStore() => new(Registry.CurrentUser, SettingsKey, StateKey);
}
