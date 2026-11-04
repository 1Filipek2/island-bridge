using IslandBridge.Core.Toasts;

namespace IslandBridge.Tests.Toasts;

internal sealed class FakeToastSignal : IToastSignal
{
    private Action? _onSignal;

    public bool CanStart { get; init; } = true;

    public bool Started { get; private set; }

    public bool Disposed { get; private set; }

    public bool TryStart(Action onSignal)
    {
        if (!CanStart)
        {
            return false;
        }

        _onSignal = onSignal;
        Started = true;
        return true;
    }

    public void Fire() => _onSignal?.Invoke();

    public void Dispose()
    {
        Disposed = true;
        _onSignal = null;
    }
}

internal sealed class FakeToastStore : IToastStore
{
    private readonly Lock _gate = new();
    private readonly List<ToastDetails> _toasts = [];
    private readonly HashSet<uint> _failingDetails = [];
    private TaskCompletionSource? _listGate;
    private readonly TaskCompletionSource _listBlocked = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource _listFailed = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private bool _failNextList;
    private int _listCalls;

    public ToastAccess Access { get; init; } = ToastAccess.Allowed;

    public int ListCalls => Volatile.Read(ref _listCalls);

    public void Add(ToastDetails toast)
    {
        lock (_gate)
        {
            _toasts.Add(toast);
        }
    }

    public void Remove(uint id)
    {
        lock (_gate)
        {
            _toasts.RemoveAll(toast => toast.Id == id);
        }
    }

    public void FailDetailsFor(uint id)
    {
        lock (_gate)
        {
            _failingDetails.Add(id);
        }
    }

    public void FailNextList()
    {
        lock (_gate)
        {
            _failNextList = true;
        }
    }

    public void BlockListing()
    {
        lock (_gate)
        {
            _listGate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }

    public Task WaitForBlockedListAsync() => _listBlocked.Task.WaitAsync(TimeSpan.FromSeconds(5));

    public Task WaitForFailedListAsync() => _listFailed.Task.WaitAsync(TimeSpan.FromSeconds(5));

    public void ReleaseListing()
    {
        lock (_gate)
        {
            _listGate?.TrySetResult();
            _listGate = null;
        }
    }

    public Task<ToastAccess> RequestAccessAsync(CancellationToken cancellationToken) => Task.FromResult(Access);

    public async Task<IReadOnlyList<uint>> ListIdsAsync(CancellationToken cancellationToken)
    {
        Interlocked.Increment(ref _listCalls);
        Task? wait;

        lock (_gate)
        {
            if (_failNextList)
            {
                _failNextList = false;
                _listFailed.TrySetResult();
                throw new InvalidOperationException("list failed");
            }

            wait = _listGate?.Task;
        }

        if (wait is not null)
        {
            _listBlocked.TrySetResult();
            await wait.WaitAsync(cancellationToken);
        }

        lock (_gate)
        {
            return [.. _toasts.Select(toast => toast.Id)];
        }
    }

    public ToastDetails? GetDetails(uint id)
    {
        lock (_gate)
        {
            return _failingDetails.Contains(id) ? null : _toasts.Find(toast => toast.Id == id);
        }
    }
}
