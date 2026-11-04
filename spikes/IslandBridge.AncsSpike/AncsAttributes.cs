using System.Buffers.Binary;
using System.Text;

namespace IslandBridge.AncsSpike;

internal enum NotificationAttributeId : byte
{
    AppIdentifier = 0,
    Title = 1,
    Subtitle = 2,
    Message = 3,
    MessageSize = 4,
    Date = 5,
}

internal enum ParseStatus
{
    NeedMoreData,
    Complete,
    Invalid,
}

internal readonly record struct AncsAttribute(byte Id, string Value);

internal static class AncsAttributes
{
    public const ushort MaxTextLength = 512;
    public const byte GetNotificationAttributesCommand = 0;
    public const byte GetAppAttributesCommand = 1;
    public const byte AppDisplayNameAttribute = 0;

    private const int TupleHeaderSize = 3;
    private const int NotificationHeaderSize = 5;
    private const int MaxAppIdentifierLength = 256;
    private const int MaxAttributeBytes = MaxTextLength * 4;

    public static ParseStatus ParseNotificationResponse(
        ReadOnlySpan<byte> data,
        uint expectedUid,
        ReadOnlySpan<byte> expectedIds,
        List<AncsAttribute> attributes)
    {
        if (data.Length < NotificationHeaderSize)
        {
            return data.IsEmpty || data[0] == GetNotificationAttributesCommand ? ParseStatus.NeedMoreData : ParseStatus.Invalid;
        }

        if (data[0] != GetNotificationAttributesCommand || BinaryPrimitives.ReadUInt32LittleEndian(data[1..]) != expectedUid)
        {
            return ParseStatus.Invalid;
        }

        return ParseTuples(data[NotificationHeaderSize..], expectedIds, attributes);
    }

    public static ParseStatus ParseAppResponse(
        ReadOnlySpan<byte> data,
        ReadOnlySpan<byte> expectedAppIdentifier,
        List<AncsAttribute> attributes)
    {
        if (data.IsEmpty)
        {
            return ParseStatus.NeedMoreData;
        }

        if (data[0] != GetAppAttributesCommand)
        {
            return ParseStatus.Invalid;
        }

        var body = data[1..];
        var terminator = body.IndexOf((byte)0);

        if (terminator < 0)
        {
            return body.Length > MaxAppIdentifierLength ? ParseStatus.Invalid : ParseStatus.NeedMoreData;
        }

        if (!body[..terminator].SequenceEqual(expectedAppIdentifier))
        {
            return ParseStatus.Invalid;
        }

        return ParseTuples(body[(terminator + 1)..], [AppDisplayNameAttribute], attributes);
    }

    public static byte[] BuildNotificationRequest(uint uid, ReadOnlySpan<byte> attributeIds)
    {
        var size = NotificationHeaderSize;

        foreach (var id in attributeIds)
        {
            size += NeedsMaxLength(id) ? TupleHeaderSize : 1;
        }

        var command = new byte[size];
        command[0] = GetNotificationAttributesCommand;
        BinaryPrimitives.WriteUInt32LittleEndian(command.AsSpan(1), uid);
        var offset = NotificationHeaderSize;

        foreach (var id in attributeIds)
        {
            command[offset++] = id;

            if (NeedsMaxLength(id))
            {
                BinaryPrimitives.WriteUInt16LittleEndian(command.AsSpan(offset), MaxTextLength);
                offset += sizeof(ushort);
            }
        }

        return command;
    }

    public static byte[] BuildAppRequest(ReadOnlySpan<byte> appIdentifier)
    {
        var command = new byte[appIdentifier.Length + 3];
        command[0] = GetAppAttributesCommand;
        appIdentifier.CopyTo(command.AsSpan(1));
        command[^2] = 0;
        command[^1] = AppDisplayNameAttribute;
        return command;
    }

    public static string Sanitize(ReadOnlySpan<byte> utf8, int maxLength)
    {
        var decoded = Encoding.UTF8.GetString(utf8);
        var builder = new StringBuilder(Math.Min(decoded.Length, maxLength));
        var lastWasSpace = true;

        foreach (var rune in decoded.EnumerateRunes())
        {
            if (builder.Length + rune.Utf16SequenceLength > maxLength)
            {
                break;
            }

            if (rune.Value is '\n' or '\r' or '\t' or ' ')
            {
                if (!lastWasSpace)
                {
                    builder.Append(' ');
                    lastWasSpace = true;
                }

                continue;
            }

            if (Rune.IsControl(rune) || IsBidiControl(rune.Value))
            {
                continue;
            }

            builder.Append(rune.ToString());
            lastWasSpace = false;
        }

        return builder.ToString().TrimEnd();
    }

    private static bool NeedsMaxLength(byte id) =>
        id is (byte)NotificationAttributeId.Title or (byte)NotificationAttributeId.Subtitle or (byte)NotificationAttributeId.Message;

    private static bool IsBidiControl(int value) =>
        value is (>= 0x202A and <= 0x202E) or (>= 0x2066 and <= 0x2069) or 0x200E or 0x200F or 0x061C;

    private static ParseStatus ParseTuples(ReadOnlySpan<byte> data, ReadOnlySpan<byte> expectedIds, List<AncsAttribute> attributes)
    {
        attributes.Clear();

        foreach (var expectedId in expectedIds)
        {
            if (data.Length < TupleHeaderSize)
            {
                return data.IsEmpty || data[0] == expectedId ? ParseStatus.NeedMoreData : ParseStatus.Invalid;
            }

            if (data[0] != expectedId)
            {
                return ParseStatus.Invalid;
            }

            var length = BinaryPrimitives.ReadUInt16LittleEndian(data[1..]);

            if (length > MaxAttributeBytes)
            {
                return ParseStatus.Invalid;
            }

            if (data.Length < TupleHeaderSize + length)
            {
                return ParseStatus.NeedMoreData;
            }

            attributes.Add(new AncsAttribute(expectedId, Sanitize(data.Slice(TupleHeaderSize, length), MaxTextLength)));
            data = data[(TupleHeaderSize + length)..];
        }

        return data.IsEmpty ? ParseStatus.Complete : ParseStatus.Invalid;
    }
}
