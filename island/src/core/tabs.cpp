#include "core/tabs.h"
#include "core/state.h"

namespace island
{

// ── Dashboard tab loop ───────────────────────────────────────────────────────
// Single source of truth for how many tabs are in the mouse-wheel scroll loop.
// The first tab's meaning depends on context -- the media view when something is
// playing, the clock when idle -- but the *count* is identical, which is why the
// renderer, the scroll handler and the hit tests can all share this. That
// arithmetic used to be copy-pasted in seven places, which is exactly how they
// drifted out of sync.
int active_tab_count(const Settings& settings)
{
    int count = 2; // primary (media or clock) + calendar
    if (settings.weather)
    {
        ++count;
    }
    if (settings.github_module)
    {
        ++count;
    }
    if (settings.hardware_monitor_module)
    {
        ++count;
    }
    if (settings.file_tray_module)
    {
        ++count;
    }
    if (settings.notification_center_module)
    {
        ++count;
    }
    return count;
}

int normalized_tab_index(const Settings& settings)
{
    const int total = std::max(1, active_tab_count(settings));
    const int raw = g_idle_tab.load(std::memory_order_relaxed);
    return ((raw % total) + total) % total;
}

// Tab order is always: primary, weather?, calendar, github?, hardware?, file tray?
// The expensive GPU / network counters are only sampled while the hardware card
// is actually on screen, so that check needs the card's real index rather than
// assuming it is the last tab -- which stopped being true once the File Tray
// was added after it.
int hardware_monitor_tab_index(const Settings& settings)
{
    if (!settings.hardware_monitor_module)
    {
        return -1;
    }
    return 2 + (settings.notification_center_module ? 1 : 0) + (settings.weather ? 1 : 0) +
           (settings.github_module ? 1 : 0);
}

int file_tray_tab_index(const Settings& settings)
{
    if (!settings.file_tray_module)
    {
        return -1;
    }
    return 2 + (settings.notification_center_module ? 1 : 0) + (settings.weather ? 1 : 0) +
           (settings.github_module ? 1 : 0) + (settings.hardware_monitor_module ? 1 : 0);
}

int notification_center_tab_index(const Settings& settings)
{
    if (!settings.notification_center_module)
    {
        return -1;
    }
    return 1;
}

// Moves the dashboard one tab (+1 = next, -1 = previous). Clamped at both ends:
// pushing past the first or last tab is ignored completely (no wrap, no bounce),
// so a free-spinning wheel or touchpad inertia can never make the edge tabs twitch.
void step_idle_tab(int direction)
{
    const Settings settings = get_settings_copy();
    const int count = std::max(1, active_tab_count(settings));
    const int next = normalized_tab_index(settings) + direction;
    if (next < 0 || next >= count)
    {
        return;
    }
    g_idle_tab = next;
    g_layout_dirty = true;
}

}
