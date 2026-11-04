namespace IslandBridge.Core.Ancs;

public sealed class AppNameCache
{
    public const int DefaultCapacity = 512;

    private readonly Lock _gate = new();
    private readonly Dictionary<string, LinkedListNode<KeyValuePair<string, string>>> _entries = new(StringComparer.Ordinal);
    private readonly LinkedList<KeyValuePair<string, string>> _recency = new();

    public AppNameCache(int capacity = DefaultCapacity)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(capacity, 1);
        Capacity = capacity;
    }

    public int Capacity { get; }

    public int Count
    {
        get
        {
            lock (_gate)
            {
                return _entries.Count;
            }
        }
    }

    public bool TryGet(string appIdentifier, out string displayName)
    {
        ArgumentNullException.ThrowIfNull(appIdentifier);

        lock (_gate)
        {
            if (_entries.TryGetValue(appIdentifier, out var node))
            {
                _recency.Remove(node);
                _recency.AddFirst(node);
                displayName = node.Value.Value;
                return true;
            }
        }

        displayName = string.Empty;
        return false;
    }

    public void Set(string appIdentifier, string displayName)
    {
        ArgumentException.ThrowIfNullOrEmpty(appIdentifier);
        ArgumentException.ThrowIfNullOrEmpty(displayName);

        lock (_gate)
        {
            if (_entries.TryGetValue(appIdentifier, out var existing))
            {
                _recency.Remove(existing);
                _entries.Remove(appIdentifier);
            }

            var node = _recency.AddFirst(KeyValuePair.Create(appIdentifier, displayName));
            _entries[appIdentifier] = node;

            if (_entries.Count > Capacity && _recency.Last is { } oldest)
            {
                _recency.RemoveLast();
                _entries.Remove(oldest.Value.Key);
            }
        }
    }

    public IReadOnlyList<KeyValuePair<string, string>> Snapshot()
    {
        lock (_gate)
        {
            return [.. _recency];
        }
    }
}
