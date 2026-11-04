using System.Buffers.Binary;
using System.Text;

namespace IslandBridge.Core.Ancs;

public static class AncsCommands
{
    public static ReadOnlySpan<NotificationAttributeId> DefaultNotificationAttributes =>
    [
        NotificationAttributeId.AppIdentifier,
        NotificationAttributeId.Title,
        NotificationAttributeId.Subtitle,
        NotificationAttributeId.Message,
        NotificationAttributeId.Date,
    ];

    public static byte[] GetNotificationAttributes(uint uid, ReadOnlySpan<NotificationAttributeId> attributes)
    {
        var size = 1 + sizeof(uint);

        foreach (var attribute in attributes)
        {
            size += 1 + (MaxLengthOf(attribute) is null ? 0 : sizeof(ushort));
        }

        var command = new byte[size];
        command[0] = (byte)AncsCommandId.GetNotificationAttributes;
        BinaryPrimitives.WriteUInt32LittleEndian(command.AsSpan(1), uid);
        var offset = 1 + sizeof(uint);

        foreach (var attribute in attributes)
        {
            command[offset++] = (byte)attribute;

            if (MaxLengthOf(attribute) is { } maxLength)
            {
                BinaryPrimitives.WriteUInt16LittleEndian(command.AsSpan(offset), maxLength);
                offset += sizeof(ushort);
            }
        }

        return command;
    }

    public static byte[] GetAppAttributes(string appIdentifier)
    {
        ArgumentNullException.ThrowIfNull(appIdentifier);

        var identifierLength = Encoding.UTF8.GetByteCount(appIdentifier);

        if (identifierLength == 0 || identifierLength > AncsLimits.MaxAppIdentifierBytes || appIdentifier.Contains('\0', StringComparison.Ordinal))
        {
            throw new ArgumentException("The app identifier must be 1 to 256 UTF-8 bytes without NUL.", nameof(appIdentifier));
        }

        var command = new byte[1 + identifierLength + 2];
        command[0] = (byte)AncsCommandId.GetAppAttributes;
        Encoding.UTF8.GetBytes(appIdentifier, command.AsSpan(1));
        command[^2] = 0;
        command[^1] = AppAttributeIds.DisplayName;
        return command;
    }

    internal static ushort? MaxLengthOf(NotificationAttributeId attribute) => attribute switch
    {
        NotificationAttributeId.Title => AncsLimits.TitleMaxLength,
        NotificationAttributeId.Subtitle => AncsLimits.SubtitleMaxLength,
        NotificationAttributeId.Message => AncsLimits.MessageMaxLength,
        _ => null,
    };
}

public static class AppAttributeIds
{
    public const byte DisplayName = 0;
}
