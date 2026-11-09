using System.Text;
using IslandBridge.Core.Updates;

namespace IslandBridge.Tests.Updates;

public sealed class UpdateManifestTests
{
    private const string Hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    private const string Valid = $"format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256={Hash}\n";

    [Fact]
    public void Parses_a_valid_manifest()
    {
        Assert.True(UpdateManifest.TryParse(Bytes(Valid), out var manifest));
        Assert.NotNull(manifest);
        Assert.Equal(new UpdateVersion(1, 2, 3), manifest.Version);
        Assert.Equal("IslandBridge-1.2.3-setup-x64.exe", manifest.FileName);
        Assert.Equal(18874368, manifest.Size);
        Assert.Equal(Convert.FromHexString(Hash), manifest.Sha256);
        Assert.Equal(Valid, manifest.ToText());
    }

    [Theory]
    [InlineData("")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=" + Hash)]
    [InlineData("format=islandbridge-update-1\r\nversion=1.2.3\r\nfile=IslandBridge-1.2.3-setup-x64.exe\r\nsize=18874368\r\nsha256=" + Hash + "\r\n")]
    [InlineData("format=islandbridge-update-2\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=" + Hash + "\n")]
    [InlineData("version=1.2.3\nformat=islandbridge-update-1\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.4-setup-x64.exe\nsize=18874368\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=..\\IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=0\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=018874368\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=999999999999\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=-5\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=0123\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=" + Hash + "\nextra=1\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nversion=9.9.9\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\n")]
    [InlineData("format=islandbridge-update-1\nversion= 1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=" + Hash + "\n")]
    [InlineData("format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=18874368\nsha256=" + Hash + "\n\n")]
    public void Rejects_malformed_manifests(string text)
    {
        Assert.False(UpdateManifest.TryParse(Bytes(text), out var manifest));
        Assert.Null(manifest);
    }

    [Fact]
    public void Rejects_control_and_non_ascii_bytes()
    {
        Assert.False(UpdateManifest.TryParse(Bytes(Valid.Replace("1.2.3-setup", "1.2.3\u202E-setup", StringComparison.Ordinal)), out _));
        Assert.False(UpdateManifest.TryParse(Bytes(Valid.Replace("size=", "size=\t", StringComparison.Ordinal)), out _));
        Assert.False(UpdateManifest.TryParse([.. Bytes(Valid)[..^1], 0, (byte)'\n'], out _));
    }

    [Fact]
    public void Rejects_oversized_input()
    {
        var big = new byte[UpdateManifest.MaxBytes + 1];
        Array.Fill(big, (byte)'a');
        big[^1] = (byte)'\n';

        Assert.False(UpdateManifest.TryParse(big, out _));
    }

    [Fact]
    public void Random_mutations_never_throw()
    {
        var random = new Random(4242);
        var original = Bytes(Valid);

        for (var round = 0; round < 20_000; round++)
        {
            var data = (byte[])original.Clone();
            var edits = random.Next(1, 4);

            for (var edit = 0; edit < edits; edit++)
            {
                data[random.Next(data.Length)] = (byte)random.Next(256);
            }

            var length = random.Next(data.Length + 1);
            _ = UpdateManifest.TryParse(data.AsSpan(0, length), out _);
        }
    }

    private static byte[] Bytes(string text) => Encoding.UTF8.GetBytes(text);
}
