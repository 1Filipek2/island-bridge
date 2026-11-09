namespace IslandBridge.Core.Updates;

public static class UpdateSchedule
{
    public static TimeSpan Interval { get; } = TimeSpan.FromHours(24);

    // Never right at startup: the first check waits until Bluetooth and the island are settled.
    public static TimeSpan MinimumDelay { get; } = TimeSpan.FromMinutes(2);

    public static TimeSpan NextCheckDelay(DateTimeOffset now, DateTimeOffset? lastCheck)
    {
        if (lastCheck is not { } last || last > now)
        {
            return MinimumDelay;
        }

        var due = last + Interval - now;
        return due > MinimumDelay ? due : MinimumDelay;
    }
}
