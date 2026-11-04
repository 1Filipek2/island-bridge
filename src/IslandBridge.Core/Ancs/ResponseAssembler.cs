using System.Buffers;
using System.Text;

namespace IslandBridge.Core.Ancs;

public abstract record ResponseExpectation
{
    private ResponseExpectation()
    {
    }

    public sealed record Notification(uint Uid) : ResponseExpectation;

    public sealed record App(string AppIdentifier) : ResponseExpectation
    {
        internal byte[] IdentifierBytes { get; } = Encoding.UTF8.GetBytes(AppIdentifier);
    }
}

public readonly record struct AssembledResponse(ParseStatus Status, NotificationAttributes? Notification, string? DisplayName);

public sealed class ResponseAssembler : IDisposable
{
    private byte[]? _buffer = ArrayPool<byte>.Shared.Rent(AncsLimits.MaxResponseBytes);
    private int _length;
    private ResponseExpectation? _expectation;

    public bool IsWaiting => _expectation is not null;

    public int Fragments { get; private set; }

    public void Begin(ResponseExpectation expectation)
    {
        ArgumentNullException.ThrowIfNull(expectation);
        _expectation = expectation;
        _length = 0;
        Fragments = 0;
    }

    public void Reset()
    {
        _expectation = null;
        _length = 0;
        Fragments = 0;
    }

    public AssembledResponse Append(ReadOnlySpan<byte> fragment)
    {
        ObjectDisposedException.ThrowIf(_buffer is null, this);

        if (_expectation is not { } expectation)
        {
            return new AssembledResponse(ParseStatus.Invalid, null, null);
        }

        Fragments++;

        if (fragment.Length > AncsLimits.MaxResponseBytes - _length)
        {
            Reset();
            return new AssembledResponse(ParseStatus.Invalid, null, null);
        }

        fragment.CopyTo(_buffer.AsSpan(_length));
        _length += fragment.Length;
        var received = _buffer.AsSpan(0, _length);

        var result = expectation switch
        {
            ResponseExpectation.Notification notification => ParseNotification(received, notification.Uid),
            ResponseExpectation.App app => ParseApp(received, app.IdentifierBytes),
            _ => new AssembledResponse(ParseStatus.Invalid, null, null),
        };

        if (result.Status != ParseStatus.NeedMoreData)
        {
            _expectation = null;
            _length = 0;
        }

        return result;
    }

    public void Dispose()
    {
        if (_buffer is { } buffer)
        {
            _buffer = null;
            ArrayPool<byte>.Shared.Return(buffer, clearArray: true);
        }
    }

    private static AssembledResponse ParseNotification(ReadOnlySpan<byte> received, uint uid)
    {
        var status = AttributeResponseParser.ParseNotification(received, uid, AncsCommands.DefaultNotificationAttributes, out var attributes);
        return new AssembledResponse(status, attributes, null);
    }

    private static AssembledResponse ParseApp(ReadOnlySpan<byte> received, ReadOnlySpan<byte> identifier)
    {
        var status = AttributeResponseParser.ParseApp(received, identifier, out var displayName);
        return new AssembledResponse(status, null, displayName);
    }
}
