#include "core/state.h"
#include "core/constants.h"
#include "core/util.h"

namespace island
{


// Returns a private copy of the current settings. Callers then read from the
// copy for the rest of the frame, which also means a settings change landing
// mid-frame can't tear a single render across two configurations.
Settings get_settings_copy() {
    std::lock_guard lock(g_settings_mutex);
    return g_settings;
}


namespace
{

std::atomic<double> g_last_nudge_time = 0.0;

}



void trigger_nudge() {
    // If we're parked (auto-hidden), any real event must wake us regardless
    // of the normal throttle, or it'd never be seen until the next hook/
    // hotkey/fullscreen-recheck wake.
    const bool was_parked = g_auto_hidden_parked.exchange(false, std::memory_order_relaxed);
    const double now = now_seconds();
    const double previous = g_last_nudge_time.load();
    if (!was_parked && now - previous < 0.45) {
        return;
    }
    g_last_nudge_time = now;
    HWND hwnd = g_hwnd;
    if (hwnd) {
        PostMessageW(hwnd, wm_app_new_event, 0, 0);
    }
}


}
