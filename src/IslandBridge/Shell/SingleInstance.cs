namespace IslandBridge.Shell;

internal sealed class SingleInstance : IDisposable
{
    private const string MutexName = @"Local\IslandBridge.Instance";
    private const string ShowEventName = @"Local\IslandBridge.ShowWindow";

    private readonly Mutex _mutex;
    private readonly EventWaitHandle _showEvent;
    private RegisteredWaitHandle? _registration;

    private SingleInstance(Mutex mutex, EventWaitHandle showEvent)
    {
        _mutex = mutex;
        _showEvent = showEvent;
    }

    public static SingleInstance? TryAcquire()
    {
        var mutex = new Mutex(initiallyOwned: true, MutexName, out var createdNew);

        if (!createdNew)
        {
            mutex.Dispose();
            return null;
        }

        return new SingleInstance(mutex, new EventWaitHandle(false, EventResetMode.AutoReset, ShowEventName));
    }

    public static bool SignalRunningInstance()
    {
        if (!EventWaitHandle.TryOpenExisting(ShowEventName, out var showEvent))
        {
            return false;
        }

        using (showEvent)
        {
            return showEvent.Set();
        }
    }

    public void OnShowRequested(Action callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        _registration?.Unregister(null);
        _registration = ThreadPool.RegisterWaitForSingleObject(
            _showEvent,
            static (state, _) => ((Action)state!)(),
            callback,
            Timeout.Infinite,
            executeOnlyOnce: false);
    }

    public void Dispose()
    {
        _registration?.Unregister(null);
        _showEvent.Dispose();
        _mutex.ReleaseMutex();
        _mutex.Dispose();
    }
}
