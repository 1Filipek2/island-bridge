using IslandBridge.Core.Ancs;
using IslandBridge.Core.Configuration;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Tests.Notifications;

public sealed class NotificationFilterTests
{
    private static readonly DateTimeOffset Now = new(2026, 11, 4, 12, 0, 0, TimeSpan.Zero);

    [Fact]
    public void Delivers_by_default()
    {
        Assert.Equal(FilterDecision.Deliver, NotificationFilter.Evaluate(Create(), new NotificationConfig(), Now, null));
    }

    [Fact]
    public void Pause_wins_until_it_expires()
    {
        var config = new NotificationConfig();

        Assert.Equal(FilterDecision.Paused, NotificationFilter.Evaluate(Create(), config, Now, Now.AddMinutes(1)));
        Assert.Equal(FilterDecision.Deliver, NotificationFilter.Evaluate(Create(), config, Now, Now));
    }

    [Fact]
    public void Silent_is_ignored_only_when_configured()
    {
        var silent = Create(flags: AncsEventFlags.Silent);

        Assert.Equal(FilterDecision.Silent, NotificationFilter.Evaluate(silent, new NotificationConfig { IgnoreSilent = true }, Now, null));
        Assert.Equal(FilterDecision.Deliver, NotificationFilter.Evaluate(silent, new NotificationConfig { IgnoreSilent = false }, Now, null));
    }

    [Fact]
    public void Muted_apps_and_categories_are_dropped()
    {
        var config = new NotificationConfig { MutedApps = ["com.muted"], MutedCategories = [AncsCategory.News] };

        Assert.Equal(FilterDecision.MutedApp, NotificationFilter.Evaluate(Create(app: "com.muted"), config, Now, null));
        Assert.Equal(FilterDecision.MutedCategory, NotificationFilter.Evaluate(Create(category: AncsCategory.News), config, Now, null));
        Assert.Equal(FilterDecision.Deliver, NotificationFilter.Evaluate(Create(app: "com.Muted"), config, Now, null));
    }

    private static IslandNotification Create(
        string app = "com.example",
        AncsCategory category = AncsCategory.Social,
        AncsEventFlags flags = AncsEventFlags.None) =>
        new(1, category, flags, app, "Example", "Title", string.Empty, "Message", null, 0);
}
