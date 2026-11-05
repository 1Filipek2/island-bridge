#include "media/media_source.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{


// ── Media source selection ───────────────────────────────────────────────────
int ActiveMediaSourceIndex(const SharedState& state) {
    for (size_t i = 0; i < state.mediaSources.size(); ++i) {
        if (state.mediaSources[i].aumid == state.media.sourceAppUserModelId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring GetPreferredMediaSource() {
    std::lock_guard lock(g_mediaSourceMutex);
    return g_preferredMediaSource;
}

// Instant switch. Empty AUMID = Auto. The timestamp tells MediaThreadProc this title
// change is not a "new track": no auto-expand, no 5s recentTrackChange window.
void RequestMediaSource(const std::wstring& aumid) {
    {
        std::lock_guard lock(g_mediaSourceMutex);
        // Dock pick: a TEMPORARY override, not a lock. MediaThreadProc drops it as soon as
        // something new starts playing or that app goes away, so Auto keeps working.
        g_dockOverrideAumid = aumid;
        g_dockOverrideRev.fetch_add(1);
    }
    g_userSourceSwitchAt.store(NowSeconds());
    if (g_mediaRefreshEvent) {
        SetEvent(g_mediaRefreshEvent);
    }
    g_layoutDirty = true;
}

// Right-click menu pick. Empty AUMID = Auto. A real AUMID LOCKS the island to that app
// until Auto or another app is picked; what starts playing elsewhere is ignored.
// Any temporary dock pick is cleared, so "Auto" really means "newest playing app".
void LockMediaSource(const std::wstring& aumid) {
    {
        std::lock_guard lock(g_mediaSourceMutex);
        g_preferredMediaSource.assign(aumid);
        g_dockOverrideAumid.clear();
        g_dockOverrideRev.fetch_add(1);
    }
    g_userSourceSwitchAt.store(NowSeconds());
    if (g_mediaRefreshEvent) {
        SetEvent(g_mediaRefreshEvent);
    }
    g_layoutDirty = true;
}

// Dock click. Animated when "Expanded player transitions" is on: the renderer slides
// the old content out first, then calls RequestMediaSource().
void RequestMediaSourceSwitch(const std::wstring& aumid) {
    int direction = 1;
    bool sameAsActive = false;
    {
        std::lock_guard lock(g_stateMutex);
        const int active = ActiveMediaSourceIndex(g_state);
        int target = -1;
        for (size_t i = 0; i < g_state.mediaSources.size(); ++i) {
            if (g_state.mediaSources[i].aumid == aumid) {
                target = static_cast<int>(i);
                break;
            }
        }
        sameAsActive = (target >= 0 && target == active);
        if (target >= 0 && active >= 0) {
            direction = target >= active ? 1 : -1;
        }
    }

    if (sameAsActive || !GetSettingsCopy().expandedMediaAnim) {
        RequestMediaSource(aumid);
        return;
    }
    {
        std::lock_guard lock(g_mediaSourceMutex);
        g_switchTarget = aumid;
    }
    g_switchDir.store(direction);
    g_switchSeq.fetch_add(1);
    g_layoutDirty = true;
}


}
