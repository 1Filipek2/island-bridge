using System.IO.Pipes;
using System.Threading.Channels;
using IslandBridge.Core.Diagnostics;
using IslandBridge.Core.Notifications;

namespace IslandBridge.Core.Island;

public enum IslandLinkState
{
    Unknown,
    Connected,
    Unavailable,
    Untrusted,
}

public sealed class IslandPipeClient : IAsyncDisposable
{
    public const int QueueCapacity = 8;

    private readonly ILog _log;
    private readonly Func<NamedPipeClientStream, bool> _verifyServer;
    private readonly string _pipeName;
    private readonly TimeSpan _connectTimeout;
    private readonly Channel<byte[]> _queue = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(QueueCapacity)
    {
        FullMode = BoundedChannelFullMode.DropOldest,
        SingleReader = true,
    });

    private NamedPipeClientStream? _stream;
    private IslandLinkState _state;

    public IslandPipeClient(
        ILog log,
        Func<NamedPipeClientStream, bool>? verifyServer = null,
        string pipeName = IslandProtocol.PipeName,
        TimeSpan? connectTimeout = null)
    {
        ArgumentNullException.ThrowIfNull(log);
        ArgumentException.ThrowIfNullOrWhiteSpace(pipeName);

        _log = log;
        _verifyServer = verifyServer ?? PipeServerVerifier.IsWindhawk;
        _pipeName = pipeName;
        _connectTimeout = connectTimeout ?? TimeSpan.FromMilliseconds(500);
    }

    public event EventHandler? StateChanged;

    public IslandLinkState State => _state;

    public long Sent { get; private set; }

    public long Dropped { get; private set; }

    public bool TrySend(IslandNotification notification, byte[]? icon = null) => _queue.Writer.TryWrite(IslandProtocol.Encode(notification, icon));

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var message in _queue.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
            {
                var sent = await TrySendAsync(message, cancellationToken).ConfigureAwait(false)
                    || (_state != IslandLinkState.Untrusted && await TrySendAsync(message, cancellationToken).ConfigureAwait(false));

                if (sent)
                {
                    Sent++;
                }
                else
                {
                    Dropped++;
                }
            }
        }
        finally
        {
            await CloseAsync().ConfigureAwait(false);
        }
    }

    public async ValueTask DisposeAsync()
    {
        _queue.Writer.TryComplete();
        await CloseAsync().ConfigureAwait(false);
    }

    private async Task<bool> TrySendAsync(byte[] message, CancellationToken cancellationToken)
    {
        var stream = _stream ?? await ConnectAsync(cancellationToken).ConfigureAwait(false);

        if (stream is null)
        {
            return false;
        }

        try
        {
            await stream.WriteAsync(message, cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (IOException)
        {
            await CloseAsync().ConfigureAwait(false);
            return false;
        }
        catch (ObjectDisposedException)
        {
            await CloseAsync().ConfigureAwait(false);
            return false;
        }
    }

    private async Task<NamedPipeClientStream?> ConnectAsync(CancellationToken cancellationToken)
    {
        var stream = new NamedPipeClientStream(
            ".",
            _pipeName,
            PipeDirection.Out,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);

        try
        {
            await stream.ConnectAsync(_connectTimeout, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is TimeoutException or IOException or UnauthorizedAccessException)
        {
            await stream.DisposeAsync().ConfigureAwait(false);
            SetState(IslandLinkState.Unavailable, $"island pipe unavailable ({exception.GetType().Name})");
            return null;
        }

        if (!_verifyServer(stream))
        {
            await stream.DisposeAsync().ConfigureAwait(false);
            SetState(IslandLinkState.Untrusted, "island pipe server is not the expected process; refusing to send");
            return null;
        }

        _stream = stream;
        SetState(IslandLinkState.Connected, "island pipe connected");
        return stream;
    }

    private async Task CloseAsync()
    {
        if (_stream is { } stream)
        {
            _stream = null;
            await stream.DisposeAsync().ConfigureAwait(false);
        }
    }

    private void SetState(IslandLinkState state, string message)
    {
        if (_state == state)
        {
            return;
        }

        _state = state;
        _log.Write(state == IslandLinkState.Untrusted ? LogLevel.Warn : LogLevel.Info, message);
        StateChanged?.Invoke(this, EventArgs.Empty);
    }
}
