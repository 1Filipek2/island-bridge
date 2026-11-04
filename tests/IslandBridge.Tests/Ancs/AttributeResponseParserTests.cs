using System.Buffers.Binary;
using System.Text;
using IslandBridge.Core.Ancs;

namespace IslandBridge.Tests.Ancs;

public sealed class AttributeResponseParserTests
{
    private static ParseStatus Parse(byte[] data, uint uid, out NotificationAttributes? attributes) =>
        AttributeResponseParser.ParseNotification(data, uid, AncsCommands.DefaultNotificationAttributes, out attributes);

    [Fact]
    public void Parses_a_complete_notification_response()
    {
        var data = AncsPackets.NotificationResponse(42, "com.apple.MobileSMS", "Bob", "Group", "See you at 8");

        Assert.Equal(ParseStatus.Complete, Parse(data, 42, out var attributes));
        Assert.NotNull(attributes);
        Assert.Equal("com.apple.MobileSMS", attributes.AppIdentifier);
        Assert.Equal("Bob", attributes.Title);
        Assert.Equal("Group", attributes.Subtitle);
        Assert.Equal("See you at 8", attributes.Message);
        Assert.Equal(new DateTime(2026, 11, 4, 10, 17, 48), attributes.Date);
    }

    [Fact]
    public void Every_strict_prefix_needs_more_data()
    {
        var data = AncsPackets.NotificationResponse(7);

        for (var length = 0; length < data.Length; length++)
        {
            Assert.Equal(ParseStatus.NeedMoreData, Parse(data[..length], 7, out var attributes));
            Assert.Null(attributes);
        }
    }

    [Fact]
    public void Rejects_a_response_for_another_uid()
    {
        Assert.Equal(ParseStatus.Invalid, Parse(AncsPackets.NotificationResponse(1), 2, out _));
    }

    [Fact]
    public void Rejects_a_wrong_command_id()
    {
        var data = AncsPackets.NotificationResponse(1);
        data[0] = 1;

        Assert.Equal(ParseStatus.Invalid, Parse(data, 1, out _));
    }

    [Fact]
    public void Rejects_trailing_bytes()
    {
        var data = AncsPackets.NotificationResponse(1).Append((byte)0).ToArray();

        Assert.Equal(ParseStatus.Invalid, Parse(data, 1, out _));
    }

    [Fact]
    public void Rejects_attributes_in_a_different_order()
    {
        var buffer = new List<byte> { 0, 1, 0, 0, 0 };
        AncsPackets.AddTuple(buffer, NotificationAttributeId.Title, "x"u8);
        AncsPackets.AddTuple(buffer, NotificationAttributeId.AppIdentifier, "a"u8);

        Assert.Equal(ParseStatus.Invalid, Parse([.. buffer], 1, out _));
    }

    [Fact]
    public void Rejects_an_oversized_attribute_before_it_arrives()
    {
        var buffer = new List<byte> { 0, 1, 0, 0, 0, (byte)NotificationAttributeId.AppIdentifier };
        var length = new byte[2];
        BinaryPrimitives.WriteUInt16LittleEndian(length, AncsLimits.MaxAttributeBytes + 1);
        buffer.AddRange(length);

        Assert.Equal(ParseStatus.Invalid, Parse([.. buffer], 1, out _));
    }

    [Fact]
    public void Truncates_long_text_to_the_field_limit()
    {
        var data = AncsPackets.NotificationResponse(1, message: new string('m', 2000));

        Assert.Equal(ParseStatus.Complete, Parse(data, 1, out var attributes));
        Assert.Equal(AncsLimits.MessageMaxLength, attributes!.Message.Length);
    }

    [Fact]
    public void Sanitizes_control_and_bidi_characters()
    {
        var data = AncsPackets.NotificationResponse(1, title: "\u202Eevil\u0007\nname");

        Assert.Equal(ParseStatus.Complete, Parse(data, 1, out var attributes));
        Assert.Equal("evil name", attributes!.Title);
    }

    [Fact]
    public void Replaces_invalid_utf8()
    {
        var buffer = new List<byte> { 0, 1, 0, 0, 0 };
        AncsPackets.AddTuple(buffer, NotificationAttributeId.AppIdentifier, "app"u8);
        AncsPackets.AddTuple(buffer, NotificationAttributeId.Title, [0x41, 0xC3, 0x28, 0x42]);
        AncsPackets.AddTuple(buffer, NotificationAttributeId.Subtitle, []);
        AncsPackets.AddTuple(buffer, NotificationAttributeId.Message, [0xFF, 0xFE, 0xFD]);
        AncsPackets.AddTuple(buffer, NotificationAttributeId.Date, []);

        Assert.Equal(ParseStatus.Complete, Parse([.. buffer], 1, out var attributes));
        Assert.Equal("A\uFFFD(B", attributes!.Title);
        Assert.Equal("\uFFFD", attributes.Message);
        Assert.Null(attributes.Date);
    }

    [Theory]
    [InlineData("")]
    [InlineData("garbage")]
    [InlineData("20261304T101748")]
    [InlineData("20261104 101748")]
    public void Invalid_dates_become_null(string date)
    {
        var data = AncsPackets.NotificationResponse(1, date: date);

        Assert.Equal(ParseStatus.Complete, Parse(data, 1, out var attributes));
        Assert.Null(attributes!.Date);
    }

    [Fact]
    public void Parses_a_complete_app_response()
    {
        var data = AncsPackets.AppResponse("net.whatsapp.WhatsApp", "WhatsApp");

        Assert.Equal(ParseStatus.Complete, AttributeResponseParser.ParseApp(data, "net.whatsapp.WhatsApp"u8, out var name));
        Assert.Equal("WhatsApp", name);
    }

    [Fact]
    public void App_response_prefixes_need_more_data()
    {
        var data = AncsPackets.AppResponse("com.example", "Example");

        for (var length = 0; length < data.Length; length++)
        {
            Assert.Equal(ParseStatus.NeedMoreData, AttributeResponseParser.ParseApp(data.AsSpan(0, length), "com.example"u8, out _));
        }
    }

    [Fact]
    public void Rejects_an_app_response_for_another_app()
    {
        var data = AncsPackets.AppResponse("com.other", "Other");

        Assert.Equal(ParseStatus.Invalid, AttributeResponseParser.ParseApp(data, "com.example"u8, out _));
    }

    [Fact]
    public void Rejects_an_unterminated_app_identifier_over_the_cap()
    {
        var data = new byte[AncsLimits.MaxAppIdentifierBytes + 2];
        data[0] = 1;
        data.AsSpan(1).Fill((byte)'a');

        Assert.Equal(ParseStatus.Invalid, AttributeResponseParser.ParseApp(data, Encoding.UTF8.GetBytes("a"), out _));
    }
}
