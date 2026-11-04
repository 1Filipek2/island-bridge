using IslandBridge.Core.Ancs;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Icons;
using IslandBridge.Core.Island;
using IslandBridge.Core.Notifications;
using Microsoft.Extensions.Time.Testing;

namespace IslandBridge.Tests.Icons;

public sealed class AppNameMatcherTests
{
    [Theory]
    [InlineData("Discord", "discord")]
    [InlineData("  WhatsApp ", "whatsapp")]
    [InlineData("Microsoft Teams", "teams")]
    [InlineData("Outlook (new)", "outlook")]
    [InlineData("Microsoft", "microsoft")]
    [InlineData("", "")]
    [InlineData(null, "")]
    public void Normalizes_names(string? input, string expected)
    {
        Assert.Equal(expected, AppNameMatcher.Normalize(input));
    }

    [Fact]
    public void Finds_an_installed_app_by_normalized_name()
    {
        List<InstalledApp> apps =
        [
            new("Discord Website", "https://discord.com"),
            new("Microsoft Teams", "MSTeams_8wekyb3d8bbwe!MSTeams"),
            new("Discord", "com.squirrel.Discord.Discord"),
        ];

        Assert.Equal("com.squirrel.Discord.Discord", AppNameMatcher.FindBest(apps, "Discord")?.ParsingName);
        Assert.Equal("MSTeams_8wekyb3d8bbwe!MSTeams", AppNameMatcher.FindBest(apps, "Teams")?.ParsingName);
        Assert.Null(AppNameMatcher.FindBest(apps, "Instagram"));
        Assert.Null(AppNameMatcher.FindBest(apps, ""));
    }

    [Fact]
    public void Url_entries_are_never_used()
    {
        List<InstalledApp> apps = [new("Gmail", "https://mail.google.com")];

        Assert.Null(AppNameMatcher.FindBest(apps, "Gmail"));
    }
}

public sealed class IconCacheTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "IslandBridge.Tests", Guid.NewGuid().ToString("N"));
    private readonly FakeTimeProvider _time = new(new DateTimeOffset(2026, 11, 4, 12, 0, 0, TimeSpan.Zero));

    [Fact]
    public void Unknown_key_reports_unknown()
    {
        Assert.Equal(IconLookupResult.Unknown, Cache().Lookup("iphone:x").Result);
    }

    [Fact]
    public void Stored_icons_survive_a_new_cache_instance()
    {
        var png = Png();
        Cache().Store("iphone:com.example", png);

        var lookup = Cache().Lookup("iphone:com.example");

        Assert.Equal(IconLookupResult.Found, lookup.Result);
        Assert.Equal(png, lookup.Png);
    }

    [Fact]
    public void Misses_expire()
    {
        var cache = Cache();
        cache.StoreMiss("iphone:none");

        Assert.Equal(IconLookupResult.KnownMissing, cache.Lookup("iphone:none").Result);
        _time.Advance(TimeSpan.FromHours(2));
        Assert.Equal(IconLookupResult.Unknown, cache.Lookup("iphone:none").Result);
    }

    [Fact]
    public void Data_that_is_not_a_png_or_too_large_is_not_stored()
    {
        var cache = Cache();
        cache.Store("a", [1, 2, 3, 4, 5, 6, 7, 8, 9]);
        cache.Store("b", [.. Png(), .. new byte[IslandProtocol.MaxIconBytes]]);

        Assert.Equal(IconLookupResult.KnownMissing, cache.Lookup("a").Result);
        Assert.Equal(IconLookupResult.KnownMissing, cache.Lookup("b").Result);
        Assert.False(Directory.Exists(_directory) && Directory.GetFiles(_directory).Length > 0);
    }

    [Fact]
    public void A_tampered_file_on_disk_is_ignored()
    {
        Cache().Store("iphone:x", Png());
        File.WriteAllBytes(Path.Combine(_directory, IconCache.FileName("iphone:x")), [1, 2, 3]);

        Assert.Equal(IconLookupResult.Unknown, Cache().Lookup("iphone:x").Result);
    }

    [Fact]
    public void Disk_and_memory_are_capped()
    {
        var cache = Cache(maxFiles: 3, memoryEntries: 2);

        for (var index = 0; index < 6; index++)
        {
            cache.Store($"key{index}", Png());
            File.SetLastWriteTimeUtc(Path.Combine(_directory, IconCache.FileName($"key{index}")), DateTime.UtcNow.AddMinutes(index));
        }

        cache.Store("key6", Png());

        Assert.Equal(3, Directory.GetFiles(_directory, "*.png").Length);
        Assert.Equal(2, cache.MemoryCount);
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }

    internal static byte[] Png() => PngWriter.EncodeBgra(new byte[4 * 4 * 4], 4, 4, false);

    private IconCache Cache(int maxFiles = 16, int memoryEntries = 8) =>
        new(_directory, _time, NullLog.Instance, maxFiles, memoryEntries);
}

public sealed class IconResolverTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "IslandBridge.Tests", Guid.NewGuid().ToString("N"));
    private readonly FakeTimeProvider _time = new(new DateTimeOffset(2026, 11, 4, 12, 0, 0, TimeSpan.Zero));

    [Fact]
    public void Keys_depend_on_origin_and_identity()
    {
        Assert.Equal("iphone:com.example.app", IconResolver.KeyFor(Notification("com.Example.App", "Example")));
        Assert.Equal("windows:contoso!app", IconResolver.KeyFor(Notification("Contoso!App", "Contoso", NotificationOrigin.Windows)));
        Assert.Equal("iphone:example", IconResolver.KeyFor(Notification(string.Empty, "Example")));
        Assert.Null(IconResolver.KeyFor(Notification(string.Empty, string.Empty)));
    }

    [Fact]
    public void Source_is_asked_once_and_the_result_is_cached()
    {
        var source = new FakeSource { Result = IconCacheTests.Png() };
        var resolver = Resolver(source);

        Assert.NotNull(resolver.Resolve(Notification("com.example", "Example"), CancellationToken.None));
        Assert.NotNull(resolver.Resolve(Notification("com.example", "Example"), CancellationToken.None));
        Assert.Equal(1, source.Calls);
        Assert.Equal(NotificationOrigin.Iphone, source.LastRequest?.Origin);
        Assert.Equal("Example", source.LastRequest?.AppName);
    }

    [Fact]
    public void Misses_are_remembered()
    {
        var source = new FakeSource();
        var resolver = Resolver(source);

        Assert.Null(resolver.Resolve(Notification("com.none", "None"), CancellationToken.None));
        Assert.Null(resolver.Resolve(Notification("com.none", "None"), CancellationToken.None));
        Assert.Equal(1, source.Calls);
    }

    [Fact]
    public void Source_failures_and_invalid_images_give_no_icon()
    {
        Assert.Null(Resolver(new FakeSource { Throw = true }).Resolve(Notification("a", "A"), CancellationToken.None));
        Assert.Null(Resolver(new FakeSource { Result = [1, 2, 3] }).Resolve(Notification("b", "B"), CancellationToken.None));
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }

    private static IslandNotification Notification(string id, string name, NotificationOrigin origin = NotificationOrigin.Iphone) =>
        new(1, AncsCategory.Social, AncsEventFlags.None, id, name, "T", string.Empty, "M", null, 0, origin);

    private IconResolver Resolver(FakeSource source) =>
        new(new IconCache(_directory, _time, NullLog.Instance), source, NullLog.Instance);

    private sealed class FakeSource : IAppIconSource
    {
        public byte[]? Result { get; init; }

        public bool Throw { get; init; }

        public int Calls { get; private set; }

        public IconRequest? LastRequest { get; private set; }

        public byte[]? TryGetPng(IconRequest request, CancellationToken cancellationToken)
        {
            Calls++;
            LastRequest = request;
            return Throw ? throw new InvalidOperationException("boom") : Result;
        }
    }
}
