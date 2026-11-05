using System.Globalization;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Island.Settings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace IslandBridge.Controls;

// Island settings page of the flyout, generated from IslandSettingsCatalog. Every change is written
// straight to the registry; the island reloads it live.
internal sealed partial class IslandSettingsView : UserControl
{
    private const double LabelFontSize = 13;
    private const double SmallFontSize = 12;
    private static readonly TimeSpan WriteDelay = TimeSpan.FromMilliseconds(250);
    private static readonly Color Caution = Color.FromArgb(255, 0xE8, 0xA3, 0x17);

    private readonly AppHost _host;
    private readonly StackPanel _groups = new() { Spacing = 8 };
    private readonly Dictionary<IslandSettingGroup, StackPanel> _groupBodies = [];
    private readonly List<(IslandSetting Setting, FrameworkElement Row)> _rows = [];
    private readonly Dictionary<string, string> _values = new(StringComparer.Ordinal);
    private readonly Dictionary<IslandSetting, string> _pending = [];
    private readonly DispatcherQueueTimer _writeTimer;
    private readonly TextBlock _title = new() { FontSize = 15, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _intro = new() { FontSize = SmallFontSize, Opacity = 0.7, TextWrapping = TextWrapping.Wrap };
    private readonly Button _back = new() { Padding = new Thickness(8, 6, 8, 6), Content = new FontIcon { Glyph = "\uE72B", FontSize = 12 } };

    public IslandSettingsView(AppHost host)
    {
        _host = host;
        _writeTimer = DispatcherQueue.GetForCurrentThread().CreateTimer();
        _writeTimer.Interval = WriteDelay;
        _writeTimer.IsRepeating = false;
        _writeTimer.Tick += (_, _) => Flush();
        _back.Click += (_, _) => BackRequested?.Invoke(this, EventArgs.Empty);

        var header = new Grid { ColumnSpacing = 8 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        header.Children.Add(_back);
        Grid.SetColumn(_title, 1);
        header.Children.Add(_title);

        Content = new StackPanel
        {
            Spacing = 10,
            Children = { header, _intro, _groups },
        };

        Rebuild();
    }

    public event EventHandler? BackRequested;

    private Localizer Strings => _host.Localizer;

    public void FocusBack() => _back.Focus(FocusState.Programmatic);

    public void Rebuild()
    {
        Flush();
        _title.Text = Strings["Island_Title"];
        _intro.Text = Strings["Island_Intro"];
        ToolTipService.SetToolTip(_back, Strings["Island_Back"]);
        _groups.Children.Clear();
        _groupBodies.Clear();
        _rows.Clear();
        _values.Clear();

        foreach (var group in Enum.GetValues<IslandSettingGroup>())
        {
            var body = new StackPanel { Spacing = 8 };
            var expander = new Expander
            {
                Header = Strings[$"IslandGroup_{group}"],
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                Content = body,
            };
            expander.Expanding += (_, _) => EnsureGroup(group);
            _groupBodies[group] = body;
            _groups.Children.Add(expander);
        }
    }

    public void Flush()
    {
        _writeTimer.Stop();

        foreach (var (setting, value) in _pending.ToList())
        {
            Write(setting, value);
        }

        _pending.Clear();
    }

    private void EnsureGroup(IslandSettingGroup group)
    {
        var body = _groupBodies[group];

        if (body.Children.Count > 0)
        {
            return;
        }

        foreach (var setting in IslandSettingsCatalog.InGroup(group))
        {
            var row = BuildRow(setting);
            _rows.Add((setting, row));
            body.Children.Add(row);
        }

        body.Children.Add(BuildResetButton(group));
        UpdateEnabled();
    }

    private void ReloadGroup(IslandSettingGroup group)
    {
        var body = _groupBodies[group];
        _rows.RemoveAll(entry => entry.Setting.Group == group);

        foreach (var setting in IslandSettingsCatalog.InGroup(group))
        {
            _values.Remove(setting.Name);
        }

        body.Children.Clear();
        EnsureGroup(group);
    }

    private string Value(IslandSetting setting)
    {
        if (!_values.TryGetValue(setting.Name, out var value))
        {
            value = _host.IslandSettings.Get(setting);
            _values[setting.Name] = value;
        }

        return value;
    }

    private bool Write(IslandSetting setting, string value)
    {
        try
        {
            if (!_host.IslandSettings.Set(setting, value))
            {
                return false;
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            _host.Log.Error($"island setting write failed: {setting.Name} {exception.GetType().Name}");
            return false;
        }

        _values.Remove(setting.Name);
        Value(setting);
        UpdateEnabled();
        return true;
    }

    private void WriteLater(IslandSetting setting, string value)
    {
        _pending[setting] = value;
        _writeTimer.Stop();
        _writeTimer.Start();
    }

    private void UpdateEnabled()
    {
        foreach (var (setting, row) in _rows)
        {
            if (setting.EnabledWhen is { } condition && IslandSettingsCatalog.Find(condition.Name) is { } parent)
            {
                var enabled = condition.IsMet(Value(parent));
                row.Opacity = enabled ? 1 : 0.5;
                SetEnabled(row, enabled);
            }
        }
    }

    private static void SetEnabled(DependencyObject element, bool enabled)
    {
        if (element is Control control)
        {
            control.IsEnabled = enabled;
        }
        else if (element is Panel panel)
        {
            foreach (var child in panel.Children)
            {
                SetEnabled(child, enabled);
            }
        }
    }

    private FrameworkElement BuildRow(IslandSetting setting) => setting.Kind switch
    {
        IslandSettingKind.Toggle => BuildToggle(setting),
        IslandSettingKind.Number when setting.Style == NumberStyle.Slider => BuildSlider(setting),
        IslandSettingKind.Number => BuildNumberBox(setting),
        IslandSettingKind.Choice => BuildChoice(setting),
        IslandSettingKind.Color => BuildColor(setting),
        IslandSettingKind.Key => BuildKey(setting),
        IslandSettingKind.Cities => BuildCities(setting),
        _ => BuildText(setting),
    };

    private TextBlock Label(IslandSetting setting)
    {
        var text = Strings[setting.ResourceKey];

        if (setting.Unit is { } unit && setting.Style == NumberStyle.Box)
        {
            text = $"{text} ({unit})";
        }

        var label = new TextBlock { Text = text, FontSize = LabelFontSize, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
        var hintKey = setting.ResourceKey + "_Hint";
        var hint = Strings[hintKey];

        if (hint != hintKey)
        {
            ToolTipService.SetToolTip(label, hint);
            label.Text += " \u24D8";
        }

        return label;
    }

    private static Grid LabeledRow(FrameworkElement label, FrameworkElement control)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.Children.Add(label);
        Grid.SetColumn(control, 1);
        grid.Children.Add(control);
        return grid;
    }

    private Grid BuildToggle(IslandSetting setting)
    {
        var toggle = new ToggleSwitch { IsOn = Value(setting) != "0" };
        MainWindow.MakeCompact(toggle);
        toggle.Toggled += (_, _) => Write(setting, toggle.IsOn ? "1" : "0");
        return LabeledRow(Label(setting), toggle);
    }

    private Grid BuildChoice(IslandSetting setting)
    {
        var current = Value(setting);
        var choices = VisibleChoices(setting, current);
        var box = new ComboBox { MinWidth = 150, MaxWidth = 190, FontSize = SmallFontSize };

        foreach (var choice in choices)
        {
            box.Items.Add(ChoiceLabel(setting, choice));
        }

        box.SelectedIndex = Math.Max(0, choices.FindIndex(choice => string.Equals(choice.Value, current, StringComparison.OrdinalIgnoreCase)));
        box.SelectionChanged += (_, _) =>
        {
            if (box.SelectedIndex >= 0 && box.SelectedIndex < choices.Count)
            {
                Write(setting, choices[box.SelectedIndex].Value);
            }
        };
        return LabeledRow(Label(setting), box);
    }

    private static List<IslandChoice> VisibleChoices(IslandSetting setting, string current)
    {
        if (setting.Name != "Appearance.TargetMonitor")
        {
            return [.. setting.Choices];
        }

        var monitors = Math.Max(1, Microsoft.UI.Windowing.DisplayArea.FindAll().Count);
        return [.. setting.Choices.Where(choice => !int.TryParse(choice.Value, CultureInfo.InvariantCulture, out var index) || index <= monitors || choice.Value == current)];
    }

    private string ChoiceLabel(IslandSetting setting, IslandChoice choice)
    {
        if (choice.Literal is { } literal)
        {
            return literal;
        }

        if (choice.LabelKey is { } key)
        {
            return Strings.Format(key, choice.Argument);
        }

        var suffix = string.Concat(choice.Value.Select(c => char.IsAsciiLetterOrDigit(c) ? c : '_'));
        return Strings[$"{setting.ResourceKey}_{suffix}"];
    }

    private StackPanel BuildSlider(IslandSetting setting)
    {
        var value = int.Parse(Value(setting), CultureInfo.InvariantCulture);
        var display = new TextBlock { FontSize = SmallFontSize, Opacity = 0.75, VerticalAlignment = VerticalAlignment.Center };
        var slider = new Slider
        {
            Minimum = setting.Minimum,
            Maximum = setting.Maximum,
            StepFrequency = setting.Step,
            SmallChange = setting.Step,
            LargeChange = 10,
            Value = value,
            IsThumbToolTipEnabled = false,
            Margin = new Thickness(0, -6, 0, -6),
        };
        display.Text = FormatNumber(setting, value);
        slider.ValueChanged += (_, args) =>
        {
            var number = (int)Math.Round(args.NewValue);
            display.Text = FormatNumber(setting, number);
            WriteLater(setting, number.ToString(CultureInfo.InvariantCulture));
        };
        slider.PointerCaptureLost += (_, _) => Flush();

        var panel = new StackPanel { Spacing = 0 };
        panel.Children.Add(LabeledRow(Label(setting), display));
        panel.Children.Add(slider);
        return panel;
    }

    private static string FormatNumber(IslandSetting setting, int value) =>
        setting.Unit is { } unit ? string.Create(CultureInfo.InvariantCulture, $"{value} {unit}") : value.ToString(CultureInfo.InvariantCulture);

    private Grid BuildNumberBox(IslandSetting setting)
    {
        var box = new NumberBox
        {
            Minimum = setting.Minimum,
            Maximum = setting.Maximum,
            SmallChange = setting.Step,
            LargeChange = setting.Step * 10,
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact,
            Width = 112,
            Value = int.Parse(Value(setting), CultureInfo.InvariantCulture),
            ValidationMode = NumberBoxValidationMode.InvalidInputOverwritten,
        };
        box.ValueChanged += (_, args) =>
        {
            if (double.IsNaN(args.NewValue))
            {
                box.Value = int.Parse(Value(setting), CultureInfo.InvariantCulture);
                return;
            }

            Write(setting, ((int)Math.Round(args.NewValue)).ToString(CultureInfo.InvariantCulture));
        };
        return LabeledRow(Label(setting), box);
    }

    private StackPanel BuildText(IslandSetting setting)
    {
        var warning = Warning();
        var box = new TextBox { Text = Value(setting), MaxLength = setting.MaxLength, FontSize = SmallFontSize };
        var placeholderKey = setting.ResourceKey + "_Placeholder";

        if (Strings[placeholderKey] is var placeholder && placeholder != placeholderKey)
        {
            box.PlaceholderText = placeholder;
        }

        AttachCommit(box, setting, warning);
        var panel = new StackPanel { Spacing = 4 };
        panel.Children.Add(Label(setting));
        panel.Children.Add(box);
        panel.Children.Add(warning);
        return panel;
    }

    private StackPanel BuildKey(IslandSetting setting)
    {
        var warning = Warning();
        var box = new TextBox { Text = Value(setting), MaxLength = setting.MaxLength, Width = 90, FontSize = SmallFontSize, TextAlignment = TextAlignment.Center };
        AttachCommit(box, setting, warning);
        var panel = new StackPanel { Spacing = 4 };
        panel.Children.Add(LabeledRow(Label(setting), box));
        panel.Children.Add(warning);
        return panel;
    }

    private TextBlock Warning() => new()
    {
        Text = Strings["Island_Invalid"],
        FontSize = SmallFontSize,
        TextWrapping = TextWrapping.Wrap,
        Foreground = new SolidColorBrush(Caution),
        Visibility = Visibility.Collapsed,
    };

    private void AttachCommit(TextBox box, IslandSetting setting, TextBlock warning)
    {
        void Commit()
        {
            if (box.Text == Value(setting))
            {
                warning.Visibility = Visibility.Collapsed;
                return;
            }

            var ok = Write(setting, box.Text);
            warning.Visibility = ok ? Visibility.Collapsed : Visibility.Visible;
            box.Text = Value(setting);
        }

        box.LostFocus += (_, _) => Commit();
        box.KeyDown += (_, args) =>
        {
            if (args.Key == Windows.System.VirtualKey.Enter)
            {
                args.Handled = true;
                Commit();
            }
        };
    }

    private StackPanel BuildColor(IslandSetting setting)
    {
        var warning = Warning();
        var withAlpha = setting.Name == "Themes.PillBgColor";
        var swatch = new Border { Width = 18, Height = 18, CornerRadius = new CornerRadius(4), BorderThickness = new Thickness(1) };
        swatch.BorderBrush = new SolidColorBrush(Color.FromArgb(0x60, 0x80, 0x80, 0x80));
        var hex = new TextBox { Text = Value(setting), Width = 100, MaxLength = 9, FontSize = SmallFontSize };
        var picker = new ColorPicker
        {
            IsAlphaEnabled = withAlpha,
            IsMoreButtonVisible = false,
            IsColorChannelTextInputVisible = false,
            IsHexInputVisible = true,
            ColorSpectrumShape = ColorSpectrumShape.Ring,
        };
        var button = new Button { Padding = new Thickness(6), Content = swatch, Flyout = new Flyout { Content = picker } };
        ToolTipService.SetToolTip(button, Strings["Island_PickColor"]);

        void ShowColor()
        {
            if (IslandColor.TryParse(Value(setting), out var r, out var g, out var b, out var a))
            {
                var color = Color.FromArgb(a, r, g, b);
                swatch.Background = new SolidColorBrush(color);
                picker.Color = color;
            }
        }

        ShowColor();
        picker.ColorChanged += (_, args) =>
        {
            var color = args.NewColor;
            var text = IslandColor.Format(color.R, color.G, color.B, color.A, withAlpha);

            if (!string.Equals(text, IslandColor.Normalize(Value(setting)), StringComparison.Ordinal))
            {
                hex.Text = text;
                swatch.Background = new SolidColorBrush(color);
                WriteLater(setting, text);
            }
        };

        void Commit()
        {
            if (hex.Text == Value(setting))
            {
                return;
            }

            var ok = Write(setting, hex.Text);
            warning.Visibility = ok ? Visibility.Collapsed : Visibility.Visible;
            hex.Text = Value(setting);
            ShowColor();
        }

        hex.LostFocus += (_, _) => Commit();
        hex.KeyDown += (_, args) =>
        {
            if (args.Key == Windows.System.VirtualKey.Enter)
            {
                args.Handled = true;
                Commit();
            }
        };

        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        controls.Children.Add(button);
        controls.Children.Add(hex);
        var panel = new StackPanel { Spacing = 4 };
        panel.Children.Add(LabeledRow(Label(setting), controls));
        panel.Children.Add(warning);
        return panel;
    }

    private StackPanel BuildCities(IslandSetting setting)
    {
        var panel = new StackPanel { Spacing = 6 };
        var list = new StackPanel { Spacing = 4 };
        var warning = Warning();
        var name = new TextBox { PlaceholderText = Strings["Island_CityName"], FontSize = SmallFontSize, MaxLength = WeatherCityList.MaxNameLength };
        var latitude = new TextBox { PlaceholderText = Strings["Island_CityLatitude"], FontSize = SmallFontSize, Width = 72, MaxLength = 12 };
        var longitude = new TextBox { PlaceholderText = Strings["Island_CityLongitude"], FontSize = SmallFontSize, Width = 72, MaxLength = 12 };
        var add = new Button { Padding = new Thickness(8, 6, 8, 6), Content = new FontIcon { Glyph = "\uE710", FontSize = 12 } };
        ToolTipService.SetToolTip(add, Strings["Island_CityAdd"]);

        List<WeatherCity> Current() => WeatherCityList.TryParse(Value(setting), out var cities) ? cities : [];

        void Save(List<WeatherCity> cities)
        {
            Write(setting, WeatherCityList.Format(cities));
            Render();
        }

        void Render()
        {
            list.Children.Clear();
            var cities = Current();

            foreach (var city in cities)
            {
                var row = new Grid { ColumnSpacing = 8 };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                var text = new StackPanel();
                text.Children.Add(new TextBlock { Text = city.Name, FontSize = LabelFontSize, TextTrimming = TextTrimming.CharacterEllipsis });
                text.Children.Add(new TextBlock
                {
                    Text = string.Create(CultureInfo.InvariantCulture, $"{city.Latitude:0.#####}, {city.Longitude:0.#####}"),
                    FontSize = SmallFontSize,
                    Opacity = 0.6,
                });
                var remove = new Button { Padding = new Thickness(8, 6, 8, 6), Content = new FontIcon { Glyph = "\uE74D", FontSize = 12 }, VerticalAlignment = VerticalAlignment.Center };
                ToolTipService.SetToolTip(remove, Strings["Island_CityRemove"]);
                remove.Click += (_, _) => Save([.. Current().Where(item => item != city)]);
                Grid.SetColumn(remove, 1);
                row.Children.Add(text);
                row.Children.Add(remove);
                list.Children.Add(row);
            }

            add.IsEnabled = cities.Count < WeatherCityList.MaxCities;
        }

        add.Click += (_, _) =>
        {
            var city = WeatherCityList.Create(name.Text, latitude.Text, longitude.Text);
            warning.Visibility = city is null ? Visibility.Visible : Visibility.Collapsed;

            if (city is null)
            {
                return;
            }

            Save([.. Current(), city]);
            name.Text = latitude.Text = longitude.Text = string.Empty;
        };

        var form = new Grid { ColumnSpacing = 4 };
        form.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        form.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        form.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        form.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        form.Children.Add(name);
        Grid.SetColumn(latitude, 1);
        Grid.SetColumn(longitude, 2);
        Grid.SetColumn(add, 3);
        form.Children.Add(latitude);
        form.Children.Add(longitude);
        form.Children.Add(add);

        Render();
        panel.Children.Add(Label(setting));
        panel.Children.Add(list);
        panel.Children.Add(form);
        panel.Children.Add(warning);
        return panel;
    }

    private Button BuildResetButton(IslandSettingGroup group)
    {
        var confirm = new Button { Content = Strings["Island_ResetConfirm"], FontSize = SmallFontSize };
        var flyout = new Flyout
        {
            Content = new StackPanel
            {
                Spacing = 10,
                MaxWidth = 260,
                Children =
                {
                    new TextBlock { Text = Strings["Island_ResetQuestion"], FontSize = SmallFontSize, TextWrapping = TextWrapping.Wrap },
                    confirm,
                },
            },
        };
        confirm.Click += (_, _) =>
        {
            flyout.Hide();
            _pending.Clear();

            try
            {
                _host.IslandSettings.Reset(IslandSettingsCatalog.InGroup(group));
                _host.Log.Info($"island settings reset: {group}");
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or System.Security.SecurityException)
            {
                _host.Log.Error($"island settings reset failed: {group} {exception.GetType().Name}");
            }

            _values.Clear();
            ReloadGroup(group);
        };

        return new Button
        {
            Content = Strings["Island_Reset"],
            FontSize = SmallFontSize,
            Padding = new Thickness(8, 4, 8, 5),
            Margin = new Thickness(0, 4, 0, 0),
            Flyout = flyout,
        };
    }
}
