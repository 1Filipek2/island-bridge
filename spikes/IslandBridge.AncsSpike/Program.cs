using System.Diagnostics;
using System.Runtime.CompilerServices;
using IslandBridge.AncsSpike;
using IslandBridge.Core.Ancs;

var options = SpikeOptions.Parse(args);
using var cancellation = options.Duration is { } duration ? new CancellationTokenSource(duration) : new CancellationTokenSource();

Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    cancellation.Cancel();
};

SpikeLog.Write($"AncsSpike, native AOT: {!RuntimeFeature.IsDynamicCodeSupported}, show text: {options.ShowText}");

var candidates = await AncsDeviceFinder.FindAsync(cancellation.Token);
var device = candidates.FirstOrDefault(candidate =>
    options.DeviceName is null || candidate.Name.Contains(options.DeviceName, StringComparison.OrdinalIgnoreCase));

foreach (var candidate in candidates)
{
    SpikeLog.Write($"ANCS device: {candidate.Name}, connected: {candidate.IsConnected}");
}

if (device is null)
{
    SpikeLog.Write("no paired device exposes ANCS");
    return 2;
}

SpikeLog.Write($"using {device.Name}");

await using var link = new WinRtAncsLink(device.DeviceId);
link.ConnectionChanged += (_, eventArgs) => SpikeLog.Write($"link connected: {eventArgs.IsConnected}");
link.ServicesChanged += (_, _) => SpikeLog.Write("GATT services changed");

using var client = new AncsClient(link, new AppNameCache(), TimeProvider.System, ReconnectBackoff.Default);
client.Supervisor.StateChanged += (_, eventArgs) =>
{
    var failure = eventArgs.LastFailure is { } result ? $", last failure {result.Status} {result.ProtocolError:X2}" : string.Empty;
    var retry = eventArgs.RetryIn is { } delay ? $", retry in {delay.TotalSeconds:F0} s" : string.Empty;
    SpikeLog.Write($"state: {eventArgs.State}{failure}{retry}");

    if (eventArgs.State == ConnectionState.PermissionRequired)
    {
        SpikeLog.Write("hint: enable 'Share System Notifications' for this PC in iPhone Settings > Bluetooth > (i)");
    }
};

var run = client.RunAsync(cancellation.Token);

try
{
    await foreach (var notification in client.Notifications.ReadAllAsync(cancellation.Token))
    {
        var latency = Stopwatch.GetElapsedTime(notification.ReceivedTimestamp).TotalMilliseconds;
        SpikeLog.Write(options.ShowText
            ? $"NOTIFICATION uid={notification.Uid} {notification.AppName} ({notification.AppIdentifier}) [{notification.Category}] \"{notification.Title}\" \"{notification.Subtitle}\" \"{notification.Message}\" {latency:F1} ms"
            : $"NOTIFICATION uid={notification.Uid} {notification.AppName} ({notification.AppIdentifier}) [{notification.Category}] flags={notification.Flags} title={notification.Title.Length} subtitle={notification.Subtitle.Length} message={notification.Message.Length} chars, {latency:F1} ms");
    }
}
catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
{
}

try
{
    await run;
}
catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
{
}

SpikeLog.Write($"stopped, malformed events: {client.MalformedEvents}, failed requests: {client.FailedRequests}");
return 0;
