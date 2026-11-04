using IslandBridge.Core.Ancs;
using IslandBridge.Core.Notifications;
using IslandBridge.Core.Toasts;

namespace IslandBridge.Tests.Toasts;

public sealed class ToastTextTests
{
    [Fact]
    public void First_non_empty_text_is_the_title_and_the_rest_is_the_message()
    {
        var notification = Map(["  ", "Anna", "Hello", "  there  "]);

        Assert.NotNull(notification);
        Assert.Equal("Anna", notification.Title);
        Assert.Equal("Hello there", notification.Message);
        Assert.Equal(string.Empty, notification.Subtitle);
        Assert.Equal(AncsCategory.Other, notification.Category);
        Assert.Equal(AncsEventFlags.None, notification.Flags);
        Assert.Equal(NotificationOrigin.Windows, notification.Origin);
    }

    [Fact]
    public void Whitespace_is_collapsed_and_controls_are_removed()
    {
        var notification = Map(["A\r\n\tB", "x\u202Ey\u0007z"]);

        Assert.NotNull(notification);
        Assert.Equal("A B", notification.Title);
        Assert.Equal("xyz", notification.Message);
    }

    [Fact]
    public void Toast_without_text_is_dropped()
    {
        Assert.Null(Map([]));
        Assert.Null(Map(["", "   ", "\u200F"]));
    }

    [Fact]
    public void Element_count_and_lengths_are_capped()
    {
        var texts = Enumerable.Range(0, 40).Select(index => index == 0 ? new string('t', 2000) : "m").ToArray();
        var notification = Map(texts);

        Assert.NotNull(notification);
        Assert.Equal(ToastText.MaxElementLength, notification.Title.Length);
        Assert.Equal(string.Join(' ', Enumerable.Repeat("m", ToastText.MaxTextElements - 1)), notification.Message);
    }

    [Fact]
    public void Message_is_capped_after_joining()
    {
        var notification = Map(["T", new string('a', 400), new string('b', 400)]);

        Assert.NotNull(notification);
        Assert.Equal(ToastText.MaxElementLength, notification.Message.Length);
    }

    [Fact]
    public void App_fields_are_capped_and_the_name_falls_back_to_windows()
    {
        var details = new ToastDetails(7, new string('a', 300), string.Empty, ["T"], DateTimeOffset.UnixEpoch);
        var notification = ToastText.Map(details, 42);

        Assert.NotNull(notification);
        Assert.Equal(AncsLimits.MaxAppIdentifierLength, notification.AppIdentifier.Length);
        Assert.Equal(ToastText.FallbackAppName, notification.AppName);
        Assert.Equal(7u, notification.Uid);
        Assert.Equal(42, notification.ReceivedTimestamp);

        var named = ToastText.Map(details with { DisplayName = new string('n', 100) }, 0);
        Assert.Equal(AncsLimits.MaxDisplayNameLength, named!.AppName.Length);
    }

    private static IslandNotification? Map(string[] texts) =>
        ToastText.Map(new ToastDetails(1, "Contoso.App!App", "Contoso", texts, DateTimeOffset.UnixEpoch), 0);
}
