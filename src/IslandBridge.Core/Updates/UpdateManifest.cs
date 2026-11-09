using System.Globalization;
using System.Text;

namespace IslandBridge.Core.Updates;

// The signed release manifest. Exact format, every line required and in this order, LF endings:
//   format=islandbridge-update-1
//   version=1.2.3
//   file=IslandBridge-1.2.3-setup-x64.exe
//   size=<bytes>
//   sha256=<64 lowercase hex>
public sealed record UpdateManifest(UpdateVersion Version, string FileName, long Size, byte[] Sha256)
{
    public const int MaxBytes = 1024;
    public const long MaxSetupBytes = 256L * 1024 * 1024;
    public const string Format = "islandbridge-update-1";

    private static readonly string[] Keys = ["format", "version", "file", "size", "sha256"];

    public static string SetupFileName(UpdateVersion version) => $"IslandBridge-{version}-setup-x64.exe";

    public static bool TryParse(ReadOnlySpan<byte> data, out UpdateManifest? manifest)
    {
        manifest = null;

        if (data.Length is 0 or > MaxBytes || data[^1] != (byte)'\n')
        {
            return false;
        }

        foreach (var b in data)
        {
            if (b is not ((>= 0x20 and < 0x7F) or (byte)'\n'))
            {
                return false;
            }
        }

        var lines = Encoding.ASCII.GetString(data[..^1]).Split('\n');

        if (lines.Length != Keys.Length)
        {
            return false;
        }

        Span<Range> values = stackalloc Range[Keys.Length];

        for (var index = 0; index < Keys.Length; index++)
        {
            var line = lines[index];
            var prefix = Keys[index] + "=";

            if (!line.StartsWith(prefix, StringComparison.Ordinal) || line.Length == prefix.Length)
            {
                return false;
            }

            values[index] = new Range(prefix.Length, line.Length);
        }

        var format = lines[0][values[0]];
        var versionText = lines[1][values[1]];
        var file = lines[2][values[2]];
        var sizeText = lines[3][values[3]];
        var hashText = lines[4][values[4]];

        if (!string.Equals(format, Format, StringComparison.Ordinal)
            || !UpdateVersion.TryParse(versionText, out var version)
            || !string.Equals(file, SetupFileName(version), StringComparison.Ordinal)
            || !TryParseSize(sizeText, out var size)
            || !TryParseHash(hashText, out var hash))
        {
            return false;
        }

        manifest = new UpdateManifest(version, file, size, hash);
        return true;
    }

    public string ToText() => string.Create(CultureInfo.InvariantCulture,
        $"format={Format}\nversion={Version}\nfile={FileName}\nsize={Size}\nsha256={Convert.ToHexStringLower(Sha256)}\n");

    private static bool TryParseSize(string text, out long size)
    {
        size = 0;

        if (text.Length > 10 || text[0] == '0' || !text.All(char.IsAsciiDigit))
        {
            return false;
        }

        size = long.Parse(text, NumberStyles.None, CultureInfo.InvariantCulture);
        return size <= MaxSetupBytes;
    }

    private static bool TryParseHash(string text, out byte[] hash)
    {
        hash = [];

        if (text.Length != 64 || !text.All(c => char.IsAsciiDigit(c) || c is >= 'a' and <= 'f'))
        {
            return false;
        }

        hash = Convert.FromHexString(text);
        return true;
    }
}
