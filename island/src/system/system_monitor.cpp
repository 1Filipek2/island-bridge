#include "system/system_monitor.h"
#include "ui/icons.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{

void UpdateBatterySnapshot() {
    SYSTEM_POWER_STATUS status = {};
    if (!GetSystemPowerStatus(&status)) {
        return;
    }

    bool newCharging = (status.ACLineStatus == 1);
    int newPercent = status.BatteryLifePercent == 255 ? 100 : status.BatteryLifePercent;

    bool triggerAlert = false;

    {
        std::lock_guard lock(g_stateMutex);
        static bool s_batteryInit = false;
        if (!s_batteryInit) {
            g_state.battery.charging = newCharging;
            g_state.battery.percent = newPercent;
            s_batteryInit = true;
        }

        if (g_state.battery.charging != newCharging) {
            triggerAlert = true;
        }

        if (!newCharging && newPercent < g_state.battery.percent && (newPercent == 20 || newPercent == 10)) {
            triggerAlert = true;
        }

        g_state.battery.charging = newCharging;
        g_state.battery.percent = newPercent;
        g_state.battery.secondsRemaining = status.BatteryLifeTime;
        g_state.battery.low = (!newCharging && newPercent <= 20);

        if (triggerAlert) {
            g_state.battery.active = true;
            g_state.battery.expiresAt = NowSeconds() + 4.0;
        }
    }

    if (triggerAlert) {
        TriggerNudge();
    }
}


namespace
{


ULONGLONG FileTimeToUInt64(FILETIME ft) {
    ULARGE_INTEGER value = {};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

static PDH_HQUERY g_gpuQuery = NULL;
static PDH_HCOUNTER g_gpuCounter = NULL;

static void InitGpuQuery() {
    if (g_gpuQuery == NULL) {
        if (PdhOpenQueryW(NULL, 0, &g_gpuQuery) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(g_gpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpuCounter);
            PdhCollectQueryData(g_gpuQuery);
        }
    }
}

static int GetGpuUsage() {
    InitGpuQuery();
    if (!g_gpuQuery || !g_gpuCounter) return 0;

    PdhCollectQueryData(g_gpuQuery);

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);

    if (bufferSize > 0) {
        std::vector<BYTE> buffer(bufferSize);
        PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());

        if (PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
            double total = 0;
            for (DWORD i = 0; i < itemCount; i++) {
                if (items[i].szName && wcsstr(items[i].szName, L"engtype_3D")) {
                    total += items[i].FmtValue.doubleValue;
                }
            }
            return ClampInt(static_cast<int>(total), 0, 100);
        }
    }
    return 0;
}

static PDH_HQUERY g_netQuery = NULL;
static PDH_HCOUNTER g_netUpCounter = NULL;
static PDH_HCOUNTER g_netDownCounter = NULL;

static void InitNetQuery() {
    if (g_netQuery == NULL) {
        if (PdhOpenQueryW(NULL, 0, &g_netQuery) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(g_netQuery, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &g_netUpCounter);
            PdhAddEnglishCounterW(g_netQuery, L"\\Network Interface(*)\\Bytes Received/sec", 0, &g_netDownCounter);
            PdhCollectQueryData(g_netQuery);
        }
    }
}

static void GetNetworkUsage(float& outUpMbps, float& outDownMbps) {
    outUpMbps = 0.0f;
    outDownMbps = 0.0f;
    InitNetQuery();
    if (!g_netQuery || !g_netUpCounter || !g_netDownCounter) return;

    PdhCollectQueryData(g_netQuery);

    auto getSum = [](PDH_HCOUNTER counter) -> double {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
        if (bufferSize > 0) {
            std::vector<BYTE> buffer(bufferSize);
            PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
                double total = 0;
                for (DWORD i = 0; i < itemCount; i++) {
                    if (items[i].szName) {
                        if (wcsstr(items[i].szName, L"Loopback") == nullptr) {
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
    outUpMbps = static_cast<float>(getSum(g_netUpCounter) * 8.0 / 1000000.0);
    outDownMbps = static_cast<float>(getSum(g_netDownCounter) * 8.0 / 1000000.0);
}

// ---- Endpoint volume notifications ----
// Event-driven rather than polled: a 1s poll turned a held volume key into
// one visible step per second.
void ApplyVolumeReading(float level, bool muted, bool canSurface) {
    const int percent = ClampInt(static_cast<int>(level * 100.0f + 0.5f), 0, 100);

    bool flyoutEnabled = false;
    {
        std::lock_guard settingsLock(g_settingsMutex);
        flyoutEnabled = g_settings.volume;
    }

    bool surfaced = false;
    {
        std::lock_guard lock(g_stateMutex);
        const bool changed = percent != g_state.system.volumePercent ||
                             muted != g_state.system.volumeMuted;
        g_state.system.volumePercent = percent;
        g_state.system.volumeMuted = muted;
        g_state.muted = muted;

        if (canSurface && changed && flyoutEnabled) {
            surfaced = !g_state.volume.active;
            g_state.volume.active = true;
            g_state.volume.percent = percent;
            g_state.volume.muted = muted;
            g_state.volume.deviceName = L"System audio";
            g_state.volume.expiresAt = NowSeconds() + 1.8;
        }
    }

    // Only the first step nudges; nudging on every step made the pill bounce.
    if (surfaced) {
        TriggerNudge();
    }
}

// g_volumeEndpointStale is declared with the other globals near the top of the file.

// Called on audio service threads, not the render thread.
class VolumeNotifyClient final : public IAudioEndpointVolumeCallback, public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
        } else if (riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (data) {
            ApplyVolumeReading(data->fMasterVolume, data->bMuted != FALSE, true);
        }
        return S_OK;
    }

    // MMDevice API calls are not allowed from inside these callbacks, so the
    // render thread rebinds; the posted message wakes it even while parked.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            g_volumeEndpointStale = true;
            if (HWND hwnd = g_hwnd) {
                PostMessageW(hwnd, WM_NULL, 0, 0);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG refs_ = 1;
};

// Owned by the render thread.
ComPtr<IMMDeviceEnumerator> g_volumeEnumerator;
VolumeNotifyClient* g_volumeNotifyClient = nullptr;

// Event-driven: runs only when BindVolumeEndpoint() rebinds (startup, default output
// changed, Bluetooth connect/disconnect). Never from a timer or a draw path.
// "BTH*" enumerator names (BTHENUM, BTHHFENUM, BTHLEENUM) mean a Bluetooth endpoint.
void UpdateDefaultOutputBluetoothFlag(IMMDevice* device) {
    // PKEY_Device_EnumeratorName {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 24
    static const PROPERTYKEY kPkeyEnumeratorName = {
        {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 24};

    bool isBluetooth = false;
    bool readFailed = false;
    if (device) {
        readFailed = true;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(kPkeyEnumeratorName, &value))) {
                readFailed = false;
                isBluetooth = value.vt == VT_LPWSTR && value.pwszVal &&
                              _wcsnicmp(value.pwszVal, L"BTH", 3) == 0;
            }
            PropVariantClear(&value);
        }
    }

    if (readFailed) {
        static bool s_loggedReadFailure = false;
        if (!s_loggedReadFailure) {
            s_loggedReadFailure = true;
            host::log(L"Lyrics sync: could not read the output's enumerator name; treating it as non-Bluetooth.");
        }
    }

    static bool s_logged = false;
    const bool changed = g_defaultOutputIsBluetooth.exchange(isBluetooth, std::memory_order_relaxed) != isBluetooth;
    if (changed || !s_logged) {
        s_logged = true;
        host::log(L"Lyrics sync: default audio output is %s.", isBluetooth ? L"Bluetooth" : L"not Bluetooth");
    }
    if (changed) {
        g_layoutDirty = true;
    }
}

}



void BindVolumeEndpoint() {
    if (!g_volumeEndpointStale.exchange(false)) {
        return;
    }

    if (!g_volumeNotifyClient) {
        g_volumeNotifyClient = new VolumeNotifyClient();
    }
    if (!g_volumeEnumerator) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&g_volumeEnumerator)))) {
            g_volumeEnumerator.Reset();
            return;
        }
        g_volumeEnumerator->RegisterEndpointNotificationCallback(g_volumeNotifyClient);
    }

    if (g_endpointVolume) {
        g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        g_endpointVolume.Reset();
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = g_volumeEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    UpdateDefaultOutputBluetoothFlag(SUCCEEDED(hr) ? device.Get() : nullptr);
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(g_endpointVolume.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) {
        hr = g_endpointVolume->RegisterControlChangeNotify(g_volumeNotifyClient);
    }

    float level = 0.0f;
    BOOL muted = FALSE;
    if (SUCCEEDED(hr) && SUCCEEDED(g_endpointVolume->GetMasterVolumeLevelScalar(&level)) &&
        SUCCEEDED(g_endpointVolume->GetMute(&muted))) {
        // Baseline only: switching device must not pop the flyout.
        ApplyVolumeReading(level, muted != FALSE, false);
    } else if (g_endpointVolume) {
        g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        g_endpointVolume.Reset();
    }
}

void UnbindVolumeEndpoint() {
    if (g_volumeNotifyClient) {
        if (g_endpointVolume) {
            g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        }
        if (g_volumeEnumerator) {
            g_volumeEnumerator->UnregisterEndpointNotificationCallback(g_volumeNotifyClient);
        }
    }
    g_endpointVolume.Reset();
    g_volumeEnumerator.Reset();
    if (g_volumeNotifyClient) {
        g_volumeNotifyClient->Release();
        g_volumeNotifyClient = nullptr;
    }
    g_volumeEndpointStale = true;
}

void UpdateSystemSnapshot(bool includeGpuStats, bool includeNetStats) {
    SystemSnapshot next;
    {
        std::lock_guard lock(g_stateMutex);
        next = g_state.system;
        next.charging = g_state.system.charging;
    }

    // GPU/network sampling is comparatively expensive (the GPU counter in
    // particular enumerates every GPU engine instance across every process
    // using the GPU), so only sample them when something on screen is
    // actually displaying them. Otherwise just carry the last sampled
    // value forward (already done via `next = g_state.system` above).
    if (includeGpuStats) {
        next.gpuPercent = GetGpuUsage();
    }
    if (includeNetStats) {
        GetNetworkUsage(next.netUpMbps, next.netDownMbps);
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        next.memoryPercent = static_cast<int>(memory.dwMemoryLoad);
        next.memoryTotalGB = static_cast<float>(memory.ullTotalPhys) / (1024.0f * 1024.0f * 1024.0f);
        next.memoryUsedGB = next.memoryTotalGB - static_cast<float>(memory.ullAvailPhys) / (1024.0f * 1024.0f * 1024.0f);
    }

    ULARGE_INTEGER freeBytesAvailable = {};
    ULARGE_INTEGER totalBytes = {};
    ULARGE_INTEGER totalFreeBytes = {};
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytesAvailable, &totalBytes, &totalFreeBytes) &&
        totalBytes.QuadPart > 0) {
        next.diskFreePercent = ClampInt(
            static_cast<int>(totalFreeBytes.QuadPart * 100 / totalBytes.QuadPart), 0, 100);
    }

    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_hwnd) {
        wchar_t title[96] = {};
        GetWindowTextW(foreground, title, ARRAYSIZE(title));
        if (!IsIgnorableForegroundWindow(foreground, title)) {
            next.foregroundTitle = title;
            if (next.foregroundTitle.size() > 42) {
                next.foregroundTitle.resize(42);
                next.foregroundTitle += L"...";
            }

        } else {
            next.foregroundTitle.clear();
        }
    }

    FILETIME idle = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const ULONGLONG idleNow = FileTimeToUInt64(idle);
        const ULONGLONG kernelNow = FileTimeToUInt64(kernel);
        const ULONGLONG userNow = FileTimeToUInt64(user);
        const ULONGLONG idlePrev = FileTimeToUInt64(g_prevIdleTime);
        const ULONGLONG kernelPrev = FileTimeToUInt64(g_prevKernelTime);
        const ULONGLONG userPrev = FileTimeToUInt64(g_prevUserTime);

        const ULONGLONG total = (kernelNow - kernelPrev) + (userNow - userPrev);
        const ULONGLONG idleDelta = idleNow - idlePrev;
        if (total > 0 && kernelPrev != 0) {
            next.cpuPercent = ClampInt(static_cast<int>((total - idleDelta) * 100 / total), 0, 100);
        }

        g_prevIdleTime = idle;
        g_prevKernelTime = kernel;
        g_prevUserTime = user;
    }

    std::lock_guard lock(g_stateMutex);
    // Volume is owned by ApplyVolumeReading and may have moved since `next` was copied.
    next.volumePercent = g_state.system.volumePercent;
    next.volumeMuted = g_state.system.volumeMuted;
    g_state.system = next;
    g_state.muted = next.volumeMuted;
}


namespace
{


// Built-in panel brightness (0-100), or -1 when there is no such panel (desktops)
// or the query fails. Same IOCTL the Windows brightness slider uses, so no WMI.
int ReadPanelBrightness(bool onAc) {
    constexpr DWORD kIoctlQueryDisplayBrightness = 0x230498;  // CTL_CODE(FILE_DEVICE_VIDEO, 0x126, METHOD_BUFFERED, FILE_ANY_ACCESS)
    struct DisplayBrightness {  // DISPLAY_BRIGHTNESS from ntddvdeo.h
        UCHAR policy;           // bit 0 = AC level valid, bit 1 = DC level valid
        UCHAR acBrightness;
        UCHAR dcBrightness;
    };

    static double s_retryAt = 0.0;  // desktops have no LCD device, so do not retry every poll
    const double now = NowSeconds();
    if (now < s_retryAt) {
        return -1;
    }

    HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (lcd == INVALID_HANDLE_VALUE) {
        lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    }
    if (lcd == INVALID_HANDLE_VALUE) {
        host::log(L"Brightness: cannot open the LCD device (error %lu), retrying in 30s", GetLastError());
        s_retryAt = now + 30.0;
        return -1;
    }

    DisplayBrightness db = {};
    DWORD returned = 0;
    const bool ok = DeviceIoControl(lcd, kIoctlQueryDisplayBrightness, nullptr, 0, &db, sizeof(db),
                                    &returned, nullptr) && returned >= sizeof(db);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(lcd);
    if (!ok) {
        host::log(L"Brightness: query failed (error %lu, %lu bytes), retrying in 30s", error, returned);
        s_retryAt = now + 30.0;
        return -1;
    }

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        s_loggedFirst = true;
        host::log(L"Brightness: policy=%d ac=%d dc=%d", db.policy, db.acBrightness, db.dcBrightness);
    }
    // Pick the level for the current power source, falling back to the other one
    // when the driver only reports one of them.
    const bool hasAc = (db.policy & 1) != 0;
    const bool hasDc = (db.policy & 2) != 0;
    int level = onAc ? db.acBrightness : db.dcBrightness;
    if (onAc && !hasAc && hasDc) {
        level = db.dcBrightness;
    } else if (!onAc && !hasDc && hasAc) {
        level = db.acBrightness;
    }
    return ClampInt(level, 0, 100);
}

}



void UpdateBrightnessSnapshot() {
    SYSTEM_POWER_STATUS power = {};
    GetSystemPowerStatus(&power);
    const int level = ReadPanelBrightness(power.ACLineStatus != 0);
    if (level < 0) {
        return;
    }

    std::lock_guard lock(g_stateMutex);
    // Plugging in or unplugging swaps between the AC and battery levels, which is
    // not the user moving the slider, so that sample only resets the baseline.
    const bool changed = g_brightnessInitialized && level != g_lastBrightness &&
                         power.ACLineStatus == g_brightnessPowerSource;
    if (changed && g_settings.brightness) {
        g_state.brightness.active = true;
        g_state.brightness.percent = level;
        g_state.brightness.expiresAt = NowSeconds() + 1.8;
        TriggerNudge();
    }
    g_lastBrightness = level;
    g_brightnessPowerSource = power.ACLineStatus;
    g_brightnessInitialized = true;
}


}
