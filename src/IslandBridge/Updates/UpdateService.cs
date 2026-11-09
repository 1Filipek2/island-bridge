using System.Diagnostics;
using IslandBridge.Core.Configuration;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Updates;

namespace IslandBridge.Updates;

internal enum UpdateState
{
    Unavailable,
    Idle,
    Checking,
    UpToDate,
    Available,
    Installing,
    CheckFailed,
    InstallFailed,
}

// IslandBridge itself never touches the network: it starts IslandBridge.Updater.exe from its own
// folder and reads the one result line. A daily timer is the only scheduling.
internal sealed class UpdateService : IDisposable
{
    private static readonly TimeSpan CheckTimeout = TimeSpan.FromMinutes(2);
    private static readonly TimeSpan InstallTimeout = TimeSpan.FromMinutes(20);

    private readonly Lock _gate = new();
    private readonly ILog _log;
    private readonly Func<UpdatesConfig> _config;
    private readonly Action<DateTimeOffset> _recordCheck;
    private readonly TimeProvider _timeProvider;
    private readonly string _updaterPath = Path.Combine(AppContext.BaseDirectory, "IslandBridge.Updater.exe");
    private ITimer? _timer;
    private bool _running;
    private bool _disposed;

    public UpdateService(ILog log, Func<UpdatesConfig> config, Action<DateTimeOffset> recordCheck, TimeProvider timeProvider)
    {
        _log = log;
        _config = config;
        _recordCheck = recordCheck;
        _timeProvider = timeProvider;
        CanInstall = InstallLocation.IsInstalledCopy(AppContext.BaseDirectory);
        State = File.Exists(_updaterPath) && UpdateSignature.HasTrustedKeys ? UpdateState.Idle : UpdateState.Unavailable;
    }

    public event EventHandler? Changed;

    public event EventHandler? ExitForInstallRequested;

    public UpdateState State { get; private set; }

    public UpdateVersion Latest { get; private set; }

    public UpdateFailure Failure { get; private set; }

    public bool CanInstall { get; }

    public static Uri ReleasesPage => UpdateEndpoints.ReleasesPage;

    public void Reschedule()
    {
        lock (_gate)
        {
            if (_disposed || State == UpdateState.Unavailable)
            {
                return;
            }

            _timer?.Dispose();
            _timer = null;

            if (!_config().CheckAutomatically)
            {
                return;
            }

            var delay = UpdateSchedule.NextCheckDelay(_timeProvider.GetUtcNow(), _config().LastCheck);
            _timer = _timeProvider.CreateTimer(_ => _ = CheckAsync(), null, delay, Timeout.InfiniteTimeSpan);
        }
    }

    public async Task CheckAsync()
    {
        if (!TryBegin(UpdateState.Checking))
        {
            return;
        }

        var report = await RunUpdaterAsync([UpdaterCommand.CheckVerb], CheckTimeout, waitForReady: false).ConfigureAwait(false);
        _recordCheck(_timeProvider.GetUtcNow());

        switch (report)
        {
            case { Outcome: UpdaterOutcome.Available }:
                Finish(UpdateState.Available, report.Version, UpdateFailure.None);
                break;
            case { Outcome: UpdaterOutcome.Current }:
                Finish(UpdateState.UpToDate, default, UpdateFailure.None);
                break;
            default:
                Finish(UpdateState.CheckFailed, Latest, report?.Failure ?? UpdateFailure.Launch);
                break;
        }

        Reschedule();
    }

    public async Task InstallAsync()
    {
        if (!CanInstall || !TryBegin(UpdateState.Installing))
        {
            return;
        }

        string[] args = [UpdaterCommand.InstallVerb, UpdaterCommand.WaitSwitch, Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture)];
        var report = await RunUpdaterAsync(args, InstallTimeout, waitForReady: true).ConfigureAwait(false);

        switch (report)
        {
            case { Outcome: UpdaterOutcome.Ready }:
                _log.Info($"update {report.Version} verified, exiting for setup");
                Finish(UpdateState.Installing, report.Version, UpdateFailure.None);
                ExitForInstallRequested?.Invoke(this, EventArgs.Empty);
                return;
            case { Outcome: UpdaterOutcome.Current }:
                Finish(UpdateState.UpToDate, default, UpdateFailure.None);
                break;
            default:
                Finish(UpdateState.InstallFailed, Latest, report?.Failure ?? UpdateFailure.Launch);
                break;
        }

        Reschedule();
    }

    public void Dispose()
    {
        lock (_gate)
        {
            _disposed = true;
            _timer?.Dispose();
            _timer = null;
        }
    }

    private bool TryBegin(UpdateState state)
    {
        lock (_gate)
        {
            if (_disposed || _running || State == UpdateState.Unavailable)
            {
                return false;
            }

            _running = true;
            State = state;
        }

        Changed?.Invoke(this, EventArgs.Empty);
        return true;
    }

    private void Finish(UpdateState state, UpdateVersion latest, UpdateFailure failure)
    {
        lock (_gate)
        {
            _running = state == UpdateState.Installing;
            State = state;
            Latest = latest;
            Failure = failure;
        }

        if (failure != UpdateFailure.None)
        {
            _log.Warn($"update {state}: {failure}");
        }

        Changed?.Invoke(this, EventArgs.Empty);
    }

    private async Task<UpdaterReport?> RunUpdaterAsync(IReadOnlyList<string> args, TimeSpan timeout, bool waitForReady)
    {
        var info = new ProcessStartInfo(_updaterPath)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            WorkingDirectory = AppContext.BaseDirectory,
        };

        foreach (var arg in args)
        {
            info.ArgumentList.Add(arg);
        }

        using var cancellation = new CancellationTokenSource(timeout);
        Process? process = null;

        try
        {
            process = Process.Start(info);

            if (process is null)
            {
                return null;
            }

            var line = await process.StandardOutput.ReadLineAsync(cancellation.Token).ConfigureAwait(false);

            if (!UpdaterReport.TryParse(line, out var report) || report is null)
            {
                _log.Warn("updater gave no valid result");
                return null;
            }

            // After "ready" the updater waits for this process to exit, so it is not awaited here.
            if (!(waitForReady && report.Outcome == UpdaterOutcome.Ready))
            {
                await process.WaitForExitAsync(cancellation.Token).ConfigureAwait(false);
            }

            return report;
        }
        catch (OperationCanceledException)
        {
            _log.Warn("updater timed out");
            Kill(process);
            return null;
        }
        catch (System.ComponentModel.Win32Exception exception)
        {
            _log.Error($"updater failed to start 0x{exception.NativeErrorCode:X8}");
            return null;
        }
        finally
        {
            process?.Dispose();
        }
    }

    private static void Kill(Process? process)
    {
        try
        {
            process?.Kill();
        }
        catch (InvalidOperationException)
        {
        }
        catch (System.ComponentModel.Win32Exception)
        {
        }
    }
}
