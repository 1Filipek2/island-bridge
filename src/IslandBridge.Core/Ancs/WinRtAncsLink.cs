using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.GenericAttributeProfile;

namespace IslandBridge.Core.Ancs;

public sealed class WinRtAncsLink : IAncsLink
{
    private readonly string _deviceId;
    private readonly SemaphoreSlim _operation = new(1, 1);
    private BluetoothLEDevice? _device;
    private GattSession? _session;
    private GattDeviceService? _service;
    private GattCharacteristic? _notificationSource;
    private GattCharacteristic? _controlPoint;
    private GattCharacteristic? _dataSource;
    private bool _disposed;

    public WinRtAncsLink(string deviceId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(deviceId);
        _deviceId = deviceId;
    }

    public event AncsPacketHandler? NotificationSourceReceived;

    public event AncsPacketHandler? DataSourceReceived;

    public event EventHandler<LinkConnectionChangedEventArgs>? ConnectionChanged;

    public event EventHandler? ServicesChanged;

    public bool IsConnected => _device?.ConnectionStatus == BluetoothConnectionStatus.Connected;

    public string? DeviceName => _device?.Name;

    public async ValueTask<LinkResult> OpenAsync(CancellationToken cancellationToken)
    {
        await _operation.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            if (_device is not null && _session is { SessionStatus: GattSessionStatus.Active })
            {
                return LinkResult.Success;
            }

            ReleaseAll();
            var device = await BluetoothLEDevice.FromIdAsync(_deviceId).AsTask(cancellationToken).ConfigureAwait(false);

            if (device is null)
            {
                return new LinkResult(LinkStatus.Unreachable);
            }

            if (!device.DeviceInformation.Pairing.IsPaired)
            {
                device.Dispose();
                return new LinkResult(LinkStatus.AccessDenied);
            }

            device.ConnectionStatusChanged += OnConnectionStatusChanged;
            device.GattServicesChanged += OnGattServicesChanged;
            _device = device;

            var session = await GattSession.FromDeviceIdAsync(device.BluetoothDeviceId).AsTask(cancellationToken).ConfigureAwait(false);

            if (session is null)
            {
                return new LinkResult(LinkStatus.Unreachable);
            }

            session.MaintainConnection = true;
            _session = session;
            return LinkResult.Success;
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not ObjectDisposedException)
        {
            ReleaseAll();
            return new LinkResult(LinkStatus.Unreachable);
        }
        finally
        {
            _operation.Release();
        }
    }

    public async ValueTask<LinkResult> SubscribeAsync(CancellationToken cancellationToken)
    {
        await _operation.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            if (_device is not { } device)
            {
                return new LinkResult(LinkStatus.Unreachable);
            }

            ReleaseService();
            var services = await device.GetGattServicesForUuidAsync(AncsUuids.Service, BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);

            if (services.Status != GattCommunicationStatus.Success)
            {
                return Map(services.Status, services.ProtocolError);
            }

            if (services.Services.Count == 0)
            {
                return new LinkResult(LinkStatus.ServiceMissing);
            }

            _service = services.Services[0];

            for (var i = 1; i < services.Services.Count; i++)
            {
                services.Services[i].Dispose();
            }

            var (notificationSourceResult, notificationSource) = await GetCharacteristicAsync(_service, AncsUuids.NotificationSource, cancellationToken).ConfigureAwait(false);
            var (controlPointResult, controlPoint) = await GetCharacteristicAsync(_service, AncsUuids.ControlPoint, cancellationToken).ConfigureAwait(false);
            var (dataSourceResult, dataSource) = await GetCharacteristicAsync(_service, AncsUuids.DataSource, cancellationToken).ConfigureAwait(false);

            if (notificationSource is null || controlPoint is null || dataSource is null)
            {
                return !notificationSourceResult.IsSuccess ? notificationSourceResult
                    : !controlPointResult.IsSuccess ? controlPointResult
                    : dataSourceResult;
            }

            _controlPoint = controlPoint;
            _dataSource = dataSource;
            _notificationSource = notificationSource;
            dataSource.ValueChanged += OnDataSourceValueChanged;
            notificationSource.ValueChanged += OnNotificationSourceValueChanged;

            var dataSourceSubscription = await EnableNotificationsAsync(dataSource, cancellationToken).ConfigureAwait(false);

            if (!dataSourceSubscription.IsSuccess)
            {
                return dataSourceSubscription;
            }

            return await EnableNotificationsAsync(notificationSource, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is not OperationCanceledException and not ObjectDisposedException)
        {
            ReleaseService();
            return new LinkResult(LinkStatus.Unreachable);
        }
        finally
        {
            _operation.Release();
        }
    }

    public async ValueTask<LinkResult> WriteControlPointAsync(ReadOnlyMemory<byte> command, CancellationToken cancellationToken)
    {
        if (_controlPoint is not { } controlPoint)
        {
            return new LinkResult(LinkStatus.Unreachable);
        }

        try
        {
            var result = await controlPoint
                .WriteValueWithResultAsync(command.ToArray().AsBuffer(), GattWriteOption.WriteWithResponse)
                .AsTask(cancellationToken)
                .ConfigureAwait(false);
            return Map(result.Status, result.ProtocolError);
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            return new LinkResult(LinkStatus.Unreachable);
        }
    }

    public async ValueTask DisposeAsync()
    {
        await _operation.WaitAsync().ConfigureAwait(false);

        try
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            ReleaseAll();
        }
        finally
        {
            _operation.Release();
        }

        _operation.Dispose();
    }

    private static async Task<(LinkResult Result, GattCharacteristic? Characteristic)> GetCharacteristicAsync(
        GattDeviceService service,
        Guid uuid,
        CancellationToken cancellationToken)
    {
        var result = await service.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);

        if (result.Status != GattCommunicationStatus.Success)
        {
            return (Map(result.Status, result.ProtocolError), null);
        }

        if (result.Characteristics.Count == 0)
        {
            return (new LinkResult(LinkStatus.ServiceMissing), null);
        }

        var characteristic = result.Characteristics[0];
        characteristic.ProtectionLevel = GattProtectionLevel.EncryptionRequired;
        return (LinkResult.Success, characteristic);
    }

    private static async Task<LinkResult> EnableNotificationsAsync(GattCharacteristic characteristic, CancellationToken cancellationToken)
    {
        var result = await characteristic
            .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify)
            .AsTask(cancellationToken)
            .ConfigureAwait(false);
        return Map(result.Status, result.ProtocolError);
    }

    private static LinkResult Map(GattCommunicationStatus status, byte? protocolError) => status switch
    {
        GattCommunicationStatus.Success => LinkResult.Success,
        GattCommunicationStatus.AccessDenied => new LinkResult(LinkStatus.AccessDenied),
        GattCommunicationStatus.ProtocolError => new LinkResult(LinkStatus.ProtocolError, protocolError),
        _ => new LinkResult(LinkStatus.Unreachable),
    };

    private void OnNotificationSourceValueChanged(GattCharacteristic sender, GattValueChangedEventArgs args) =>
        NotificationSourceReceived?.Invoke(args.CharacteristicValue.ToArray());

    private void OnDataSourceValueChanged(GattCharacteristic sender, GattValueChangedEventArgs args) =>
        DataSourceReceived?.Invoke(args.CharacteristicValue.ToArray());

    private void OnConnectionStatusChanged(BluetoothLEDevice sender, object args) =>
        ConnectionChanged?.Invoke(this, new LinkConnectionChangedEventArgs(sender.ConnectionStatus == BluetoothConnectionStatus.Connected));

    private void OnGattServicesChanged(BluetoothLEDevice sender, object args) => ServicesChanged?.Invoke(this, EventArgs.Empty);

    private void ReleaseService()
    {
        if (_notificationSource is { } notificationSource)
        {
            notificationSource.ValueChanged -= OnNotificationSourceValueChanged;
        }

        if (_dataSource is { } dataSource)
        {
            dataSource.ValueChanged -= OnDataSourceValueChanged;
        }

        _notificationSource = null;
        _controlPoint = null;
        _dataSource = null;
        _service?.Dispose();
        _service = null;
    }

    private void ReleaseAll()
    {
        ReleaseService();
        _session?.Dispose();
        _session = null;

        if (_device is { } device)
        {
            device.ConnectionStatusChanged -= OnConnectionStatusChanged;
            device.GattServicesChanged -= OnGattServicesChanged;
            device.Dispose();
        }

        _device = null;
    }
}

public sealed record AncsDeviceCandidate(string DeviceId, string Name, bool IsConnected);

public static class AncsDeviceFinder
{
    public static async Task<IReadOnlyList<AncsDeviceCandidate>> FindAsync(CancellationToken cancellationToken)
    {
        var selector = BluetoothLEDevice.GetDeviceSelectorFromPairingState(true);
        var paired = await Windows.Devices.Enumeration.DeviceInformation.FindAllAsync(selector).AsTask(cancellationToken).ConfigureAwait(false);
        var candidates = new List<AncsDeviceCandidate>();

        foreach (var info in paired)
        {
            using var device = await BluetoothLEDevice.FromIdAsync(info.Id).AsTask(cancellationToken).ConfigureAwait(false);

            if (device is null)
            {
                continue;
            }

            var services = await device.GetGattServicesForUuidAsync(AncsUuids.Service, BluetoothCacheMode.Uncached).AsTask(cancellationToken).ConfigureAwait(false);
            var hasAncs = services.Status == GattCommunicationStatus.Success && services.Services.Count > 0;

            foreach (var service in services.Services)
            {
                service.Dispose();
            }

            if (hasAncs)
            {
                candidates.Add(new AncsDeviceCandidate(device.DeviceId, info.Name, device.ConnectionStatus == BluetoothConnectionStatus.Connected));
            }
        }

        return candidates;
    }
}
