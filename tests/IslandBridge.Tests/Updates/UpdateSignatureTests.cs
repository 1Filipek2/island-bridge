using System.Security.Cryptography;
using System.Text;
using IslandBridge.Core.Updates;

namespace IslandBridge.Tests.Updates;

public sealed class UpdateSignatureTests : IDisposable
{
    private static readonly byte[] Manifest = Encoding.ASCII.GetBytes(
        "format=islandbridge-update-1\nversion=1.2.3\nfile=IslandBridge-1.2.3-setup-x64.exe\nsize=100\nsha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n");

    private readonly ECDsa _key = ECDsa.Create(ECCurve.NamedCurves.nistP384);
    private readonly ECDsa _other = ECDsa.Create(ECCurve.NamedCurves.nistP384);

    private string PublicKey => Convert.ToBase64String(_key.ExportSubjectPublicKeyInfo());

    [Fact]
    public void Accepts_a_signature_from_a_trusted_key()
    {
        var signature = Encoding.ASCII.GetBytes(UpdateSignature.Sign(Manifest, _key));

        Assert.True(UpdateSignature.Verify(Manifest, signature, [PublicKey]));
    }

    [Fact]
    public void Accepts_any_key_of_the_trusted_list()
    {
        var signature = Encoding.ASCII.GetBytes(UpdateSignature.Sign(Manifest, _key));
        var otherKey = Convert.ToBase64String(_other.ExportSubjectPublicKeyInfo());

        Assert.True(UpdateSignature.Verify(Manifest, signature, [otherKey, PublicKey]));
    }

    [Fact]
    public void Rejects_a_changed_manifest()
    {
        var signature = Encoding.ASCII.GetBytes(UpdateSignature.Sign(Manifest, _key));
        var changed = (byte[])Manifest.Clone();
        changed[40] ^= 1;

        Assert.False(UpdateSignature.Verify(changed, signature, [PublicKey]));
    }

    [Fact]
    public void Rejects_a_signature_from_an_untrusted_key()
    {
        var signature = Encoding.ASCII.GetBytes(UpdateSignature.Sign(Manifest, _other));

        Assert.False(UpdateSignature.Verify(Manifest, signature, [PublicKey]));
    }

    [Fact]
    public void Rejects_everything_without_trusted_keys()
    {
        var signature = Encoding.ASCII.GetBytes(UpdateSignature.Sign(Manifest, _key));

        Assert.False(UpdateSignature.Verify(Manifest, signature, []));
    }

    [Fact]
    public void Ignores_keys_on_other_curves()
    {
        using var p256 = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var signature = Convert.ToBase64String(p256.SignData(Manifest, HashAlgorithmName.SHA384, DSASignatureFormat.IeeeP1363FixedFieldConcatenation));
        var key = Convert.ToBase64String(p256.ExportSubjectPublicKeyInfo());

        Assert.False(UpdateSignature.Verify(Manifest, Encoding.ASCII.GetBytes(signature), [key]));
    }

    [Fact]
    public void Ignores_malformed_trusted_keys()
    {
        var signature = Encoding.ASCII.GetBytes(UpdateSignature.Sign(Manifest, _key));

        Assert.True(UpdateSignature.Verify(Manifest, signature, ["not base64!", "AAAA", PublicKey]));
    }

    [Fact]
    public void Rejects_der_encoded_signatures()
    {
        var der = Convert.ToBase64String(_key.SignData(Manifest, HashAlgorithmName.SHA384, DSASignatureFormat.Rfc3279DerSequence));

        Assert.False(UpdateSignature.Verify(Manifest, Encoding.ASCII.GetBytes(der), [PublicKey]));
    }

    [Theory]
    [InlineData("")]
    [InlineData("\n")]
    [InlineData("not a signature")]
    [InlineData("AAAA\n")]
    public void Rejects_malformed_signature_files(string file)
    {
        Assert.False(UpdateSignature.Verify(Manifest, Encoding.ASCII.GetBytes(file), [PublicKey]));
    }

    [Fact]
    public void Rejects_signature_files_with_extra_bytes()
    {
        var signature = UpdateSignature.Sign(Manifest, _key);

        Assert.False(UpdateSignature.Verify(Manifest, Encoding.ASCII.GetBytes(" " + signature), [PublicKey]));
        Assert.False(UpdateSignature.Verify(Manifest, Encoding.ASCII.GetBytes(signature + "\n"), [PublicKey]));
        Assert.False(UpdateSignature.Verify(Manifest, Encoding.ASCII.GetBytes(signature.Replace("\n", "\r\n", StringComparison.Ordinal)), [PublicKey]));
        Assert.False(UpdateSignature.Verify(Manifest, new byte[UpdateSignature.MaxFileBytes + 1], [PublicKey]));
    }

    [Fact]
    public void Rejects_an_oversized_manifest()
    {
        var big = new byte[UpdateManifest.MaxBytes + 1];
        var signature = Encoding.ASCII.GetBytes(UpdateSignature.Sign(big, _key));

        Assert.False(UpdateSignature.Verify(big, signature, [PublicKey]));
    }

    [Fact]
    public void Shipped_keys_are_p384_public_keys()
    {
        foreach (var key in UpdateSignature.TrustedKeys)
        {
            using var ecdsa = ECDsa.Create();
            ecdsa.ImportSubjectPublicKeyInfo(Convert.FromBase64String(key), out var read);

            Assert.Equal(384, ecdsa.KeySize);
            Assert.Equal(Convert.FromBase64String(key).Length, read);
        }
    }

    public void Dispose()
    {
        _key.Dispose();
        _other.Dispose();
    }
}
