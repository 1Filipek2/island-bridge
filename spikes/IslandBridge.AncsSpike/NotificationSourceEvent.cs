using System.Buffers.Binary;

namespace IslandBridge.AncsSpike;

internal enum AncsEventId : byte
{
    Added = 0,
    Modified = 1,
    Removed = 2,
}

[Flags]
internal enum AncsEventFlags : byte
{
    None = 0,
    Silent = 1 << 0,
    Important = 1 << 1,
    PreExisting = 1 << 2,
    PositiveAction = 1 << 3,
    NegativeAction = 1 << 4,
}

internal enum AncsCategory : byte
{
    Other = 0,
    IncomingCall = 1,
    MissedCall = 2,
    Voicemail = 3,
    Social = 4,
    Schedule = 5,
    Email = 6,
    News = 7,
    HealthAndFitness = 8,
    BusinessAndFinance = 9,
    Location = 10,
    Entertainment = 11,
}

internal readonly record struct NotificationSourceEvent(
    AncsEventId EventId,
    AncsEventFlags Flags,
    AncsCategory Category,
    byte CategoryCount,
    uint Uid)
{
    public const int Size = 8;

    public static bool TryParse(ReadOnlySpan<byte> data, out NotificationSourceEvent result)
    {
        result = default;

        if (data.Length != Size || data[0] > (byte)AncsEventId.Removed)
        {
            return false;
        }

        result = new NotificationSourceEvent(
            (AncsEventId)data[0],
            (AncsEventFlags)data[1],
            (AncsCategory)data[2],
            data[3],
            BinaryPrimitives.ReadUInt32LittleEndian(data[4..]));
        return true;
    }
}
