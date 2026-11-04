using IslandBridge.Core.Icons;
using IslandBridge.Core.Island;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Tests.Icons;

public sealed class BundledIconSourceTests
{
    private static readonly BundledIconSource Source = new();

    [Fact]
    public void Catalog_is_embedded_and_every_brand_has_a_valid_png()
    {
        Assert.True(Source.Brands.Count >= 50);

        foreach (var brand in Source.Brands)
        {
            var png = BundledIconSource.ReadPng(brand.Slug);
            Assert.NotNull(png);
            Assert.True(PngWriter.HasSignature(png));
            Assert.InRange(png.Length, 9, IslandProtocol.MaxIconBytes);
        }
    }

    [Fact]
    public void Bundle_ids_and_slugs_are_unique()
    {
        var bundleIds = Source.Brands.SelectMany(brand => brand.BundleIds).ToList();

        Assert.Equal(bundleIds.Count, bundleIds.Distinct(StringComparer.OrdinalIgnoreCase).Count());
        Assert.Equal(Source.Brands.Count, Source.Brands.Select(brand => brand.Slug).Distinct(StringComparer.Ordinal).Count());
    }

    [Theory]
    [InlineData("com.burbn.instagram", "Instagram", "instagram")]
    [InlineData("COM.BURBN.INSTAGRAM", "", "instagram")]
    [InlineData("net.whatsapp.WhatsApp", "WhatsApp", "whatsapp")]
    [InlineData("com.unknown.app", "Booking.com", "bookingdotcom")]
    [InlineData("", "Glovo", "glovo")]
    public void Finds_iphone_apps_by_bundle_id_then_by_name(string bundleId, string name, string slug)
    {
        Assert.Equal(slug, Source.Find(new IconRequest(NotificationOrigin.Iphone, bundleId, name))?.Slug);
        Assert.NotNull(Source.TryGetPng(new IconRequest(NotificationOrigin.Iphone, bundleId, name), CancellationToken.None));
    }

    [Fact]
    public void Unknown_apps_and_windows_toasts_get_nothing()
    {
        Assert.Null(Source.Find(new IconRequest(NotificationOrigin.Iphone, "com.unknown.app", "Unknown App")));
        Assert.Null(Source.Find(new IconRequest(NotificationOrigin.Windows, "com.burbn.instagram", "Instagram")));
    }

    [Fact]
    public void Catalog_parser_skips_malformed_lines()
    {
        var brands = BundledIconSource.ParseCatalog("ok|OK|a.b, c.d\r\nbad line\n../evil|Evil|x\n\nempty|Empty|\n");

        Assert.Equal(["ok", "empty"], brands.Select(brand => brand.Slug));
        Assert.Equal(["a.b", "c.d"], brands[0].BundleIds);
        Assert.Empty(brands[1].BundleIds);
    }

    [Fact]
    public void Chained_source_returns_the_first_icon()
    {
        var chained = new ChainedIconSource(new FixedSource(null), new FixedSource([1]), new FixedSource([2]));

        Assert.Equal([1], chained.TryGetPng(new IconRequest(NotificationOrigin.Iphone, "a", "A"), CancellationToken.None));
    }

    private sealed class FixedSource(byte[]? png) : IAppIconSource
    {
        public byte[]? TryGetPng(IconRequest request, CancellationToken cancellationToken) => png;
    }
}
