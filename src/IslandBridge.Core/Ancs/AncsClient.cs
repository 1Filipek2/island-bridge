using System.Diagnostics;
using System.Threading.Channels;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Core.Ancs;

public sealed record AncsClientOptions
{
    public TimeSpan ResponseTimeout { get; init; } = TimeSpan.FromSeconds(5);

    public int EventQueueCapacity { get; init; } = 32;

    public int OutputQueueCapacity { get; init; } = 32;

    public int RemovedUidMemory { get; init; } = 64;
}

public sealed class AncsClient : IDisposable
{
    private readonly IAncsLink _link;
    private readonly AppNameCache _appNames;
    private readonly AncsRequester _requester;
    private readonly Channel<(NotificationSourceEvent Event, long ReceivedAt)> _events;
    private readonly Channel<IslandNotification> _notifications;
    private readonly Lock _removedGate = new();
    private readonly HashSet<uint> _removed = [];
    private readonly Queue<uint> _removedOrder = new();
    private readonly int _removedCapacity;
    private long _malformedEvents;
    private long _failedRequests;

    public AncsClient(IAncsLink link, AppNameCache appNames, TimeProvider timeProvider, ReconnectBackoff backoff, AncsClientOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(link);
        ArgumentNullException.ThrowIfNull(appNames);
        ArgumentNullException.ThrowIfNull(timeProvider);
        ArgumentNullException.ThrowIfNull(backoff);

        options ??= new AncsClientOptions();
        _link = link;
        _appNames = appNames;
        _removedCapacity = options.RemovedUidMemory;
        _requester = new AncsRequester(link, timeProvider, options.ResponseTimeout);
        _events = Channel.CreateBounded<(NotificationSourceEvent, long)>(new BoundedChannelOptions(options.EventQueueCapacity)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
        });
        _notifications = Channel.CreateBounded<IslandNotification>(new BoundedChannelOptions(options.OutputQueueCapacity)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleReader = true,
        });
        Supervisor = new ConnectionSupervisor(link, timeProvider, backoff, OnLinkLost);
    }

    public ConnectionSupervisor Supervisor { get; }

    public ChannelReader<IslandNotification> Notifications => _notifications.Reader;

    public long MalformedEvents => Interlocked.Read(ref _malformedEvents);

    public long FailedRequests => Interlocked.Read(ref _failedRequests);

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        _link.NotificationSourceReceived += OnNotificationSource;
        _link.DataSourceReceived += _requester.OnDataSource;

        try
        {
            var supervisor = Supervisor.RunAsync(cancellationToken);
            var processor = ProcessAsync(cancellationToken);
            await Task.WhenAll(supervisor, processor).ConfigureAwait(false);
        }
        finally
        {
            _link.NotificationSourceReceived -= OnNotificationSource;
            _link.DataSourceReceived -= _requester.OnDataSource;
            _notifications.Writer.TryComplete();
        }
    }

    public void Dispose() => _requester.Dispose();

    internal void OnNotificationSource(ReadOnlySpan<byte> packet)
    {
        var receivedAt = Stopwatch.GetTimestamp();

        if (!NotificationSourceEvent.TryParse(packet, out var notification))
        {
            Interlocked.Increment(ref _malformedEvents);
            return;
        }

        if (notification.EventId == AncsEventId.Removed)
        {
            RememberRemoved(notification.Uid);
            return;
        }

        if (notification.IsNew)
        {
            _events.Writer.TryWrite((notification, receivedAt));
        }
    }

    private async Task ProcessAsync(CancellationToken cancellationToken)
    {
        await foreach (var (notification, receivedAt) in _events.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
        {
            if (WasRemoved(notification.Uid))
            {
                continue;
            }

            var attributes = await _requester.GetNotificationAttributesAsync(notification.Uid, cancellationToken).ConfigureAwait(false);

            if (!attributes.IsSuccess)
            {
                if (attributes.Status != RequestStatus.UnknownNotification)
                {
                    Interlocked.Increment(ref _failedRequests);
                }

                continue;
            }

            var value = attributes.Value!;
            var appName = await ResolveAppNameAsync(value.AppIdentifier, cancellationToken).ConfigureAwait(false);

            _notifications.Writer.TryWrite(new IslandNotification(
                notification.Uid,
                notification.Category,
                notification.Flags,
                value.AppIdentifier,
                appName,
                value.Title,
                value.Subtitle,
                value.Message,
                value.Date,
                receivedAt));
        }
    }

    private async Task<string> ResolveAppNameAsync(string appIdentifier, CancellationToken cancellationToken)
    {
        if (appIdentifier.Length == 0)
        {
            return string.Empty;
        }

        if (_appNames.TryGet(appIdentifier, out var cached))
        {
            return cached;
        }

        var result = await _requester.GetAppDisplayNameAsync(appIdentifier, cancellationToken).ConfigureAwait(false);

        if (result is { IsSuccess: true, Value.Length: > 0 })
        {
            _appNames.Set(appIdentifier, result.Value);
            return result.Value;
        }

        return appIdentifier;
    }

    private void OnLinkLost()
    {
        while (_events.Reader.TryRead(out _))
        {
        }

        _requester.CancelPending();

        lock (_removedGate)
        {
            _removed.Clear();
            _removedOrder.Clear();
        }
    }

    private void RememberRemoved(uint uid)
    {
        lock (_removedGate)
        {
            if (!_removed.Add(uid))
            {
                return;
            }

            _removedOrder.Enqueue(uid);

            while (_removedOrder.Count > _removedCapacity)
            {
                _removed.Remove(_removedOrder.Dequeue());
            }
        }
    }

    private bool WasRemoved(uint uid)
    {
        lock (_removedGate)
        {
            return _removed.Contains(uid);
        }
    }
}
