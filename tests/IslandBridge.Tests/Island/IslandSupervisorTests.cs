using System.Threading.Channels;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Island;
using Microsoft.Extensions.Time.Testing;

namespace IslandBridge.Tests.Island;

public sealed class IslandSupervisorTests
{
    private static readonly TimeSpan WaitLimit = TimeSpan.FromSeconds(10);

    private readonly FakeTimeProvider _time = new();
    private readonly FakeLauncher _launcher = new();

    [Fact]
    public async Task Reports_a_missing_executable_and_stops()
    {
        _launcher.Exists = false;
        var supervisor = new IslandSupervisor(_launcher, NullLog.Instance, _time);

        await supervisor.RunAsync(TestContext.Current.CancellationToken).WaitAsync(WaitLimit, TestContext.Current.CancellationToken);

        Assert.Equal(0, _launcher.StartCount);
        Assert.Equal(IslandProcessState.Missing, supervisor.State);
    }

    [Fact]
    public async Task Restarts_a_crashed_island_after_the_backoff()
    {
        var supervisor = new IslandSupervisor(_launcher, NullLog.Instance, _time);
        using var cancellation = new CancellationTokenSource();
        var run = supervisor.RunAsync(cancellation.Token);

        var first = await NextStartAsync();
        Assert.Equal(IslandProcessState.Running, supervisor.State);
        first.Exit(5);

        var second = await NextStartAdvancingAsync(IslandSupervisor.DelayAfter(1));
        Assert.Equal(1, supervisor.Restarts);

        await cancellation.CancelAsync();
        second.Exit(0);
        await run.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
    }

    [Fact]
    public async Task Backoff_grows_with_repeated_crashes()
    {
        Assert.Equal(TimeSpan.FromSeconds(1), IslandSupervisor.DelayAfter(1));
        Assert.Equal(TimeSpan.FromSeconds(2), IslandSupervisor.DelayAfter(2));
        Assert.Equal(TimeSpan.FromSeconds(30), IslandSupervisor.DelayAfter(5));
        Assert.Equal(TimeSpan.FromSeconds(30), IslandSupervisor.DelayAfter(50));

        var supervisor = new IslandSupervisor(_launcher, NullLog.Instance, _time);
        using var cancellation = new CancellationTokenSource();
        var run = supervisor.RunAsync(cancellation.Token);

        (await NextStartAsync()).Exit(1);
        (await NextStartAdvancingAsync(TimeSpan.FromSeconds(1))).Exit(1);
        var third = await NextStartAdvancingAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(3, _launcher.StartCount);

        await cancellation.CancelAsync();
        third.Exit(0);
        await run.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
    }

    [Fact]
    public async Task Waits_for_an_island_that_is_already_running()
    {
        var supervisor = new IslandSupervisor(_launcher, NullLog.Instance, _time);
        using var cancellation = new CancellationTokenSource();
        var run = supervisor.RunAsync(cancellation.Token);

        (await NextStartAsync()).Exit(0);
        await _launcher.OtherInstanceWaits.Reader.ReadAsync(TestContext.Current.CancellationToken).AsTask().WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
        Assert.Equal(IslandProcessState.External, supervisor.State);

        _launcher.OtherInstance.TrySetResult();
        var next = await NextStartAsync();
        Assert.Equal(0, supervisor.Restarts);

        await cancellation.CancelAsync();
        next.Exit(0);
        await run.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
    }

    [Fact]
    public async Task Stops_the_island_gracefully_on_shutdown()
    {
        var supervisor = new IslandSupervisor(_launcher, NullLog.Instance, _time);
        using var cancellation = new CancellationTokenSource();
        var run = supervisor.RunAsync(cancellation.Token);
        var process = await NextStartAsync();
        _launcher.StopRequested += (_, _) => process.Exit(0);

        await cancellation.CancelAsync();
        await run.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);

        Assert.Equal(1, _launcher.StopRequests);
        Assert.False(process.Killed);
        Assert.True(process.Disposed);
        Assert.Equal(IslandProcessState.Stopped, supervisor.State);
    }

    [Fact]
    public async Task Kills_an_island_that_ignores_the_stop_request()
    {
        var supervisor = new IslandSupervisor(_launcher, NullLog.Instance, _time);
        using var cancellation = new CancellationTokenSource();
        var run = supervisor.RunAsync(cancellation.Token);
        var process = await NextStartAsync();
        var stopped = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _launcher.StopRequested += (_, _) => stopped.TrySetResult();

        await cancellation.CancelAsync();
        await stopped.Task.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);

        while (!run.IsCompleted)
        {
            _time.Advance(IslandSupervisor.StopTimeout);
            await Task.WhenAny(run, Task.Delay(10, TestContext.Current.CancellationToken));
        }

        await run;
        Assert.True(process.Killed);
    }

    [Fact]
    public async Task Treats_a_failed_start_as_a_crash()
    {
        _launcher.FailNextStart = true;
        var supervisor = new IslandSupervisor(_launcher, NullLog.Instance, _time);
        using var cancellation = new CancellationTokenSource();
        var run = supervisor.RunAsync(cancellation.Token);

        var process = await NextStartAdvancingAsync(IslandSupervisor.DelayAfter(1));
        Assert.Equal(2, _launcher.StartCount);

        await cancellation.CancelAsync();
        process.Exit(0);
        await run.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
    }

    private async Task<FakeProcess> NextStartAsync() =>
        await _launcher.Started.Reader.ReadAsync(TestContext.Current.CancellationToken).AsTask().WaitAsync(WaitLimit, TestContext.Current.CancellationToken);

    private async Task<FakeProcess> NextStartAdvancingAsync(TimeSpan step)
    {
        var next = _launcher.Started.Reader.ReadAsync(TestContext.Current.CancellationToken).AsTask();
        var deadline = DateTime.UtcNow + WaitLimit;

        while (!next.IsCompleted && DateTime.UtcNow < deadline)
        {
            _time.Advance(step);
            await Task.WhenAny(next, Task.Delay(10, TestContext.Current.CancellationToken));
        }

        return await next.WaitAsync(WaitLimit, TestContext.Current.CancellationToken);
    }

    private sealed class FakeLauncher : IIslandLauncher
    {
        public bool Exists { get; set; } = true;

        public bool FailNextStart { get; set; }

        public int StartCount { get; private set; }

        public int StopRequests { get; private set; }

        public Channel<FakeProcess> Started { get; } = Channel.CreateUnbounded<FakeProcess>();

        public Channel<bool> OtherInstanceWaits { get; } = Channel.CreateUnbounded<bool>();

        public TaskCompletionSource OtherInstance { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public event EventHandler? StopRequested;

        public IIslandProcess Start()
        {
            StartCount++;

            if (FailNextStart)
            {
                FailNextStart = false;
                throw new InvalidOperationException("start failed");
            }

            var process = new FakeProcess();
            Started.Writer.TryWrite(process);
            return process;
        }

        public void RequestStop()
        {
            StopRequests++;
            StopRequested?.Invoke(this, EventArgs.Empty);
        }

        public Task WaitForOtherInstanceAsync(CancellationToken cancellationToken)
        {
            OtherInstanceWaits.Writer.TryWrite(true);
            return OtherInstance.Task.WaitAsync(cancellationToken);
        }
    }

    private sealed class FakeProcess : IIslandProcess
    {
        private readonly TaskCompletionSource<int> _exit = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public bool Killed { get; private set; }

        public bool Disposed { get; private set; }

        public void Exit(int code) => _exit.TrySetResult(code);

        public Task<int> WaitForExitAsync(CancellationToken cancellationToken) => _exit.Task.WaitAsync(cancellationToken);

        public void Kill()
        {
            Killed = true;
            _exit.TrySetResult(-1);
        }

        public void Dispose() => Disposed = true;
    }
}
