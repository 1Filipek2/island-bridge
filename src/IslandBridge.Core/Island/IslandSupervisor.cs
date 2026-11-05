using IslandBridge.Core.Diagnostics;

namespace IslandBridge.Core.Island;

public enum IslandProcessState
{
    Stopped,
    Missing,
    Running,
    External,
    Restarting,
}

public interface IIslandProcess : IDisposable
{
    Task<int> WaitForExitAsync(CancellationToken cancellationToken);

    void Kill();
}

public interface IIslandLauncher
{
    bool Exists { get; }

    IIslandProcess Start();

    void RequestStop();

    Task WaitForOtherInstanceAsync(CancellationToken cancellationToken);
}

public sealed class IslandSupervisor
{
    public static readonly TimeSpan AlreadyRunningWindow = TimeSpan.FromSeconds(3);
    public static readonly TimeSpan StableAfter = TimeSpan.FromMinutes(2);
    public static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(3);

    private static readonly TimeSpan[] Backoff =
    [
        TimeSpan.FromSeconds(1),
        TimeSpan.FromSeconds(2),
        TimeSpan.FromSeconds(5),
        TimeSpan.FromSeconds(10),
        TimeSpan.FromSeconds(30),
    ];

    private readonly IIslandLauncher _launcher;
    private readonly ILog _log;
    private readonly TimeProvider _timeProvider;
    private IslandProcessState _state;

    public IslandSupervisor(IIslandLauncher launcher, ILog log, TimeProvider? timeProvider = null)
    {
        ArgumentNullException.ThrowIfNull(launcher);
        ArgumentNullException.ThrowIfNull(log);

        _launcher = launcher;
        _log = log;
        _timeProvider = timeProvider ?? TimeProvider.System;
    }

    public event EventHandler? StateChanged;

    public IslandProcessState State => _state;

    public int Restarts { get; private set; }

    public static TimeSpan DelayAfter(int failures) => Backoff[Math.Clamp(failures, 1, Backoff.Length) - 1];

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        if (!_launcher.Exists)
        {
            _log.Warn("island executable missing");
            SetState(IslandProcessState.Missing);
            return;
        }

        var failures = 0;

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var started = _timeProvider.GetTimestamp();
                var exitCode = await RunOnceAsync(cancellationToken).ConfigureAwait(false);

                if (exitCode is null)
                {
                    return;
                }

                var lived = _timeProvider.GetElapsedTime(started);

                if (exitCode == 0 && lived < AlreadyRunningWindow)
                {
                    _log.Info("island already running, waiting for it to exit");
                    SetState(IslandProcessState.External);
                    await _launcher.WaitForOtherInstanceAsync(cancellationToken).ConfigureAwait(false);
                    failures = 0;
                    continue;
                }

                failures = lived >= StableAfter ? 1 : failures + 1;
                var delay = DelayAfter(failures);
                _log.Warn($"island exited with code {exitCode} after {lived.TotalSeconds:F1} s, restart in {delay.TotalSeconds:F0} s");
                SetState(IslandProcessState.Restarting);
                await Task.Delay(delay, _timeProvider, cancellationToken).ConfigureAwait(false);
                Restarts++;
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            SetState(IslandProcessState.Stopped);
        }
    }

    private async Task<int?> RunOnceAsync(CancellationToken cancellationToken)
    {
        IIslandProcess process;

        try
        {
            process = _launcher.Start();
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception or IOException)
        {
            _log.Warn($"island start failed: {exception.GetType().Name}");
            return -1;
        }

        using (process)
        {
            SetState(IslandProcessState.Running);

            try
            {
                return await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                await StopAsync(process).ConfigureAwait(false);
                return null;
            }
        }
    }

    private async Task StopAsync(IIslandProcess process)
    {
        _launcher.RequestStop();

        try
        {
            await process.WaitForExitAsync(CancellationToken.None).WaitAsync(StopTimeout, _timeProvider).ConfigureAwait(false);
            _log.Info("island stopped");
        }
        catch (TimeoutException)
        {
            _log.Warn("island did not stop in time, killing it");
            process.Kill();
        }
    }

    private void SetState(IslandProcessState state)
    {
        if (_state == state)
        {
            return;
        }

        _state = state;
        StateChanged?.Invoke(this, EventArgs.Empty);
    }
}
