using IslandBridge.Core.Ancs;

namespace IslandBridge.Tests.Ancs;

public sealed class AppNameCacheTests
{
    [Fact]
    public void Stores_and_returns_names()
    {
        var cache = new AppNameCache();
        cache.Set("net.whatsapp.WhatsApp", "WhatsApp");

        Assert.True(cache.TryGet("net.whatsapp.WhatsApp", out var name));
        Assert.Equal("WhatsApp", name);
        Assert.False(cache.TryGet("net.whatsapp.whatsapp", out _));
    }

    [Fact]
    public void Evicts_the_least_recently_used_entry()
    {
        var cache = new AppNameCache(2);
        cache.Set("a", "A");
        cache.Set("b", "B");
        cache.TryGet("a", out _);
        cache.Set("c", "C");

        Assert.True(cache.TryGet("a", out _));
        Assert.False(cache.TryGet("b", out _));
        Assert.True(cache.TryGet("c", out _));
        Assert.Equal(2, cache.Count);
    }

    [Fact]
    public void Overwrites_an_existing_entry()
    {
        var cache = new AppNameCache(2);
        cache.Set("a", "Old");
        cache.Set("a", "New");

        Assert.True(cache.TryGet("a", out var name));
        Assert.Equal("New", name);
        Assert.Equal(1, cache.Count);
    }

    [Fact]
    public void Snapshot_lists_most_recent_first()
    {
        var cache = new AppNameCache();
        cache.Set("a", "A");
        cache.Set("b", "B");

        Assert.Equal(["b", "a"], cache.Snapshot().Select(entry => entry.Key));
    }
}
