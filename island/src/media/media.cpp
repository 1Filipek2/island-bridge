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
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession select_active_media_session(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    const std::wstring& preferred_aumid)
{
    using PlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    auto session_is_playing = [](auto const& candidate) -> bool
    {
        if (!candidate)
        {
            return false;
        }
        try
        {
            return candidate.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
        }
        catch (...)
        {
            return false;
        }
    };

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession current = nullptr;
    try
    {
        current = manager.GetCurrentSession();
    }
    catch (...)
    {
    }

    // The user picked a source (dock / menu): follow it, playing or not.
    if (!preferred_aumid.empty())
    {
        try
        {
            for (auto const& candidate : manager.GetSessions())
            {
                if (preferred_aumid == candidate.SourceAppUserModelId().c_str())
                {
                    return candidate;
                }
            }
        }
        catch (...)
        {
        }
    }

    if (session_is_playing(current))
    {
        return current;
    }

    try
    {
        for (auto const& candidate : manager.GetSessions())
        {
            if (session_is_playing(candidate))
            {
                return candidate;
            }
        }
    }
    catch (...)
    {
    }

    return current;
}

// Exe name for path-style AUMIDs (classic desktop apps), friendly name otherwise.
std::wstring media_source_display_name(const std::wstring& aumid)
{
    std::wstring name = friendly_media_source_name(aumid);
    if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos ||
        name.find(L':') != std::wstring::npos)
    {
        name = strip_extension(base_name_from_path(aumid));
        if (!name.empty())
        {
            name[0] = static_cast<wchar_t>(towupper(name[0]));
        }
    }
    return name.empty() ? std::wstring(L"Media") : name;
}

struct MediaSourceTracker
{
    // Auto mode / dock pick bookkeeping (see ChooseMediaSourceAumid).
    std::unordered_map<std::wstring, bool> was_playing;     // playing state at the previous poll
    std::unordered_map<std::wstring, uint64_t> started_seq; // when each source last STARTED playing (higher = newer)
    uint64_t seq_counter = 0;
    std::wstring dock_override;          // temporary dock pick; empty = none
    uint64_t dock_override_base_seq = 0; // any start newer than this ends the dock pick
    unsigned dock_override_rev_seen = 0;
    std::vector<std::wstring> order; // first seen = first in the dock
    std::unordered_map<std::wstring, PixelsRef> icons;
    std::unordered_map<std::wstring, int> icon_retry; // polls before searching for an icon again
};

bool collect_media_sources(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    MediaSourceTracker& tracker, std::vector<MediaSourceInfo>* out)
{
    using PlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    std::vector<std::pair<std::wstring, bool>> live; // AUMID, playing
    try
    {
        for (auto const& session : manager.GetSessions())
        {
            std::wstring aumid = session.SourceAppUserModelId().c_str();
            if (aumid.empty())
            {
                continue;
            }
            bool playing = false;
            try
            {
                playing = session.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
            }
            catch (...)
            {
            }
            auto it = std::find_if(live.begin(), live.end(),
                                   [&](const auto& e)
                                   {
                                       return e.first == aumid;
                                   });
            if (it != live.end())
            {
                it->second = it->second || playing;
            }
            else
            {
                live.emplace_back(std::move(aumid), playing);
            }
        }
    }
    catch (...)
    {
        return false; // keep whatever the dock showed before
    }

    // Remember when each source last STARTED playing (not playing -> playing).
    // Auto mode follows the newest start.
    {
        std::wstring current_aumid;
        try
        {
            auto current = manager.GetCurrentSession();
            if (current)
            {
                current_aumid = current.SourceAppUserModelId().c_str();
            }
        }
        catch (...)
        {
        }

        // A source that disappeared forgets its history, so reopening it counts as a fresh start.
        for (auto it = tracker.was_playing.begin(); it != tracker.was_playing.end();)
        {
            const std::wstring& id = it->first;
            const bool still_live = std::any_of(live.begin(), live.end(),
                                                [&](const auto& e)
                                                {
                                                    return e.first == id;
                                                });
            if (still_live)
            {
                ++it;
            }
            else
            {
                tracker.started_seq.erase(id);
                it = tracker.was_playing.erase(it);
            }
        }

        std::vector<std::wstring> started;
        for (const auto& e : live)
        {
            bool& was = tracker.was_playing[e.first];
            if (e.second && !was)
            {
                started.push_back(e.first);
            }
            was = e.second;
        }
        // Several sources starting inside one poll: the one Windows calls "current" counts as newest.
        std::stable_sort(started.begin(), started.end(),
                         [&](const std::wstring& a, const std::wstring& b)
                         {
                             return a != current_aumid && b == current_aumid;
                         });
        for (const std::wstring& id : started)
        {
            tracker.started_seq[id] = ++tracker.seq_counter;
        }
    }

    // Stable order: new sources go on the end, vanished ones drop out.
    tracker.order.erase(std::remove_if(tracker.order.begin(), tracker.order.end(),
                                       [&](const std::wstring& id)
                                       {
                                           return std::none_of(live.begin(), live.end(),
                                                               [&](const auto& e)
                                                               {
                                                                   return e.first == id;
                                                               });
                                       }),
                        tracker.order.end());
    for (const auto& e : live)
    {
        if (std::find(tracker.order.begin(), tracker.order.end(), e.first) == tracker.order.end())
        {
            tracker.order.push_back(e.first);
        }
    }
    for (auto it = tracker.icons.begin(); it != tracker.icons.end();)
    {
        if (std::find(tracker.order.begin(), tracker.order.end(), it->first) == tracker.order.end())
        {
            tracker.icon_retry.erase(it->first);
            it = tracker.icons.erase(it);
        }
        else
        {
            ++it;
        }
    }

    out->clear();
    for (const std::wstring& id : tracker.order)
    {
        MediaSourceInfo info;
        info.aumid = id;
        info.name = media_source_display_name(id);
        info.badge = media_source_badge(info.name);
        for (const auto& e : live)
        {
            if (e.first == id)
            {
                info.playing = e.second;
            }
        }

        PixelsRef& icon = tracker.icons[id];
        if (!icon)
        { // window enumeration is not free, so retry rarely
            int& wait = tracker.icon_retry[id];
            if (wait <= 0)
            {
                icon = find_media_source_icon(id);
                wait = 8;
            }
            else
            {
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
std::wstring choose_media_source_aumid(MediaSourceTracker& tracker, const std::wstring& locked_aumid)
{
    auto exists = [&](const std::wstring& id)
    {
        return std::find(tracker.order.begin(), tracker.order.end(), id) != tracker.order.end();
    };
    auto seq_of = [&](const std::wstring& id) -> uint64_t
    {
        auto it = tracker.started_seq.find(id);
        return it == tracker.started_seq.end() ? 0 : it->second;
    };

    // Pick up a new dock request (or an Auto reset from the menu).
    const unsigned rev = g_dock_override_rev.load();
    if (rev != tracker.dock_override_rev_seen)
    {
        tracker.dock_override_rev_seen = rev;
        {
            std::lock_guard lock(g_media_source_mutex);
            tracker.dock_override = g_dock_override_aumid;
        }
        tracker.dock_override_base_seq = tracker.seq_counter;
    }

    if (!tracker.dock_override.empty())
    {
        bool something_new_started = false;
        for (const std::wstring& id : tracker.order)
        {
            if (seq_of(id) > tracker.dock_override_base_seq)
            {
                something_new_started = true;
                break;
            }
        }
        if (!exists(tracker.dock_override) || something_new_started)
        {
            tracker.dock_override.clear(); // back to the lock / Auto
        }
        else
        {
            return tracker.dock_override;
        }
    }

    if (!locked_aumid.empty() && exists(locked_aumid))
    {
        return locked_aumid;
    }

    // Auto. Playing sources beat paused ones; within each group the newest start wins.
    std::wstring best;
    bool best_playing = false;
    uint64_t best_seq = 0;
    for (const std::wstring& id : tracker.order)
    {
        const auto pit = tracker.was_playing.find(id);
        const bool playing = pit != tracker.was_playing.end() && pit->second;
        const uint64_t seq = seq_of(id);
        if (best.empty() || (playing && !best_playing) || (playing == best_playing && seq > best_seq))
        {
            best = id;
            best_playing = playing;
            best_seq = seq;
        }
    }
    if (!best_playing && best_seq == 0)
    {
        return std::wstring(); // nothing has ever played: let Windows decide
    }
    return best;
}

}

DWORD WINAPI media_thread_proc(void*)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    using PlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    Manager manager{nullptr};
    bool logged_unavailable = false;
    int64_t pending_seek_baseline = -1; // guards against bogus "went backwards" readings
    MediaSourceTracker source_tracker;
    ULONGLONG pending_seek_baseline_tick_count = 0;

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        MediaSnapshot next;
        std::vector<MediaSourceInfo> new_sources;
        bool got_sources = false;

        try
        {
            if (!manager)
            {
                manager = Manager::RequestAsync().get();
            }

            if (manager)
            {
                got_sources = collect_media_sources(manager, source_tracker, &new_sources);
                const std::wstring chosen_aumid =
                    got_sources ? choose_media_source_aumid(source_tracker, get_preferred_media_source())
                                : get_preferred_media_source();
                auto session = select_active_media_session(manager, chosen_aumid);
                if (session)
                {
                    auto properties = session.TryGetMediaPropertiesAsync().get();
                    auto playback = session.GetPlaybackInfo();
                    auto timeline = session.GetTimelineProperties();

                    next.available = true;
                    next.playing = playback.PlaybackStatus() == PlaybackStatus::Playing;
                    next.title = properties.Title().c_str();
                    next.artist = properties.Artist().c_str();
                    next.album_title = properties.AlbumTitle().c_str();

                    if (timeline)
                    {
                        const int64_t raw_pos = timeline.Position().count();
                        int64_t np = raw_pos;
                        int64_t ne = timeline.EndTime().count();
                        bool np_p = (playback.PlaybackStatus() == PlaybackStatus::Playing);
                        if (np_p)
                        {
                            // Position() is the position at LastUpdatedTime, not "now".
                            try
                            {
                                const auto last_updated = timeline.LastUpdatedTime();
                                const int64_t age_ticks = (winrt::clock::now() - last_updated).count();
                                if (age_ticks > 0 && age_ticks < 10 * 10000000LL)
                                {
                                    np = raw_pos + age_ticks;
                                }
                            }
                            catch (...)
                            {
                            }
                        }

                        std::lock_guard lock(g_state_mutex);
                        // Some sources (notably the Spotify desktop app) refresh LastUpdatedTime
                        // without moving Position(), which makes the estimate jump backwards.
                        // Hold off until a second poll confirms it is a real seek.
                        constexpr int64_t jitter_tolerance_ticks = 15000000LL; // 1.5s
                        constexpr int64_t confirm_tolerance_ticks = 5000000LL; // 0.5s
                        const bool same_track_playing = np_p && g_state.media.playing && ne == g_state.media.end_ticks;

                        if (same_track_playing)
                        {
                            const int64_t predicted_now =
                                g_state.media.position_ticks +
                                (static_cast<int64_t>(GetTickCount64()) - g_state.media.last_updated_ticks) * 10000;

                            if (np < predicted_now - jitter_tolerance_ticks)
                            {
                                const ULONGLONG now_ticks64 = GetTickCount64();
                                bool confirmed = false;
                                if (pending_seek_baseline >= 0)
                                {
                                    const int64_t expected_if_real_seek =
                                        pending_seek_baseline +
                                        (static_cast<int64_t>(now_ticks64) -
                                         static_cast<int64_t>(pending_seek_baseline_tick_count)) *
                                            10000;
                                    const int64_t diff = np - expected_if_real_seek;
                                    if ((diff < 0 ? -diff : diff) < confirm_tolerance_ticks)
                                    {
                                        confirmed = true;
                                    }
                                }

                                if (confirmed)
                                {
                                    pending_seek_baseline = -1;
                                }
                                else
                                {
                                    pending_seek_baseline = np;
                                    pending_seek_baseline_tick_count = now_ticks64;
                                    np = predicted_now;
                                }
                            }
                            else
                            {
                                pending_seek_baseline = -1;
                            }
                        }
                        else
                        {
                            pending_seek_baseline = -1;
                        }
                        if (np != g_state.media.position_ticks || ne != g_state.media.end_ticks ||
                            np_p != g_state.media.playing)
                        {
                            next.position_ticks = np;
                            next.end_ticks = ne;
                            next.last_updated_ticks = GetTickCount64();
                        }
                        else
                        {
                            next.position_ticks = g_state.media.position_ticks;
                            next.end_ticks = g_state.media.end_ticks;
                            next.last_updated_ticks = g_state.media.last_updated_ticks;
                        }
                    }

                    next.source_app_user_model_id = session.SourceAppUserModelId().c_str();
                    next.source_name = media_source_display_name(next.source_app_user_model_id);

                    // Fallback for VLC, which often exposes a session with no
                    // Title metadata at all.
                    //
                    // The marker must still be at the END of the caption, just
                    // without insisting on the exact " - " separator that skins
                    // and fullscreen do not reliably produce. Matching the
                    // marker anywhere would happily pick up a browser tab named
                    // "How to use VLC media player - YouTube". The owning
                    // process is checked too, so only a real VLC window counts.
                    if (next.title.empty() && next.source_name == L"VLC")
                    {
                        const std::wstring marker = L"VLC media player";
                        HWND hwnd = nullptr;
                        while ((hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr)) != nullptr)
                        {
                            if (!IsWindowVisible(hwnd))
                            {
                                continue;
                            }

                            wchar_t window_title[512];
                            if (GetWindowTextW(hwnd, window_title, ARRAYSIZE(window_title)) <= 0)
                            {
                                continue;
                            }

                            std::wstring caption(window_title);
                            while (!caption.empty() && caption.back() == L' ')
                            {
                                caption.pop_back();
                            }
                            if (caption.size() <= marker.size() ||
                                caption.compare(caption.size() - marker.size(), marker.size(), marker) != 0)
                            {
                                continue;
                            }

                            std::wstring image;
                            if (!process_image_name_for_window(hwnd, &image) ||
                                to_lower_copy(base_name_from_path(image)).find(L"vlc") == std::wstring::npos)
                            {
                                continue;
                            }

                            // Everything before the app name is the media title,
                            // minus any trailing " - " separator.
                            std::wstring candidate = caption.substr(0, caption.size() - marker.size());
                            while (!candidate.empty() && (candidate.back() == L' ' || candidate.back() == L'-'))
                            {
                                candidate.pop_back();
                            }
                            if (!candidate.empty())
                            {
                                next.title = candidate;
                                break;
                            }
                        }
                    }

                    std::wstring prev_source_app_user_model_id;
                    bool has_prev_icon = false;
                    PixelsRef prev_icon;
                    uint64_t prev_icon_generation = 0;
                    std::wstring prev_badge;

                    std::wstring prev_title;
                    std::wstring prev_artist;
                    PixelsRef prev_art;
                    uint64_t prev_art_generation = 0;
                    double prev_art_changed_at = 0.0;
                    double prev_title_changed_at = 0.0;

                    bool prev_playing = false;
                    {
                        std::lock_guard lock(g_state_mutex);
                        prev_source_app_user_model_id = g_state.media.source_app_user_model_id;
                        has_prev_icon = static_cast<bool>(g_state.media.source_icon);
                        prev_icon = g_state.media.source_icon;
                        prev_icon_generation = g_state.media.source_icon_generation;
                        prev_badge = g_state.media.source_badge;

                        prev_title = g_state.media.title;
                        prev_artist = g_state.media.artist;
                        prev_art = g_state.media.art;
                        prev_art_generation = g_state.media.art_generation;
                        prev_art_changed_at = g_state.media.art_changed_at;
                        prev_title_changed_at = g_state.media.title_changed_at;
                        prev_playing = g_state.media.playing;
                    }

                    if (next.source_app_user_model_id == prev_source_app_user_model_id)
                    {
                        next.source_badge = prev_badge;
                        next.source_icon = prev_icon;
                        next.source_icon_generation = prev_icon_generation;

                        if (!has_prev_icon)
                        {
                            next.source_icon = find_media_source_icon(next.source_app_user_model_id);
                            next.source_icon_generation = next.source_icon->generation;
                        }
                    }
                    else
                    {
                        next.source_badge = media_source_badge(next.source_name);
                        if (is_browser_media_source(next.source_app_user_model_id))
                        {
                            next.source_badge = find_browser_media_site_badge(next.source_app_user_model_id);
                        }
                        next.source_icon = find_media_source_icon(next.source_app_user_model_id);
                        next.source_icon_generation = next.source_icon->generation;
                    }

                    // Track when title/artist last changed. Browsers often update
                    // the text metadata a beat before they swap the artwork bytes,
                    // so we keep retrying the art fetch for a short "settle window"
                    // after any track change instead of locking onto the first
                    // (possibly stale/empty) result forever.
                    const bool source_changed = (next.source_app_user_model_id != prev_source_app_user_model_id);
                    const bool metadata_changed = (source_changed || next.title != prev_title ||
                                                   next.artist != prev_artist || (!prev_playing && next.playing));
                    next.title_changed_at = (metadata_changed && !next.title.empty() && next.playing)
                                                ? now_seconds()
                                                : prev_title_changed_at;
                    constexpr double art_settle_seconds = 3.0;
                    const bool art_settled =
                        !metadata_changed && (now_seconds() - prev_title_changed_at) > art_settle_seconds;

                    if (art_settled && prev_art)
                    {
                        next.art = prev_art;
                        next.art_generation = prev_art_generation;
                        next.art_changed_at = prev_art_changed_at;
                    }
                    else if (auto thumbnail = properties.Thumbnail())
                    {
                        std::vector<uint8_t> bytes = read_win_rt_stream_bytes(thumbnail);
                        if (!bytes.empty())
                        {
                            BitmapPixels decoded;
                            if (decode_image_bytes_to_pixels(bytes, &decoded))
                            {
                                next.art = PixelsRef(std::move(decoded));
                                next.art_generation = next.art->generation;
                                next.art_changed_at = now_seconds();
                            }
                        }
                    }

                    // If this particular poll's fetch came back empty (thumbnail
                    // not ready yet), don't flash a blank cover for a track that
                    // hasn't actually changed — keep the old art until a fresh
                    // fetch succeeds. But never do this across an actual track
                    // change, or we're back to showing the wrong song's art.
                    if (!next.art && prev_art && !metadata_changed)
                    {
                        next.art = prev_art;
                        next.art_generation = prev_art_generation;
                        next.art_changed_at = prev_art_changed_at;
                    }
                }
            }
        }
        catch (...)
        {
            if (!logged_unavailable)
            {
                host::log(L"WinRT media session unavailable; media module will fall back to idle.");
                logged_unavailable = true;
            }
        }

        bool track_just_changed = false;
        {
            std::lock_guard lock(g_state_mutex);

            // A switch the user asked for is not a "new track": it must not auto-expand
            // the island or open the 5s recentTrackChange window.
            const bool user_source_switch = !g_state.media.source_app_user_model_id.empty() &&
                                            next.source_app_user_model_id != g_state.media.source_app_user_model_id &&
                                            now_seconds() - g_user_source_switch_at.load() < 4.0;
            if (user_source_switch)
            {
                next.title_changed_at = g_state.media.title_changed_at;
            }

            const bool is_different_track = !user_source_switch && (!next.title.empty() && next.playing) &&
                                            (next.title != g_state.media.title || next.artist != g_state.media.artist ||
                                             (!g_state.media.playing && next.playing));

            if (is_different_track)
            {
                next.title_changed_at = now_seconds();
                track_just_changed = true;
                g_idle_tab = 0;
            }

            if (g_state.media.art && next.title == g_state.media.title && next.artist == g_state.media.artist &&
                next.position_ticks >= g_state.media.position_ticks)
            {
                next.art = g_state.media.art;
                next.art_generation = g_state.media.art_generation;
                next.art_changed_at = g_state.media.art_changed_at;
            }
            if (!next.source_icon && next.source_app_user_model_id == g_state.media.source_app_user_model_id)
            {
                next.source_icon = g_state.media.source_icon;
                next.source_icon_generation = g_state.media.source_icon_generation;
            }

            g_state.media = std::move(next);
            if (got_sources)
            {
                g_state.media_sources = std::move(new_sources);
            }
        }

        if (track_just_changed)
        {
            g_layout_dirty = true;
            if (g_settings.media_auto_expand)
            {
                trigger_nudge();
            }
        }

        HANDLE media_waits[2] = {g_stop_event, g_media_refresh_event};
        WaitForMultipleObjects(g_media_refresh_event ? 2 : 1, media_waits, FALSE, 1500);
    }

    winrt::uninit_apartment();
    return 0;
}

}
