using IslandBridge.Core.Updates;

namespace IslandBridge.Tests.Updates;

public sealed class UpdateEndpointsTests
{
    [Theory]
    [InlineData("https://github.com/1Filipek2/island-bridge/releases/latest/download/update-manifest.txt")]
    [InlineData("https://objects.githubusercontent.com/github-production-release-asset/1?X-Amz-Signature=abc")]
    [InlineData("https://release-assets.githubusercontent.com/github-production-release-asset/1?sp=r")]
    [InlineData("https://GitHub.com/x")]
    public void Allows_github_release_hosts(string uri)
    {
        Assert.True(UpdateEndpoints.IsAllowed(new Uri(uri)));
    }

    [Theory]
    [InlineData("http://github.com/x")]
    [InlineData("https://github.com:8443/x")]
    [InlineData("https://user:pass@github.com/x")]
    [InlineData("https://api.github.com/x")]
    [InlineData("https://github.com.evil.example/x")]
    [InlineData("https://evilgithub.com/x")]
    [InlineData("https://raw.githubusercontent.com/x")]
    [InlineData("https://140.82.121.4/x")]
    [InlineData("ftp://github.com/x")]
    [InlineData("file:///C:/Windows/System32/cmd.exe")]
    public void Rejects_everything_else(string uri)
    {
        Assert.False(UpdateEndpoints.IsAllowed(new Uri(uri)));
    }

    [Fact]
    public void Built_in_addresses_are_allowed()
    {
        var manifest = new UpdateManifest(new UpdateVersion(1, 2, 3), "IslandBridge-1.2.3-setup-x64.exe", 1, new byte[32]);

        Assert.True(UpdateEndpoints.IsAllowed(UpdateEndpoints.Manifest));
        Assert.True(UpdateEndpoints.IsAllowed(UpdateEndpoints.Signature));
        Assert.True(UpdateEndpoints.IsAllowed(UpdateEndpoints.ReleasesPage));
        Assert.Equal(
            "https://github.com/1Filipek2/island-bridge/releases/download/v1.2.3/IslandBridge-1.2.3-setup-x64.exe",
            UpdateEndpoints.Setup(manifest).AbsoluteUri);
    }

    [Fact]
    public void Resolves_relative_redirects_and_drops_foreign_ones()
    {
        var current = UpdateEndpoints.Manifest;

        Assert.Equal(new Uri("https://github.com/other"), UpdateEndpoints.ResolveRedirect(current, new Uri("/other", UriKind.Relative)));
        Assert.Null(UpdateEndpoints.ResolveRedirect(current, new Uri("https://evil.example/setup.exe")));
        Assert.Null(UpdateEndpoints.ResolveRedirect(current, new Uri("//evil.example/setup.exe", UriKind.Relative)));
        Assert.Null(UpdateEndpoints.ResolveRedirect(current, new Uri("http://github.com/x")));
        Assert.Null(UpdateEndpoints.ResolveRedirect(current, null));
    }
}
