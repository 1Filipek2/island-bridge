#include "ui/hit_test.h"
#include "media/media_source.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{


// ── Media dashboard hit-testing (see the notes next to MediaLayout) ──────────

// Converts a client-area mouse position into the media dashboard's content
// space. Returns valid == false when DrawMedia has not painted recently, so a
// stale frame can never produce a phantom hit.
MediaContentPoint MediaContentFromClient(int clientX, int clientY) {
    MediaContentPoint out;

    // Seqlock read: retry until a write is not in progress and the counter has
    // not moved, so all six fields come from the same frame.
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f, scale = 1.0f;
    float trim = 0.0f;
    unsigned long long stamp = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const unsigned before = g_mediaHitSeq.load(std::memory_order_relaxed);
        if (before & 1u) {
            continue;  // write in progress
        }
        std::atomic_thread_fence(std::memory_order_acquire);

        left = g_mediaHitLeft.load(std::memory_order_relaxed);
        top = g_mediaHitTop.load(std::memory_order_relaxed);
        right = g_mediaHitRight.load(std::memory_order_relaxed);
        bottom = g_mediaHitBottom.load(std::memory_order_relaxed);
        scale = g_mediaHitScale.load(std::memory_order_relaxed);
        trim = g_mediaHitTrim.load(std::memory_order_relaxed);
        stamp = g_mediaHitStamp.load(std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_mediaHitSeq.load(std::memory_order_relaxed) == before) {
            break;
        }
        stamp = 0;  // torn; force another attempt (or bail out below)
    }

    if (stamp == 0 || GetTickCount64() - stamp > 500) {
        return out;
    }

    if (scale < 0.05f) {
        scale = 1.0f;
    }

    const float width = right - left - std::max(0.0f, trim);
    const float height = bottom - top;
    if (width <= 1.0f || height <= 1.0f) {
        return out;
    }

    // DrawPill scales content about the pill centre, so undo that about the
    // same point to get back into content space.
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;
    const float contentX = (static_cast<float>(clientX) - centerX) / scale + centerX;
    const float contentY = (static_cast<float>(clientY) - centerY) / scale + centerY;

    out.valid = true;
    out.x = contentX - left;
    out.y = contentY - top;
    out.width = width;
    out.height = height;
    return out;
}


namespace
{


// The immersive lyrics view draws the transport block (scrubber, controls, dock,
// lyrics button) slightly smaller. This maps a content-space point back into that
// block's own design space, so the hit tests below keep using the MediaLayout
// constants. A no-op unless the immersive view is showing.
MediaContentPoint MediaPlayerZonePoint(const MediaContentPoint& pt) {
    const float mix = g_immersiveMixPub.load(std::memory_order_relaxed);
    if (!pt.valid || mix <= 0.001f) {
        return pt;
    }
    const float s = 1.0f + (ImmersiveLayout::kPlayerScale - 1.0f) * mix;
    const float cx = pt.width * 0.5f;
    MediaContentPoint out = pt;
    out.x = cx + (pt.x - cx) / s;
    out.y = ImmersiveLayout::kPlayerPivotY + (pt.y - ImmersiveLayout::kPlayerPivotY) / s;
    return out;
}

}



// Returns 0 = previous, 1 = play/pause, 2 = next, or -1 for no button.
int MediaTransportHitTest(const MediaContentPoint& rawPt) {
    const MediaContentPoint pt = MediaPlayerZonePoint(rawPt);
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1;
    }

    const float centerX = pt.width * 0.5f;
    const float dy = pt.y - MediaLayout::kControlsY;

    struct Target {
        float offsetX;
        float radius;
        int command;
    };
    const Target targets[] = {
        {-MediaLayout::kControlSpacing, MediaLayout::kNavButtonRadius, 0},
        {0.0f, MediaLayout::kPlayButtonRadius, 1},
        {MediaLayout::kControlSpacing, MediaLayout::kNavButtonRadius, 2},
    };

    for (const Target& t : targets) {
        const float reach = t.radius + MediaLayout::kControlHitPad;
        if (std::fabs(dy) <= reach && std::fabs(pt.x - (centerX + t.offsetX)) <= reach) {
            return t.command;
        }
    }
    return -1;
}

// Fraction along the scrubber bar (0..1) for a content-space point, or -1 when
// the point is not on the bar.
float MediaScrubFractionFromContent(const MediaContentPoint& rawPt) {
    const MediaContentPoint pt = MediaPlayerZonePoint(rawPt);
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1.0f;
    }

    const float barLeft = MediaLayout::kScrubInsetLeft;
    const float barRight = pt.width - MediaLayout::kScrubInsetRight;
    if (barRight - barLeft <= 1.0f) {
        return -1.0f;
    }

    if (std::fabs(pt.y - MediaLayout::kScrubberY) > MediaLayout::kScrubHitHalfHeight ||
        pt.x < barLeft - MediaLayout::kScrubHitPadX ||
        pt.x > barRight + MediaLayout::kScrubHitPadX) {
        return -1.0f;
    }

    return Clamp((Clamp(pt.x, barLeft, barRight) - barLeft) / (barRight - barLeft), 0.0f, 1.0f);
}

// Clamped fraction for an ongoing drag, ignoring the on-bar test so the scrub
// keeps tracking once the press has been captured.
float MediaScrubFractionUnbounded(const MediaContentPoint& rawPt) {
    const MediaContentPoint pt = MediaPlayerZonePoint(rawPt);
    if (!pt.valid) {
        return -1.0f;
    }
    const float barLeft = MediaLayout::kScrubInsetLeft;
    const float barRight = pt.width - MediaLayout::kScrubInsetRight;
    if (barRight - barLeft <= 1.0f) {
        return -1.0f;
    }
    return Clamp((Clamp(pt.x, barLeft, barRight) - barLeft) / (barRight - barLeft), 0.0f, 1.0f);
}

// Index of the File Tray row under a content-space point, or -1. Rows are drawn
// newest-first, so index 0 is the most recently dropped file.
int FileTrayRowAtContentPoint(const MediaContentPoint& pt, int itemCount) {
    if (!pt.valid || itemCount <= 0 || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1;
    }
    if (pt.x < FileTrayLayout::kPadX || pt.x > pt.width - FileTrayLayout::kPadX) {
        return -1;
    }

    const float listBottom = pt.height - FileTrayLayout::kListBottomInset;
    if (pt.y < FileTrayLayout::kListTop || pt.y > listBottom) {
        return -1;
    }

    const float stride = FileTrayLayout::kRowHeight + FileTrayLayout::kRowGap;
    const int index = static_cast<int>((pt.y - FileTrayLayout::kListTop) / stride);
    // Reject the gap between rows so hovering dead space highlights nothing.
    const float rowTop = FileTrayLayout::kListTop + index * stride;
    if (pt.y > rowTop + FileTrayLayout::kRowHeight) {
        return -1;
    }

    const int capacity = FileTrayLayout::VisibleRowCapacity(pt.height);
    if (index < 0 || index >= capacity || index >= itemCount) {
        return -1;
    }
    return index;
}

// True when the point is over the bin button in the File Tray header.
bool FileTrayClearHitTest(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return false;
    }
    constexpr float kSlop = 3.0f;
    const float right = pt.width - FileTrayLayout::kPadX;
    const float left = right - FileTrayLayout::kClearBtnSize;
    const float top = FileTrayLayout::kClearBtnTop;
    const float bottom = top + FileTrayLayout::kClearBtnSize;
    return pt.x >= left - kSlop && pt.x <= right + kSlop &&
           pt.y >= top - kSlop && pt.y <= bottom + kSlop;
}

// True when the point is over the paste button (left of the bin) in the File Tray header.
bool FileTrayPasteHitTest(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return false;
    }
    constexpr float kSlop = 3.0f;
    const float right = pt.width - FileTrayLayout::kPadX - FileTrayLayout::kClearBtnSize -
                        FileTrayLayout::kPasteBtnGap;
    const float left = right - FileTrayLayout::kClearBtnSize;
    const float top = FileTrayLayout::kClearBtnTop;
    const float bottom = top + FileTrayLayout::kClearBtnSize;
    return pt.x >= left - kSlop && pt.x <= right + kSlop &&
           pt.y >= top - kSlop && pt.y <= bottom + kSlop;
}

// True when the point is over the cut button of the given visible row.
bool FileTrayRemoveHitTest(const MediaContentPoint& pt, int row) {
    if (!pt.valid || row < 0 || pt.height <= MediaLayout::kExpandedMinHeight) {
        return false;
    }
    constexpr float kSlop = 3.0f;
    const float half = FileTrayLayout::kRemoveBtnSize * 0.5f + kSlop;
    const float centerX = pt.width - FileTrayLayout::kPadX - FileTrayLayout::kRemoveBtnMargin -
                          FileTrayLayout::kRemoveBtnSize * 0.5f;
    const float centerY = FileTrayLayout::kListTop +
                          static_cast<float>(row) * (FileTrayLayout::kRowHeight + FileTrayLayout::kRowGap) +
                          FileTrayLayout::kRowHeight * 0.5f;
    return std::fabs(pt.x - centerX) <= half && std::fabs(pt.y - centerY) <= half;
}

const notification_center::Group* OpenNotificationGroup(const SharedState& state) {
    if (!state.notificationCenterView.open() || !state.notificationCenter) {
        return nullptr;
    }
    return state.notificationCenter->find(state.notificationCenterView.key);
}

NotificationCenterHit NotificationCenterHitTest(const MediaContentPoint& pt) {
    NotificationCenterHit hit;
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return hit;
    }
    std::lock_guard lock(g_stateMutex);
    if (OpenNotificationGroup(g_state)) {
        if (notification_center::clear_hit(pt.x, pt.y, pt.width)) {
            hit.action = kNotificationCenterClear;
        } else if (notification_center::back_hit(pt.x, pt.y, pt.width)) {
            hit.action = kNotificationCenterBack;
        }
        return hit;
    }
    const int groupCount = g_state.notificationCenter ? static_cast<int>(g_state.notificationCenter->groups().size()) : 0;
    hit.row = notification_center::row_at(pt.x, pt.y, pt.width, pt.height, groupCount);
    if (groupCount > 0 && notification_center::clear_hit(pt.x, pt.y, pt.width)) {
        hit.action = kNotificationCenterClear;
    } else if (notification_center::remove_hit(pt.x, pt.y, pt.width, hit.row)) {
        hit.action = hit.row;
    }
    return hit;
}


namespace
{


void SetNotificationCenterView(notification_center::View view) {
    g_notificationCenterDetailOpen.store(view.open(), std::memory_order_relaxed);
    g_state.notificationCenterView = std::move(view);
}

}



void CloseNotificationCenterDetail() {
    if (!g_notificationCenterDetailOpen.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock(g_stateMutex);
    SetNotificationCenterView({});
}

void HandleNotificationCenterClick(const NotificationCenterHit& hit) {
    std::lock_guard lock(g_stateMutex);
    const notification_center::History* history = g_state.notificationCenter.get();
    if (!history) {
        SetNotificationCenterView({});
        return;
    }

    if (const notification_center::Group* open = OpenNotificationGroup(g_state)) {
        if (hit.action == kNotificationCenterClear) {
            auto next = std::make_shared<notification_center::History>(*history);
            next->remove(open->key);
            g_state.notificationCenter = std::move(next);
        }
        if (hit.action == kNotificationCenterClear || hit.action == kNotificationCenterBack) {
            SetNotificationCenterView({});
        }
        return;
    }

    if (hit.action == kNotificationCenterClear) {
        g_state.notificationCenter.reset();
        SetNotificationCenterView({});
    } else if (hit.action >= 0) {
        auto next = std::make_shared<notification_center::History>(*history);
        if (next->remove(static_cast<std::size_t>(hit.action))) {
            g_state.notificationCenter = std::move(next);
        }
    } else if (hit.row >= 0 && static_cast<std::size_t>(hit.row) < history->groups().size()) {
        SetNotificationCenterView(notification_center::View{history->groups()[static_cast<std::size_t>(hit.row)].key, 0});
    }
}

bool ScrollNotificationCenterDetail(int step) {
    if (!g_notificationCenterDetailOpen.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard lock(g_stateMutex);
    const notification_center::Group* open = OpenNotificationGroup(g_state);
    if (!open) {
        return false;
    }
    notification_center::View& view = g_state.notificationCenterView;
    const std::size_t next = notification_center::scrolled(view.offset, step, open->entries.size(),
                                                           g_notificationCenterDetailRows.load(std::memory_order_relaxed));
    if (next != view.offset) {
        view.offset = next;
        g_layoutDirty = true;
    }
    return true;
}

// True when the point is over the expanded layout's album art.
bool MediaArtHitTest(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return false;
    }
    if (g_immersiveMixPub.load(std::memory_order_relaxed) > 0.5f) {
        return pt.x >= ImmersiveLayout::kArtLeft &&
               pt.x <= ImmersiveLayout::kArtLeft + ImmersiveLayout::kArtSize &&
               pt.y >= ImmersiveLayout::kArtTop &&
               pt.y <= ImmersiveLayout::kArtTop + ImmersiveLayout::kArtSize;
    }
    return pt.x >= MediaLayout::kArtInsetX &&
           pt.x <= MediaLayout::kArtInsetX + MediaLayout::kArtSize &&
           pt.y >= MediaLayout::kArtInsetY &&
           pt.y <= MediaLayout::kArtInsetY + MediaLayout::kArtSize;
}

// True when the point is over the immersive-lyrics toggle button.
bool MediaLyricsButtonHitTest(const MediaContentPoint& rawPt) {
    const MediaContentPoint pt = MediaPlayerZonePoint(rawPt);
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return false;
    }
    const float cx = pt.width - ImmersiveLayout::kBtnInsetRight - ImmersiveLayout::kBtnRadius;
    const float reach = ImmersiveLayout::kBtnRadius + ImmersiveLayout::kBtnHitPad;
    return std::fabs(pt.x - cx) <= reach &&
           std::fabs(pt.y - MediaLayout::kControlsY) <= reach;
}

// Drawing and hit testing call this with the same inputs, so they always agree.
DockSlots ComputeDockSlots(int sourceCount, int activeIndex) {
    DockSlots d;
    if (sourceCount < 2) return d;
    if (sourceCount <= MediaLayout::kDockMaxSlots) {
        d.count = sourceCount;
        for (int i = 0; i < sourceCount; ++i) d.sourceIndex[i] = i;
    } else {
        d.count = 3;  // first, the active one, and "+N" for the rest
        d.sourceIndex[0] = 0;
        d.sourceIndex[1] = activeIndex >= 2 ? activeIndex : 1;
        d.sourceIndex[2] = -1;
        d.overflow = sourceCount - 2;
    }
    return d;
}


namespace
{


// Slot under a content-space point, or -1. Boundaries sit in the middle of the gaps,
// so the targets tile the pill (28px+ each) with no dead space.
int MediaSourceDockHitTest(const MediaContentPoint& rawPt, int slotCount) {
    const MediaContentPoint pt = MediaPlayerZonePoint(rawPt);
    if (slotCount < 2 || !pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1;
    }
    const float halfH = std::max(MediaLayout::kDockHitMin, MediaLayout::kDockHeight) * 0.5f;
    if (std::fabs(pt.y - MediaLayout::kControlsY) > halfH) return -1;

    const float left = MediaLayout::kDockLeft;
    const float right = left + MediaLayout::DockWidth(slotCount);
    if (pt.x < left || pt.x > right) return -1;

    const float stride = MediaLayout::kDockSlot + MediaLayout::kDockGap;
    const float origin = left + MediaLayout::kDockPad - MediaLayout::kDockGap * 0.5f;
    const int index = static_cast<int>(std::floor((pt.x - origin) / stride));
    return ClampInt(index, 0, slotCount - 1);
}

}



DockHit ResolveSourceDockHit(const MediaContentPoint& pt) {
    DockHit hit;
    std::lock_guard lock(g_stateMutex);
    const int count = static_cast<int>(g_state.mediaSources.size());
    const int active = ActiveMediaSourceIndex(g_state);
    const DockSlots slots = ComputeDockSlots(count, active);
    const int slot = MediaSourceDockHitTest(pt, slots.count);
    if (slot < 0) return hit;

    hit.slot = slot;
    const int idx = slots.sourceIndex[slot];
    if (idx < 0) {
        hit.overflow = true;
        return hit;
    }
    hit.aumid = g_state.mediaSources[idx].aumid;
    hit.isActive = (idx == active);
    return hit;
}


}
