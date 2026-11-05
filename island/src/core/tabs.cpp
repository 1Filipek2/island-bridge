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
int ActiveTabCount(const Settings& settings) {
    int count = 2;  // primary (media or clock) + calendar
    if (settings.weather) ++count;
    if (settings.githubModule) ++count;
    if (settings.hardwareMonitorModule) ++count;
    if (settings.fileTrayModule) ++count;
    if (settings.notificationCenterModule) ++count;
    if (settings.lyrics && g_lyricsTabAvailable.load(std::memory_order_relaxed)) ++count;
    return count;
}

int NormalizedTabIndex(const Settings& settings) {
    const int total = std::max(1, ActiveTabCount(settings));
    const int raw = g_idleTab.load(std::memory_order_relaxed);
    return ((raw % total) + total) % total;
}

// Tab order is always: primary, weather?, calendar, github?, hardware?, file tray?
// The expensive GPU / network counters are only sampled while the hardware card
// is actually on screen, so that check needs the card's real index rather than
// assuming it is the last tab -- which stopped being true once the File Tray
// was added after it.
// With "Pin lyrics as first tab" on (and a Lyrics tab present), the media view's
// order is: lyrics, media, calendar, ... so the media tab and every tab after it
// sit one position later. Returns the media tab's index: 0 normally, 1 when pinned.
int MediaTabIndex(const Settings& settings) {
    return (settings.lyricsPinnedFirst && settings.lyrics &&
            g_lyricsTabAvailable.load(std::memory_order_relaxed)) ? 1 : 0;
}

int HardwareMonitorTabIndex(const Settings& settings) {
    if (!settings.hardwareMonitorModule) {
        return -1;
    }
    return 2 + (settings.notificationCenterModule ? 1 : 0) + (settings.weather ? 1 : 0) +
           (settings.githubModule ? 1 : 0) + MediaTabIndex(settings);
}

int FileTrayTabIndex(const Settings& settings) {
    if (!settings.fileTrayModule) {
        return -1;
    }
    return 2 + (settings.notificationCenterModule ? 1 : 0) + (settings.weather ? 1 : 0) +
           (settings.githubModule ? 1 : 0) + (settings.hardwareMonitorModule ? 1 : 0) +
           MediaTabIndex(settings);
}

int NotificationCenterTabIndex(const Settings& settings) {
    if (!settings.notificationCenterModule) {
        return -1;
    }
    return 1 + MediaTabIndex(settings);
}

// Moves the dashboard one tab (+1 = next, -1 = previous). Clamped at both ends:
// pushing past the first or last tab is ignored completely (no wrap, no bounce),
// so a free-spinning wheel or touchpad inertia can never make the edge tabs twitch.
void StepIdleTab(int direction) {
    const Settings settings = GetSettingsCopy();
    const int count = std::max(1, ActiveTabCount(settings));
    const int next = NormalizedTabIndex(settings) + direction;
    if (next < 0 || next >= count) {
        return;
    }
    g_idleTab = next;
    g_layoutDirty = true;
}


}
