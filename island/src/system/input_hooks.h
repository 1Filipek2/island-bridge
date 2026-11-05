#pragma once

#include "core/common.h"
namespace island
{


constexpr UINT WM_APP_CAPSLOCK = WM_APP + 0x444;
inline HANDLE g_keyboardThread = nullptr;
inline DWORD g_keyboardThreadId = 0;
void NotifyKeyboardThreadSettingChanged();
DWORD WINAPI KeyboardThreadProc(void*);
inline HANDLE g_mouseThread = nullptr;
inline DWORD g_mouseThreadId = 0;

// Wakes the mouse thread so it re-evaluates whether the wake hook is needed.
// Posted whenever the island parks or unparks, so the hook is installed and
// removed in step with that rather than on the thread's backstop timeout.
inline void NotifyMouseThreadParkedChanged() {
    if (g_mouseThreadId != 0) {
        PostThreadMessageW(g_mouseThreadId, WM_NULL, 0, 0);
    }
}
DWORD WINAPI MouseThreadProc(void*);

}
