using System.Buffers.Binary;
using System.Text;
using IslandBridge.Core.Ancs;
using IslandBridge.Core.Island;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Tests.Island;

public sealed class IslandProtocolTests
{
    [Fact]
    public void Encodes_header_and_fields_in_order()
    {
        var message = IslandProtocol.Encode(Create(app: "WhatsApp", bundle: "net.whatsapp.WhatsApp", title: "Alice", subtitle: "", body: "Hi"));

        Assert.Equal(IslandProtocol.Version, message[0]);
        Assert.Equal(IslandProtocol.KindNotification, message[1]);
        Assert.Equal((byte)AncsCategory.Social, message[2]);
        Assert.Equal((byte)AncsEventFlags.Important, message[3]);
        Assert.Equal(["WhatsApp", "net.whatsapp.WhatsApp", "Alice", "", "Hi"], Decode(message));
    }

    [Fact]
    public void Lengths_add_up_exactly()
    {
        var message = IslandProtocol.Encode(Create());
        var offset = IslandProtocol.HeaderBytes;

        for (var i = 0; i < IslandProtocol.FieldCount; i++)
        {
            offset += sizeof(ushort) + BinaryPrimitives.ReadUInt16LittleEndian(message.AsSpan(offset));
        }

        Assert.Equal(message.Length, offset);
    }

    [Fact]
    public void Worst_case_stays_under_the_size_limit_and_field_caps()
    {
        var huge = string.Concat(Enumerable.Repeat("\U0001F600", 5000));
        var message = IslandProtocol.Encode(Create(app: huge, bundle: huge, title: huge, subtitle: huge, body: huge));
        var fields = Decode(message);

        Assert.True(message.Length <= IslandProtocol.MaxMessageBytes);

        for (var i = 0; i < IslandProtocol.FieldCount; i++)
        {
            Assert.True(Encoding.UTF8.GetByteCount(fields[i]) <= IslandProtocol.MaxFieldBytes[i]);
            Assert.DoesNotContain('\uFFFD', fields[i]);
        }
    }

    [Fact]
    public void Truncation_never_splits_a_character()
    {
        var bytes = IslandProtocol.TruncateUtf8("ab\U0001F600", 5);

        Assert.Equal("ab", Encoding.UTF8.GetString(bytes));
    }

    [Fact]
    public void Sanitizes_text_before_encoding()
    {
        var fields = Decode(IslandProtocol.Encode(Create(title: "\u202Eevil\nline", body: "a\u0000b")));

        Assert.Equal("evil line", fields[2]);
        Assert.Equal("ab", fields[4]);
    }

    private static IslandNotification Create(
        string app = "App",
        string bundle = "com.example.app",
        string title = "Title",
        string subtitle = "Subtitle",
        string body = "Body") =>
        new(1, AncsCategory.Social, AncsEventFlags.Important, bundle, app, title, subtitle, body, null, 0);

    private static string[] Decode(byte[] message)
    {
        var fields = new string[IslandProtocol.FieldCount];
        var offset = IslandProtocol.HeaderBytes;

        for (var i = 0; i < fields.Length; i++)
        {
            var length = BinaryPrimitives.ReadUInt16LittleEndian(message.AsSpan(offset));
            offset += sizeof(ushort);
            fields[i] = Encoding.UTF8.GetString(message, offset, length);
            offset += length;
        }

        return fields;
    }
}
