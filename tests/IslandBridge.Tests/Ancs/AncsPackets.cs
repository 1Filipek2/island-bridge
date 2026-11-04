using System.Buffers.Binary;
using System.Text;
using IslandBridge.Core.Ancs;

namespace IslandBridge.Tests.Ancs;

internal static class AncsPackets
{
    public static byte[] NotificationSource(AncsEventId eventId, AncsEventFlags flags, AncsCategory category, uint uid, byte count = 1)
    {
        var packet = new byte[NotificationSourceEvent.Size];
        packet[0] = (byte)eventId;
        packet[1] = (byte)flags;
        packet[2] = (byte)category;
        packet[3] = count;
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), uid);
        return packet;
    }

    public static byte[] NotificationResponse(
        uint uid,
        string appIdentifier = "net.whatsapp.WhatsApp",
        string title = "Alice",
        string subtitle = "",
        string message = "Hello there",
        string date = "20261104T101748")
    {
        var buffer = new List<byte> { (byte)AncsCommandId.GetNotificationAttributes };
        var uidBytes = new byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(uidBytes, uid);
        buffer.AddRange(uidBytes);
        AddTuple(buffer, NotificationAttributeId.AppIdentifier, Encoding.UTF8.GetBytes(appIdentifier));
        AddTuple(buffer, NotificationAttributeId.Title, Encoding.UTF8.GetBytes(title));
        AddTuple(buffer, NotificationAttributeId.Subtitle, Encoding.UTF8.GetBytes(subtitle));
        AddTuple(buffer, NotificationAttributeId.Message, Encoding.UTF8.GetBytes(message));
        AddTuple(buffer, NotificationAttributeId.Date, Encoding.UTF8.GetBytes(date));
        return [.. buffer];
    }

    public static byte[] AppResponse(string appIdentifier, string displayName)
    {
        var buffer = new List<byte> { (byte)AncsCommandId.GetAppAttributes };
        buffer.AddRange(Encoding.UTF8.GetBytes(appIdentifier));
        buffer.Add(0);
        AddTuple(buffer, (NotificationAttributeId)AppAttributeIds.DisplayName, Encoding.UTF8.GetBytes(displayName));
        return [.. buffer];
    }

    public static void AddTuple(List<byte> buffer, NotificationAttributeId id, ReadOnlySpan<byte> value)
    {
        buffer.Add((byte)id);
        var length = new byte[sizeof(ushort)];
        BinaryPrimitives.WriteUInt16LittleEndian(length, (ushort)value.Length);
        buffer.AddRange(length);
        buffer.AddRange(value.ToArray());
    }

    public static IEnumerable<byte[]> Split(byte[] packet, params int[] cuts)
    {
        var start = 0;

        foreach (var cut in cuts)
        {
            yield return packet[start..cut];
            start = cut;
        }

        yield return packet[start..];
    }
}
