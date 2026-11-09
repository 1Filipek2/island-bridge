using System.Security.Cryptography;

namespace IslandBridge.Core.Updates;

// ECDSA P-384 over SHA-384, signature in IEEE P1363 form (r || s, 96 bytes), stored as one base64 line.
public static class UpdateSignature
{
    public const int SignatureBytes = 96;
    public const int MaxFileBytes = 256;

    private const string P384Oid = "1.3.132.0.34";

    // Release signing keys (SubjectPublicKeyInfo, base64). The private key lives only in the GitHub
    // "release" environment; several entries allow a key rotation across one release.
    public static IReadOnlyList<string> TrustedKeys { get; } =
    [
        "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEb16n92mJkoX5FJifOXJoDLaeC3acXzXhSadR0uuqYhQubvOlq1e0ytFei2HIIj7gjySRaIvtJBPI5JT7JmFA2d+nYFTo2ZdoeqyAZ5wOc6c1iAYz75ctk7nvkY3rV2Yb",
    ];

    public static bool HasTrustedKeys => TrustedKeys.Count > 0;

    public static bool Verify(ReadOnlySpan<byte> manifest, ReadOnlySpan<byte> signatureFile) =>
        Verify(manifest, signatureFile, TrustedKeys);

    public static bool Verify(ReadOnlySpan<byte> manifest, ReadOnlySpan<byte> signatureFile, IReadOnlyList<string> trustedKeys)
    {
        ArgumentNullException.ThrowIfNull(trustedKeys);

        if (manifest.IsEmpty || manifest.Length > UpdateManifest.MaxBytes || !TryDecode(signatureFile, out var signature))
        {
            return false;
        }

        foreach (var key in trustedKeys)
        {
            using var ecdsa = ImportKey(key);

            if (ecdsa is not null && ecdsa.VerifyData(manifest, signature, HashAlgorithmName.SHA384, DSASignatureFormat.IeeeP1363FixedFieldConcatenation))
            {
                return true;
            }
        }

        return false;
    }

    public static string Sign(ReadOnlySpan<byte> manifest, ECDsa key)
    {
        ArgumentNullException.ThrowIfNull(key);
        return Convert.ToBase64String(key.SignData(manifest, HashAlgorithmName.SHA384, DSASignatureFormat.IeeeP1363FixedFieldConcatenation)) + "\n";
    }

    private static bool TryDecode(ReadOnlySpan<byte> file, out byte[] signature)
    {
        signature = [];

        if (file.Length is 0 or > MaxFileBytes)
        {
            return false;
        }

        if (file[^1] == (byte)'\n')
        {
            file = file[..^1];
        }

        Span<char> text = stackalloc char[file.Length];

        for (var index = 0; index < file.Length; index++)
        {
            var b = file[index];

            if (!(char.IsAsciiLetterOrDigit((char)b) || b is (byte)'+' or (byte)'/' or (byte)'='))
            {
                return false;
            }

            text[index] = (char)b;
        }

        Span<byte> decoded = stackalloc byte[SignatureBytes + 3];

        if (!Convert.TryFromBase64Chars(text, decoded, out var written) || written != SignatureBytes)
        {
            return false;
        }

        signature = decoded[..written].ToArray();
        return true;
    }

    private static ECDsa? ImportKey(string key)
    {
        var ecdsa = ECDsa.Create();

        try
        {
            ecdsa.ImportSubjectPublicKeyInfo(Convert.FromBase64String(key), out _);

            var curve = ecdsa.ExportParameters(false).Curve;

            if (ecdsa.KeySize == 384 && curve.IsNamed && (curve.Oid.Value == P384Oid || curve.Oid.FriendlyName is "nistP384" or "ECDSA_P384" or "secP384r1"))
            {
                return ecdsa;
            }
        }
        catch (CryptographicException)
        {
        }
        catch (FormatException)
        {
        }

        ecdsa.Dispose();
        return null;
    }
}
