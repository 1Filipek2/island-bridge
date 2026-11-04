using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace IslandBridge.Core.Toasts;

public sealed unsafe partial class WnfToastSignal : IToastSignal
{
    private static readonly ConcurrentDictionary<nint, WnfToastSignal> Registry = new();
    private static long _nextContext;

    private readonly Lock _gate = new();
    private readonly List<nint> _subscriptions = [];
    private nint _context;
    private Action? _onSignal;

    public static IReadOnlyList<ulong> StateNames { get; } = [0x0D83063EA3BC1035, 0x0D83063EA3BD0035];

    public int LastStatus { get; private set; }

    public bool TryStart(Action onSignal)
    {
        ArgumentNullException.ThrowIfNull(onSignal);

        lock (_gate)
        {
            if (_context != 0)
            {
                return false;
            }

            _onSignal = onSignal;
            _context = (nint)Interlocked.Increment(ref _nextContext);
            Registry[_context] = this;

            foreach (var stateName in StateNames)
            {
                nint subscription;
                var status = RtlSubscribeWnfStateChangeNotification(&subscription, stateName, 0, &OnStateChanged, _context, 0, 0, 0);

                if (status < 0)
                {
                    LastStatus = status;
                    StopLocked();
                    return false;
                }

                _subscriptions.Add(subscription);
            }

            return true;
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            StopLocked();
        }
    }

    private void StopLocked()
    {
        foreach (var subscription in _subscriptions)
        {
            _ = RtlUnsubscribeWnfStateChangeNotification(subscription);
        }

        _subscriptions.Clear();

        if (_context != 0)
        {
            Registry.TryRemove(_context, out _);
            _context = 0;
        }

        _onSignal = null;
    }

    [UnmanagedCallersOnly]
    private static int OnStateChanged(ulong stateName, uint changeStamp, nint typeId, nint context, nint buffer, uint length)
    {
        try
        {
            if (Registry.TryGetValue(context, out var signal))
            {
                signal._onSignal?.Invoke();
            }
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
        }

        return 0;
    }

    [LibraryImport("ntdll.dll")]
    private static partial int RtlSubscribeWnfStateChangeNotification(
        nint* subscription,
        ulong stateName,
        uint changeStamp,
        delegate* unmanaged<ulong, uint, nint, nint, nint, uint, int> callback,
        nint callbackContext,
        nint typeId,
        uint serializationGroup,
        uint unknown);

    [LibraryImport("ntdll.dll")]
    private static partial int RtlUnsubscribeWnfStateChangeNotification(nint subscription);
}
