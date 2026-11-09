namespace IslandBridge.Core.Updates;

// The only addresses the updater may contact. GitHub serves release assets through a redirect to
// its asset CDN; any other host, scheme or port ends the request.
public static class UpdateEndpoints
{
    public const string ManifestName = "update-manifest.txt";
    public const string SignatureName = "update-manifest.sig";
    public const int MaxRedirects = 5;

    private const string Releases = "https://github.com/1Filipek2/island-bridge/releases";

    private static readonly string[] AllowedHosts =
    [
        "github.com",
        "objects.githubusercontent.com",
        "release-assets.githubusercontent.com",
    ];

    public static Uri ReleasesPage { get; } = new($"{Releases}/latest");

    public static Uri Manifest { get; } = new($"{Releases}/latest/download/{ManifestName}");

    public static Uri Signature { get; } = new($"{Releases}/latest/download/{SignatureName}");

    public static Uri Setup(UpdateManifest manifest)
    {
        ArgumentNullException.ThrowIfNull(manifest);
        return new Uri($"{Releases}/download/v{manifest.Version}/{manifest.FileName}");
    }

    public static bool IsAllowed(Uri? uri) =>
        uri is { IsAbsoluteUri: true }
        && uri.Scheme == Uri.UriSchemeHttps
        && uri.Port == 443
        && uri.UserInfo.Length == 0
        && uri.HostNameType == UriHostNameType.Dns
        && AllowedHosts.Contains(uri.IdnHost, StringComparer.OrdinalIgnoreCase);

    public static Uri? ResolveRedirect(Uri current, Uri? location)
    {
        ArgumentNullException.ThrowIfNull(current);

        if (location is null)
        {
            return null;
        }

        var next = location.IsAbsoluteUri ? location : new Uri(current, location);
        return IsAllowed(next) ? next : null;
    }
}
