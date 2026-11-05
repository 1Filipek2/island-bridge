#include "core/common.h"
#include "core/settings.h"
#include "core/state.h"
#include "island.h"
#include "media/audio.h"
#include "media/lyrics.h"
#include "media/media.h"
#include "notifications/dnd.h"
#include "notifications/toast_listener.h"
#include "pipe/pipe_server.h"
#include "services/github.h"
#include "services/weather.h"
#include "system/bluetooth.h"
#include "system/input_hooks.h"
#include "ui/renderer.h"

namespace island
{
namespace
{



bool StartThreads() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_settingsChangedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_mediaRefreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_settingsChangedEvent) {
        return false;
    }

    g_running = true;
    g_renderThread = CreateThread(nullptr, 0, RenderThreadProc, nullptr, 0, nullptr);
    if (!g_renderThread) {
        return false;
    }

    g_mediaThread = CreateThread(nullptr, 0, MediaThreadProc, nullptr, 0, nullptr);
    g_audioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    g_weatherThread = CreateThread(nullptr, 0, WeatherThreadProc, nullptr, 0, nullptr);
    g_githubThread = CreateThread(nullptr, 0, GitHubThreadProc, nullptr, 0, nullptr);
    g_lyricsThread = CreateThread(nullptr, 0, LyricsThreadProc, nullptr, 0, nullptr);
    g_keyboardThread = CreateThread(nullptr, 0, KeyboardThreadProc, nullptr, 0, &g_keyboardThreadId);
    g_mouseThread = CreateThread(nullptr, 0, MouseThreadProc, nullptr, 0, &g_mouseThreadId);
    toast_listener::sync_thread();
    g_bluetoothThread = CreateThread(nullptr, 0, BluetoothThreadProc, nullptr, 0, nullptr);
    g_islandPipeThread = CreateThread(nullptr, 0, island_pipe::thread_proc, nullptr, 0, nullptr);
    SubscribeDndNotification();

    return true;
}

void StopThreads() {
    UnsubscribeDndNotification();
    if (g_keyboardThreadId != 0) {
        PostThreadMessageW(g_keyboardThreadId, WM_QUIT, 0, 0);
    }
    if (g_mouseThreadId != 0) {
        PostThreadMessageW(g_mouseThreadId, WM_QUIT, 0, 0);
    }
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }

    HANDLE handles[] = {g_renderThread, g_mediaThread, g_audioThread, g_weatherThread, g_githubThread, g_lyricsThread, g_notificationThread, g_keyboardThread, g_mouseThread, g_bluetoothThread, g_islandPipeThread};
    for (HANDLE handle : handles) {
        if (handle) {
            WaitForSingleObject(handle, 3000);
            CloseHandle(handle);
        }
    }

    g_renderThread = nullptr;
    g_mediaThread = nullptr;
    g_audioThread = nullptr;
    g_weatherThread = nullptr;
    g_githubThread = nullptr;
    g_lyricsThread = nullptr;
    g_notificationThread = nullptr;
    g_keyboardThread = nullptr;
    g_keyboardThreadId = 0;
    g_mouseThread = nullptr;
    g_mouseThreadId = 0;
    g_bluetoothThread = nullptr;
    g_islandPipeThread = nullptr;

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_settingsChangedEvent) {
        CloseHandle(g_settingsChangedEvent);
        g_settingsChangedEvent = nullptr;
    }
    if (g_mediaRefreshEvent) {
        CloseHandle(g_mediaRefreshEvent);
        g_mediaRefreshEvent = nullptr;
    }

    g_running = false;
}

}

}

using namespace island;

bool island_start() {
    LoadSettings();

    if (!StartThreads()) {
        StopThreads();
        return false;
    }

    g_layoutDirty = true;
    host::log(L"Island started.");
    return true;
}

void island_settings_changed() {
    LoadSettings();
    toast_listener::sync_thread();
}

void island_stop() {
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    StopThreads();
    host::log(L"Island stopped.");
}

HANDLE island_render_thread() {
    return g_renderThread;
}
