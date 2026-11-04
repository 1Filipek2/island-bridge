using System.Globalization;

namespace IslandBridge;

internal enum HintKind
{
    None,
    Informational,
    Warning,
}

internal static class StatusPresenter
{
    public static string Describe(AppHost host)
    {
        ArgumentNullException.ThrowIfNull(host);

        var localizer = host.Localizer;
        var device = host.Config.Device.Name is { Length: > 0 } name ? name : localizer["Device_YourIphone"];

        var status = host.Status switch
        {
            HostStatus.NoDevice => localizer["Status_NoDevice"],
            HostStatus.Delayed => localizer["Status_Delayed"],
            HostStatus.Connecting => localizer.Format("Status_Connecting", device),
            HostStatus.Ready => localizer.Format("Status_Ready", device),
            HostStatus.WaitingForDevice => localizer.Format("Status_WaitingForDevice", device),
            HostStatus.PermissionRequired => localizer.Format("Status_PermissionRequired", device),
            HostStatus.Retrying => localizer["Status_Retrying"],
            _ => localizer["Status_Stopped"],
        };

        return host.PausedUntil is { } until ? $"{status}. {DescribePause(host, until)}" : status;
    }

    public static string DescribePause(AppHost host, DateTimeOffset until)
    {
        ArgumentNullException.ThrowIfNull(host);

        return until == DateTimeOffset.MaxValue
            ? host.Localizer["Status_PausedIndefinitely"]
            : host.Localizer.Format("Status_PausedUntil", until.ToLocalTime().ToString("HH:mm", CultureInfo.InvariantCulture));
    }

    public static (HintKind Kind, string Text) Hint(AppHost host)
    {
        ArgumentNullException.ThrowIfNull(host);

        return host.Status switch
        {
            HostStatus.NoDevice => (HintKind.Informational, host.Localizer["Hint_NoDevice"]),
            HostStatus.WaitingForDevice => (HintKind.Informational, host.Localizer["Hint_Waiting"]),
            HostStatus.PermissionRequired => (HintKind.Warning, host.Localizer["Hint_Permission"]),
            HostStatus.Retrying => (HintKind.Informational, host.Localizer["Hint_Retrying"]),
            _ => (HintKind.None, string.Empty),
        };
    }
}
