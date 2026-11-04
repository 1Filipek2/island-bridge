using System.Diagnostics;
using System.Threading.Channels;
using IslandBridge.Core.Ancs;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Core.Toasts;

public enum ToastWatcherState
{
    Stopped,
    Starting,
    Running,
    AccessDenied,
    Unavailable,
}

public sealed class ToastWatcher
{
    public const int OutputCapacity = 16;
    public const int MaxDetailsPerScan = 16;
    public const string PhoneLinkAppId = "Microsoft.YourPhone_8wekyb3d8bbwe!App";

    private readonly IToastSignal _signal;
    private readonly IToastStore _store;
    private readonly AppNameCache _appNames;
    private readonly ILog _log;
    private readonly Channel<long> _signals = Channel.CreateBounded<long>(new BoundedChannelOptions(1)
    {
        FullMode = BoundedChannelFullMode.DropWrite,
        SingleReader = true,
    });

    private readonly Channel<IslandNotification> _output = Channel.CreateBounded<IslandNotification>(new BoundedChannelOptions(OutputCapacity)
    {
        FullMode = BoundedChannelFullMode.DropOldest,
        SingleReader = true,
    });

    private HashSet<uint>? _seen;
    private volatile ToastWatcherState _state;
    private long _failedDetails;
    private long _failedScans;

    public ToastWatcher(IToastSignal signal, IToastStore store, AppNameCache appNames, ILog log)
    {
        ArgumentNullException.ThrowIfNull(signal);
        ArgumentNullException.ThrowIfNull(store);
        ArgumentNullException.ThrowIfNull(appNames);
        ArgumentNullException.ThrowIfNull(log);

        _signal = signal;
        _store = store;
        _appNames = appNames;
        _log = log;
    }

    public event EventHandler? StateChanged;

    public ToastWatcherState State => _state;

    public ChannelReader<IslandNotification> Notifications => _output.Reader;

    public long FailedDetails => Interlocked.Read(ref _failedDetails);

    public long FailedScans => Interlocked.Read(ref _failedScans);

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        var finalState = ToastWatcherState.Stopped;

        try
        {
            SetState(ToastWatcherState.Starting);
            var access = await _store.RequestAccessAsync(cancellationToken).ConfigureAwait(false);

            if (access != ToastAccess.Allowed)
            {
                _log.Warn($"windows toasts: access {access}");
                finalState = ToastWatcherState.AccessDenied;
                return;
            }

            if (!_signal.TryStart(OnSignal))
            {
                _log.Error("windows toasts: change signal unavailable");
                finalState = ToastWatcherState.Unavailable;
                return;
            }

            await ScanAsync(Stopwatch.GetTimestamp(), cancellationToken).ConfigureAwait(false);
            SetState(ToastWatcherState.Running);
            _log.Info($"windows toasts: watching ({_seen?.Count ?? 0} existing)");

            await foreach (var signaledAt in _signals.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
            {
                await ScanAsync(signaledAt, cancellationToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception) when (exception is not OutOfMemoryException and not OperationCanceledException)
        {
            _log.Error($"windows toasts: failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
            finalState = ToastWatcherState.Unavailable;
        }
        finally
        {
            _signal.Dispose();
            _output.Writer.TryComplete();
            SetState(finalState);
        }
    }

    private void OnSignal() => _signals.Writer.TryWrite(Stopwatch.GetTimestamp());

    private async Task ScanAsync(long signaledAt, CancellationToken cancellationToken)
    {
        IReadOnlyList<uint> ids;

        try
        {
            ids = await _store.ListIdsAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not OutOfMemoryException)
        {
            Interlocked.Increment(ref _failedScans);
            _log.Warn($"windows toasts: list failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
            return;
        }

        var previous = _seen;
        _seen = [.. ids];

        if (previous is null)
        {
            return;
        }

        List<uint> fresh = [.. ids.Where(id => !previous.Contains(id))];

        if (fresh.Count > MaxDetailsPerScan)
        {
            _log.Warn($"windows toasts: {fresh.Count - MaxDetailsPerScan} skipped in a burst");
            fresh = fresh[^MaxDetailsPerScan..];
        }

        foreach (var id in fresh)
        {
            Publish(id, signaledAt);
        }
    }

    private void Publish(uint id, long signaledAt)
    {
        ToastDetails? details;

        try
        {
            details = _store.GetDetails(id);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            _log.Warn($"windows toasts: details failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
            details = null;
        }

        if (details is null)
        {
            Interlocked.Increment(ref _failedDetails);
            return;
        }

        if (string.Equals(details.AppUserModelId, PhoneLinkAppId, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        if (ToastText.Map(details, signaledAt) is not { } notification)
        {
            return;
        }

        if (notification.AppIdentifier.Length > 0 && notification.AppName != ToastText.FallbackAppName)
        {
            _appNames.Set(notification.AppIdentifier, notification.AppName);
        }

        _output.Writer.TryWrite(notification);
    }

    private void SetState(ToastWatcherState state)
    {
        if (_state == state)
        {
            return;
        }

        _state = state;
        StateChanged?.Invoke(this, EventArgs.Empty);
    }
}
