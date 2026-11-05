#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{


// Guards every access to g_settings. LoadSettings() replaces the whole struct
// from Windhawk's settings thread *and* from the tray context menu (which runs
// on the render thread), while the render, weather and media threads read it
// concurrently. Settings holds std::wstring / std::vector members, so an
// unsynchronised assignment can free a buffer a reader is still walking -- not
// a stale-value glitch but a genuine use-after-free.
//
// This is a strict LEAF lock: never acquire another mutex while holding it.
// That keeps it deadlock-free even where a caller already owns g_stateMutex
// (see WeatherThreadProc), because no path can ever take the two in the
// opposite order.
inline std::mutex g_settingsMutex;
Settings GetSettingsCopy();

inline std::mutex g_stateMutex;
inline SharedState g_state;
inline std::atomic<uint64_t> g_artGenerationCounter = 0;

inline HWND g_hwnd = nullptr;
inline HANDLE g_stopEvent = nullptr;
inline HANDLE g_settingsChangedEvent = nullptr;
inline HANDLE g_renderThread = nullptr;
inline HANDLE g_mediaThread = nullptr;
inline HANDLE g_audioThread = nullptr;
inline HANDLE g_weatherThread = nullptr;
inline HANDLE g_githubThread = nullptr;
inline HANDLE g_lyricsThread = nullptr;
inline HANDLE g_notificationThread = nullptr;
inline HANDLE g_bluetoothThread = nullptr;
inline HANDLE g_islandPipeThread = nullptr;
inline std::atomic<bool> g_running = false;
inline std::atomic<int> g_idleTab = 0;

// Lyrics sync offset. Written rarely (settings load, right-click nudge, default-output
// change) and only read by draw code, so no locks are needed.
inline std::atomic<bool> g_defaultOutputIsBluetooth{false};  // set by BindVolumeEndpoint(), event-driven
inline std::atomic<int> g_lyricsEffectiveOffsetMs{0};        // speakers / wired: setting + right-click nudge
inline std::atomic<int> g_lyricsEffectiveOffsetBtMs{0};      // Bluetooth output: setting + right-click nudge
// Set when the default render endpoint must be re-read. Declared up here (not next to
// VolumeNotifyClient) so the Bluetooth watcher, which comes earlier in the file, can set it.
inline std::atomic<bool> g_volumeEndpointStale{true};
inline std::atomic<bool> g_layoutDirty = true;
inline std::atomic<bool> g_clickExpanded = false;
inline std::atomic<int> g_pressedMediaButton = -1;
inline std::atomic<int> g_hoveredMediaButton = -1;
inline std::atomic<unsigned> g_skipTriggerPrev{0};  // bumped on click, the render thread plays the prev button's animation
inline std::atomic<unsigned> g_skipTriggerNext{0};  // same for the next button

// Media source selection (source dock + right-click submenu).
inline std::mutex g_mediaSourceMutex;                     // leaf lock for the two strings below
inline std::wstring g_preferredMediaSource;               // AUMID the user picked; empty = Auto
inline std::wstring g_switchTarget;                       // pending animated switch
inline std::wstring g_dockOverrideAumid;                  // temporary pick from the dock; guarded by g_mediaSourceMutex
inline std::atomic<unsigned> g_dockOverrideRev{0};        // bumped on every dock pick / Auto reset
inline std::atomic<int> g_switchDir{1};
inline std::atomic<unsigned> g_switchSeq{0};
inline std::atomic<double> g_userSourceSwitchAt{-100.0};  // NowSeconds() of the last user switch
inline HANDLE g_mediaRefreshEvent = nullptr;              // wakes MediaThreadProc
inline std::atomic<int> g_hoveredSourceSlot{-1};
inline std::atomic<int> g_pressedSourceSlot{-1};
inline std::atomic<int> g_hoveredFileTrayRow = -1;  // row index under the cursor on the File Tray card
inline std::atomic<int> g_hoveredFileTrayAction = -1;  // -2 = bin button, >=0 = cut button of that row
inline std::atomic<int> g_hoveredNotificationRow = -1;
inline std::atomic<int> g_hoveredNotificationAction = -1;
inline std::atomic<bool> g_notificationCenterUnread{false};
inline std::atomic<bool> g_notificationCenterDetailOpen{false};
inline std::atomic<int> g_notificationCenterDetailRows{1};
inline std::atomic<bool> g_trayDragOver{false};      // a file drag from outside is hovering the island
inline std::atomic<bool> g_trayInternalDrag{false};  // a drag that started from the tray itself is running
inline std::atomic<int> g_tabEdgeBump{0};            // +1 / -1: wheel or swipe pushed past the last / first tab
inline std::atomic<double> g_tabLastPaintAt{0.0};    // last time the tab view painted a moving frame
inline std::atomic<bool> g_tabAnimating{false};      // a tab transition is in motion (render loop paces frames for it)
inline bool g_oleInitialized = false;
inline std::atomic<bool> g_scrubbing = false;           // true while press-dragging the media timeline scrubber
inline std::atomic<float> g_scrubDragFraction = 0.0f;   // live 0..1 drag position while g_scrubbing is true
inline std::atomic<double> g_lastLiveSeekTime = 0.0;    // throttle gate for live seeks while dragging
inline std::atomic<bool> g_audioCaptureNeeded = false;  // gates the WASAPI loopback thread
inline std::atomic<bool> g_spectrumBandsNeeded = false; // only Pulse Orb / Plasma Thread read the frequency bands
inline std::atomic<bool> g_manuallyHidden = false;      // user-toggled hide, via menu or hotkey
inline UINT g_registeredHotkeyModifiers = 0;            // modifiers currently registered with the OS
inline UINT g_registeredHotkeyVk = 0;                   // vk currently registered with the OS
inline bool g_hotkeyRegistered = false;

// --- Zero-CPU parking for idle/fullscreen auto-hide (mirrors g_manuallyHidden) ---
inline std::atomic<bool> g_autoHiddenParked = false;          // true while OS-hidden due to idle/fullscreen auto-hide
inline std::atomic<bool> g_fullscreenOverrideVisible = false; // user forced the island visible via hotkey while fullscreen
inline std::atomic<bool> g_isFullscreen = false;              // cached fullscreen state, shared with the hotkey handler
inline std::atomic<double> g_hotkeyUnhideUntil = 0.0;         // grace deadline to keep island visible after hotkey / settings unhide
inline FILETIME g_prevIdleTime = {};
inline FILETIME g_prevKernelTime = {};
inline FILETIME g_prevUserTime = {};
inline UINT g_shellHookMessage = 0;
inline UINT g_taskbarCreatedMessage = 0;
inline bool g_brightnessInitialized = false;
inline int g_lastBrightness = -1;
inline BYTE g_brightnessPowerSource = 255;  // ACLineStatus of the previous sample
inline std::atomic<double> g_lastLyricsViewTime = 0.0;

inline std::mutex g_bluetoothBatteryCacheMutex;
inline std::unordered_map<std::wstring, int> g_bluetoothBatteryCache;  // Bluetooth device Id -> last known battery percent (-1 = never learned)
inline std::atomic<uint64_t> g_bluetoothConnectGeneration = 0;
inline std::atomic<bool> g_isDnDActive = false;
inline void* g_wnfDndSubscription = nullptr;

inline std::atomic<bool> g_gameOverlayPinned{false};
inline std::atomic<bool> g_pinnedExpanded{false};
inline std::atomic<int> g_pillOpacityOverride{-1};
void TriggerNudge();

}
