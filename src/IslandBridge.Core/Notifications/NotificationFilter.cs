using IslandBridge.Core.Ancs;
using IslandBridge.Core.Configuration;

namespace IslandBridge.Core.Notifications;

public enum FilterDecision
{
    Deliver,
    Paused,
    Silent,
    MutedApp,
    MutedCategory,
}

public static class NotificationFilter
{
    public static FilterDecision Evaluate(
        IslandNotification notification,
        NotificationConfig config,
        DateTimeOffset now,
        DateTimeOffset? pausedUntil)
    {
        ArgumentNullException.ThrowIfNull(notification);
        ArgumentNullException.ThrowIfNull(config);

        if (pausedUntil is { } until && now < until)
        {
            return FilterDecision.Paused;
        }

        if (config.IgnoreSilent && notification.Flags.HasFlag(AncsEventFlags.Silent))
        {
            return FilterDecision.Silent;
        }

        if (config.MutedApps.Contains(notification.AppIdentifier, StringComparer.Ordinal))
        {
            return FilterDecision.MutedApp;
        }

        if (config.MutedCategories.Contains(notification.Category))
        {
            return FilterDecision.MutedCategory;
        }

        return FilterDecision.Deliver;
    }
}
