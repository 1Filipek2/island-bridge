using System.Collections.Frozen;
using IslandBridge.Core.Island;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Core.Icons;

public sealed record BundledBrand(string Slug, string Title, IReadOnlyList<string> BundleIds);

public sealed class BundledIconSource : IAppIconSource
{
    private const string ResourcePrefix = "IslandBridge.Core.Icons.Bundled.";

    private readonly FrozenDictionary<string, BundledBrand> _byBundleId;
    private readonly FrozenDictionary<string, BundledBrand> _byName;

    public BundledIconSource()
        : this(LoadCatalog())
    {
    }

    internal BundledIconSource(IReadOnlyList<BundledBrand> brands)
    {
        ArgumentNullException.ThrowIfNull(brands);

        Brands = brands;
        var byBundleId = new Dictionary<string, BundledBrand>(StringComparer.OrdinalIgnoreCase);
        var byName = new Dictionary<string, BundledBrand>(StringComparer.Ordinal);

        foreach (var brand in brands)
        {
            foreach (var bundleId in brand.BundleIds)
            {
                byBundleId.TryAdd(bundleId, brand);
            }

            var name = AppNameMatcher.Normalize(brand.Title);

            if (name.Length > 0)
            {
                byName.TryAdd(name, brand);
            }
        }

        _byBundleId = byBundleId.ToFrozenDictionary(StringComparer.OrdinalIgnoreCase);
        _byName = byName.ToFrozenDictionary(StringComparer.Ordinal);
    }

    public IReadOnlyList<BundledBrand> Brands { get; }

    public static IReadOnlyList<BundledBrand> ParseCatalog(string text)
    {
        ArgumentNullException.ThrowIfNull(text);

        List<BundledBrand> brands = [];

        foreach (var line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var parts = line.Split('|');

            if (parts.Length != 3 || parts[0].Length == 0 || !parts[0].All(char.IsAsciiLetterOrDigit))
            {
                continue;
            }

            brands.Add(new BundledBrand(
                parts[0],
                parts[1],
                [.. parts[2].Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)]));
        }

        return brands;
    }

    public BundledBrand? Find(IconRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);

        if (request.Origin != NotificationOrigin.Iphone)
        {
            return null;
        }

        if (request.AppIdentifier.Length > 0 && _byBundleId.TryGetValue(request.AppIdentifier, out var byId))
        {
            return byId;
        }

        return _byName.GetValueOrDefault(AppNameMatcher.Normalize(request.AppName));
    }

    public byte[]? TryGetPng(IconRequest request, CancellationToken cancellationToken) =>
        Find(request) is { } brand ? ReadPng(brand.Slug) : null;

    internal static byte[]? ReadPng(string slug)
    {
        using var stream = typeof(BundledIconSource).Assembly.GetManifestResourceStream(ResourcePrefix + slug + ".png");

        if (stream is null || stream.Length is 0 or > IslandProtocol.MaxIconBytes)
        {
            return null;
        }

        var png = new byte[stream.Length];
        stream.ReadExactly(png);
        return PngWriter.HasSignature(png) ? png : null;
    }

    private static IReadOnlyList<BundledBrand> LoadCatalog()
    {
        using var stream = typeof(BundledIconSource).Assembly.GetManifestResourceStream(ResourcePrefix + "brands.txt");

        if (stream is null)
        {
            return [];
        }

        using var reader = new StreamReader(stream);
        return ParseCatalog(reader.ReadToEnd());
    }
}

public sealed class ChainedIconSource(params IReadOnlyList<IAppIconSource> sources) : IAppIconSource
{
    public byte[]? TryGetPng(IconRequest request, CancellationToken cancellationToken)
    {
        foreach (var source in sources)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (source.TryGetPng(request, cancellationToken) is { } png)
            {
                return png;
            }
        }

        return null;
    }
}
