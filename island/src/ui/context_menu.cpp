#include "ui/context_menu.h"
#include "ui/actions.h"
#include "ui/backdrop.h"
#include "ui/file_tray.h"
#include "core/localization.h"
#include "media/lyrics_cache.h"
#include "media/media_source.h"
#include "core/model.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/util.h"
#include "services/weather.h"

namespace island
{
namespace
{


// ── Media source menu (right-click submenu and the dock's "+N" list) ─────────
static constexpr UINT kMediaSourceMenuIdBase = 100;  // 100 = Auto, 101.. = sources

// Shape style submenu: +0 = iPhone pill, +1 = Windows 11 flyout, +2 = macOS notch.
static constexpr UINT kShapeMenuIdBase = 50;

// Lyrics sync submenu: +0 header (disabled), +1..+3 earlier 10/50/250, +4..+6 later 10/50/250, +7 reset.
static constexpr UINT kLyricsSyncMenuIdBase = 60;

// Right-click nudge for the profile that is active right now (Bluetooth or speakers).
// Stored separately from the Mod Settings value; effective offset = setting + nudge.
// Deliberately does not call LoadSettings(), which also resets the pinned state.
void AdjustLyricsNudge(int deltaMs) {
    const bool bt = g_defaultOutputIsBluetooth.load(std::memory_order_relaxed);
    const wchar_t* key = bt ? L"LyricsNudgeBtMs" : L"LyricsNudgeMs";
    const Settings s = GetSettingsCopy();
    const int setting = bt ? s.lyricsOffsetBtMs : s.lyricsOffsetMs;
    // Clamp the nudge so setting + nudge stays in range; repeated presses never pile up invisibly.
    const int nudge = ClampInt(host::int_value(key, 0) + deltaMs,
                               -kLyricsOffsetLimitMs - setting, kLyricsOffsetLimitMs - setting);
    host::set_int_value(key, nudge);
    PublishLyricsOffsets(s.lyricsOffsetMs, s.lyricsOffsetBtMs);
    g_layoutDirty = true;
}

void ResetLyricsNudge() {
    const bool bt = g_defaultOutputIsBluetooth.load(std::memory_order_relaxed);
    host::set_int_value(bt ? L"LyricsNudgeBtMs" : L"LyricsNudgeMs", 0);
    const Settings s = GetSettingsCopy();
    PublishLyricsOffsets(s.lyricsOffsetMs, s.lyricsOffsetBtMs);
    g_layoutDirty = true;
}

struct MediaSourceMenuItem {
    std::wstring aumid;
    std::wstring name;
};

// Ticks: "Auto" = automatic mode, an app = the one currently being controlled.
std::vector<MediaSourceMenuItem> AppendMediaSourceItems(HMENU menu) {
    std::vector<MediaSourceMenuItem> items;
    std::wstring active;
    {
        std::lock_guard lock(g_stateMutex);
        active = g_state.media.sourceAppUserModelId;
        for (const MediaSourceInfo& source : g_state.mediaSources) {
            items.push_back(MediaSourceMenuItem{source.aumid, source.name});
        }
    }

    const bool autoMode = GetPreferredMediaSource().empty();
    AppendMenuW(menu, MF_STRING | (autoMode ? MF_CHECKED : 0), kMediaSourceMenuIdBase, Loc(L"Auto"));
    if (!items.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    for (size_t i = 0; i < items.size(); ++i) {
        AppendMenuW(menu, MF_STRING | (items[i].aumid == active ? MF_CHECKED : 0),
                    kMediaSourceMenuIdBase + 1 + static_cast<UINT>(i), items[i].name.c_str());
    }
    return items;
}

void ApplyMediaSourceMenuCommand(UINT cmd, const std::vector<MediaSourceMenuItem>& items) {
    if (cmd < kMediaSourceMenuIdBase) return;
    if (cmd == kMediaSourceMenuIdBase) {
        LockMediaSource(L"");
        return;
    }
    const size_t index = cmd - kMediaSourceMenuIdBase - 1;
    if (index < items.size()) {
        LockMediaSource(items[index].aumid);
    }
}

}



void ShowMediaSourcePopup(HWND hwnd, POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    const std::vector<MediaSourceMenuItem> items = AppendMediaSourceItems(menu);
    SetForegroundWindow(hwnd);
    DwmBackdrop::EnsureBehind(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    screenPoint.x, screenPoint.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DwmBackdrop::EnsureBehind(hwnd);
    g_layoutDirty = true;
    ApplyMediaSourceMenuCommand(cmd, items);
}


namespace
{


void OpenIslandBridgeWindow() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = length > 0 && length < ARRAYSIZE(path) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) {
        return;
    }
    slash[1] = L'\0';
    if (wcscat_s(path, ARRAYSIZE(path), L"IslandBridge.exe") != 0) {
        return;
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path, L"--show", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        host::log(L"Could not open IslandBridge.");
    }
}

}



void ShowContextMenu(HWND hwnd, POINT screenPoint) {
    bool timerActive = false;
    bool timerRunning = false;
    {
        std::lock_guard lock(g_stateMutex);
        timerActive = g_state.timer.active;
        timerRunning = g_state.timer.running;
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 40, g_manuallyHidden.load() ? L"Show Island" : L"Hide Island");
    AppendMenuW(menu, MF_STRING, 1, L"Dismiss");

    HMENU timerMenu = CreatePopupMenu();
    AppendMenuW(timerMenu, MF_STRING, 30, L"Start 25 min Focus");
    AppendMenuW(timerMenu, MF_STRING, 31, L"Start 50 min Focus");
    AppendMenuW(timerMenu, MF_STRING, 32, L"Start 5 min Break");
    if (timerActive) {
        AppendMenuW(timerMenu, MF_STRING, 33, timerRunning ? L"Pause Timer" : L"Resume Timer");
        AppendMenuW(timerMenu, MF_STRING, 34, L"Stop Timer");
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(timerMenu), L"Focus Timer");

    std::vector<MediaSourceMenuItem> mediaSourceItems;
    {
        HMENU sourceMenu = CreatePopupMenu();
        mediaSourceItems = AppendMediaSourceItems(sourceMenu);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sourceMenu), Loc(L"Media source"));
    }
    if (g_settings.fileTrayModule) {
        size_t trayCount = 0;
        {
            std::lock_guard lock(g_stateMutex);
            trayCount = g_state.fileTrayItems.size();
        }
        HMENU trayMenu = CreatePopupMenu();
        AppendMenuW(trayMenu, MF_STRING, 42, L"Paste from clipboard");
        const std::wstring clearLabel = trayCount == 0
            ? std::wstring(L"Clear tray")
            : L"Clear tray (" + std::to_wstring(trayCount) + L")";
        AppendMenuW(trayMenu, MF_STRING | (trayCount == 0 ? (MF_GRAYED | MF_DISABLED) : 0), 41,
                    clearLabel.c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(trayMenu), Loc(L"File Tray"));
    }
    AppendMenuW(menu, MF_STRING, 2, L"Pin expanded");
    AppendMenuW(menu, MF_STRING, 3, g_gameOverlayPinned.load() ? L"Hide game overlay" : L"Show game overlay");
    {
        // g_settings already reflects any right-click override, so it is the
        // source of truth for which style is active.
        const int currentShape = g_settings.notchStyle ? 2 : (g_settings.w11Style ? 1 : 0);
        HMENU shapeMenu = CreatePopupMenu();
        AppendMenuW(shapeMenu, MF_STRING, kShapeMenuIdBase, L"iPhone Pill");
        AppendMenuW(shapeMenu, MF_STRING, kShapeMenuIdBase + 1, L"Windows 11 Flyout");
        if (IsBottomPosition(g_settings.position)) {
            AppendMenuW(shapeMenu, MF_STRING | MF_GRAYED | MF_DISABLED, kShapeMenuIdBase + 2,
                        L"macOS Notch (unavailable at bottom)");
        } else {
            AppendMenuW(shapeMenu, MF_STRING, kShapeMenuIdBase + 2, L"macOS Notch");
        }
        CheckMenuRadioItem(shapeMenu, kShapeMenuIdBase, kShapeMenuIdBase + 2,
                           kShapeMenuIdBase + static_cast<UINT>(currentShape), MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(shapeMenu), L"Shape style");
    }
    const int activeExpandOnHover = host::int_value(L"ExpandOnHoverOverride", -1) >= 0
                          ? host::int_value(L"ExpandOnHoverOverride", 0)
                          : (host::int_setting(L"Behavior.ExpandOnHover") != 0);
    AppendMenuW(menu, MF_STRING, 11, activeExpandOnHover ? L"Expand on Click" : L"Expand on Hover");
    AppendMenuW(menu, MF_STRING | (host::int_value(L"CollapsedLyrics", 0) ? MF_CHECKED : 0), 13,
                L"Show lyrics on collapsed island");
    AppendMenuW(menu,
                MF_STRING | (g_settings.mediaTabLyrics ? MF_CHECKED : 0),
                15, L"Show lyrics in media tab");
    {
        bool cacheOn = false;
        {
            std::lock_guard lock(g_settingsMutex);
            cacheOn = g_settings.lyricsCache && g_settings.lyricsCacheMaxMB > 0;
        }
        // Counters are maintained by the lyrics thread; nothing is scanned here.
        std::wstring cacheLabel = L"Clear lyrics cache";
        if (g_lyricsCacheCountsValid.load(std::memory_order_relaxed)) {
            const int songs = g_lyricsCacheFileCount.load(std::memory_order_relaxed);
            const uint64_t bytes = g_lyricsCacheBytes.load(std::memory_order_relaxed);
            wchar_t detail[64] = {};
            if (bytes >= 1048576ull) {
                swprintf_s(detail, L" (%d songs, %.1f MB)", songs, static_cast<double>(bytes) / 1048576.0);
            } else {
                swprintf_s(detail, L" (%d songs, %llu KB)", songs,
                           static_cast<unsigned long long>((bytes + 1023ull) / 1024ull));
            }
            cacheLabel += detail;
        }
        AppendMenuW(menu, MF_STRING | (cacheOn ? 0 : (MF_GRAYED | MF_DISABLED)), 43, cacheLabel.c_str());

        {
            // Shows and adjusts whichever profile is active right now.
            const bool bt = g_defaultOutputIsBluetooth.load(std::memory_order_relaxed);
            const int effective = bt ? g_lyricsEffectiveOffsetBtMs.load(std::memory_order_relaxed)
                                     : g_lyricsEffectiveOffsetMs.load(std::memory_order_relaxed);
            wchar_t header[96] = {};
            swprintf_s(header, L"%s: %+d ms", bt ? L"Bluetooth" : L"Speakers", effective);

            HMENU lyricsSyncMenu = CreatePopupMenu();
            AppendMenuW(lyricsSyncMenu, MF_STRING | MF_GRAYED | MF_DISABLED, kLyricsSyncMenuIdBase, header);
            AppendMenuW(lyricsSyncMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(lyricsSyncMenu, MF_STRING, kLyricsSyncMenuIdBase + 1, L"Earlier +10 ms");
            AppendMenuW(lyricsSyncMenu, MF_STRING, kLyricsSyncMenuIdBase + 2, L"Earlier +50 ms");
            AppendMenuW(lyricsSyncMenu, MF_STRING, kLyricsSyncMenuIdBase + 3, L"Earlier +250 ms");
            AppendMenuW(lyricsSyncMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(lyricsSyncMenu, MF_STRING, kLyricsSyncMenuIdBase + 4, L"Later -10 ms");
            AppendMenuW(lyricsSyncMenu, MF_STRING, kLyricsSyncMenuIdBase + 5, L"Later -50 ms");
            AppendMenuW(lyricsSyncMenu, MF_STRING, kLyricsSyncMenuIdBase + 6, L"Later -250 ms");
            AppendMenuW(lyricsSyncMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(lyricsSyncMenu, MF_STRING, kLyricsSyncMenuIdBase + 7, L"Reset this profile");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(lyricsSyncMenu), L"Lyrics sync");
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const int activeOpacity = g_pillOpacityOverride.load();
    HMENU opacityMenu = CreatePopupMenu();
    AppendMenuW(opacityMenu, MF_STRING | (activeOpacity == 100 ? MF_CHECKED : 0), 4, L"100% (Opaque)");
    AppendMenuW(opacityMenu, MF_STRING | (activeOpacity == 85 ? MF_CHECKED : 0), 5, L"85%");
    AppendMenuW(opacityMenu, MF_STRING | (activeOpacity == 70 ? MF_CHECKED : 0), 6, L"70%");
    AppendMenuW(opacityMenu, MF_STRING | (activeOpacity == 55 ? MF_CHECKED : 0), 7, L"55%");
    AppendMenuW(opacityMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(opacityMenu, MF_STRING | (activeOpacity < 0 ? MF_CHECKED : 0), 8, L"Theme default");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacityMenu), L"Transparency");
    {
        const std::vector<WeatherCityEntry> weatherCities = LoadWeatherCities();
        if (!weatherCities.empty()) {
            const bool autoCity = IsWeatherCityAuto();
            const int activeCity = GetActiveWeatherCityIndex(weatherCities.size());
            HMENU weatherMenu = CreatePopupMenu();
            std::wstring autoLabel = L"Auto (by location)";
            if (autoCity) {
                std::wstring shownCity;
                {
                    std::lock_guard lock(g_stateMutex);
                    shownCity = g_state.weather.city;
                }
                if (!shownCity.empty()) {
                    autoLabel += L" \x2013 " + shownCity;
                }
            }
            AppendMenuW(weatherMenu, MF_STRING | (autoCity ? MF_CHECKED : 0), kWeatherCityAutoMenuId,
                        autoLabel.c_str());
            AppendMenuW(weatherMenu, MF_SEPARATOR, 0, nullptr);
            for (size_t i = 0; i < weatherCities.size(); ++i) {
                AppendMenuW(weatherMenu,
                            MF_STRING | (!autoCity && static_cast<int>(i) == activeCity ? MF_CHECKED : 0),
                            kWeatherCityMenuIdBase + static_cast<UINT>(i), weatherCities[i].name.c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(weatherMenu), L"Weather city");
        }
    }
    // Color theme presets. Built from kThemePalettes and nested in a submenu:
    // nine palettes listed flat would dominate the root menu.
    const int activeTheme = host::int_value(kThemeValueName, static_cast<int>(g_settings.themePreset));
    HMENU themeMenu = CreatePopupMenu();
    for (int i = 0; i < kCustomThemeIndex; ++i) {
        AppendMenuW(themeMenu, MF_STRING | (activeTheme == i ? MF_CHECKED : 0),
                    kThemeMenuIdBase + static_cast<UINT>(i), kThemePalettes[i].label);
    }
    AppendMenuW(themeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(themeMenu, MF_STRING | (activeTheme >= kCustomThemeIndex ? MF_CHECKED : 0),
                kThemeMenuIdBase + static_cast<UINT>(kCustomThemeIndex), L"Custom Colors");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), L"Theme");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 9, L"Open IslandBridge");

    SetForegroundWindow(hwnd);
    DwmBackdrop::EnsureBehind(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   screenPoint.x, screenPoint.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DwmBackdrop::EnsureBehind(hwnd);
    g_layoutDirty = true;

    // Theme ids are a contiguous range rather than individual cases, so adding a
    // palette to kThemePalettes needs no change here.
    if (cmd >= kThemeMenuIdBase &&
        cmd <= kThemeMenuIdBase + static_cast<UINT>(kCustomThemeIndex)) {
        host::set_int_value(kThemeValueName, static_cast<int>(cmd - kThemeMenuIdBase));
        LoadSettings();
        return;
    }

    if (cmd == kWeatherCityAutoMenuId ||
        (cmd >= kWeatherCityMenuIdBase && cmd < kWeatherCityMenuIdBase + kWeatherCityMenuMax)) {
        host::set_int_value(L"WeatherCityIndex", cmd == kWeatherCityAutoMenuId
                                                ? kWeatherCityAuto
                                                : static_cast<int>(cmd - kWeatherCityMenuIdBase));
        if (g_settingsChangedEvent) {
            SetEvent(g_settingsChangedEvent);
        }
        return;
    }

    if (cmd >= kMediaSourceMenuIdBase) {
        ApplyMediaSourceMenuCommand(cmd, mediaSourceItems);
        return;
    }

    switch (cmd) {
        case 1:
            DismissTransientState();
            g_clickExpanded = false;
            g_layoutDirty = true;
            TriggerNudge();
            break;
        case 2: {
            const bool nextVal = !g_pinnedExpanded.load();
            g_pinnedExpanded.store(nextVal);
            host::set_int_value(L"PinnedExpanded", nextVal ? 1 : 0);
            break;
        }
        case 3: {
            const bool nextVal = !g_gameOverlayPinned.load();
            g_gameOverlayPinned.store(nextVal);
            host::set_int_value(L"GameOverlayPinned", nextVal ? 1 : 0);
            break;
        }
        case 4:
            g_pillOpacityOverride.store(100);
            host::set_int_value(L"PillOpacityOverride", 100);
            LoadSettings();
            break;
        case 5:
            g_pillOpacityOverride.store(85);
            host::set_int_value(L"PillOpacityOverride", 85);
            LoadSettings();
            break;
        case 6:
            g_pillOpacityOverride.store(70);
            host::set_int_value(L"PillOpacityOverride", 70);
            LoadSettings();
            break;
        case 7:
            g_pillOpacityOverride.store(55);
            host::set_int_value(L"PillOpacityOverride", 55);
            LoadSettings();
            break;
        case 8:
            g_pillOpacityOverride.store(-1);
            host::set_int_value(L"PillOpacityOverride", -1);
            LoadSettings();
            break;
        case 9: {
            OpenIslandBridgeWindow();
            break;
        }
        case kShapeMenuIdBase:
        case kShapeMenuIdBase + 1:
        case kShapeMenuIdBase + 2: {
            const bool wantW11 = (cmd == kShapeMenuIdBase + 1);
            const bool wantNotch = (cmd == kShapeMenuIdBase + 2);
            if (wantNotch && IsBottomPosition(g_settings.position)) {
                break;  // a bottom-anchored island can't be a top-edge notch
            }
            // Both overrides are written every time, so exactly one style is active.
            host::set_int_value(L"W11StyleOverride", wantW11 ? 1 : 0);
            host::set_int_value(L"NotchStyleOverride", wantNotch ? 1 : 0);
            LoadSettings();
            g_layoutDirty = true;
            TriggerNudge();
            break;
        }
        case 11: {
            const int activeExpandOnHover = host::int_value(L"ExpandOnHoverOverride", -1) >= 0
                                  ? host::int_value(L"ExpandOnHoverOverride", 0)
                                  : (host::int_setting(L"Behavior.ExpandOnHover") != 0);
            host::set_int_value(L"ExpandOnHoverOverride", activeExpandOnHover ? 0 : 1);
            LoadSettings();
            g_layoutDirty = true;
            break;
        }
        case 13: {
            host::set_int_value(L"CollapsedLyrics", host::int_value(L"CollapsedLyrics", 0) ? 0 : 1);
            LoadSettings();
            g_layoutDirty = true;
            TriggerNudge();
            break;
        }
        case 15: {
            const int mediaTabLyricsSetting = host::int_setting(L"Modules.MediaTabLyrics") != 0 ? 1 : 0;
            host::set_int_value(L"MediaTabLyricsOverride", GetSettingsCopy().mediaTabLyrics ? 0 : 1);
            host::set_int_value(L"MediaTabLyricsOverrideBase", mediaTabLyricsSetting);
            LoadSettings();
            g_layoutDirty = true;
            TriggerNudge();
            break;
        }
        case 30:
            StartFocusTimer(25, false);
            break;
        case 31:
            StartFocusTimer(50, false);
            break;
        case 32:
            StartFocusTimer(5, true);
            break;
        case 33:
            ToggleTimerPause();
            break;
        case 34:
            StopFocusTimer();
            break;
        case 40: {
            // Only reachable while visible (a hidden window can't be
            // right-clicked), so in practice this toggles hidden -> the
            // hotkey is the only way back. That's intentional; see readme.
            const bool nowHidden = !g_manuallyHidden.load();
            g_manuallyHidden = nowHidden;
            host::set_int_value(L"ManuallyHidden", nowHidden ? 1 : 0);
            if (nowHidden) {
                g_hotkeyUnhideUntil.store(0.0);
            }
            g_layoutDirty = true;
            break;
        }
        case kLyricsSyncMenuIdBase + 1:
        case kLyricsSyncMenuIdBase + 2:
        case kLyricsSyncMenuIdBase + 3:
        case kLyricsSyncMenuIdBase + 4:
        case kLyricsSyncMenuIdBase + 5:
        case kLyricsSyncMenuIdBase + 6: {
            static const int kLyricsSteps[] = {10, 50, 250, -10, -50, -250};
            AdjustLyricsNudge(kLyricsSteps[cmd - kLyricsSyncMenuIdBase - 1]);
            break;
        }
        case kLyricsSyncMenuIdBase + 7:
            ResetLyricsNudge();
            break;
        case 43:
            // Only sets a flag: the lyrics thread does the deletion (never the render thread)
            // and resets the counters. Lyrics currently on screen are left alone.
            g_lyricsCacheClearRequested.store(true);
            break;
        case 42:
            PasteIntoFileTray(hwnd);
            break;
        case 41: {
            // Clear the File Tray. Only the island's references are dropped --
            // nothing on disk is touched.
            {
                std::lock_guard lock(g_stateMutex);
                g_state.fileTrayItems.clear();
            }
            SaveFileTray();
            g_hoveredFileTrayRow = -1;
            g_layoutDirty = true;
            break;
        }
    }
}


}
