using System.Diagnostics;
using IslandBridge.Core;
using IslandBridge.Core.Ancs;
using IslandBridge.Core.Configuration;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Island;
using IslandBridge.Core.Notifications;
using IslandBridge.Core.Startup;
using IslandBridge.Core.Toasts;

namespace IslandBridge;

internal enum HostStatus
{
    NoDevice,
    Delayed,
    Connecting,
    Ready,
    WaitingForDevice,
    PermissionRequired,
    Retrying,
    Stopped,
}

internal enum PauseOption
{
    None,
    Minutes15,
    Hour1,
    Hours8,
    UntilResumed,
}

internal sealed record RecentNotification(
    DateTimeOffset ReceivedAt,
    string AppName,
    AncsCategory Category,
    NotificationOrigin Origin,
    string Title,
    string Message,
    FilterDecision Decision);

internal sealed class AppHost : IAsyncDisposable
{
    public const int RecentCapacity = 20;

    private readonly Lock _gate = new();
    private readonly ConfigStore _store;
    private readonly AppNameCache _appNames;
    private readonly List<RecentNotification> _recent = [];
    private readonly TimeProvider _timeProvider = TimeProvider.System;
    private CancellationTokenSource? _bluetoothCancellation;
    private Task _bluetoothTask = Task.CompletedTask;
    private HostStatus _status = HostStatus.Stopped;
    private LinkResult? _lastFailure;
    private DateTimeOffset? _pausedUntil;
    private PauseOption _pauseOption;
    private int _appNamesAtLastSave;
    private readonly CancellationTokenSource _lifetime = new();
    private readonly IslandPipeClient _island;
    private readonly Task _islandTask;
    private CancellationTokenSource? _toastCancellation;
    private Task _toastTask = Task.CompletedTask;
    private ToastWatcherState _toastState;

    public AppHost(AppPaths paths, FileLog log)
    {
        ArgumentNullException.ThrowIfNull(paths);
        ArgumentNullException.ThrowIfNull(log);

        Paths = paths;
        Log = log;
        _store = new ConfigStore(paths.ConfigFile, log);
        Config = _store.Load();
        log.Info($"config {_store.LastOutcome}");
        _appNames = new AppNameCache();

        foreach (var (identifier, name) in Config.AppNames)
        {
            _appNames.Set(identifier, name);
        }

        _appNamesAtLastSave = _appNames.Count;
        Localizer = new Localizer(Config.Appearance.Language);
        _island = new IslandPipeClient(log);
        _islandTask = Task.Run(() => _island.RunAsync(_lifetime.Token));
    }

    public event EventHandler? StatusChanged;

    public event EventHandler? NotificationsChanged;

    public event EventHandler? ConfigChanged;

    public event EventHandler? ShowWindowRequested;

    public event EventHandler? ExitRequested;

    public AppPaths Paths { get; }

    public FileLog Log { get; }

    public AppConfig Config { get; }

    public Localizer Localizer { get; }

    public AutostartRegistration Autostart { get; } = new();

    public string ExecutablePath { get; } = Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "IslandBridge.exe");

    public string IconPath { get; } = Path.Combine(AppContext.BaseDirectory, "Assets", "IslandBridge.ico");

    public HostStatus Status
    {
        get
        {
            lock (_gate)
            {
                return _status;
            }
        }
    }

    public LinkResult? LastFailure
    {
        get
        {
            lock (_gate)
            {
                return _lastFailure;
            }
        }
    }

    public DateTimeOffset? PausedUntil
    {
        get
        {
            lock (_gate)
            {
                return _pausedUntil is { } until && until > _timeProvider.GetUtcNow() ? until : null;
            }
        }
    }

    public PauseOption CurrentPause => PausedUntil is null ? PauseOption.None : _pauseOption;

    public ToastWatcherState ToastState
    {
        get
        {
            lock (_gate)
            {
                return _toastState;
            }
        }
    }

    public IReadOnlyList<RecentNotification> Recent
    {
        get
        {
            lock (_gate)
            {
                return [.. _recent];
            }
        }
    }

    public void CompleteFirstRun()
    {
        if (Config.FirstRunCompleted)
        {
            Autostart.RefreshPath(ExecutablePath);
            return;
        }

        try
        {
            Autostart.Enable(ExecutablePath);
            Log.Info("first run: autostart enabled");
        }
        catch (UnauthorizedAccessException exception)
        {
            Log.Error($"first run: autostart failed: {exception.GetType().Name}");
        }

        UpdateConfig(config => config.FirstRunCompleted = true);
    }

    public void UpdateConfig(Action<AppConfig> change)
    {
        ArgumentNullException.ThrowIfNull(change);

        lock (_gate)
        {
            change(Config);
            Config.Normalize();
            SaveLocked();
        }

        Localizer.SetLanguage(Config.Appearance.Language);
        SyncWindowsToasts();
        ConfigChanged?.Invoke(this, EventArgs.Empty);
    }

    public void SyncWindowsToasts()
    {
        CancellationTokenSource? stop = null;
        CancellationTokenSource? start = null;

        lock (_gate)
        {
            var enabled = Config.Notifications.WindowsToasts;

            if (enabled && _toastCancellation is null)
            {
                _toastCancellation = start = new CancellationTokenSource();
            }
            else if (!enabled && _toastCancellation is not null)
            {
                stop = _toastCancellation;
                _toastCancellation = null;
            }
        }

        if (stop is not null)
        {
            Log.Info("windows toasts: disabled");
            CancelQuietly(stop);
        }

        if (start is not null)
        {
            var previous = _toastTask;
            _toastTask = Task.Run(async () =>
            {
                await previous.ConfigureAwait(false);
                await RunWindowsToastsAsync(start).ConfigureAwait(false);
            });
        }
    }

    public void StartBluetooth(TimeSpan delay)
    {
        var deviceId = Config.Device.DeviceId;
        CancellationTokenSource cancellation;

        lock (_gate)
        {
            _bluetoothCancellation?.Cancel();
            _bluetoothCancellation?.Dispose();
            _bluetoothCancellation = cancellation = new CancellationTokenSource();
        }

        if (deviceId is null)
        {
            SetStatus(HostStatus.NoDevice, null);
            return;
        }

        SetStatus(delay > TimeSpan.Zero ? HostStatus.Delayed : HostStatus.Connecting, null);
        var previous = _bluetoothTask;
        _bluetoothTask = Task.Run(async () =>
        {
            await previous.ConfigureAwait(false);
            await RunBluetoothAsync(deviceId, delay, cancellation.Token).ConfigureAwait(false);
        });
    }

    public void UseDevice(AncsDeviceCandidate device)
    {
        ArgumentNullException.ThrowIfNull(device);
        Log.Info("device selected");
        UpdateConfig(config =>
        {
            config.Device.DeviceId = device.DeviceId;
            config.Device.Name = device.Name;
        });
        StartBluetooth(TimeSpan.Zero);
    }

    public void ForgetDevice()
    {
        Log.Info("device forgotten");
        UpdateConfig(config => config.Device = new DeviceConfig());
        StartBluetooth(TimeSpan.Zero);
    }

    public void Pause(PauseOption option)
    {
        if (option == PauseOption.None)
        {
            Resume();
            return;
        }

        TimeSpan? duration = option switch
        {
            PauseOption.Minutes15 => TimeSpan.FromMinutes(15),
            PauseOption.Hour1 => TimeSpan.FromHours(1),
            PauseOption.Hours8 => TimeSpan.FromHours(8),
            _ => null,
        };

        lock (_gate)
        {
            _pauseOption = option;
            _pausedUntil = duration is { } value ? _timeProvider.GetUtcNow() + value : DateTimeOffset.MaxValue;
        }

        Log.Info($"paused: {option}");
        StatusChanged?.Invoke(this, EventArgs.Empty);
    }

    public void Resume()
    {
        lock (_gate)
        {
            _pausedUntil = null;
        }

        Log.Info("resumed");
        StatusChanged?.Invoke(this, EventArgs.Empty);
    }

    public void SendTestNotification()
    {
        var notification = new IslandNotification(
            0,
            AncsCategory.Other,
            AncsEventFlags.None,
            "dev.islandbridge.test",
            "IslandBridge",
            Localizer["Test_Title"],
            string.Empty,
            Localizer["Test_Message"],
            DateTime.Now,
            Stopwatch.GetTimestamp());
        Deliver(notification, FilterDecision.Deliver);
        Log.Info("test notification sent");
    }

    public void RequestShowWindow() => ShowWindowRequested?.Invoke(this, EventArgs.Empty);

    public void RequestExit() => ExitRequested?.Invoke(this, EventArgs.Empty);

    public async ValueTask DisposeAsync()
    {
        CancellationTokenSource? cancellation;

        lock (_gate)
        {
            cancellation = _bluetoothCancellation;
            _bluetoothCancellation = null;
        }

        if (cancellation is not null)
        {
            await cancellation.CancelAsync().ConfigureAwait(false);
        }

        try
        {
            await _bluetoothTask.WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            Log.Warn("bluetooth shutdown timed out");
        }
        catch (OperationCanceledException)
        {
        }

        cancellation?.Dispose();
        CancellationTokenSource? toasts;

        lock (_gate)
        {
            toasts = _toastCancellation;
            _toastCancellation = null;
        }

        if (toasts is not null)
        {
            CancelQuietly(toasts);
        }

        try
        {
            await _toastTask.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            Log.Warn("windows toasts shutdown timed out");
        }

        await _lifetime.CancelAsync().ConfigureAwait(false);

        try
        {
            await _islandTask.WaitAsync(TimeSpan.FromSeconds(2)).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            Log.Warn("island pipe shutdown timed out");
        }
        catch (OperationCanceledException)
        {
        }

        await _island.DisposeAsync().ConfigureAwait(false);
        _lifetime.Dispose();

        lock (_gate)
        {
            SaveLocked();
        }
    }

    private async Task RunBluetoothAsync(string deviceId, TimeSpan delay, CancellationToken cancellationToken)
    {
        try
        {
            if (delay > TimeSpan.Zero)
            {
                Log.Info($"bluetooth start delayed by {delay.TotalSeconds:F0} s");
                await Task.Delay(delay, _timeProvider, cancellationToken).ConfigureAwait(false);
            }

            await using var link = new WinRtAncsLink(deviceId);
            using var client = new AncsClient(link, _appNames, _timeProvider, ReconnectBackoff.Default);
            client.Supervisor.StateChanged += OnSupervisorStateChanged;
            client.Supervisor.StateChanged += (_, args) =>
            {
                if (args.State == ConnectionState.Ready)
                {
                    RefreshDeviceName(deviceId, link.DeviceName);
                }
            };
            var run = client.RunAsync(cancellationToken);

            await foreach (var notification in client.Notifications.ReadAllAsync(cancellationToken).ConfigureAwait(false))
            {
                var decision = NotificationFilter.Evaluate(notification, Config.Notifications, _timeProvider.GetUtcNow(), PausedUntil);
                Log.Info($"notification uid={notification.Uid} app={notification.AppIdentifier} category={notification.Category} {decision}");
                Deliver(notification, decision);
                PersistNewAppNames();
            }

            await run.ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            Log.Error($"bluetooth loop failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
            SetStatus(HostStatus.Retrying, null);
        }

        if (cancellationToken.IsCancellationRequested)
        {
            SetStatus(Config.Device.DeviceId is null ? HostStatus.NoDevice : HostStatus.Stopped, null);
        }
    }

    private async Task RunWindowsToastsAsync(CancellationTokenSource cancellation)
    {
        var cancellationToken = cancellation.Token;

        try
        {
            var watcher = new ToastWatcher(new WnfToastSignal(), new WinRtToastStore(), _appNames, Log);
            watcher.StateChanged += (_, _) => SetToastState(cancellation, watcher.State);
            var run = watcher.RunAsync(cancellationToken);

            try
            {
                await foreach (var notification in watcher.Notifications.ReadAllAsync(cancellationToken).ConfigureAwait(false))
                {
                    var decision = NotificationFilter.Evaluate(notification, Config.Notifications, _timeProvider.GetUtcNow(), PausedUntil);
                    Log.Info($"toast uid={notification.Uid} app={notification.AppIdentifier} {decision}");
                    Deliver(notification, decision);
                    PersistNewAppNames();
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
            }

            await run.ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            Log.Error($"windows toasts failed: {exception.GetType().Name} 0x{exception.HResult:X8}");
            SetToastState(cancellation, ToastWatcherState.Unavailable);
        }
        finally
        {
            cancellation.Dispose();
        }
    }

    private static void CancelQuietly(CancellationTokenSource cancellation)
    {
        try
        {
            cancellation.Cancel();
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private void SetToastState(CancellationTokenSource owner, ToastWatcherState state)
    {
        lock (_gate)
        {
            if (_toastCancellation is not null && !ReferenceEquals(_toastCancellation, owner))
            {
                return;
            }

            _toastState = state;
        }

        StatusChanged?.Invoke(this, EventArgs.Empty);
    }

    private void RefreshDeviceName(string deviceId, string? reportedName)
    {
        lock (_gate)
        {
            if (!string.Equals(Config.Device.DeviceId, deviceId, StringComparison.Ordinal) || !Config.Device.TryUpdateName(reportedName))
            {
                return;
            }

            SaveLocked();
        }

        Log.Info("device name updated");
        ConfigChanged?.Invoke(this, EventArgs.Empty);
    }

    private void OnSupervisorStateChanged(object? sender, ConnectionStateChangedEventArgs args)
    {
        var status = args.State switch
        {
            ConnectionState.Connecting or ConnectionState.Subscribing => HostStatus.Connecting,
            ConnectionState.Ready => HostStatus.Ready,
            ConnectionState.WaitingForDevice => HostStatus.WaitingForDevice,
            ConnectionState.PermissionRequired => HostStatus.PermissionRequired,
            ConnectionState.RetryScheduled => HostStatus.Retrying,
            _ => HostStatus.Stopped,
        };

        if (status != Status)
        {
            Log.Info($"connection {args.State}{(args.LastFailure is { } failure ? $" ({failure.Status} {failure.ProtocolError:X2})" : string.Empty)}");
        }

        SetStatus(status, args.LastFailure);
    }

    private void Deliver(IslandNotification notification, FilterDecision decision)
    {
        if (decision == FilterDecision.Deliver && !_island.TrySend(notification))
        {
            Log.Warn("island queue rejected a notification");
        }

        var showText = Config.Notifications.ShowPreviews;
        var recent = new RecentNotification(
            _timeProvider.GetLocalNow(),
            notification.AppName.Length > 0 ? notification.AppName : notification.AppIdentifier,
            notification.Category,
            notification.Origin,
            showText ? notification.Title : string.Empty,
            showText ? notification.Message : string.Empty,
            decision);

        lock (_gate)
        {
            _recent.Insert(0, recent);

            if (_recent.Count > RecentCapacity)
            {
                _recent.RemoveAt(_recent.Count - 1);
            }
        }

        NotificationsChanged?.Invoke(this, EventArgs.Empty);
    }

    private void PersistNewAppNames()
    {
        if (_appNames.Count == _appNamesAtLastSave)
        {
            return;
        }

        UpdateConfig(_ => { });
    }

    private void SaveLocked()
    {
        Config.AppNames = _appNames.Snapshot().ToDictionary(entry => entry.Key, entry => entry.Value, StringComparer.Ordinal);
        _appNamesAtLastSave = Config.AppNames.Count;

        try
        {
            _store.Save(Config);
        }
        catch (IOException exception)
        {
            Log.Error($"config save failed: {exception.GetType().Name}");
        }
        catch (UnauthorizedAccessException exception)
        {
            Log.Error($"config save failed: {exception.GetType().Name}");
        }
    }

    private void SetStatus(HostStatus status, LinkResult? failure)
    {
        lock (_gate)
        {
            _status = status;
            _lastFailure = failure;
        }

        StatusChanged?.Invoke(this, EventArgs.Empty);
    }
}
