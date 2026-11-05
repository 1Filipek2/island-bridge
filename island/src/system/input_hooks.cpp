#include "system/input_hooks.h"
#include "core/constants.h"
#include "core/model.h"
#include "ui/placement.h"
#include "core/state.h"

namespace island
{
namespace
{

HHOOK g_keyboardHook = nullptr;

// Deliberately does nothing but forward the event.
//
// This used to write g_state.capsLock here, under g_stateMutex, before posting.
// Two problems with that. It recorded the pill even when the Caps Lock module was
// switched off, because the setting is only checked once the message reaches the
// window thread -- so the pill still appeared for its full 2.5s. And a
// WH_KEYBOARD_LL callback runs inline on the input path, blocking every keystroke
// system-wide until it returns, so taking a lock that the render, weather or
// media threads also hold risked stalling typing on the whole desktop.
//
// The WM_APP_CAPSLOCK handler writes exactly the same fields on the window
// thread, after checking the setting. Nothing is lost by only posting.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            if (kbd->vkCode == VK_CAPITAL || kbd->vkCode == VK_NUMLOCK) {
                const bool capsOn = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
                const bool numOn = (GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
                if (HWND hwnd = g_hwnd) {
                    const LPARAM state = (capsOn ? 1 : 0) | (numOn ? 2 : 0);
                    PostMessageW(hwnd, WM_APP_CAPSLOCK, kbd->vkCode, state);
                }
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

// A system-wide WH_KEYBOARD_LL puts this process on the path of every keystroke
// on the desktop, so it is only installed while it has something to report. The
// hook exists solely to notice Caps Lock and Num Lock for the indicator pill;
// with that module off it was still running for nothing.
static bool CapsLockHookWanted() {
    return g_settings.capsLock;
}

}



// Wakes the keyboard thread so it re-evaluates whether the hook is needed.
void NotifyKeyboardThreadSettingChanged() {
    if (g_keyboardThreadId != 0) {
        PostThreadMessageW(g_keyboardThreadId, WM_NULL, 0, 0);
    }
}

DWORD WINAPI KeyboardThreadProc(void*) {
    // Bounded by the stop event rather than an open-ended Sleep loop, so an early
    // unload cannot leave this spinning while waiting for a window that is never
    // going to appear.
    while (!g_hwnd && WaitForSingleObject(g_stopEvent, 10) == WAIT_TIMEOUT) {
    }

    bool quit = false;
    while (!quit && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        const bool wanted = CapsLockHookWanted();
        if (wanted && !g_keyboardHook) {
            g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
        } else if (!wanted && g_keyboardHook) {
            UnhookWindowsHookEx(g_keyboardHook);
            g_keyboardHook = nullptr;
        }

        // A low-level hook is delivered through the installing thread's message
        // queue, so this has to keep pumping while the hook is up.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) {
            break;
        }

        // Blocks until the stop event fires, a message arrives, or the timeout.
        // LoadSettings posts a WM_NULL when the module is toggled so the hook is
        // reconciled at once; the timeout is only a backstop. A timed wait, not a
        // spin, so an idle keyboard thread costs nothing.
        MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, 1000, QS_ALLINPUT);
    }

    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    return 0;
}


namespace
{


// --- Low-level mouse hook: wakes the parked render thread when the cursor
// approaches where the (currently OS-hidden) island sits, so hover-to-unhide
// keeps working without polling GetCursorPos every frame.
HHOOK g_mouseHook = nullptr;
std::atomic<int64_t> g_lastMouseWakeCheckMs = 0;

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE &&
        g_settings.unhideOnHover &&
        g_autoHiddenParked.load(std::memory_order_relaxed)) {
        const int64_t nowMs = static_cast<int64_t>(GetTickCount64());
        const int64_t last = g_lastMouseWakeCheckMs.load(std::memory_order_relaxed);
        if (nowMs - last >= 60) {  // ~16Hz check rate for responsive unhide
            g_lastMouseWakeCheckMs.store(nowMs, std::memory_order_relaxed);
            auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            HWND hwnd = g_hwnd;
            RECT dockRect = GetIslandDockRect();
            if (hwnd && PtInRect(&dockRect, info->pt)) {
                g_autoHiddenParked = false;
                PostMessageW(hwnd, WM_APP_MOUSE_WAKE, 0, 0);
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// A system-wide WH_MOUSE_LL routes every mouse event on the desktop through this
// thread, so it is only installed while it can actually do something: the hook's
// whole job is to notice a hover over a *parked* island.
//
// Gating on the settings instead was not enough. UnhideOnHover and
// AutoHideFullscreen both default to true, so every user on defaults still got
// the hook from startup -- the exact case the gate was added to avoid. Tying it
// to the parked state means it exists only while the island is actually hidden.
static bool MouseWakeHookWanted() {
    return g_settings.unhideOnHover &&
           g_autoHiddenParked.load(std::memory_order_relaxed);
}

}



DWORD WINAPI MouseThreadProc(void*) {
    while (!g_hwnd && WaitForSingleObject(g_stopEvent, 10) == WAIT_TIMEOUT) {
    }

    bool quit = false;
    while (!quit && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        const bool wanted = MouseWakeHookWanted();
        if (wanted && !g_mouseHook) {
            g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
        } else if (!wanted && g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = nullptr;
        }

        // A low-level hook is delivered through the installing thread's message
        // queue, so this has to keep pumping while the hook is up.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) {
            break;
        }

        // Blocks until the stop event fires, a message arrives, or the timeout.
        // The render thread posts a WM_NULL when it parks or unparks, so the hook
        // is reconciled immediately rather than up to a tick later; the timeout
        // is only a backstop in case a transition is ever missed. This is a timed
        // wait, not a spin, so an idle mouse thread costs nothing.
        MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, 1000, QS_ALLINPUT);
    }

    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    return 0;
}


}
