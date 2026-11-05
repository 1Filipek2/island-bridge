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



bool start_threads() {
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_settings_changed_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_media_refresh_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stop_event || !g_settings_changed_event) {
        return false;
    }

    g_running = true;
    g_render_thread = CreateThread(nullptr, 0, render_thread_proc, nullptr, 0, nullptr);
    if (!g_render_thread) {
        return false;
    }

    g_media_thread = CreateThread(nullptr, 0, media_thread_proc, nullptr, 0, nullptr);
    g_audio_thread = CreateThread(nullptr, 0, audio_thread_proc, nullptr, 0, nullptr);
    g_weather_thread = CreateThread(nullptr, 0, weather_thread_proc, nullptr, 0, nullptr);
    g_github_thread = CreateThread(nullptr, 0, git_hub_thread_proc, nullptr, 0, nullptr);
    g_lyrics_thread = CreateThread(nullptr, 0, lyrics_thread_proc, nullptr, 0, nullptr);
    g_keyboard_thread = CreateThread(nullptr, 0, keyboard_thread_proc, nullptr, 0, &g_keyboard_thread_id);
    g_mouse_thread = CreateThread(nullptr, 0, mouse_thread_proc, nullptr, 0, &g_mouse_thread_id);
    toast_listener::sync_thread();
    g_bluetooth_thread = CreateThread(nullptr, 0, bluetooth_thread_proc, nullptr, 0, nullptr);
    g_island_pipe_thread = CreateThread(nullptr, 0, island_pipe::thread_proc, nullptr, 0, nullptr);
    subscribe_dnd_notification();

    return true;
}

void stop_threads() {
    unsubscribe_dnd_notification();
    if (g_keyboard_thread_id != 0) {
        PostThreadMessageW(g_keyboard_thread_id, WM_QUIT, 0, 0);
    }
    if (g_mouse_thread_id != 0) {
        PostThreadMessageW(g_mouse_thread_id, WM_QUIT, 0, 0);
    }
    if (g_stop_event) {
        SetEvent(g_stop_event);
    }

    HANDLE handles[] = {g_render_thread, g_media_thread, g_audio_thread, g_weather_thread, g_github_thread, g_lyrics_thread, g_notification_thread, g_keyboard_thread, g_mouse_thread, g_bluetooth_thread, g_island_pipe_thread};
    for (HANDLE handle : handles) {
        if (handle) {
            WaitForSingleObject(handle, 3000);
            CloseHandle(handle);
        }
    }

    g_render_thread = nullptr;
    g_media_thread = nullptr;
    g_audio_thread = nullptr;
    g_weather_thread = nullptr;
    g_github_thread = nullptr;
    g_lyrics_thread = nullptr;
    g_notification_thread = nullptr;
    g_keyboard_thread = nullptr;
    g_keyboard_thread_id = 0;
    g_mouse_thread = nullptr;
    g_mouse_thread_id = 0;
    g_bluetooth_thread = nullptr;
    g_island_pipe_thread = nullptr;

    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    if (g_settings_changed_event) {
        CloseHandle(g_settings_changed_event);
        g_settings_changed_event = nullptr;
    }
    if (g_media_refresh_event) {
        CloseHandle(g_media_refresh_event);
        g_media_refresh_event = nullptr;
    }

    g_running = false;
}

}

}

using namespace island;

bool island_start() {
    load_settings();

    if (!start_threads()) {
        stop_threads();
        return false;
    }

    g_layout_dirty = true;
    host::log(L"Island started.");
    return true;
}

void island_settings_changed() {
    load_settings();
    toast_listener::sync_thread();
}

void island_stop() {
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    stop_threads();
    host::log(L"Island stopped.");
}

HANDLE island_render_thread() {
    return g_render_thread;
}
