#include "system/system_monitor.h"
#include "ui/icons.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{

void update_battery_snapshot()
{
    SYSTEM_POWER_STATUS status = {};
    if (!GetSystemPowerStatus(&status))
    {
        return;
    }

    bool new_charging = (status.ACLineStatus == 1);
    int new_percent = status.BatteryLifePercent == 255 ? 100 : status.BatteryLifePercent;

    bool trigger_alert = false;

    {
        std::lock_guard lock(g_state_mutex);
        static bool s_battery_init = false;
        if (!s_battery_init)
        {
            g_state.battery.charging = new_charging;
            g_state.battery.percent = new_percent;
            s_battery_init = true;
        }

        if (g_state.battery.charging != new_charging)
        {
            trigger_alert = true;
        }

        if (!new_charging && new_percent < g_state.battery.percent && (new_percent == 20 || new_percent == 10))
        {
            trigger_alert = true;
        }

        g_state.battery.charging = new_charging;
        g_state.battery.percent = new_percent;
        g_state.battery.seconds_remaining = status.BatteryLifeTime;
        g_state.battery.low = (!new_charging && new_percent <= 20);

        if (trigger_alert)
        {
            g_state.battery.active = true;
            g_state.battery.expires_at = now_seconds() + 4.0;
        }
    }

    if (trigger_alert)
    {
        trigger_nudge();
    }
}

namespace
{

ULONGLONG file_time_to_u_int64(FILETIME ft)
{
    ULARGE_INTEGER value = {};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

static PDH_HQUERY g_gpu_query = NULL;
static PDH_HCOUNTER g_gpu_counter = NULL;

static void init_gpu_query()
{
    if (g_gpu_query == NULL)
    {
        if (PdhOpenQueryW(NULL, 0, &g_gpu_query) == ERROR_SUCCESS)
        {
            PdhAddEnglishCounterW(g_gpu_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpu_counter);
            PdhCollectQueryData(g_gpu_query);
        }
    }
}

static int get_gpu_usage()
{
    init_gpu_query();
    if (!g_gpu_query || !g_gpu_counter)
    {
        return 0;
    }

    PdhCollectQueryData(g_gpu_query);

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PdhGetFormattedCounterArrayW(g_gpu_counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, NULL);

    if (buffer_size > 0)
    {
        std::vector<BYTE> buffer(buffer_size);
        PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());

        if (PdhGetFormattedCounterArrayW(g_gpu_counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, items) ==
            ERROR_SUCCESS)
        {
            double total = 0;
            for (DWORD i = 0; i < item_count; i++)
            {
                if (items[i].szName && wcsstr(items[i].szName, L"engtype_3D"))
                {
                    total += items[i].FmtValue.doubleValue;
                }
            }
            return clamp_int(static_cast<int>(total), 0, 100);
        }
    }
    return 0;
}

static PDH_HQUERY g_net_query = NULL;
static PDH_HCOUNTER g_net_up_counter = NULL;
static PDH_HCOUNTER g_net_down_counter = NULL;

static void init_net_query()
{
    if (g_net_query == NULL)
    {
        if (PdhOpenQueryW(NULL, 0, &g_net_query) == ERROR_SUCCESS)
        {
            PdhAddEnglishCounterW(g_net_query, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &g_net_up_counter);
            PdhAddEnglishCounterW(g_net_query, L"\\Network Interface(*)\\Bytes Received/sec", 0, &g_net_down_counter);
            PdhCollectQueryData(g_net_query);
        }
    }
}

static void get_network_usage(float& out_up_mbps, float& out_down_mbps)
{
    out_up_mbps = 0.0f;
    out_down_mbps = 0.0f;
    init_net_query();
    if (!g_net_query || !g_net_up_counter || !g_net_down_counter)
    {
        return;
    }

    PdhCollectQueryData(g_net_query);

    auto get_sum = [](PDH_HCOUNTER counter) -> double
    {
        DWORD buffer_size = 0;
        DWORD item_count = 0;
        PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, NULL);
        if (buffer_size > 0)
        {
            std::vector<BYTE> buffer(buffer_size);
            PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, items) ==
                ERROR_SUCCESS)
            {
                double total = 0;
                for (DWORD i = 0; i < item_count; i++)
                {
                    if (items[i].szName)
                    {
                        if (wcsstr(items[i].szName, L"Loopback") == nullptr)
                        {
                            total += items[i].FmtValue.doubleValue;
                        }
                    }
                }
                return total;
            }
        }
        return 0.0;
    };

    // Bytes to Mbps
    out_up_mbps = static_cast<float>(get_sum(g_net_up_counter) * 8.0 / 1000000.0);
    out_down_mbps = static_cast<float>(get_sum(g_net_down_counter) * 8.0 / 1000000.0);
}

// ---- Endpoint volume notifications ----
// Event-driven rather than polled: a 1s poll turned a held volume key into
// one visible step per second.
void apply_volume_reading(float level, bool muted, bool can_surface)
{
    const int percent = clamp_int(static_cast<int>(level * 100.0f + 0.5f), 0, 100);

    bool flyout_enabled = false;
    {
        std::lock_guard settings_lock(g_settings_mutex);
        flyout_enabled = g_settings.volume;
    }

    bool surfaced = false;
    {
        std::lock_guard lock(g_state_mutex);
        const bool changed = percent != g_state.system.volume_percent || muted != g_state.system.volume_muted;
        g_state.system.volume_percent = percent;
        g_state.system.volume_muted = muted;
        g_state.muted = muted;

        if (can_surface && changed && flyout_enabled)
        {
            surfaced = !g_state.volume.active;
            g_state.volume.active = true;
            g_state.volume.percent = percent;
            g_state.volume.muted = muted;
            g_state.volume.device_name = L"System audio";
            g_state.volume.expires_at = now_seconds() + 1.8;
        }
    }

    // Only the first step nudges; nudging on every step made the pill bounce.
    if (surfaced)
    {
        trigger_nudge();
    }
}

// g_volumeEndpointStale is declared with the other globals near the top of the file.

// Called on audio service threads, not the render thread.
class VolumeNotifyClient final : public IAudioEndpointVolumeCallback, public IMMNotificationClient
{
public:
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&refs_);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0)
        {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback))
        {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
        }
        else if (riid == __uuidof(IMMNotificationClient))
        {
            *ppv = static_cast<IMMNotificationClient*>(this);
        }
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override
    {
        if (data)
        {
            apply_volume_reading(data->fMasterVolume, data->bMuted != FALSE, true);
        }
        return S_OK;
    }

    // MMDevice API calls are not allowed from inside these callbacks, so the
    // render thread rebinds; the posted message wakes it even while parked.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eConsole)
        {
            g_volume_endpoint_stale = true;
            if (HWND hwnd = g_hwnd)
            {
                PostMessageW(hwnd, WM_NULL, 0, 0);
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    LONG refs_ = 1;
};

// Owned by the render thread.
ComPtr<IMMDeviceEnumerator> g_volume_enumerator;
VolumeNotifyClient* g_volume_notify_client = nullptr;

// Event-driven: runs only when BindVolumeEndpoint() rebinds (startup, default output
// changed, Bluetooth connect/disconnect). Never from a timer or a draw path.
// "BTH*" enumerator names (BTHENUM, BTHHFENUM, BTHLEENUM) mean a Bluetooth endpoint.
void update_default_output_bluetooth_flag(IMMDevice* device)
{
    // PKEY_Device_EnumeratorName {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 24
    static const PROPERTYKEY pkey_enumerator_name = {
        {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 24};

    bool is_bluetooth = false;
    bool read_failed = false;
    if (device)
    {
        read_failed = true;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store)
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(pkey_enumerator_name, &value)))
            {
                read_failed = false;
                is_bluetooth = value.vt == VT_LPWSTR && value.pwszVal && _wcsnicmp(value.pwszVal, L"BTH", 3) == 0;
            }
            PropVariantClear(&value);
        }
    }

    if (read_failed)
    {
        static bool s_logged_read_failure = false;
        if (!s_logged_read_failure)
        {
            s_logged_read_failure = true;
            host::log(L"Lyrics sync: could not read the output's enumerator name; treating it as non-Bluetooth.");
        }
    }

    static bool s_logged = false;
    const bool changed =
        g_default_output_is_bluetooth.exchange(is_bluetooth, std::memory_order_relaxed) != is_bluetooth;
    if (changed || !s_logged)
    {
        s_logged = true;
        host::log(L"Lyrics sync: default audio output is %s.", is_bluetooth ? L"Bluetooth" : L"not Bluetooth");
    }
    if (changed)
    {
        g_layout_dirty = true;
    }
}

}

void bind_volume_endpoint()
{
    if (!g_volume_endpoint_stale.exchange(false))
    {
        return;
    }

    if (!g_volume_notify_client)
    {
        g_volume_notify_client = new VolumeNotifyClient();
    }
    if (!g_volume_enumerator)
    {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&g_volume_enumerator))))
        {
            g_volume_enumerator.Reset();
            return;
        }
        g_volume_enumerator->RegisterEndpointNotificationCallback(g_volume_notify_client);
    }

    if (g_endpoint_volume)
    {
        g_endpoint_volume->UnregisterControlChangeNotify(g_volume_notify_client);
        g_endpoint_volume.Reset();
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = g_volume_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    update_default_output_bluetooth_flag(SUCCEEDED(hr) ? device.Get() : nullptr);
    if (SUCCEEDED(hr))
    {
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(g_endpoint_volume.GetAddressOf()));
    }
    if (SUCCEEDED(hr))
    {
        hr = g_endpoint_volume->RegisterControlChangeNotify(g_volume_notify_client);
    }

    float level = 0.0f;
    BOOL muted = FALSE;
    if (SUCCEEDED(hr) && SUCCEEDED(g_endpoint_volume->GetMasterVolumeLevelScalar(&level)) &&
        SUCCEEDED(g_endpoint_volume->GetMute(&muted)))
    {
        // Baseline only: switching device must not pop the flyout.
        apply_volume_reading(level, muted != FALSE, false);
    }
    else if (g_endpoint_volume)
    {
        g_endpoint_volume->UnregisterControlChangeNotify(g_volume_notify_client);
        g_endpoint_volume.Reset();
    }
}

void unbind_volume_endpoint()
{
    if (g_volume_notify_client)
    {
        if (g_endpoint_volume)
        {
            g_endpoint_volume->UnregisterControlChangeNotify(g_volume_notify_client);
        }
        if (g_volume_enumerator)
        {
            g_volume_enumerator->UnregisterEndpointNotificationCallback(g_volume_notify_client);
        }
    }
    g_endpoint_volume.Reset();
    g_volume_enumerator.Reset();
    if (g_volume_notify_client)
    {
        g_volume_notify_client->Release();
        g_volume_notify_client = nullptr;
    }
    g_volume_endpoint_stale = true;
}

void update_system_snapshot(bool include_gpu_stats, bool include_net_stats)
{
    SystemSnapshot next;
    {
        std::lock_guard lock(g_state_mutex);
        next = g_state.system;
        next.charging = g_state.system.charging;
    }

    // GPU/network sampling is comparatively expensive (the GPU counter in
    // particular enumerates every GPU engine instance across every process
    // using the GPU), so only sample them when something on screen is
    // actually displaying them. Otherwise just carry the last sampled
    // value forward (already done via `next = g_state.system` above).
    if (include_gpu_stats)
    {
        next.gpu_percent = get_gpu_usage();
    }
    if (include_net_stats)
    {
        get_network_usage(next.net_up_mbps, next.net_down_mbps);
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        next.memory_percent = static_cast<int>(memory.dwMemoryLoad);
        next.memory_total_gb = static_cast<float>(memory.ullTotalPhys) / (1024.0f * 1024.0f * 1024.0f);
        next.memory_used_gb =
            next.memory_total_gb - static_cast<float>(memory.ullAvailPhys) / (1024.0f * 1024.0f * 1024.0f);
    }

    ULARGE_INTEGER free_bytes_available = {};
    ULARGE_INTEGER total_bytes = {};
    ULARGE_INTEGER total_free_bytes = {};
    if (GetDiskFreeSpaceExW(L"C:\\", &free_bytes_available, &total_bytes, &total_free_bytes) &&
        total_bytes.QuadPart > 0)
    {
        next.disk_free_percent =
            clamp_int(static_cast<int>(total_free_bytes.QuadPart * 100 / total_bytes.QuadPart), 0, 100);
    }

    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_hwnd)
    {
        wchar_t title[96] = {};
        GetWindowTextW(foreground, title, ARRAYSIZE(title));
        if (!is_ignorable_foreground_window(foreground, title))
        {
            next.foreground_title = title;
            if (next.foreground_title.size() > 42)
            {
                next.foreground_title.resize(42);
                next.foreground_title += L"...";
            }
        }
        else
        {
            next.foreground_title.clear();
        }
    }

    FILETIME idle = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (GetSystemTimes(&idle, &kernel, &user))
    {
        const ULONGLONG idle_now = file_time_to_u_int64(idle);
        const ULONGLONG kernel_now = file_time_to_u_int64(kernel);
        const ULONGLONG user_now = file_time_to_u_int64(user);
        const ULONGLONG idle_prev = file_time_to_u_int64(g_prev_idle_time);
        const ULONGLONG kernel_prev = file_time_to_u_int64(g_prev_kernel_time);
        const ULONGLONG user_prev = file_time_to_u_int64(g_prev_user_time);

        const ULONGLONG total = (kernel_now - kernel_prev) + (user_now - user_prev);
        const ULONGLONG idle_delta = idle_now - idle_prev;
        if (total > 0 && kernel_prev != 0)
        {
            next.cpu_percent = clamp_int(static_cast<int>((total - idle_delta) * 100 / total), 0, 100);
        }

        g_prev_idle_time = idle;
        g_prev_kernel_time = kernel;
        g_prev_user_time = user;
    }

    std::lock_guard lock(g_state_mutex);
    // Volume is owned by ApplyVolumeReading and may have moved since `next` was copied.
    next.volume_percent = g_state.system.volume_percent;
    next.volume_muted = g_state.system.volume_muted;
    g_state.system = next;
    g_state.muted = next.volume_muted;
}

namespace
{

// Built-in panel brightness (0-100), or -1 when there is no such panel (desktops)
// or the query fails. Same IOCTL the Windows brightness slider uses, so no WMI.
int read_panel_brightness(bool on_ac)
{
    constexpr DWORD ioctl_query_display_brightness =
        0x230498; // CTL_CODE(FILE_DEVICE_VIDEO, 0x126, METHOD_BUFFERED, FILE_ANY_ACCESS)

    struct DisplayBrightness
    {                 // DISPLAY_BRIGHTNESS from ntddvdeo.h
        UCHAR policy; // bit 0 = AC level valid, bit 1 = DC level valid
        UCHAR ac_brightness;
        UCHAR dc_brightness;
    };

    static double s_retry_at = 0.0; // desktops have no LCD device, so do not retry every poll
    const double now = now_seconds();
    if (now < s_retry_at)
    {
        return -1;
    }

    HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (lcd == INVALID_HANDLE_VALUE)
    {
        lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                          nullptr);
    }
    if (lcd == INVALID_HANDLE_VALUE)
    {
        host::log(L"Brightness: cannot open the LCD device (error %lu), retrying in 30s", GetLastError());
        s_retry_at = now + 30.0;
        return -1;
    }

    DisplayBrightness db = {};
    DWORD returned = 0;
    const bool ok =
        DeviceIoControl(lcd, ioctl_query_display_brightness, nullptr, 0, &db, sizeof(db), &returned, nullptr) &&
        returned >= sizeof(db);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(lcd);
    if (!ok)
    {
        host::log(L"Brightness: query failed (error %lu, %lu bytes), retrying in 30s", error, returned);
        s_retry_at = now + 30.0;
        return -1;
    }

    static bool s_logged_first = false;
    if (!s_logged_first)
    {
        s_logged_first = true;
        host::log(L"Brightness: policy=%d ac=%d dc=%d", db.policy, db.ac_brightness, db.dc_brightness);
    }
    // Pick the level for the current power source, falling back to the other one
    // when the driver only reports one of them.
    const bool has_ac = (db.policy & 1) != 0;
    const bool has_dc = (db.policy & 2) != 0;
    int level = on_ac ? db.ac_brightness : db.dc_brightness;
    if (on_ac && !has_ac && has_dc)
    {
        level = db.dc_brightness;
    }
    else if (!on_ac && !has_dc && has_ac)
    {
        level = db.ac_brightness;
    }
    return clamp_int(level, 0, 100);
}

}

void update_brightness_snapshot()
{
    SYSTEM_POWER_STATUS power = {};
    GetSystemPowerStatus(&power);
    const int level = read_panel_brightness(power.ACLineStatus != 0);
    if (level < 0)
    {
        return;
    }

    std::lock_guard lock(g_state_mutex);
    // Plugging in or unplugging swaps between the AC and battery levels, which is
    // not the user moving the slider, so that sample only resets the baseline.
    const bool changed =
        g_brightness_initialized && level != g_last_brightness && power.ACLineStatus == g_brightness_power_source;
    if (changed && g_settings.brightness)
    {
        g_state.brightness.active = true;
        g_state.brightness.percent = level;
        g_state.brightness.expires_at = now_seconds() + 1.8;
        trigger_nudge();
    }
    g_last_brightness = level;
    g_brightness_power_source = power.ACLineStatus;
    g_brightness_initialized = true;
}

}
