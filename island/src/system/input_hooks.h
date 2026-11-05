#pragma once

#include "core/common.h"

namespace island
{
constexpr UINT wm_app_capslock = WM_APP + 0x444;
inline HANDLE g_keyboard_thread = nullptr;
inline DWORD g_keyboard_thread_id = 0;
void notify_keyboard_thread_setting_changed();
DWORD WINAPI keyboard_thread_proc(void*);
inline HANDLE g_mouse_thread = nullptr;
inline DWORD g_mouse_thread_id = 0;

// Lets the mouse thread install or remove the wake hook when the island parks or unparks.
inline void notify_mouse_thread_parked_changed()
{
    if (g_mouse_thread_id != 0)
    {
        PostThreadMessageW(g_mouse_thread_id, WM_NULL, 0, 0);
    }
}

DWORD WINAPI mouse_thread_proc(void*);

}
