using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace IslandBridge.Controls;

public sealed partial class Card : UserControl
{
    private Action? _action;

    public Card(string heading, string meta, string body)
    {
        InitializeComponent();
        HeadingText.Text = heading;
        MetaText.Text = meta;
        MetaText.Visibility = meta.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        BodyText.Text = body;
        BodyText.Visibility = body.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    public void SetAction(string label, Action action)
    {
        _action = action;
        ActionButton.Content = label;
        ActionButton.Visibility = Visibility.Visible;
    }

    private void OnActionClick(object sender, RoutedEventArgs args) => _action?.Invoke();
}
