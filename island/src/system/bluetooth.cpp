#include "system/bluetooth.h"
#include "ui/icons.h"
#include "core/model.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{
BluetoothDeviceCategory classify_bluetooth_device(const winrt::Windows::Devices::Enumeration::DeviceInformation& info)
{
    std::wstring name = to_lower_copy(std::wstring(info.Name().c_str()));

    if (name.find(L"headphone") != std::wstring::npos || name.find(L"headset") != std::wstring::npos ||
        name.find(L"earbud") != std::wstring::npos || name.find(L"buds") != std::wstring::npos ||
        name.find(L"airpods") != std::wstring::npos)
    {
        return BluetoothDeviceCategory::Headphones;
    }
    if (name.find(L"speaker") != std::wstring::npos || name.find(L"soundbar") != std::wstring::npos ||
        name.find(L"boombox") != std::wstring::npos)
    {
        return BluetoothDeviceCategory::Speaker;
    }
    if (name.find(L"mouse") != std::wstring::npos)
    {
        return BluetoothDeviceCategory::Mouse;
    }
    if (name.find(L"keyboard") != std::wstring::npos)
    {
        return BluetoothDeviceCategory::Keyboard;
    }
    if (name.find(L"iphone") != std::wstring::npos || name.find(L"phone") != std::wstring::npos ||
        name.find(L"galaxy") != std::wstring::npos || name.find(L"pixel") != std::wstring::npos)
    {
        return BluetoothDeviceCategory::Phone;
    }

    try
    {
        using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
        using winrt::Windows::Devices::Bluetooth::BluetoothMajorClass;
        auto device = BluetoothDevice::FromIdAsync(info.Id()).get();
        if (device)
        {
            switch (device.ClassOfDevice().MajorClass())
            {
                case BluetoothMajorClass::Phone:
                    return BluetoothDeviceCategory::Phone;
                case BluetoothMajorClass::AudioVideo:
                    return BluetoothDeviceCategory::Headphones;
                case BluetoothMajorClass::Peripheral:
                    return BluetoothDeviceCategory::Mouse;
                default:
                    break;
            }
        }
    }
    catch (...)
    {
    }

    return BluetoothDeviceCategory::Generic;
}

int try_read_bluetooth_battery_percent_ble(winrt::hstring const& device_id)
{
    namespace bluetooth = winrt::Windows::Devices::Bluetooth;
    namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

    try
    {
        auto ble_device = bluetooth::BluetoothLEDevice::FromIdAsync(device_id).get();
        if (!ble_device)
        {
            return -1;
        }

        auto services_result =
            ble_device.GetGattServicesForUuidAsync(gatt::GattServiceUuids::Battery(), bluetooth::BluetoothCacheMode::Uncached).get();
        if (services_result.Status() != gatt::GattCommunicationStatus::Success || services_result.Services().Size() == 0)
        {
            return -1;
        }

        auto service = services_result.Services().GetAt(0);
        auto chars_result =
            service
                .GetCharacteristicsForUuidAsync(gatt::GattCharacteristicUuids::BatteryLevel(), bluetooth::BluetoothCacheMode::Uncached)
                .get();
        if (chars_result.Status() != gatt::GattCommunicationStatus::Success || chars_result.Characteristics().Size() == 0)
        {
            return -1;
        }

        auto characteristic = chars_result.Characteristics().GetAt(0);
        auto read_result = characteristic.ReadValueAsync(bluetooth::BluetoothCacheMode::Uncached).get();
        if (read_result.Status() != gatt::GattCommunicationStatus::Success)
        {
            return -1;
        }

        auto buffer = read_result.Value();
        if (buffer.Length() < 1)
        {
            return -1;
        }

        auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
        std::uint8_t raw = reader.ReadByte();
        return clamp_int(static_cast<int>(raw), 0, 100);
    }
    catch (...)
    {
        return -1;
    }
}

// Classic (non-BLE) headphones expose battery through an undocumented devnode property of the
// Microsoft Bluetooth stack (what Settings shows); read it via SetupAPI.
static const GUID guid_dev_class_bluetooth = {
    0xe0cbf06c, 0xcd8b, 0x4647, {0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74}};

static const DEVPROPKEY pkey_bluetooth_battery = {
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};

int try_read_classic_bluetooth_battery_percent(std::uint64_t address)
{
    if (!address)
    {
        host::log(L"BT battery: no address to search for.");
        return -1;
    }

    wchar_t addr_hex[16] = {};
    swprintf_s(addr_hex, L"%012llX", static_cast<unsigned long long>(address));
    const std::wstring addr_lower = to_lower_copy(addr_hex);
    host::log(L"BT battery: searching devnodes for address %s", addr_lower.c_str());

    HDEVINFO device_info_set = SetupDiGetClassDevsExW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT,
                                                      nullptr, nullptr, nullptr);

    if (device_info_set == INVALID_HANDLE_VALUE)
    {
        host::log(L"BT battery: SetupDiGetClassDevsExW(BTHENUM) failed (0x%lx), falling back to class GUID.",
                  GetLastError());
        device_info_set = SetupDiGetClassDevsW(&guid_dev_class_bluetooth, nullptr, nullptr, DIGCF_PRESENT);
        if (device_info_set == INVALID_HANDLE_VALUE)
        {
            host::log(L"BT battery: fallback enumeration also failed.");
            return -1;
        }
    }

    int result = -1;
    int matched_count = 0;
    SP_DEVINFO_DATA dev_info_data = {};
    dev_info_data.cbSize = sizeof(dev_info_data);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(device_info_set, i, &dev_info_data); ++i)
    {
        wchar_t instance_id[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(device_info_set, &dev_info_data, instance_id, ARRAYSIZE(instance_id), nullptr))
        {
            continue;
        }

        if (to_lower_copy(instance_id).find(addr_lower) == std::wstring::npos)
        {
            continue;
        }

        ++matched_count;
        host::log(L"BT battery: matched devnode %s", instance_id);

        DEVPROPTYPE prop_type = 0;
        BYTE battery = 0;
        DWORD required = 0;
        if (SetupDiGetDevicePropertyW(device_info_set, &dev_info_data, &pkey_bluetooth_battery, &prop_type, &battery,
                                      sizeof(battery), &required, 0))
        {
            host::log(L"BT battery: property present on this devnode, type=%lu value=%u", prop_type, battery);
            if (prop_type == DEVPROP_TYPE_BYTE && battery != 0xFF)
            {
                result = clamp_int(static_cast<int>(battery), 0, 100);
                break;
            }
        }
        else
        {
            host::log(L"BT battery: PKEY_Bluetooth_Battery not set on this devnode yet (error 0x%lx).", GetLastError());
        }
    }

    if (matched_count == 0)
    {
        host::log(L"BT battery: no devnode instance ID contained address %s.", addr_lower.c_str());
    }

    SetupDiDestroyDeviceInfoList(device_info_set);
    return result;
}

int try_read_bluetooth_battery_percent(winrt::hstring const& device_id)
{
    int result = -1;

    // Classic devnode property first (covers most BR/EDR headsets), then BLE GATT.
    try
    {
        using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
        auto classic_device = BluetoothDevice::FromIdAsync(device_id).get();
        if (classic_device)
        {
            result = try_read_classic_bluetooth_battery_percent(classic_device.BluetoothAddress());
        }
    }
    catch (...)
    {
    }

    if (result < 0)
    {
        result = try_read_bluetooth_battery_percent_ble(device_id);
    }

    if (result >= 0)
    {
        std::lock_guard lock(g_bluetooth_battery_cache_mutex);
        g_bluetooth_battery_cache[std::wstring(device_id.c_str())] = result;
    }

    return result;
}

int get_last_known_bluetooth_battery_percent(const std::wstring& device_id)
{
    std::lock_guard lock(g_bluetooth_battery_cache_mutex);
    auto it = g_bluetooth_battery_cache.find(device_id);
    return it != g_bluetooth_battery_cache.end() ? it->second : -1;
}

struct BluetoothTrackedDevice
{
    std::wstring name;
    BluetoothDeviceCategory category;
};

// Only devices matching Modules.BluetoothAllowList (semicolon list, case-insensitive) get a card;
// an empty list allows all.
bool is_bluetooth_card_allowed(const std::wstring& device_name)
{
    const std::wstring list = to_lower_copy(get_string_setting_copy(L"Modules.BluetoothAllowList"));
    const std::wstring name = to_lower_copy(device_name);
    bool any_entry = false;
    std::size_t start = 0;
    while (start <= list.size())
    {
        std::size_t end = list.find(L';', start);
        if (end == std::wstring::npos)
        {
            end = list.size();
        }
        std::wstring entry = list.substr(start, end - start);
        start = end + 1;
        const std::size_t first = entry.find_first_not_of(L" \t");
        if (first == std::wstring::npos)
        {
            continue;
        }
        entry = entry.substr(first, entry.find_last_not_of(L" \t") - first + 1);
        any_entry = true;
        if (!name.empty() && name.find(entry) != std::wstring::npos)
        {
            return true;
        }
    }
    return !any_entry;
}

void handle_bluetooth_connected(const winrt::Windows::Devices::Enumeration::DeviceInformation& info,
                                std::unordered_map<std::wstring, BluetoothTrackedDevice>& cache,
                                std::mutex& cache_mutex)
{
    std::wstring name = info.Name().c_str();
    if (name.empty())
    {
        return;
    }

    g_volume_endpoint_stale = true;
    if (HWND recheck_hwnd = g_hwnd)
    {
        PostMessageW(recheck_hwnd, WM_NULL, 0, 0);
    }

    BluetoothDeviceCategory category = classify_bluetooth_device(info);
    {
        std::lock_guard lock(cache_mutex);
        cache[std::wstring(info.Id().c_str())] = BluetoothTrackedDevice{name, category};
    }

    if (!g_settings.bluetooth_indicator)
    {
        return;
    }

    if (category == BluetoothDeviceCategory::Phone || !is_bluetooth_card_allowed(name))
    {
        host::log(L"Bluetooth: connected (card suppressed) - %s", name.c_str());
        return;
    }

    host::log(L"Bluetooth: connected - %s", name.c_str());

    const std::uint64_t my_generation = ++g_bluetooth_connect_generation;
    const std::wstring device_id = info.Id().c_str();

    BluetoothDeviceSnapshot snapshot;
    snapshot.active = true;
    snapshot.connected = true;
    snapshot.device_name = name;
    snapshot.category = category;
    snapshot.expires_at = now_seconds() + 4.0;
    snapshot.battery_percent = g_settings.bluetooth_show_battery ? try_read_bluetooth_battery_percent(info.Id()) : -1;

    {
        std::lock_guard lock(g_state_mutex);
        g_state.bluetooth_device = std::move(snapshot);
    }
    trigger_nudge();

    // Windows fills the battery property a moment after connecting, so keep retrying in the background.
    if (g_settings.bluetooth_show_battery && snapshot.battery_percent < 0)
    {
        std::thread(
            [device_id, my_generation]()
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                for (int attempt = 0; attempt < 6; ++attempt)
                {
                    Sleep(1500);
                    if (g_bluetooth_connect_generation.load() != my_generation)
                    {
                        break; // a newer connect/disconnect event superseded this one
                    }

                    int battery = try_read_bluetooth_battery_percent(winrt::hstring(device_id));
                    if (battery >= 0)
                    {
                        std::lock_guard lock(g_state_mutex);
                        if (g_bluetooth_connect_generation.load() == my_generation &&
                            g_state.bluetooth_device.connected)
                        {
                            g_state.bluetooth_device.battery_percent = battery;
                            host::log(L"Bluetooth: battery arrived late (%d%%) on retry %d.", battery, attempt + 1);
                        }
                        trigger_nudge();
                        break;
                    }
                }
                winrt::uninit_apartment();
            })
            .detach();
    }
}

void handle_bluetooth_disconnected(winrt::hstring const& id,
                                   std::unordered_map<std::wstring, BluetoothTrackedDevice>& cache,
                                   std::mutex& cache_mutex)
{
    {
        // Phones drop and reopen their links constantly; their cards are noise.
        std::lock_guard lock(cache_mutex);
        auto it = cache.find(std::wstring(id.c_str()));
        if (it == cache.end() || it->second.category == BluetoothDeviceCategory::Phone ||
            !is_bluetooth_card_allowed(it->second.name))
        {
            host::log(L"Bluetooth: disconnected (card suppressed) - %s",
                      it != cache.end() ? it->second.name.c_str() : L"(unknown)");
            return;
        }
    }

    ++g_bluetooth_connect_generation; // cancel any pending battery retry for the old connection

    g_volume_endpoint_stale = true;
    if (HWND recheck_hwnd = g_hwnd)
    {
        PostMessageW(recheck_hwnd, WM_NULL, 0, 0);
    }

    if (!g_settings.bluetooth_indicator)
    {
        return;
    }

    std::wstring name;
    BluetoothDeviceCategory category = BluetoothDeviceCategory::Generic;
    {
        std::lock_guard lock(cache_mutex);
        auto it = cache.find(std::wstring(id.c_str()));
        if (it != cache.end())
        {
            name = it->second.name;
            category = it->second.category;
        }
    }
    if (name.empty())
    {
        name = L"Bluetooth Device";
    }

    host::log(L"Bluetooth: disconnected - %s", name.c_str());

    BluetoothDeviceSnapshot snapshot;
    snapshot.active = true;
    snapshot.connected = false;
    snapshot.device_name = name;
    snapshot.category = category;
    snapshot.battery_percent = get_last_known_bluetooth_battery_percent(std::wstring(id.c_str()));
    snapshot.expires_at = now_seconds() + 4.0;

    {
        std::lock_guard lock(g_state_mutex);
        g_state.bluetooth_device = std::move(snapshot);
    }
    trigger_nudge();
}

}

DWORD WINAPI bluetooth_thread_proc(void*)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
    using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
    using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
    using winrt::Windows::Devices::Enumeration::DeviceInformation;
    using winrt::Windows::Devices::Enumeration::DeviceInformationUpdate;
    using winrt::Windows::Devices::Enumeration::DeviceWatcher;

    std::unordered_map<std::wstring, BluetoothTrackedDevice> device_cache;
    std::mutex cache_mutex;
    std::vector<DeviceWatcher> watchers;

    // The "Connected" selector turns Added into connect and Removed into disconnect, with no polling.
    auto start_watcher = [&](winrt::hstring const& selector, const wchar_t* label)
    {
        try
        {
            DeviceWatcher watcher = DeviceInformation::CreateWatcher(selector);
            auto enum_done = std::make_shared<std::atomic<bool>>(false);

            watcher.EnumerationCompleted(
                [enum_done](DeviceWatcher const&, winrt::Windows::Foundation::IInspectable const&)
                {
                    *enum_done = true;
                });

            watcher.Added(
                [&device_cache, &cache_mutex, enum_done](DeviceWatcher const&, DeviceInformation const& info)
                {
                    // Devices in the initial snapshot were already connected: cache them, but show no card.
                    if (!*enum_done)
                    {
                        std::wstring name = info.Name().c_str();
                        if (!name.empty())
                        {
                            std::lock_guard lock(cache_mutex);
                            device_cache[std::wstring(info.Id().c_str())] =
                                BluetoothTrackedDevice{name, classify_bluetooth_device(info)};
                        }
                        return;
                    }
                    handle_bluetooth_connected(info, device_cache, cache_mutex);
                });

            watcher.Removed(
                [&device_cache, &cache_mutex](DeviceWatcher const&, DeviceInformationUpdate const& update)
                {
                    handle_bluetooth_disconnected(update.Id(), device_cache, cache_mutex);
                });

            watcher.Start();
            watchers.push_back(watcher);
            host::log(L"Bluetooth: %s watcher started.", label);
        }
        catch (...)
        {
            host::log(L"Bluetooth: failed to start %s watcher.", label);
        }
    };

    start_watcher(BluetoothDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                  L"classic");
    start_watcher(BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
                  L"BLE");

    while (WaitForSingleObject(g_stop_event, 1000) == WAIT_TIMEOUT)
    {
    }

    for (auto& watcher : watchers)
    {
        try
        {
            watcher.Stop();
        }
        catch (...)
        {
        }
    }

    winrt::uninit_apartment();
    return 0;
}

}
