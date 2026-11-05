#include "core/tabs.h"
#include "core/state.h"

namespace island
{
// Tab loop: the first tab is media or the Clock, the count is the same either way.
int active_tab_count(const Settings& settings)
{
    int count = 2;
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

// Tab order: primary, notification center?, weather?, calendar, github?, hardware?, file tray?
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

// Clamped at both ends (no wrap), so wheel inertia cannot make the edge tabs twitch.
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
