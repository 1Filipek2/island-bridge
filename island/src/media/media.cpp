#include "media/media.h"
#include "ui/icons.h"
#include "ui/image.h"
#include "media/media_source.h"
#include "core/model.h"
#include "core/pixels.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{


// Chooses which SMTC session the island should follow.
//
// GetCurrentSession() returns whatever Windows last treated as the foreground
// media app, which is often a stale or paused session belonging to a different
// player. Following it blindly meant a player that was genuinely playing could
// be ignored entirely -- the reason VLC frequently produced no reaction at all
// while Spotify worked. Prefer the current session when it is actually playing,
// otherwise the first session that is, and only then fall back to the current
// one so paused/idle state still shows.
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession SelectActiveMediaSession(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    const std::wstring& preferredAumid) {
    using PlaybackStatus =
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    auto sessionIsPlaying = [](auto const& candidate) -> bool {
        if (!candidate) {
            return false;
        }
        try {
            return candidate.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
        } catch (...) {
            return false;
        }
    };

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession current = nullptr;
    try {
        current = manager.GetCurrentSession();
    } catch (...) {
    }

    // The user picked a source (dock / menu): follow it, playing or not.
    if (!preferredAumid.empty()) {
        try {
            for (auto const& candidate : manager.GetSessions()) {
                if (preferredAumid == candidate.SourceAppUserModelId().c_str()) {
                    return candidate;
                }
            }
        } catch (...) {
        }
    }

    if (sessionIsPlaying(current)) {
        return current;
    }

    try {
        for (auto const& candidate : manager.GetSessions()) {
            if (sessionIsPlaying(candidate)) {
                return candidate;
            }
        }
    } catch (...) {
    }

    return current;
}

// Exe name for path-style AUMIDs (classic desktop apps), friendly name otherwise.
std::wstring MediaSourceDisplayName(const std::wstring& aumid) {
    std::wstring name = FriendlyMediaSourceName(aumid);
    if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos ||
        name.find(L':') != std::wstring::npos) {
        name = StripExtension(BaseNameFromPath(aumid));
        if (!name.empty()) {
            name[0] = static_cast<wchar_t>(towupper(name[0]));
        }
    }
    return name.empty() ? std::wstring(L"Media") : name;
}

struct MediaSourceTracker {
    // Auto mode / dock pick bookkeeping (see ChooseMediaSourceAumid).
    std::unordered_map<std::wstring, bool> wasPlaying;      // playing state at the previous poll
    std::unordered_map<std::wstring, uint64_t> startedSeq;  // when each source last STARTED playing (higher = newer)
    uint64_t seqCounter = 0;
    std::wstring dockOverride;                              // temporary dock pick; empty = none
    uint64_t dockOverrideBaseSeq = 0;                       // any start newer than this ends the dock pick
    unsigned dockOverrideRevSeen = 0;
    std::vector<std::wstring> order;                     // first seen = first in the dock
    std::unordered_map<std::wstring, PixelsRef> icons;
    std::unordered_map<std::wstring, int> iconRetry;     // polls before searching for an icon again
};

bool CollectMediaSources(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    MediaSourceTracker& tracker, std::vector<MediaSourceInfo>* out) {
    using PlaybackStatus =
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    std::vector<std::pair<std::wstring, bool>> live;  // AUMID, playing
    try {
        for (auto const& session : manager.GetSessions()) {
            std::wstring aumid = session.SourceAppUserModelId().c_str();
            if (aumid.empty()) continue;
            bool playing = false;
            try {
                playing = session.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
            } catch (...) {
            }
            auto it = std::find_if(live.begin(), live.end(),
                                   [&](const auto& e) { return e.first == aumid; });
            if (it != live.end()) {
                it->second = it->second || playing;
            } else {
                live.emplace_back(std::move(aumid), playing);
            }
        }
    } catch (...) {
        return false;  // keep whatever the dock showed before
    }

    // Remember when each source last STARTED playing (not playing -> playing).
    // Auto mode follows the newest start.
    {
        std::wstring currentAumid;
        try {
            auto current = manager.GetCurrentSession();
            if (current) {
                currentAumid = current.SourceAppUserModelId().c_str();
            }
        } catch (...) {
        }

        // A source that disappeared forgets its history, so reopening it counts as a fresh start.
        for (auto it = tracker.wasPlaying.begin(); it != tracker.wasPlaying.end();) {
            const std::wstring& id = it->first;
            const bool stillLive = std::any_of(live.begin(), live.end(),
                                               [&](const auto& e) { return e.first == id; });
            if (stillLive) {
                ++it;
            } else {
                tracker.startedSeq.erase(id);
                it = tracker.wasPlaying.erase(it);
            }
        }

        std::vector<std::wstring> started;
        for (const auto& e : live) {
            bool& was = tracker.wasPlaying[e.first];
            if (e.second && !was) {
                started.push_back(e.first);
            }
            was = e.second;
        }
        // Several sources starting inside one poll: the one Windows calls "current" counts as newest.
        std::stable_sort(started.begin(), started.end(),
                         [&](const std::wstring& a, const std::wstring& b) {
                             return a != currentAumid && b == currentAumid;
                         });
        for (const std::wstring& id : started) {
            tracker.startedSeq[id] = ++tracker.seqCounter;
        }
    }

    // Stable order: new sources go on the end, vanished ones drop out.
    tracker.order.erase(
        std::remove_if(tracker.order.begin(), tracker.order.end(),
                       [&](const std::wstring& id) {
                           return std::none_of(live.begin(), live.end(),
                                               [&](const auto& e) { return e.first == id; });
                       }),
        tracker.order.end());
    for (const auto& e : live) {
        if (std::find(tracker.order.begin(), tracker.order.end(), e.first) == tracker.order.end()) {
            tracker.order.push_back(e.first);
        }
    }
    for (auto it = tracker.icons.begin(); it != tracker.icons.end();) {
        if (std::find(tracker.order.begin(), tracker.order.end(), it->first) == tracker.order.end()) {
            tracker.iconRetry.erase(it->first);
            it = tracker.icons.erase(it);
        } else {
            ++it;
        }
    }

    out->clear();
    for (const std::wstring& id : tracker.order) {
        MediaSourceInfo info;
        info.aumid = id;
        info.name = MediaSourceDisplayName(id);
        info.badge = MediaSourceBadge(info.name);
        for (const auto& e : live) {
            if (e.first == id) info.playing = e.second;
        }

        PixelsRef& icon = tracker.icons[id];
        if (!icon) {  // window enumeration is not free, so retry rarely
            int& wait = tracker.iconRetry[id];
            if (wait <= 0) {
                icon = FindMediaSourceIcon(id);
                wait = 8;
            } else {
                --wait;
            }
        }
        info.icon = icon;
        out->push_back(std::move(info));
    }
    return true;
}

// Which source should the island show?  Returns "" to fall back to the system's current session.
//   1. Dock pick: temporary. Ends when something NEW starts playing anywhere, or when
//      that app goes away. After that, the lock or Auto applies again.
//   2. Locked app (right-click menu): wins over Auto.
//   3. Auto: the playing app that started most recently. If it closes, the next most
//      recent playing app. With nothing playing: the app that played last.
std::wstring ChooseMediaSourceAumid(MediaSourceTracker& tracker, const std::wstring& lockedAumid) {
    auto exists = [&](const std::wstring& id) {
        return std::find(tracker.order.begin(), tracker.order.end(), id) != tracker.order.end();
    };
    auto seqOf = [&](const std::wstring& id) -> uint64_t {
        auto it = tracker.startedSeq.find(id);
        return it == tracker.startedSeq.end() ? 0 : it->second;
    };

    // Pick up a new dock request (or an Auto reset from the menu).
    const unsigned rev = g_dockOverrideRev.load();
    if (rev != tracker.dockOverrideRevSeen) {
        tracker.dockOverrideRevSeen = rev;
        {
            std::lock_guard lock(g_mediaSourceMutex);
            tracker.dockOverride = g_dockOverrideAumid;
        }
        tracker.dockOverrideBaseSeq = tracker.seqCounter;
    }

    if (!tracker.dockOverride.empty()) {
        bool somethingNewStarted = false;
        for (const std::wstring& id : tracker.order) {
            if (seqOf(id) > tracker.dockOverrideBaseSeq) {
                somethingNewStarted = true;
                break;
            }
        }
        if (!exists(tracker.dockOverride) || somethingNewStarted) {
            tracker.dockOverride.clear();  // back to the lock / Auto
        } else {
            return tracker.dockOverride;
        }
    }

    if (!lockedAumid.empty() && exists(lockedAumid)) {
        return lockedAumid;
    }

    // Auto. Playing sources beat paused ones; within each group the newest start wins.
    std::wstring best;
    bool bestPlaying = false;
    uint64_t bestSeq = 0;
    for (const std::wstring& id : tracker.order) {
        const auto pit = tracker.wasPlaying.find(id);
        const bool playing = pit != tracker.wasPlaying.end() && pit->second;
        const uint64_t seq = seqOf(id);
        if (best.empty() || (playing && !bestPlaying) || (playing == bestPlaying && seq > bestSeq)) {
            best = id;
            bestPlaying = playing;
            bestSeq = seq;
        }
    }
    if (!bestPlaying && bestSeq == 0) {
        return std::wstring();  // nothing has ever played: let Windows decide
    }
    return best;
}

}



DWORD WINAPI MediaThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    using PlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    Manager manager{nullptr};
    bool loggedUnavailable = false;
    int64_t pendingSeekBaseline = -1;  // guards against bogus "went backwards" readings
    MediaSourceTracker sourceTracker;
    ULONGLONG pendingSeekBaselineTickCount = 0;

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MediaSnapshot next;
        std::vector<MediaSourceInfo> newSources;
        bool gotSources = false;

        try {
            if (!manager) {
                manager = Manager::RequestAsync().get();
            }

            if (manager) {
                gotSources = CollectMediaSources(manager, sourceTracker, &newSources);
                const std::wstring chosenAumid = gotSources
                    ? ChooseMediaSourceAumid(sourceTracker, GetPreferredMediaSource())
                    : GetPreferredMediaSource();
                auto session = SelectActiveMediaSession(manager, chosenAumid);
                if (session) {
                    auto properties = session.TryGetMediaPropertiesAsync().get();
                    auto playback = session.GetPlaybackInfo();
                    auto timeline = session.GetTimelineProperties();

                    next.available = true;
                    next.playing = playback.PlaybackStatus() == PlaybackStatus::Playing;
                    next.title = properties.Title().c_str();
                    next.artist = properties.Artist().c_str();
                    next.albumTitle = properties.AlbumTitle().c_str();

                    if (timeline) {
                        const int64_t rawPos = timeline.Position().count();
                        int64_t np = rawPos;
                        int64_t ne = timeline.EndTime().count();
                        bool npP = (playback.PlaybackStatus() == PlaybackStatus::Playing);
                        if (npP) {
                            // Position() is the position at LastUpdatedTime, not "now".
                            try {
                                const auto lastUpdated = timeline.LastUpdatedTime();
                                const int64_t ageTicks = (winrt::clock::now() - lastUpdated).count();
                                if (ageTicks > 0 && ageTicks < 10 * 10000000LL) {
                                    np = rawPos + ageTicks;
                                }
                            } catch (...) {
                            }
                        }

                        std::lock_guard lock(g_stateMutex);
                        // Some sources (notably the Spotify desktop app) refresh LastUpdatedTime
                        // without moving Position(), which makes the estimate jump backwards.
                        // Hold off until a second poll confirms it is a real seek.
                        constexpr int64_t kJitterToleranceTicks = 15000000LL;   // 1.5s
                        constexpr int64_t kConfirmToleranceTicks = 5000000LL;   // 0.5s
                        const bool sameTrackPlaying =
                            npP && g_state.media.playing && ne == g_state.media.endTicks;

                        if (sameTrackPlaying) {
                            const int64_t predictedNow = g_state.media.positionTicks +
                                (static_cast<int64_t>(GetTickCount64()) - g_state.media.lastUpdatedTicks) * 10000;

                            if (np < predictedNow - kJitterToleranceTicks) {
                                const ULONGLONG nowTicks64 = GetTickCount64();
                                bool confirmed = false;
                                if (pendingSeekBaseline >= 0) {
                                    const int64_t expectedIfRealSeek = pendingSeekBaseline +
                                        (static_cast<int64_t>(nowTicks64) -
                                         static_cast<int64_t>(pendingSeekBaselineTickCount)) * 10000;
                                    const int64_t diff = np - expectedIfRealSeek;
                                    if ((diff < 0 ? -diff : diff) < kConfirmToleranceTicks) {
                                        confirmed = true;
                                    }
                                }

                                if (confirmed) {
                                    pendingSeekBaseline = -1;
                                } else {
                                    pendingSeekBaseline = np;
                                    pendingSeekBaselineTickCount = nowTicks64;
                                    np = predictedNow;
                                }
                            } else {
                                pendingSeekBaseline = -1;
                            }
                        } else {
                            pendingSeekBaseline = -1;
                        }
                        if (np != g_state.media.positionTicks ||
                            ne != g_state.media.endTicks ||
                            npP != g_state.media.playing) {
                            next.positionTicks = np;
                            next.endTicks = ne;
                            next.lastUpdatedTicks = GetTickCount64();
                        } else {
                            next.positionTicks = g_state.media.positionTicks;
                            next.endTicks = g_state.media.endTicks;
                            next.lastUpdatedTicks = g_state.media.lastUpdatedTicks;
                        }
                    }

                    next.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
                    next.sourceName = MediaSourceDisplayName(next.sourceAppUserModelId);

                    // Fallback for VLC, which often exposes a session with no
                    // Title metadata at all.
                    //
                    // The marker must still be at the END of the caption, just
                    // without insisting on the exact " - " separator that skins
                    // and fullscreen do not reliably produce. Matching the
                    // marker anywhere would happily pick up a browser tab named
                    // "How to use VLC media player - YouTube". The owning
                    // process is checked too, so only a real VLC window counts.
                    if (next.title.empty() && next.sourceName == L"VLC") {
                        const std::wstring marker = L"VLC media player";
                        HWND hwnd = nullptr;
                        while ((hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr)) != nullptr) {
                            if (!IsWindowVisible(hwnd)) {
                                continue;
                            }

                            wchar_t windowTitle[512];
                            if (GetWindowTextW(hwnd, windowTitle, ARRAYSIZE(windowTitle)) <= 0) {
                                continue;
                            }

                            std::wstring caption(windowTitle);
                            while (!caption.empty() && caption.back() == L' ') {
                                caption.pop_back();
                            }
                            if (caption.size() <= marker.size() ||
                                caption.compare(caption.size() - marker.size(),
                                                marker.size(), marker) != 0) {
                                continue;
                            }

                            std::wstring image;
                            if (!ProcessImageNameForWindow(hwnd, &image) ||
                                ToLowerCopy(BaseNameFromPath(image)).find(L"vlc") == std::wstring::npos) {
                                continue;
                            }

                            // Everything before the app name is the media title,
                            // minus any trailing " - " separator.
                            std::wstring candidate = caption.substr(0, caption.size() - marker.size());
                            while (!candidate.empty() &&
                                   (candidate.back() == L' ' || candidate.back() == L'-')) {
                                candidate.pop_back();
                            }
                            if (!candidate.empty()) {
                                next.title = candidate;
                                break;
                            }
                        }
                    }

                    std::wstring prevSourceAppUserModelId;
                    bool hasPrevIcon = false;
                    PixelsRef prevIcon;
                    uint64_t prevIconGeneration = 0;
                    std::wstring prevBadge;

                    std::wstring prevTitle;
                    std::wstring prevArtist;
                    PixelsRef prevArt;
                    uint64_t prevArtGeneration = 0;
                    double prevArtChangedAt = 0.0;
                    double prevTitleChangedAt = 0.0;

                    bool prevPlaying = false;
                    {
                        std::lock_guard lock(g_stateMutex);
                        prevSourceAppUserModelId = g_state.media.sourceAppUserModelId;
                        hasPrevIcon = static_cast<bool>(g_state.media.sourceIcon);
                        prevIcon = g_state.media.sourceIcon;
                        prevIconGeneration = g_state.media.sourceIconGeneration;
                        prevBadge = g_state.media.sourceBadge;

                        prevTitle = g_state.media.title;
                        prevArtist = g_state.media.artist;
                        prevArt = g_state.media.art;
                        prevArtGeneration = g_state.media.artGeneration;
                        prevArtChangedAt = g_state.media.artChangedAt;
                        prevTitleChangedAt = g_state.media.titleChangedAt;
                        prevPlaying = g_state.media.playing;
                    }

                    if (next.sourceAppUserModelId == prevSourceAppUserModelId) {
                        next.sourceBadge = prevBadge;
                        next.sourceIcon = prevIcon;
                        next.sourceIconGeneration = prevIconGeneration;

                        if (!hasPrevIcon) {
                            next.sourceIcon = FindMediaSourceIcon(next.sourceAppUserModelId);
                            next.sourceIconGeneration = next.sourceIcon->generation;
                        }
                    } else {
                        next.sourceBadge = MediaSourceBadge(next.sourceName);
                        if (IsBrowserMediaSource(next.sourceAppUserModelId)) {
                            next.sourceBadge = FindBrowserMediaSiteBadge(next.sourceAppUserModelId);
                        }
                        next.sourceIcon = FindMediaSourceIcon(next.sourceAppUserModelId);
                        next.sourceIconGeneration = next.sourceIcon->generation;
                    }

                    // Track when title/artist last changed. Browsers often update
                    // the text metadata a beat before they swap the artwork bytes,
                    // so we keep retrying the art fetch for a short "settle window"
                    // after any track change instead of locking onto the first
                    // (possibly stale/empty) result forever.
                    const bool sourceChanged = (next.sourceAppUserModelId != prevSourceAppUserModelId);
                    const bool metadataChanged = (sourceChanged || next.title != prevTitle || next.artist != prevArtist || (!prevPlaying && next.playing));
                    next.titleChangedAt = (metadataChanged && !next.title.empty() && next.playing) ? NowSeconds() : prevTitleChangedAt;
                    constexpr double kArtSettleSeconds = 3.0;
                    const bool artSettled = !metadataChanged &&
                        (NowSeconds() - prevTitleChangedAt) > kArtSettleSeconds;

                    if (artSettled && prevArt) {
                        next.art = prevArt;
                        next.artGeneration = prevArtGeneration;
                        next.artChangedAt = prevArtChangedAt;
                    } else if (auto thumbnail = properties.Thumbnail()) {
                        std::vector<uint8_t> bytes = ReadWinRtStreamBytes(thumbnail);
                        if (!bytes.empty()) {
                            BitmapPixels decoded;
                            if (DecodeImageBytesToPixels(bytes, &decoded)) {
                                next.art = PixelsRef(std::move(decoded));
                                next.artGeneration = next.art->generation;
                                next.artChangedAt = NowSeconds();
                            }
                        }
                    }

                    // If this particular poll's fetch came back empty (thumbnail
                    // not ready yet), don't flash a blank cover for a track that
                    // hasn't actually changed — keep the old art until a fresh
                    // fetch succeeds. But never do this across an actual track
                    // change, or we're back to showing the wrong song's art.
                    if (!next.art && prevArt && !metadataChanged) {
                        next.art = prevArt;
                        next.artGeneration = prevArtGeneration;
                        next.artChangedAt = prevArtChangedAt;
                    }
                }
            }
        } catch (...) {
            if (!loggedUnavailable) {
                host::log(L"WinRT media session unavailable; media module will fall back to idle.");
                loggedUnavailable = true;
            }
        }

        bool trackJustChanged = false;
        {
            std::lock_guard lock(g_stateMutex);

            // A switch the user asked for is not a "new track": it must not auto-expand
            // the island or open the 5s recentTrackChange window.
            const bool userSourceSwitch =
                !g_state.media.sourceAppUserModelId.empty() &&
                next.sourceAppUserModelId != g_state.media.sourceAppUserModelId &&
                NowSeconds() - g_userSourceSwitchAt.load() < 4.0;
            if (userSourceSwitch) {
                next.titleChangedAt = g_state.media.titleChangedAt;
            }

            const bool isDifferentTrack = !userSourceSwitch && (!next.title.empty() && next.playing) &&
                (next.title != g_state.media.title || next.artist != g_state.media.artist || (!g_state.media.playing && next.playing));

            if (isDifferentTrack) {
                next.titleChangedAt = NowSeconds();
                trackJustChanged = true;
                g_idleTab = 0;
            }

            if (g_state.media.art &&
                next.title == g_state.media.title && next.artist == g_state.media.artist &&
                next.positionTicks >= g_state.media.positionTicks) {
                next.art = g_state.media.art;
                next.artGeneration = g_state.media.artGeneration;
                next.artChangedAt = g_state.media.artChangedAt;
            }
            if (!next.sourceIcon &&
                next.sourceAppUserModelId == g_state.media.sourceAppUserModelId) {
                next.sourceIcon = g_state.media.sourceIcon;
                next.sourceIconGeneration = g_state.media.sourceIconGeneration;
            }

            g_state.media = std::move(next);
            if (gotSources) {
                g_state.mediaSources = std::move(newSources);
            }
        }

        if (trackJustChanged) {
            g_layoutDirty = true;
            if (g_settings.mediaAutoExpand) {
                TriggerNudge();
            }
        }

        HANDLE mediaWaits[2] = {g_stopEvent, g_mediaRefreshEvent};
        WaitForMultipleObjects(g_mediaRefreshEvent ? 2 : 1, mediaWaits, FALSE, 1500);
    }

    winrt::uninit_apartment();
    return 0;
}


}
