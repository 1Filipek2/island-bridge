using System.Threading.Channels;
using IslandBridge.Core.Ancs;
using Microsoft.Extensions.Time.Testing;

namespace IslandBridge.Tests.Ancs;

public sealed class ConnectionSupervisorTests
{
    private static readonly TimeSpan WaitLimit = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task Opens_subscribes_and_becomes_ready()
    {
        await using var harness = new Harness();

        await harness.WaitForAsync(ConnectionState.Ready);

        Assert.Equal(1, harness.Link.OpenCalls);
        Assert.Equal(1, harness.Link.SubscribeCalls);
        Assert.Null(harness.Supervisor.LastFailure);
    }

    [Fact]
    public async Task Retries_with_backoff_until_subscribe_succeeds()
    {
        await using var harness = new Harness(link => link.EnqueueSubscribe(
            new LinkResult(LinkStatus.Unreachable),
            new LinkResult(LinkStatus.ProtocolError, 0x0E)));

        var waiting = await harness.WaitForAsync(ConnectionState.WaitingForDevice);
        Assert.Equal(TimeSpan.FromSeconds(1), waiting.RetryIn);

        harness.Time.Advance(TimeSpan.FromSeconds(1));
        var retry = await harness.WaitForAsync(ConnectionState.RetryScheduled);
        Assert.Equal(TimeSpan.FromSeconds(2), retry.RetryIn);

        harness.Time.Advance(TimeSpan.FromSeconds(2));
        await harness.WaitForAsync(ConnectionState.Ready);

        Assert.Equal(3, harness.Link.SubscribeCalls);
    }

    [Theory]
    [InlineData(LinkStatus.AccessDenied, null)]
    [InlineData(LinkStatus.ProtocolError, AncsErrorCodes.InsufficientAuthentication)]
    [InlineData(LinkStatus.ProtocolError, AncsErrorCodes.InsufficientEncryption)]
    public async Task Reports_permission_problems(LinkStatus status, byte? protocolError)
    {
        await using var harness = new Harness(link => link.EnqueueSubscribe(new LinkResult(status, protocolError)));

        var state = await harness.WaitForAsync(ConnectionState.PermissionRequired);

        Assert.Equal(status, state.LastFailure!.Value.Status);
        Assert.NotNull(state.RetryIn);
    }

    [Fact]
    public async Task Open_failure_skips_subscribe()
    {
        await using var harness = new Harness(link => link.EnqueueOpen(new LinkResult(LinkStatus.Unreachable)));

        await harness.WaitForAsync(ConnectionState.WaitingForDevice);

        Assert.Equal(0, harness.Link.SubscribeCalls);
    }

    [Fact]
    public async Task Disconnect_cancels_pending_work_and_reconnect_resubscribes()
    {
        await using var harness = new Harness();
        await harness.WaitForAsync(ConnectionState.Ready);

        harness.Link.RaiseConnection(false);
        await harness.WaitForAsync(ConnectionState.WaitingForDevice);
        Assert.Equal(1, harness.LinkLostCount);

        harness.Link.RaiseConnection(true);
        await harness.WaitForAsync(ConnectionState.Ready);

        Assert.Equal(2, harness.Link.SubscribeCalls);
    }

    [Fact]
    public async Task Services_changed_resubscribes()
    {
        await using var harness = new Harness();
        await harness.WaitForAsync(ConnectionState.Ready);

        harness.Link.RaiseServicesChanged();
        await harness.WaitForAsync(ConnectionState.Subscribing);
        await harness.WaitForAsync(ConnectionState.Ready);

        Assert.Equal(2, harness.Link.SubscribeCalls);
    }

    [Fact]
    public async Task Successful_attempt_resets_the_backoff()
    {
        await using var harness = new Harness(link => link.EnqueueSubscribe(new LinkResult(LinkStatus.Unreachable)));
        await harness.WaitForAsync(ConnectionState.WaitingForDevice);
        harness.Time.Advance(TimeSpan.FromSeconds(1));
        await harness.WaitForAsync(ConnectionState.Ready);

        harness.Link.EnqueueSubscribe(new LinkResult(LinkStatus.Unreachable));
        harness.Link.RaiseServicesChanged();
        var waiting = await harness.WaitForAsync(ConnectionState.WaitingForDevice);

        Assert.Equal(TimeSpan.FromSeconds(1), waiting.RetryIn);
    }

    [Fact]
    public async Task Stops_when_cancelled()
    {
        var harness = new Harness();
        await harness.WaitForAsync(ConnectionState.Ready);

        await harness.DisposeAsync();

        Assert.Equal(ConnectionState.Stopped, harness.Supervisor.State);
    }

    private sealed class Harness : IAsyncDisposable
    {
        private readonly CancellationTokenSource _cancellation = new();
        private readonly Channel<ConnectionStateChangedEventArgs> _states = Channel.CreateUnbounded<ConnectionStateChangedEventArgs>();
        private readonly Task _run;
        private int _linkLostCount;

        public Harness(Action<FakeAncsLink>? arrange = null)
        {
            arrange?.Invoke(Link);
            Supervisor = new ConnectionSupervisor(
                Link,
                Time,
                new ReconnectBackoff(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(30)),
                () => Interlocked.Increment(ref _linkLostCount));
            Supervisor.StateChanged += (_, args) => _states.Writer.TryWrite(args);
            _run = Task.Run(() => Supervisor.RunAsync(_cancellation.Token));
        }

        public FakeAncsLink Link { get; } = new();

        public FakeTimeProvider Time { get; } = new();

        public ConnectionSupervisor Supervisor { get; }

        public int LinkLostCount => Volatile.Read(ref _linkLostCount);

        public async Task<ConnectionStateChangedEventArgs> WaitForAsync(ConnectionState state)
        {
            using var timeout = new CancellationTokenSource(WaitLimit);

            while (true)
            {
                var next = await _states.Reader.ReadAsync(timeout.Token);

                if (next.State == state)
                {
                    return next;
                }
            }
        }

        public async ValueTask DisposeAsync()
        {
            await _cancellation.CancelAsync();

            try
            {
                await _run;
            }
            catch (OperationCanceledException)
            {
            }

            _cancellation.Dispose();
        }
    }
}
