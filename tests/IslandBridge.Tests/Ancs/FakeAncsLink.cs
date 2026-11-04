using IslandBridge.Core.Ancs;

namespace IslandBridge.Tests.Ancs;

internal sealed class FakeAncsLink : IAncsLink
{
    private readonly Queue<LinkResult> _openResults = new();
    private readonly Queue<LinkResult> _subscribeResults = new();

    public event AncsPacketHandler? NotificationSourceReceived;

    public event AncsPacketHandler? DataSourceReceived;

    public event EventHandler<LinkConnectionChangedEventArgs>? ConnectionChanged;

    public event EventHandler? ServicesChanged;

    public bool IsConnected { get; set; } = true;

    public int OpenCalls { get; private set; }

    public int SubscribeCalls { get; private set; }

    public List<byte[]> Writes { get; } = [];

    public Func<byte[], (LinkResult Result, IEnumerable<byte[]> Fragments)>? Responder { get; set; }

    public TaskCompletionSource<int> SubscribeObserved { get; private set; } = NewSignal();

    public void EnqueueOpen(params LinkResult[] results)
    {
        foreach (var result in results)
        {
            _openResults.Enqueue(result);
        }
    }

    public void EnqueueSubscribe(params LinkResult[] results)
    {
        foreach (var result in results)
        {
            _subscribeResults.Enqueue(result);
        }
    }

    public void ResetSubscribeSignal() => SubscribeObserved = NewSignal();

    public ValueTask<LinkResult> OpenAsync(CancellationToken cancellationToken)
    {
        OpenCalls++;
        return ValueTask.FromResult(_openResults.TryDequeue(out var result) ? result : LinkResult.Success);
    }

    public ValueTask<LinkResult> SubscribeAsync(CancellationToken cancellationToken)
    {
        SubscribeCalls++;
        var result = _subscribeResults.TryDequeue(out var queued) ? queued : LinkResult.Success;
        SubscribeObserved.TrySetResult(SubscribeCalls);
        return ValueTask.FromResult(result);
    }

    public ValueTask<LinkResult> WriteControlPointAsync(ReadOnlyMemory<byte> command, CancellationToken cancellationToken)
    {
        var bytes = command.ToArray();
        Writes.Add(bytes);

        if (Responder is null)
        {
            return ValueTask.FromResult(LinkResult.Success);
        }

        var (result, fragments) = Responder(bytes);

        foreach (var fragment in fragments)
        {
            DataSourceReceived?.Invoke(fragment);
        }

        return ValueTask.FromResult(result);
    }

    public void RaiseNotificationSource(byte[] packet) => NotificationSourceReceived?.Invoke(packet);

    public void RaiseDataSource(byte[] packet) => DataSourceReceived?.Invoke(packet);

    public void RaiseConnection(bool connected)
    {
        IsConnected = connected;
        ConnectionChanged?.Invoke(this, new LinkConnectionChangedEventArgs(connected));
    }

    public void RaiseServicesChanged() => ServicesChanged?.Invoke(this, EventArgs.Empty);

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;

    private static TaskCompletionSource<int> NewSignal() => new(TaskCreationOptions.RunContinuationsAsynchronously);
}
