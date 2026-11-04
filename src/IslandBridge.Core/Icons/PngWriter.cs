using System.Buffers.Binary;
using System.IO.Compression;

namespace IslandBridge.Core.Icons;

public static class PngWriter
{
    public const int MaxDimension = 256;

    private static ReadOnlySpan<byte> Signature => [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];

    private static readonly uint[] CrcTable = BuildCrcTable();

    public static bool HasSignature(ReadOnlySpan<byte> data) => data.StartsWith(Signature);

    public static byte[] EncodeBgra(ReadOnlySpan<byte> bgra, int width, int height, bool premultiplied)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(width, 1);
        ArgumentOutOfRangeException.ThrowIfLessThan(height, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(width, MaxDimension);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(height, MaxDimension);

        if (bgra.Length != width * height * 4)
        {
            throw new ArgumentException("Pixel buffer does not match the dimensions.", nameof(bgra));
        }

        var stride = (width * 4) + 1;
        var raw = new byte[stride * height];

        for (var y = 0; y < height; y++)
        {
            var row = raw.AsSpan(y * stride);
            row[0] = 0;

            for (var x = 0; x < width; x++)
            {
                var source = bgra.Slice(((y * width) + x) * 4, 4);
                var target = row.Slice(1 + (x * 4), 4);
                var alpha = source[3];
                target[0] = Straight(source[2], alpha, premultiplied);
                target[1] = Straight(source[1], alpha, premultiplied);
                target[2] = Straight(source[0], alpha, premultiplied);
                target[3] = alpha;
            }
        }

        byte[] compressed;

        using (var buffer = new MemoryStream())
        {
            using (var zlib = new ZLibStream(buffer, CompressionLevel.SmallestSize, leaveOpen: true))
            {
                zlib.Write(raw);
            }

            compressed = buffer.ToArray();
        }

        var header = new byte[13];
        BinaryPrimitives.WriteUInt32BigEndian(header, (uint)width);
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(4), (uint)height);
        header[8] = 8;
        header[9] = 6;

        using var output = new MemoryStream();
        output.Write(Signature);
        WriteChunk(output, "IHDR"u8, header);
        WriteChunk(output, "IDAT"u8, compressed);
        WriteChunk(output, "IEND"u8, []);
        return output.ToArray();
    }

    public static bool LooksPremultiplied(ReadOnlySpan<byte> bgra)
    {
        for (var index = 0; index + 3 < bgra.Length; index += 4)
        {
            var alpha = bgra[index + 3];

            if (bgra[index] > alpha || bgra[index + 1] > alpha || bgra[index + 2] > alpha)
            {
                return false;
            }
        }

        return true;
    }

    internal static uint Crc32(ReadOnlySpan<byte> type, ReadOnlySpan<byte> data)
    {
        var crc = 0xFFFFFFFFu;

        foreach (var value in type)
        {
            crc = CrcTable[(crc ^ value) & 0xFF] ^ (crc >> 8);
        }

        foreach (var value in data)
        {
            crc = CrcTable[(crc ^ value) & 0xFF] ^ (crc >> 8);
        }

        return crc ^ 0xFFFFFFFFu;
    }

    private static byte Straight(byte channel, byte alpha, bool premultiplied)
    {
        if (!premultiplied || alpha == 255)
        {
            return channel;
        }

        if (alpha == 0)
        {
            return 0;
        }

        return (byte)Math.Min(255, ((channel * 255) + (alpha / 2)) / alpha);
    }

    private static void WriteChunk(Stream output, ReadOnlySpan<byte> type, ReadOnlySpan<byte> data)
    {
        Span<byte> number = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(number, (uint)data.Length);
        output.Write(number);
        output.Write(type);
        output.Write(data);
        BinaryPrimitives.WriteUInt32BigEndian(number, Crc32(type, data));
        output.Write(number);
    }

    private static uint[] BuildCrcTable()
    {
        var table = new uint[256];

        for (var n = 0u; n < 256; n++)
        {
            var c = n;

            for (var k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }

            table[n] = c;
        }

        return table;
    }
}
