using System.Diagnostics;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using System.Threading.Channels;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;

namespace IslandBridge.AncsSpike;

internal sealed class AncsSession(SpikeOptions options)
{
    private const int MaxResponseBytes = 8 * 1024;
    private static readonly TimeSpan ResponseTimeout = TimeSpan.FromSeconds(5);

    private static readonly byte[] RequestedAttributes =
    [
        (byte)NotificationAttributeId.AppIdentifier,
        (byte)NotificationAttributeId.Title,
        (byte)NotificationAttributeId.Subtitle,
        (byte)NotificationAttributeId.Message,
        (byte)NotificationAttributeId.Date,
    ];

    private readonly Channel<(NotificationSourceEvent Event, long ReceivedAt)> _events =
        Channel.CreateBounded<(NotificationSourceEvent, long)>(new BoundedChannelOptions(64)
        {
            FullMode = BoundedChannelFullMode.DropOldest,
            SingleReader = true,
        });

    private readonly Dictionary<string, string> _appNames = new(StringComparer.Ordinal);
    private readonly Lock _responseGate = new();
    private readonly byte[] _responseBuffer = new byte[MaxResponseBytes];
    private int _responseLength;
    private PendingResponse? _pending;
    private GattCharacteristic? _controlPoint;

    public async Task<int> RunAsync(CancellationToken cancellationToken)
    {
        var device = await FindDeviceAsync(cancellationToken);

        if (device is null)
        {
            return 2;
        }

        using (device)
        {
            device.ConnectionStatusChanged += (sender, _) => SpikeLog.Write($"connection status: {sender.ConnectionStatus}");

            using var session = await GattSession.FromDeviceIdAsync(device.BluetoothDeviceId).AsTask(cancellationToken);
            session.MaintainConnection = true;
            session.SessionStatusChanged += (_, args) => SpikeLog.Write($"session status: {args.Status} ({args.Error})");
            SpikeLog.Write($"GattSession open, MaintainConnection={session.MaintainConnection}, CanMaintainConnection={session.CanMaintainConnection}");

            var servicesResult = await device.GetGattServicesForUuidAsync(AncsUuids.Service, BluetoothCacheMode.Uncached).AsTask(cancellationToken);

            if (servicesResult.Status != GattCommunicationStatus.Success || servicesResult.Services.Count == 0)
            {
                ReportFailure("ANCS service lookup", servicesResult.Status, servicesResult.ProtocolError);
                return 3;
            }

            using var service = servicesResult.Services[0];
            var notificationSource = await GetCharacteristicAsync(service, AncsUuids.NotificationSource, cancellationToken);
            var controlPoint = await GetCharacteristicAsync(service, AncsUuids.ControlPoint, cancellationToken);
            var dataSource = await GetCharacteristicAsync(service, AncsUuids.DataSource, cancellationToken);

            if (notificationSource is null || controlPoint is null || dataSource is null)
            {
                return 4;
            }

            _controlPoint = controlPoint;
            dataSource.ValueChanged += OnDataSourceChanged;
            notificationSource.ValueChanged += OnNotificationSourceChanged;

            if (!await SubscribeAsync(dataSource, "Data Source", cancellationToken)
                || !await SubscribeAsync(notificationSource, "Notification Source", cancellationToken))
            {
                return 5;
            }

            SpikeLog.Write("subscribed, waiting for notifications (Ctrl+C to stop)");

            try
            {
                await ProcessEventsAsync(cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                SpikeLog.Write("stopping");
            }

            notificationSource.ValueChanged -= OnNotificationSourceChanged;
            dataSource.ValueChanged -= OnDataSourceChanged;
            await UnsubscribeAsync(notificationSource);
            await UnsubscribeAsync(dataSource);
            return 0;
        }
    }

    private async Task<BluetoothLEDevice?> FindDeviceAsync(CancellationToken cancellationToken)
    {
        var selector = BluetoothLEDevice.GetDeviceSelectorFromPairingState(true);
        var paired = await DeviceInformation.FindAllAsync(selector).AsTask(cancellationToken);
        SpikeLog.Write($"paired BLE devices: {paired.Count}");

        foreach (var info in paired)
        {
            if (options.DeviceName is { } wanted && !info.Name.Contains(wanted, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var device = await BluetoothLEDevice.FromIdAsync(info.Id).AsTask(cancellationToken);

            if (device is null)
            {
                SpikeLog.Write($"  {info.Name}: cannot open");
                continue;
            }

            var probe = await device.GetGattServicesForUuidAsync(AncsUuids.Service, BluetoothCacheMode.Uncached).AsTask(cancellationToken);
            var hasAncs = probe.Status == GattCommunicationStatus.Success && probe.Services.Count > 0;

            foreach (var probed in probe.Services)
            {
                probed.Dispose();
            }

            SpikeLog.Write($"  {info.Name}: ANCS {(hasAncs ? "found" : $"not found ({probe.Status})")}, {device.ConnectionStatus}");

            if (hasAncs)
            {
                SpikeLog.Write($"using {info.Name}, DeviceId {device.DeviceId}");
                return device;
            }

            device.Dispose();
        }

        SpikeLog.Write("no paired device exposes ANCS");
        return null;
    }

    private static async Task<GattCharacteristic?> GetCharacteristicAsync(GattDeviceService service, Guid uuid, CancellationToken cancellationToken)
    {
        var result = await service.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode.Uncached).AsTask(cancellationToken);

        if (result.Status != GattCommunicationStatus.Success || result.Characteristics.Count == 0)
        {
            ReportFailure($"characteristic {uuid}", result.Status, result.ProtocolError);
            return null;
        }

        var characteristic = result.Characteristics[0];
        characteristic.ProtectionLevel = GattProtectionLevel.EncryptionRequired;
        return characteristic;
    }

    private static async Task<bool> SubscribeAsync(GattCharacteristic characteristic, string name, CancellationToken cancellationToken)
    {
        var result = await characteristic
            .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify)
            .AsTask(cancellationToken);

        if (result.Status != GattCommunicationStatus.Success)
        {
            ReportFailure($"subscribe {name}", result.Status, result.ProtocolError);
            return false;
        }

        SpikeLog.Write($"subscribed to {name}");
        return true;
    }

    private static async Task UnsubscribeAsync(GattCharacteristic characteristic)
    {
        try
        {
            await characteristic.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(GattClientCharacteristicConfigurationDescriptorValue.None);
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private static void ReportFailure(string step, GattCommunicationStatus status, byte? protocolError)
    {
        SpikeLog.Write($"{step} failed: {status}, protocol error {(protocolError is { } code ? $"0x{code:X2}" : "none")}");

        if (status is GattCommunicationStatus.AccessDenied or GattCommunicationStatus.ProtocolError)
        {
            SpikeLog.Write("hint: enable 'Share System Notifications' for this PC in iPhone Settings > Bluetooth > (i)");
        }
    }

    private void OnNotificationSourceChanged(GattCharacteristic sender, GattValueChangedEventArgs args)
    {
        var receivedAt = Stopwatch.GetTimestamp();
        var data = args.CharacteristicValue.ToArray();

        if (!NotificationSourceEvent.TryParse(data, out var notification))
        {
            SpikeLog.Write($"NS: malformed packet ({data.Length} bytes)");
            return;
        }

        SpikeLog.Write($"NS: {notification.EventId} uid={notification.Uid} category={notification.Category} flags={notification.Flags} count={notification.CategoryCount}");
        _events.Writer.TryWrite((notification, receivedAt));
    }

    private void OnDataSourceChanged(GattCharacteristic sender, GattValueChangedEventArgs args)
    {
        var data = args.CharacteristicValue.ToArray();

        lock (_responseGate)
        {
            if (_pending is not { } pending)
            {
                SpikeLog.Write($"DS: unexpected packet ({data.Length} bytes), dropped");
                return;
            }

            pending.Fragments++;

            if (_responseLength + data.Length > _responseBuffer.Length)
            {
                CompletePending(pending, null);
                return;
            }

            data.CopyTo(_responseBuffer.AsSpan(_responseLength));
            _responseLength += data.Length;
            var attributes = new List<AncsAttribute>();
            var received = _responseBuffer.AsSpan(0, _responseLength);
            var status = pending.AppIdentifier is { } appIdentifier
                ? AncsAttributes.ParseAppResponse(received, appIdentifier, attributes)
                : AncsAttributes.ParseNotificationResponse(received, pending.Uid, RequestedAttributes, attributes);

            switch (status)
            {
                case ParseStatus.Complete:
                    CompletePending(pending, attributes);
                    break;
                case ParseStatus.Invalid:
                    CompletePending(pending, null);
                    break;
            }
        }
    }

    private void CompletePending(PendingResponse pending, List<AncsAttribute>? attributes)
    {
        _pending = null;
        _responseLength = 0;
        pending.Completion.TrySetResult(attributes);
    }

    private async Task ProcessEventsAsync(CancellationToken cancellationToken)
    {
        await foreach (var (notification, receivedAt) in _events.Reader.ReadAllAsync(cancellationToken))
        {
            if (notification.EventId != AncsEventId.Added)
            {
                continue;
            }

            if (notification.Flags.HasFlag(AncsEventFlags.PreExisting) && !options.FetchExisting)
            {
                continue;
            }

            var attributes = await ExecuteAsync(
                PendingResponse.ForNotification(notification.Uid),
                AncsAttributes.BuildNotificationRequest(notification.Uid, RequestedAttributes),
                cancellationToken);

            if (attributes is null)
            {
                continue;
            }

            var appIdentifier = Find(attributes, NotificationAttributeId.AppIdentifier);
            var appName = await ResolveAppNameAsync(appIdentifier, cancellationToken);
            var latency = Stopwatch.GetElapsedTime(receivedAt).TotalMilliseconds;
            var title = Find(attributes, NotificationAttributeId.Title);
            var subtitle = Find(attributes, NotificationAttributeId.Subtitle);
            var message = Find(attributes, NotificationAttributeId.Message);

            SpikeLog.Write(options.ShowText
                ? $"NOTIFICATION uid={notification.Uid} app={appName} ({appIdentifier}) [{notification.Category}] title=\"{title}\" subtitle=\"{subtitle}\" message=\"{message}\" date={Find(attributes, NotificationAttributeId.Date)} latency={latency:F1} ms"
                : $"NOTIFICATION uid={notification.Uid} app={appName} ({appIdentifier}) [{notification.Category}] title={title.Length} subtitle={subtitle.Length} message={message.Length} chars, latency={latency:F1} ms");
        }
    }

    private async Task<string> ResolveAppNameAsync(string appIdentifier, CancellationToken cancellationToken)
    {
        if (appIdentifier.Length == 0)
        {
            return "?";
        }

        if (_appNames.TryGetValue(appIdentifier, out var cached))
        {
            return cached;
        }

        var identifierBytes = Encoding.UTF8.GetBytes(appIdentifier);
        var attributes = await ExecuteAsync(
            PendingResponse.ForApp(identifierBytes),
            AncsAttributes.BuildAppRequest(identifierBytes),
            cancellationToken);
        var name = attributes is [{ Value.Length: > 0 } displayName] ? displayName.Value : appIdentifier;
        _appNames[appIdentifier] = name;
        return name;
    }

    private async Task<List<AncsAttribute>?> ExecuteAsync(PendingResponse pending, byte[] command, CancellationToken cancellationToken)
    {
        if (_controlPoint is null)
        {
            return null;
        }

        lock (_responseGate)
        {
            _pending = pending;
            _responseLength = 0;
        }

        var started = Stopwatch.GetTimestamp();
        var write = await _controlPoint.WriteValueWithResultAsync(command.AsBuffer(), GattWriteOption.WriteWithResponse).AsTask(cancellationToken);

        if (write.Status != GattCommunicationStatus.Success)
        {
            ClearPending(pending);
            ReportFailure($"control point command {command[0]}", write.Status, write.ProtocolError);
            return null;
        }

        try
        {
            var attributes = await pending.Completion.Task.WaitAsync(ResponseTimeout, cancellationToken);
            var elapsed = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
            SpikeLog.Write(attributes is null
                ? $"DS: invalid response to command {command[0]} after {pending.Fragments} fragment(s)"
                : $"DS: response to command {command[0]} in {pending.Fragments} fragment(s), {elapsed:F1} ms");
            return attributes;
        }
        catch (TimeoutException)
        {
            ClearPending(pending);
            SpikeLog.Write($"DS: timeout waiting for command {command[0]} ({pending.Fragments} fragment(s) received)");
            return null;
        }
    }

    private void ClearPending(PendingResponse pending)
    {
        lock (_responseGate)
        {
            if (ReferenceEquals(_pending, pending))
            {
                _pending = null;
                _responseLength = 0;
            }
        }
    }

    private static string Find(List<AncsAttribute> attributes, NotificationAttributeId id)
    {
        foreach (var attribute in attributes)
        {
            if (attribute.Id == (byte)id)
            {
                return attribute.Value;
            }
        }

        return string.Empty;
    }

    private sealed class PendingResponse
    {
        private PendingResponse(uint uid, byte[]? appIdentifier)
        {
            Uid = uid;
            AppIdentifier = appIdentifier;
        }

        public uint Uid { get; }

        public byte[]? AppIdentifier { get; }

        public int Fragments { get; set; }

        public TaskCompletionSource<List<AncsAttribute>?> Completion { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public static PendingResponse ForNotification(uint uid) => new(uid, null);

        public static PendingResponse ForApp(byte[] appIdentifier) => new(0, appIdentifier);
    }
}
