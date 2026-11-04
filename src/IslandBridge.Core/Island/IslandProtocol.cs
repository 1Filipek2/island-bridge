using System.Buffers.Binary;
using System.Text;
using IslandBridge.Core.Ancs;
using IslandBridge.Core.Text;

namespace IslandBridge.Core.Island;

public static class IslandProtocol
{
    public const string PipeName = "DynamicIslandNotify";
    public const int MaxMessageBytes = 4096;
    public const byte Version = 1;
    public const byte KindNotification = 1;
    public const int HeaderBytes = 4;
    public const int FieldCount = 5;

    public static ReadOnlySpan<int> MaxFieldBytes => [256, 256, 1024, 1024, 3072];

    public static ReadOnlySpan<int> MaxFieldChars => [64, 128, 120, 120, 512];

    public static byte[] Encode(PhoneNotification notification)
    {
        ArgumentNullException.ThrowIfNull(notification);

        string[] values =
        [
            notification.AppName,
            notification.AppIdentifier,
            notification.Title,
            notification.Subtitle,
            notification.Message,
        ];

        var fields = new byte[FieldCount][];
        var size = HeaderBytes;

        for (var index = 0; index < FieldCount; index++)
        {
            var clean = TextSanitizer.Sanitize(values[index] ?? string.Empty, MaxFieldChars[index]);
            fields[index] = TruncateUtf8(clean, MaxFieldBytes[index]);
            size += sizeof(ushort) + fields[index].Length;
        }

        var message = new byte[size];
        message[0] = Version;
        message[1] = KindNotification;
        message[2] = (byte)notification.Category;
        message[3] = (byte)notification.Flags;
        var offset = HeaderBytes;

        foreach (var field in fields)
        {
            BinaryPrimitives.WriteUInt16LittleEndian(message.AsSpan(offset), (ushort)field.Length);
            offset += sizeof(ushort);
            field.CopyTo(message, offset);
            offset += field.Length;
        }

        return message;
    }

    internal static byte[] TruncateUtf8(string text, int maxBytes)
    {
        var bytes = Encoding.UTF8.GetBytes(text);

        if (bytes.Length <= maxBytes)
        {
            return bytes;
        }

        var length = 0;

        foreach (var rune in text.EnumerateRunes())
        {
            if (length + rune.Utf8SequenceLength > maxBytes)
            {
                break;
            }

            length += rune.Utf8SequenceLength;
        }

        return bytes[..length];
    }
}
