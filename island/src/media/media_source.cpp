#include "media/media_source.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
// Media source selection
int active_media_source_index(const SharedState& state)
{
    for (std::size_t i = 0; i < state.media_sources.size(); ++i)
    {
        if (state.media_sources[i].aumid == state.media.source_app_user_model_id)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring get_preferred_media_source()
{
    std::lock_guard lock(g_media_source_mutex);
    return g_preferred_media_source;
}

// Instant switch, empty = Auto. The timestamp tells the media thread this is not a new track.
void request_media_source(const std::wstring& aumid)
{
    {
        std::lock_guard lock(g_media_source_mutex);
        // Dock pick: temporary, dropped when something new starts playing or that app goes away.
        g_dock_override_aumid = aumid;
        g_dock_override_rev.fetch_add(1);
    }
    g_user_source_switch_at.store(now_seconds());
    if (g_media_refresh_event)
    {
        SetEvent(g_media_refresh_event);
    }
    g_layout_dirty = true;
}

// Context menu pick: locks the island to that app until Auto or another app is picked.
void lock_media_source(const std::wstring& aumid)
{
    {
        std::lock_guard lock(g_media_source_mutex);
        g_preferred_media_source.assign(aumid);
        g_dock_override_aumid.clear();
        g_dock_override_rev.fetch_add(1);
    }
    g_user_source_switch_at.store(now_seconds());
    if (g_media_refresh_event)
    {
        SetEvent(g_media_refresh_event);
    }
    g_layout_dirty = true;
}

void request_media_source_switch(const std::wstring& aumid)
{
    int direction = 1;
    bool same_as_active = false;
    {
        std::lock_guard lock(g_state_mutex);
        const int active = active_media_source_index(g_state);
        int target = -1;
        for (std::size_t i = 0; i < g_state.media_sources.size(); ++i)
        {
            if (g_state.media_sources[i].aumid == aumid)
            {
                target = static_cast<int>(i);
                break;
            }
        }
        same_as_active = (target >= 0 && target == active);
        if (target >= 0 && active >= 0)
        {
            direction = target >= active ? 1 : -1;
        }
    }

    if (same_as_active || !get_settings_copy().expanded_media_anim)
    {
        request_media_source(aumid);
        return;
    }
    {
        std::lock_guard lock(g_media_source_mutex);
        g_switch_target = aumid;
    }
    g_switch_dir.store(direction);
    g_switch_seq.fetch_add(1);
    g_layout_dirty = true;
}

}
