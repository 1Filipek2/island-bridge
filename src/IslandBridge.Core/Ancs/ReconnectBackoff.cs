namespace IslandBridge.Core.Ancs;

public sealed class ReconnectBackoff
{
    private int _attempt;

    public ReconnectBackoff(TimeSpan initial, TimeSpan maximum)
    {
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(initial, TimeSpan.Zero);
        ArgumentOutOfRangeException.ThrowIfLessThan(maximum, initial);

        Initial = initial;
        Maximum = maximum;
    }

    public static ReconnectBackoff Default => new(TimeSpan.FromSeconds(1), TimeSpan.FromMinutes(1));

    public TimeSpan Initial { get; }

    public TimeSpan Maximum { get; }

    public TimeSpan Next()
    {
        var factor = Math.Pow(2, Math.Min(_attempt, 30));
        _attempt++;
        var delay = Initial.TotalMilliseconds * factor;
        return delay >= Maximum.TotalMilliseconds ? Maximum : TimeSpan.FromMilliseconds(delay);
    }

    public void Reset() => _attempt = 0;
}
