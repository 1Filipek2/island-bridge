using System.Buffers.Binary;
using System.Globalization;
using IslandBridge.Core.Text;

namespace IslandBridge.Core.Ancs;

public enum ParseStatus
{
    NeedMoreData,
    Complete,
    Invalid,
}

public sealed record NotificationAttributes(
    string AppIdentifier,
    string Title,
    string Subtitle,
    string Message,
    DateTime? Date)
{
    public static NotificationAttributes Empty { get; } = new(string.Empty, string.Empty, string.Empty, string.Empty, null);
}

public static class AttributeResponseParser
{
    private const int TupleHeaderSize = 3;
    private const int NotificationHeaderSize = 1 + sizeof(uint);
    private const string DateFormat = "yyyyMMdd'T'HHmmss";

    public static ParseStatus ParseNotification(
        ReadOnlySpan<byte> data,
        uint expectedUid,
        ReadOnlySpan<NotificationAttributeId> expectedIds,
        out NotificationAttributes? attributes)
    {
        attributes = null;

        if (data.IsEmpty)
        {
            return ParseStatus.NeedMoreData;
        }

        if (data[0] != (byte)AncsCommandId.GetNotificationAttributes)
        {
            return ParseStatus.Invalid;
        }

        if (data.Length < NotificationHeaderSize)
        {
            return ParseStatus.NeedMoreData;
        }

        if (BinaryPrimitives.ReadUInt32LittleEndian(data[1..]) != expectedUid)
        {
            return ParseStatus.Invalid;
        }

        var tuples = data[NotificationHeaderSize..];
        var status = ScanTuples(tuples, expectedIds);

        if (status != ParseStatus.Complete)
        {
            return status;
        }

        attributes = DecodeNotification(tuples, expectedIds);
        return ParseStatus.Complete;
    }

    public static ParseStatus ParseApp(ReadOnlySpan<byte> data, ReadOnlySpan<byte> expectedAppIdentifier, out string? displayName)
    {
        displayName = null;

        if (data.IsEmpty)
        {
            return ParseStatus.NeedMoreData;
        }

        if (data[0] != (byte)AncsCommandId.GetAppAttributes)
        {
            return ParseStatus.Invalid;
        }

        var body = data[1..];
        var terminator = body[..Math.Min(body.Length, AncsLimits.MaxAppIdentifierBytes + 1)].IndexOf((byte)0);

        if (terminator < 0)
        {
            return body.Length > AncsLimits.MaxAppIdentifierBytes ? ParseStatus.Invalid : ParseStatus.NeedMoreData;
        }

        if (!body[..terminator].SequenceEqual(expectedAppIdentifier))
        {
            return ParseStatus.Invalid;
        }

        var tuples = body[(terminator + 1)..];
        ReadOnlySpan<byte> expectedIds = [AppAttributeIds.DisplayName];
        var status = ScanTuples(tuples, expectedIds);

        if (status != ParseStatus.Complete)
        {
            return status;
        }

        displayName = TextSanitizer.Sanitize(tuples.Slice(TupleHeaderSize, ReadLength(tuples)), AncsLimits.MaxDisplayNameLength);
        return ParseStatus.Complete;
    }

    internal static DateTime? ParseDate(string value) =>
        DateTime.TryParseExact(value, DateFormat, CultureInfo.InvariantCulture, DateTimeStyles.None, out var date) ? date : null;

    private static ParseStatus ScanTuples(ReadOnlySpan<byte> data, ReadOnlySpan<NotificationAttributeId> expectedIds) =>
        ScanTuples(data, System.Runtime.InteropServices.MemoryMarshal.AsBytes(expectedIds));

    private static ParseStatus ScanTuples(ReadOnlySpan<byte> data, ReadOnlySpan<byte> expectedIds)
    {
        foreach (var expectedId in expectedIds)
        {
            if (data.IsEmpty)
            {
                return ParseStatus.NeedMoreData;
            }

            if (data[0] != expectedId)
            {
                return ParseStatus.Invalid;
            }

            if (data.Length < TupleHeaderSize)
            {
                return ParseStatus.NeedMoreData;
            }

            var length = ReadLength(data);

            if (length > AncsLimits.MaxAttributeBytes)
            {
                return ParseStatus.Invalid;
            }

            if (data.Length < TupleHeaderSize + length)
            {
                return ParseStatus.NeedMoreData;
            }

            data = data[(TupleHeaderSize + length)..];
        }

        return data.IsEmpty ? ParseStatus.Complete : ParseStatus.Invalid;
    }

    private static NotificationAttributes DecodeNotification(ReadOnlySpan<byte> data, ReadOnlySpan<NotificationAttributeId> ids)
    {
        var result = NotificationAttributes.Empty;

        foreach (var id in ids)
        {
            var length = ReadLength(data);
            var value = data.Slice(TupleHeaderSize, length);
            data = data[(TupleHeaderSize + length)..];

            result = id switch
            {
                NotificationAttributeId.AppIdentifier => result with { AppIdentifier = TextSanitizer.Sanitize(value, AncsLimits.MaxAppIdentifierLength) },
                NotificationAttributeId.Title => result with { Title = TextSanitizer.Sanitize(value, AncsLimits.TitleMaxLength) },
                NotificationAttributeId.Subtitle => result with { Subtitle = TextSanitizer.Sanitize(value, AncsLimits.SubtitleMaxLength) },
                NotificationAttributeId.Message => result with { Message = TextSanitizer.Sanitize(value, AncsLimits.MessageMaxLength) },
                NotificationAttributeId.Date => result with { Date = ParseDate(TextSanitizer.Sanitize(value, 32)) },
                _ => result,
            };
        }

        return result;
    }

    private static int ReadLength(ReadOnlySpan<byte> tuple) => BinaryPrimitives.ReadUInt16LittleEndian(tuple[1..]);
}
