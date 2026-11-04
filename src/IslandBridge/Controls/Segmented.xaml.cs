using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace IslandBridge.Controls;

public sealed class SegmentSelectedEventArgs(int index) : EventArgs
{
    public int Index { get; } = index;
}

public sealed partial class Segmented : UserControl
{
    private readonly List<ToggleButton> _buttons = [];
    private int _selectedIndex = -1;

    public Segmented()
    {
        InitializeComponent();
    }

    public event EventHandler<SegmentSelectedEventArgs>? SegmentSelected;

    public int SelectedIndex
    {
        get => _selectedIndex;
        set
        {
            _selectedIndex = value;

            for (var i = 0; i < _buttons.Count; i++)
            {
                _buttons[i].IsChecked = i == value;
            }
        }
    }

    public void SetItems(IReadOnlyList<string> items)
    {
        ArgumentNullException.ThrowIfNull(items);

        Host.Children.Clear();
        Host.ColumnDefinitions.Clear();
        _buttons.Clear();

        for (var i = 0; i < items.Count; i++)
        {
            Host.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var button = new ToggleButton
            {
                Content = items[i],
                HorizontalAlignment = HorizontalAlignment.Stretch,
                Padding = new Thickness(4, 5, 4, 6),
                FontSize = 12,
            };
            button.Click += OnButtonClick;
            Grid.SetColumn(button, i);
            Host.Children.Add(button);
            _buttons.Add(button);
        }

        SelectedIndex = _selectedIndex;
    }

    private void OnButtonClick(object sender, RoutedEventArgs args)
    {
        var index = sender is ToggleButton button ? _buttons.IndexOf(button) : -1;

        if (index < 0)
        {
            return;
        }

        var changed = index != _selectedIndex;
        SelectedIndex = index;

        if (changed)
        {
            SegmentSelected?.Invoke(this, new SegmentSelectedEventArgs(index));
        }
    }
}
