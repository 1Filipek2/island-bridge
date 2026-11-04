using IslandBridge.Core.Ancs;

namespace IslandBridge.Tests.Ancs;

public sealed class ReconnectBackoffTests
{
    [Fact]
    public void Doubles_up_to_the_cap_and_stays_there()
    {
        var backoff = new ReconnectBackoff(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(10));

        var delays = Enumerable.Range(0, 100).Select(_ => backoff.Next().TotalSeconds).ToArray();

        Assert.Equal([1, 2, 4, 8, 10, 10], delays[..6]);
        Assert.All(delays[4..], delay => Assert.Equal(10, delay));
    }

    [Fact]
    public void Reset_starts_over()
    {
        var backoff = new ReconnectBackoff(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(10));
        backoff.Next();
        backoff.Next();
        backoff.Reset();

        Assert.Equal(TimeSpan.FromSeconds(1), backoff.Next());
    }
}
