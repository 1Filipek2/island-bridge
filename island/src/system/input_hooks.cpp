#include "system/input_hooks.h"
#include "core/constants.h"
#include "core/model.h"
#include "ui/placement.h"
#include "core/state.h"

namespace island
{
namespace
{
HHOOK g_keyboard_hook = nullptr;

// Only forwards the event: a WH_KEYBOARD_LL callback blocks every keystroke on the desktop until it
// returns, so the window thread does the work (and checks the setting).
LRESULT CALLBACK low_level_keyboard_proc(int n_code, WPARAM w_param, LPARAM l_param)
{
    if (n_code == HC_ACTION)
    {
        if (w_param == WM_KEYUP || w_param == WM_SYSKEYUP)
        {
            auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(l_param);
            if (kbd->vkCode == VK_CAPITAL || kbd->vkCode == VK_NUMLOCK)
            {
                const bool caps_on = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
                const bool num_on = (GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
                if (HWND hwnd = g_hwnd)
                {
                    const LPARAM state = (caps_on ? 1 : 0) | (num_on ? 2 : 0);
                    PostMessageW(hwnd, wm_app_capslock, kbd->vkCode, state);
                }
            }
        }
    }
    return CallNextHookEx(g_keyboard_hook, n_code, w_param, l_param);
}

// The system-wide keyboard hook is installed only while the Caps Lock module is on.
static bool caps_lock_hook_wanted()
{
    return g_settings.caps_lock;
}

}

void notify_keyboard_thread_setting_changed()
{
    if (g_keyboard_thread_id != 0)
    {
        PostThreadMessageW(g_keyboard_thread_id, WM_NULL, 0, 0);
    }
}

DWORD WINAPI keyboard_thread_proc(void*)
{
    while (!g_hwnd && WaitForSingleObject(g_stop_event, 10) == WAIT_TIMEOUT)
    {
    }

    bool quit = false;
    while (!quit && WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        const bool wanted = caps_lock_hook_wanted();
        if (wanted && !g_keyboard_hook)
        {
            g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard_proc, nullptr, 0);
        }
        else if (!wanted && g_keyboard_hook)
        {
            UnhookWindowsHookEx(g_keyboard_hook);
            g_keyboard_hook = nullptr;
        }

        // A low-level hook is delivered through this thread's message queue, so keep pumping.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit)
        {
            break;
        }

        MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, 1000, QS_ALLINPUT);
    }

    if (g_keyboard_hook)
    {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
    return 0;
}

namespace
{
// Low-level mouse hook: wakes the parked render thread when the cursor nears the hidden island, so
// hover-to-unhide needs no polling.
HHOOK g_mouse_hook = nullptr;
std::atomic<int64_t> g_last_mouse_wake_check_ms = 0;

LRESULT CALLBACK low_level_mouse_proc(int n_code, WPARAM w_param, LPARAM l_param)
{
    if (n_code == HC_ACTION && w_param == WM_MOUSEMOVE && g_settings.unhide_on_hover &&
        g_auto_hidden_parked.load(std::memory_order_relaxed))
    {
        const int64_t now_ms = static_cast<int64_t>(GetTickCount64());
        const int64_t last = g_last_mouse_wake_check_ms.load(std::memory_order_relaxed);
        if (now_ms - last >= 60)
        {
            g_last_mouse_wake_check_ms.store(now_ms, std::memory_order_relaxed);
            auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(l_param);
            HWND hwnd = g_hwnd;
            RECT dock_rect = get_island_dock_rect();
            if (hwnd && PtInRect(&dock_rect, info->pt))
            {
                g_auto_hidden_parked = false;
                PostMessageW(hwnd, wm_app_mouse_wake, 0, 0);
            }
        }
    }
    return CallNextHookEx(g_mouse_hook, n_code, w_param, l_param);
}

// The system-wide mouse hook exists only while the island is parked (hidden).
static bool mouse_wake_hook_wanted()
{
    return g_settings.unhide_on_hover && g_auto_hidden_parked.load(std::memory_order_relaxed);
}

}

DWORD WINAPI mouse_thread_proc(void*)
{
    while (!g_hwnd && WaitForSingleObject(g_stop_event, 10) == WAIT_TIMEOUT)
    {
    }

    bool quit = false;
    while (!quit && WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        const bool wanted = mouse_wake_hook_wanted();
        if (wanted && !g_mouse_hook)
        {
            g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, low_level_mouse_proc, nullptr, 0);
        }
        else if (!wanted && g_mouse_hook)
        {
            UnhookWindowsHookEx(g_mouse_hook);
            g_mouse_hook = nullptr;
        }

        // A low-level hook is delivered through this thread's message queue, so keep pumping.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit)
        {
            break;
        }

        MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, 1000, QS_ALLINPUT);
    }

    if (g_mouse_hook)
    {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = nullptr;
    }
    return 0;
}

}
