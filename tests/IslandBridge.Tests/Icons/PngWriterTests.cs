using System.Buffers.Binary;
using System.IO.Compression;
using IslandBridge.Core.Icons;

namespace IslandBridge.Tests.Icons;

public sealed class PngWriterTests
{
    [Fact]
    public void Writes_signature_header_and_valid_chunk_crcs()
    {
        var png = PngWriter.EncodeBgra(Solid(3, 2, 10, 20, 30, 255), 3, 2, premultiplied: false);

        Assert.True(PngWriter.HasSignature(png));
        var chunks = Chunks(png);
        Assert.Equal(["IHDR", "IDAT", "IEND"], chunks.Select(chunk => chunk.Type));

        var header = chunks[0].Data;
        Assert.Equal(3u, BinaryPrimitives.ReadUInt32BigEndian(header));
        Assert.Equal(2u, BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(4)));
        Assert.Equal(8, header[8]);
        Assert.Equal(6, header[9]);
    }

    [Fact]
    public void Pixels_are_stored_as_rgba_rows_with_filter_zero()
    {
        var png = PngWriter.EncodeBgra(Solid(2, 1, 10, 20, 30, 255), 2, 1, premultiplied: false);
        var raw = Inflate(Chunks(png)[1].Data);

        Assert.Equal([0, 30, 20, 10, 255, 30, 20, 10, 255], raw);
    }

    [Fact]
    public void Premultiplied_pixels_are_converted_to_straight_alpha()
    {
        var png = PngWriter.EncodeBgra(Solid(1, 1, 0, 64, 128, 128), 1, 1, premultiplied: true);
        var raw = Inflate(Chunks(png)[1].Data);

        Assert.Equal([0, 255, 128, 0, 128], raw);
    }

    [Fact]
    public void Premultiplied_detection_looks_at_channels_above_alpha()
    {
        Assert.True(PngWriter.LooksPremultiplied(Solid(1, 1, 10, 10, 10, 20)));
        Assert.False(PngWriter.LooksPremultiplied(Solid(1, 1, 30, 10, 10, 20)));
    }

    [Theory]
    [InlineData(0, 1)]
    [InlineData(1, 0)]
    [InlineData(PngWriter.MaxDimension + 1, 1)]
    public void Rejects_bad_dimensions(int width, int height)
    {
        Assert.ThrowsAny<ArgumentException>(() => PngWriter.EncodeBgra(new byte[Math.Max(0, width * height * 4)], width, height, false));
    }

    [Fact]
    public void Rejects_a_buffer_that_does_not_match()
    {
        Assert.Throws<ArgumentException>(() => PngWriter.EncodeBgra(new byte[7], 1, 2, false));
    }

    [Fact]
    public void Crc_matches_the_png_reference_value()
    {
        Assert.Equal(0xAE426082u, PngWriter.Crc32("IEND"u8, []));
    }

    private static byte[] Solid(int width, int height, byte b, byte g, byte r, byte a)
    {
        var pixels = new byte[width * height * 4];

        for (var index = 0; index < pixels.Length; index += 4)
        {
            pixels[index] = b;
            pixels[index + 1] = g;
            pixels[index + 2] = r;
            pixels[index + 3] = a;
        }

        return pixels;
    }

    private static byte[] Inflate(byte[] data)
    {
        using var input = new MemoryStream(data);
        using var zlib = new ZLibStream(input, CompressionMode.Decompress);
        using var output = new MemoryStream();
        zlib.CopyTo(output);
        return output.ToArray();
    }

    private static List<(string Type, byte[] Data)> Chunks(byte[] png)
    {
        List<(string, byte[])> chunks = [];
        var offset = 8;

        while (offset < png.Length)
        {
            var length = (int)BinaryPrimitives.ReadUInt32BigEndian(png.AsSpan(offset));
            var type = png.AsSpan(offset + 4, 4);
            var data = png.AsSpan(offset + 8, length);
            var crc = BinaryPrimitives.ReadUInt32BigEndian(png.AsSpan(offset + 8 + length));
            Assert.Equal(PngWriter.Crc32(type, data), crc);
            chunks.Add((System.Text.Encoding.ASCII.GetString(type), data.ToArray()));
            offset += 12 + length;
        }

        return chunks;
    }
}
