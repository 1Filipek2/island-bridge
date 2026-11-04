namespace IslandBridge.Core.Ancs;

public enum LinkStatus
{
    Success,
    Unreachable,
    AccessDenied,
    ProtocolError,
    ServiceMissing,
}

public readonly record struct LinkResult(LinkStatus Status, byte? ProtocolError = null)
{
    public static LinkResult Success { get; } = new(LinkStatus.Success);

    public bool IsSuccess => Status == LinkStatus.Success;

    public bool IsPermissionProblem =>
        Status == LinkStatus.AccessDenied
        || Status == LinkStatus.ProtocolError && ProtocolError is { } code && AncsErrorCodes.IsPermissionError(code);
}

public delegate void AncsPacketHandler(ReadOnlySpan<byte> packet);

public interface IAncsControlPoint
{
    ValueTask<LinkResult> WriteControlPointAsync(ReadOnlyMemory<byte> command, CancellationToken cancellationToken);
}

public interface IAncsLink : IAncsControlPoint, IAsyncDisposable
{
    event AncsPacketHandler? NotificationSourceReceived;

    event AncsPacketHandler? DataSourceReceived;

    event EventHandler<LinkConnectionChangedEventArgs>? ConnectionChanged;

    event EventHandler? ServicesChanged;

    bool IsConnected { get; }

    ValueTask<LinkResult> OpenAsync(CancellationToken cancellationToken);

    ValueTask<LinkResult> SubscribeAsync(CancellationToken cancellationToken);
}

public sealed class LinkConnectionChangedEventArgs(bool isConnected) : EventArgs
{
    public bool IsConnected { get; } = isConnected;
}
