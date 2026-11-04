using System.Threading.Channels;

namespace IslandBridge.Core.Ancs;

public enum ConnectionState
{
    Stopped,
    Connecting,
    Subscribing,
    Ready,
    WaitingForDevice,
    PermissionRequired,
    RetryScheduled,
}

public sealed class ConnectionStateChangedEventArgs(ConnectionState state, LinkResult? lastFailure, TimeSpan? retryIn) : EventArgs
{
    public ConnectionState State { get; } = state;

    public LinkResult? LastFailure { get; } = lastFailure;

    public TimeSpan? RetryIn { get; } = retryIn;
}

public sealed class ConnectionSupervisor
{
    private readonly IAncsLink _link;
    private readonly TimeProvider _timeProvider;
    private readonly ReconnectBackoff _backoff;
    private readonly Action _onLinkLost;
    private readonly Channel<Signal> _signals = Channel.CreateUnbounded<Signal>(new UnboundedChannelOptions { SingleReader = true });
    private readonly Lock _timerGate = new();
    private ITimer? _retryTimer;
    private TimeSpan? _retryIn;

    public ConnectionSupervisor(IAncsLink link, TimeProvider timeProvider, ReconnectBackoff backoff, Action onLinkLost)
    {
        ArgumentNullException.ThrowIfNull(link);
        ArgumentNullException.ThrowIfNull(timeProvider);
        ArgumentNullException.ThrowIfNull(backoff);
        ArgumentNullException.ThrowIfNull(onLinkLost);

        _link = link;
        _timeProvider = timeProvider;
        _backoff = backoff;
        _onLinkLost = onLinkLost;
    }

    public event EventHandler<ConnectionStateChangedEventArgs>? StateChanged;

    [Flags]
    private enum Signal
    {
        None = 0,
        Start = 1 << 0,
        Connected = 1 << 1,
        Disconnected = 1 << 2,
        ServicesChanged = 1 << 3,
        Retry = 1 << 4,
    }

    public ConnectionState State { get; private set; } = ConnectionState.Stopped;

    public LinkResult? LastFailure { get; private set; }

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        _link.ConnectionChanged += OnConnectionChanged;
        _link.ServicesChanged += OnServicesChanged;

        try
        {
            Post(Signal.Start);

            while (await _signals.Reader.WaitToReadAsync(cancellationToken).ConfigureAwait(false))
            {
                var signals = Signal.None;

                while (_signals.Reader.TryRead(out var signal))
                {
                    signals |= signal;
                }

                await HandleAsync(signals, cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            _link.ConnectionChanged -= OnConnectionChanged;
            _link.ServicesChanged -= OnServicesChanged;
            CancelRetry();
            SetState(ConnectionState.Stopped);
        }
    }

    private async Task HandleAsync(Signal signals, CancellationToken cancellationToken)
    {
        if (signals.HasFlag(Signal.Disconnected))
        {
            _onLinkLost();

            if (!_link.IsConnected)
            {
                LastFailure = new LinkResult(LinkStatus.Unreachable);
                ScheduleRetry();
                SetState(ConnectionState.WaitingForDevice);
                return;
            }
        }

        var shouldAttempt =
            signals.HasFlag(Signal.Start)
            || signals.HasFlag(Signal.Connected)
            || signals.HasFlag(Signal.ServicesChanged)
            || signals.HasFlag(Signal.Disconnected)
            || signals.HasFlag(Signal.Retry) && State != ConnectionState.Ready;

        if (shouldAttempt)
        {
            await AttemptAsync(cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task AttemptAsync(CancellationToken cancellationToken)
    {
        CancelRetry();
        SetState(ConnectionState.Connecting);
        LinkResult result;

        try
        {
            result = await _link.OpenAsync(cancellationToken).ConfigureAwait(false);

            if (result.IsSuccess)
            {
                SetState(ConnectionState.Subscribing);
                result = await _link.SubscribeAsync(cancellationToken).ConfigureAwait(false);
            }
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            result = new LinkResult(LinkStatus.Unreachable);
        }

        if (result.IsSuccess)
        {
            _backoff.Reset();
            LastFailure = null;
            SetState(ConnectionState.Ready);
            return;
        }

        _onLinkLost();
        LastFailure = result;
        ScheduleRetry();

        SetState(result switch
        {
            { IsPermissionProblem: true } => ConnectionState.PermissionRequired,
            { Status: LinkStatus.Unreachable } => ConnectionState.WaitingForDevice,
            _ => ConnectionState.RetryScheduled,
        });
    }

    private void ScheduleRetry()
    {
        var delay = _backoff.Next();

        lock (_timerGate)
        {
            _retryTimer?.Dispose();
            _retryTimer = _timeProvider.CreateTimer(
                static state => ((ConnectionSupervisor)state!).Post(Signal.Retry),
                this,
                delay,
                Timeout.InfiniteTimeSpan);
            _retryIn = delay;
        }
    }

    private void CancelRetry()
    {
        lock (_timerGate)
        {
            _retryTimer?.Dispose();
            _retryTimer = null;
            _retryIn = null;
        }
    }

    private void SetState(ConnectionState state)
    {
        TimeSpan? retryIn;

        lock (_timerGate)
        {
            retryIn = _retryIn;
        }

        State = state;
        StateChanged?.Invoke(this, new ConnectionStateChangedEventArgs(state, LastFailure, retryIn));
    }

    private void Post(Signal signal) => _signals.Writer.TryWrite(signal);

    private void OnConnectionChanged(object? sender, LinkConnectionChangedEventArgs args) =>
        Post(args.IsConnected ? Signal.Connected : Signal.Disconnected);

    private void OnServicesChanged(object? sender, EventArgs args) => Post(Signal.ServicesChanged);
}
