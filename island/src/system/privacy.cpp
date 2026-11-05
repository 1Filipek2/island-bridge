#include "system/privacy.h"
#include "ui/icons.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{


// ---- Privacy indicator helpers ----
std::wstring CleanActiveAppName(const std::wstring& raw) {
    std::wstring name;
    size_t hashPos = raw.rfind(L'#');
    if (hashPos != std::wstring::npos) {
        name = raw.substr(hashPos + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) {
            name.resize(name.size() - 4);
        }
    } else {
        size_t underPos = raw.find(L'_');
        name = (underPos != std::wstring::npos) ? raw.substr(0, underPos) : raw;
        if (name.rfind(L"Microsoft.", 0) == 0) name = name.substr(10);
        if (name.rfind(L"Windows.", 0) == 0) name = name.substr(8);
        else if (name.rfind(L"Windows", 0) == 0 && name.size() > 7) name = name.substr(7);
    }
    std::wstring lower = ToLowerCopy(name);
    if (lower == L"msedge") return L"Microsoft Edge";
    if (lower == L"chrome") return L"Google Chrome";
    if (lower == L"brave") return L"Brave";
    if (lower == L"firefox") return L"Firefox";
    if (lower == L"discord") return L"Discord";
    if (lower == L"zoom") return L"Zoom";
    if (lower == L"obs64" || lower == L"obs32" || lower == L"obs") return L"OBS Studio";
    if (lower == L"teams") return L"Microsoft Teams";
    if (lower == L"skype") return L"Skype";
    if (lower == L"audacity") return L"Audacity";
    if (lower == L"fl64" || lower == L"fl") return L"FL Studio";
    if (lower == L"ciscocollabhost") return L"Webex";
    return name;
}

// Reads a REG_QWORD, returning false when the value is absent or is not a
// well-formed 64-bit quantity. The previous code read straight into a uint64_t
// without checking the type or size, so a shorter value could leave the high
// bytes untouched and fake a zero timestamp.
bool ReadRegQword(HKEY key, const wchar_t* name, uint64_t* out) {
    DWORD type = 0;
    uint64_t value = 0;
    DWORD dataSize = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &dataSize) != ERROR_SUCCESS) {
        return false;
    }
    if (type != REG_QWORD || dataSize != sizeof(value)) {
        return false;
    }
    *out = value;
    return true;
}

// A ConsentStore entry counts as in-use only when Windows has recorded a start
// and has not yet recorded a stop. Testing LastUsedTimeStop == 0 on its own
// also matched entries that had never been used at all (both timestamps zero),
// which is what left the microphone dot lit permanently for some users even
// though Windows' own privacy indicator showed nothing in use.
bool ConsentStoreEntryInUse(HKEY key) {
    uint64_t stopTime = 0;
    if (!ReadRegQword(key, L"LastUsedTimeStop", &stopTime) || stopTime != 0) {
        return false;
    }

    uint64_t startTime = 0;
    if (ReadRegQword(key, L"LastUsedTimeStart", &startTime)) {
        return startTime != 0;
    }

    // Stop says "not stopped" but there is no readable start timestamp. This is
    // the shape of a never-used entry, so it is reported as not in use, but log
    // it once: if some Windows build stores these differently it would otherwise
    // be an indicator that silently stops working with nothing to explain it.
    static std::atomic<bool> loggedMissingStart{false};
    if (!loggedMissingStart.exchange(true)) {
        host::log(L"Privacy indicator: a ConsentStore entry has LastUsedTimeStop=0 but no "
               L"readable REG_QWORD LastUsedTimeStart; treating it as not in use.");
    }
    return false;
}

bool IsDeviceActiveViaRegistry(const wchar_t* capability, std::wstring* outAppName = nullptr) {
    bool isActive = false;
    std::wstring basePath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    basePath += capability;

    auto CheckSubkeys = [&](HKEY hKeyParent) -> bool {
        constexpr REGSAM kSam = KEY_READ | KEY_WOW64_64KEY;
        DWORD index = 0;
        wchar_t subKeyName[256];
        DWORD nameLen = ARRAYSIZE(subKeyName);
        while (RegEnumKeyExW(hKeyParent, index, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY hSub;
            if (RegOpenKeyExW(hKeyParent, subKeyName, 0, kSam, &hSub) == ERROR_SUCCESS) {
                if (_wcsicmp(subKeyName, L"NonPackaged") == 0) {
                    DWORD npIndex = 0;
                    wchar_t npSubKeyName[256];
                    DWORD npNameLen = ARRAYSIZE(npSubKeyName);
                    while (RegEnumKeyExW(hSub, npIndex, npSubKeyName, &npNameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                        HKEY hNpSub;
                        if (RegOpenKeyExW(hSub, npSubKeyName, 0, kSam, &hNpSub) == ERROR_SUCCESS) {
                            if (ConsentStoreEntryInUse(hNpSub)) {
                                if (outAppName && outAppName->empty()) {
                                    *outAppName = CleanActiveAppName(npSubKeyName);
                                }
                                RegCloseKey(hNpSub);
                                RegCloseKey(hSub);
                                return true;
                            }
                            RegCloseKey(hNpSub);
                        }
                        npIndex++;
                        npNameLen = ARRAYSIZE(npSubKeyName);
                    }
                } else {
                    if (ConsentStoreEntryInUse(hSub)) {
                        if (outAppName && outAppName->empty()) {
                            *outAppName = CleanActiveAppName(subKeyName);
                        }
                        RegCloseKey(hSub);
                        return true;
                    }
                }
                RegCloseKey(hSub);
            }
            index++;
            nameLen = ARRAYSIZE(subKeyName);
        }
        return false;
    };

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, basePath.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        isActive = CheckSubkeys(hKey);
        RegCloseKey(hKey);
    }

    if (!isActive) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, basePath.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            isActive = CheckSubkeys(hKey);
            RegCloseKey(hKey);
        }
    }

    return isActive;
}

bool IsMicrophoneActive(std::wstring* outAppName = nullptr) {
    return IsDeviceActiveViaRegistry(L"microphone", outAppName);
}

bool IsCameraActive(std::wstring* outAppName = nullptr) {
    return IsDeviceActiveViaRegistry(L"webcam", outAppName);
}

}



void UpdateProgressSnapshot() {
    const int progress = host::int_value(L"ProgressPercent", -1);
    std::lock_guard lock(g_stateMutex);
    g_state.progress.active = progress >= 0 && progress <= 100;
    g_state.progress.percent = ClampInt(progress, 0, 100);
}

void UpdatePrivacyIndicators() {
    std::wstring micApp;
    std::wstring camApp;
    const bool mic = (g_settings.privacyDots && g_settings.privacyDotsMic) ? IsMicrophoneActive(&micApp) : false;
    const bool cam = (g_settings.privacyDots && g_settings.privacyDotsCam) ? IsCameraActive(&camApp) : false;
    std::lock_guard lock(g_stateMutex);
    g_state.system.micActive = mic;
    g_state.system.cameraActive = cam;
    g_state.system.micApp = micApp;
    g_state.system.cameraApp = camApp;
}


}
