namespace IslandBridge.Core.Ancs;

public enum RequestStatus
{
    Success,
    UnknownNotification,
    Timeout,
    InvalidResponse,
    LinkLost,
    LinkError,
}

public readonly record struct RequestResult<T>(RequestStatus Status, T? Value, LinkResult Link = default)
    where T : class
{
    public bool IsSuccess => Status == RequestStatus.Success && Value is not null;
}

public sealed class AncsRequester : IDisposable
{
    private readonly IAncsControlPoint _controlPoint;
    private readonly TimeProvider _timeProvider;
    private readonly TimeSpan _responseTimeout;
    private readonly SemaphoreSlim _oneAtATime = new(1, 1);
    private readonly Lock _gate = new();
    private readonly ResponseAssembler _assembler = new();
    private TaskCompletionSource<AssembledResponse?>? _pending;

    public AncsRequester(IAncsControlPoint controlPoint, TimeProvider timeProvider, TimeSpan responseTimeout)
    {
        ArgumentNullException.ThrowIfNull(controlPoint);
        ArgumentNullException.ThrowIfNull(timeProvider);
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(responseTimeout, TimeSpan.Zero);

        _controlPoint = controlPoint;
        _timeProvider = timeProvider;
        _responseTimeout = responseTimeout;
    }

    public long UnexpectedPackets { get; private set; }

    public async Task<RequestResult<NotificationAttributes>> GetNotificationAttributesAsync(uint uid, CancellationToken cancellationToken)
    {
        var command = AncsCommands.GetNotificationAttributes(uid, AncsCommands.DefaultNotificationAttributes);
        var (status, response, link) = await ExecuteAsync(new ResponseExpectation.Notification(uid), command, cancellationToken).ConfigureAwait(false);
        return new RequestResult<NotificationAttributes>(status, response?.Notification, link);
    }

    public async Task<RequestResult<string>> GetAppDisplayNameAsync(string appIdentifier, CancellationToken cancellationToken)
    {
        var command = AncsCommands.GetAppAttributes(appIdentifier);
        var (status, response, link) = await ExecuteAsync(new ResponseExpectation.App(appIdentifier), command, cancellationToken).ConfigureAwait(false);
        return new RequestResult<string>(status, response?.DisplayName, link);
    }

    public void OnDataSource(ReadOnlySpan<byte> packet)
    {
        lock (_gate)
        {
            if (_pending is not { } pending || !_assembler.IsWaiting)
            {
                UnexpectedPackets++;
                return;
            }

            var result = _assembler.Append(packet);

            if (result.Status != ParseStatus.NeedMoreData)
            {
                _pending = null;
                pending.TrySetResult(result);
            }
        }
    }

    public void CancelPending()
    {
        lock (_gate)
        {
            _assembler.Reset();

            if (_pending is { } pending)
            {
                _pending = null;
                pending.TrySetResult(null);
            }
        }
    }

    public void Dispose()
    {
        CancelPending();
        _assembler.Dispose();
        _oneAtATime.Dispose();
    }

    private async Task<(RequestStatus Status, AssembledResponse? Response, LinkResult Link)> ExecuteAsync(
        ResponseExpectation expectation,
        byte[] command,
        CancellationToken cancellationToken)
    {
        await _oneAtATime.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            var pending = new TaskCompletionSource<AssembledResponse?>(TaskCreationOptions.RunContinuationsAsynchronously);

            lock (_gate)
            {
                _assembler.Begin(expectation);
                _pending = pending;
            }

            try
            {
                var write = await _controlPoint.WriteControlPointAsync(command, cancellationToken).ConfigureAwait(false);

                if (!write.IsSuccess)
                {
                    Abandon(pending);
                    var status = write is { Status: LinkStatus.ProtocolError, ProtocolError: AncsErrorCodes.InvalidParameter }
                        ? RequestStatus.UnknownNotification
                        : RequestStatus.LinkError;
                    return (status, null, write);
                }

                var response = await pending.Task.WaitAsync(_responseTimeout, _timeProvider, cancellationToken).ConfigureAwait(false);

                return response switch
                {
                    null => (RequestStatus.LinkLost, null, LinkResult.Success),
                    { Status: ParseStatus.Complete } => (RequestStatus.Success, response, LinkResult.Success),
                    _ => (RequestStatus.InvalidResponse, null, LinkResult.Success),
                };
            }
            catch (TimeoutException)
            {
                Abandon(pending);
                return (RequestStatus.Timeout, null, LinkResult.Success);
            }
            catch (OperationCanceledException)
            {
                Abandon(pending);
                throw;
            }
        }
        finally
        {
            _oneAtATime.Release();
        }
    }

    private void Abandon(TaskCompletionSource<AssembledResponse?> pending)
    {
        lock (_gate)
        {
            if (ReferenceEquals(_pending, pending))
            {
                _pending = null;
                _assembler.Reset();
            }
        }
    }
}
