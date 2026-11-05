#pragma once

#include "core/common.h"
namespace island
{
namespace MediaLayout
{

    constexpr float kExpandedWidth = 380.0f;
    constexpr float kExpandedHeight = 184.0f;

    // Extra width added on the right of the expanded media tab when "Lyrics in the
    // media tab" is on. The normal 380px player is left untouched.
    constexpr float kSideLyricsWidth = 220.0f;

    constexpr float kScrubberY = 114.0f;
    constexpr float kScrubMargin = 24.0f;
    constexpr float kScrubBarLeftInset = 48.0f;
    constexpr float kScrubBarRightInset = 48.0f;

    constexpr float kScrubHitHalfHeight = 14.0f;
    constexpr float kScrubHitPadX = 6.0f;

    // Distance from the content rect's left/right edge to each end of the
    // scrubber bar. Used by the content-space hit test so the bar's clickable
    // span follows the actual pill width instead of assuming 380px.
    constexpr float kScrubInsetLeft = kScrubMargin + kScrubBarLeftInset;
    constexpr float kScrubInsetRight = kScrubMargin + kScrubBarRightInset;

    // Transport controls exactly as DrawMedia paints them: y is measured down
    // from the content rect's top edge, x as an offset from the content rect's
    // horizontal center. The hit test derives its boxes from these same values.
    constexpr float kControlsY = 148.0f;
    constexpr float kControlSpacing = 64.0f;
    constexpr float kNavButtonRadius = 16.0f;
    constexpr float kPlayButtonRadius = 22.0f;
    // Extra slack around each button so the round targets are comfortable to
    // hit. Kept small enough that prev/next never overlap play/pause:
    // prev spans -86..-42, play spans -28..+28.
    constexpr float kControlHitPad = 6.0f;

    // Expanded-layout album art, as drawn by DrawMedia.
    constexpr float kArtInsetX = 24.0f;
    constexpr float kArtInsetY = 20.0f;
    constexpr float kArtSize = 64.0f;

    // The expanded dashboard is faded in by DrawMedia via
    // expandedAlpha = clamp((contentHeight - 60) / 60), so anything at or below
    // this height is still the collapsed pill and must not be hit-tested.
    constexpr float kExpandedMinHeight = 60.0f;

    // ── Source dock ──────────────────────────────────────────────────────────
    // Widest layout is 3 slots = 86px, so it ends at x=106, clear of the prev
    // button (~110). Draw code and hit test both read these.
    constexpr float kDockLeft = 20.0f;
    constexpr float kDockHeight = 30.0f;
    constexpr float kDockSlot = 26.0f;
    constexpr float kDockGap = 2.0f;
    constexpr float kDockPad = 2.0f;
    constexpr float kDockIcon = 16.0f;
    constexpr float kDockHitMin = 28.0f;
    constexpr int kDockMaxSlots = 3;            // 3 sources, or 2 sources + "+N"
    constexpr double kDockSlideSec = 0.30;

    // Source-switch animation
    constexpr float kSwitchShift = 14.0f;
    constexpr double kSwitchOutSec = 0.17;
    constexpr double kSwitchInSec = 0.24;
    constexpr double kSwitchHoldTimeoutSec = 1.5;

    constexpr float DockWidth(int slots) {
        return slots <= 0 ? 0.0f
                          : kDockPad * 2.0f + slots * kDockSlot + (slots - 1) * kDockGap;
    }
    constexpr float DockSlotLeft(int slot) {
        return kDockLeft + kDockPad + slot * (kDockSlot + kDockGap);
    }

}
}

namespace island
{
namespace ImmersiveLayout
{

    // Lyrics toggle button on the controls row, right-aligned (content space).
    // Drawn inside the scaled player block, so these are design-space values.
    constexpr float kBtnInsetRight = 24.0f;
    constexpr float kBtnRadius = 14.0f;
    constexpr float kBtnHitPad = 6.0f;

    // Small cover circle + one-line header. The left margin lines up with the
    // scaled player block below (dock / scrubber start at roughly x=37..40).
    constexpr float kArtLeft = 38.0f;
    constexpr float kArtTop = 12.0f;
    constexpr float kArtSize = 26.0f;
    constexpr float kHeaderTextLeft = kArtLeft + kArtSize + 10.0f;
    constexpr float kHeaderRightPad = 38.0f;

    // Lyrics zone. Sits between the header (ends ~y=38) and the scaled
    // scrubber (starts ~y=116), with an equal gap above and below.
    constexpr float kLyricsTop = 42.0f;
    constexpr float kLyricsBottom = 112.0f;
    constexpr float kPadX = 34.0f;
    constexpr float kLinePitch = 25.0f;       // centre -> neighbour (was 19: lines touched)
    constexpr float kOuterPitch = 14.0f;      // beyond the neighbour, lines are fading out

    // The transport block (scrubber, time labels, controls, source dock and the
    // lyrics button) is drawn at this scale about (content centre, pivotY).
    // Hit tests undo exactly the same transform (see MediaPlayerZonePoint).
    constexpr float kPlayerScale = 0.90f;
    constexpr float kPlayerPivotY = 176.0f;

    constexpr double kFadeSec = 0.32;         // cross-fade between normal and lyrics view
    constexpr float kSpringStiffness = 190.0f;
    constexpr float kSpringDamping = 26.0f;   // ~critically damped: no wobble on line change

}
}

namespace island
{

inline std::atomic<bool> g_immersiveLyrics{false};   // lyrics view requested
inline std::atomic<bool> g_hoveredLyricsBtn{false};
inline std::atomic<float> g_immersiveMixPub{0.0f};   // eased 0..1 mix, published by DrawMedia for hit tests

}

namespace island
{
namespace FileTrayLayout
{

    constexpr float kPadX = 26.0f;
    constexpr float kListTop = 46.0f;
    constexpr float kListBottomInset = 16.0f;
    constexpr float kRowHeight = 30.0f;
    constexpr float kRowGap = 6.0f;

    constexpr float kRemoveBtnSize = 18.0f;    // cut button at the right end of each row
    constexpr float kRemoveBtnMargin = 7.0f;   // gap between the cut button and the row's right edge
    constexpr float kClearBtnSize = 24.0f;     // bin button in the header
    constexpr float kClearBtnTop = 16.0f;      // bin top edge, measured from the content top
    constexpr float kPasteBtnGap = 4.0f;       // gap between the paste button and the bin

    constexpr int VisibleRowCapacity(float contentHeight) {
        const float span = contentHeight - kListBottomInset - kListTop + kRowGap;
        const int capacity = static_cast<int>(span / (kRowHeight + kRowGap));
        return capacity < 1 ? 1 : capacity;
    }

}
}

namespace island
{
namespace IdleStripLayout
{

    constexpr float kPadX = 14.0f;         // inner padding at each end
    constexpr float kSlotGap = 9.0f;       // clock <-> divider <-> weather
    constexpr float kDividerWidth = 1.0f;
    constexpr float kDividerInsetY = 9.0f;

    // Room reserved on the right for the mic/camera dot. DrawPrivacyDots anchors
    // it at rect.right - 20 with a 4px radius, so 18px clears it without letting
    // the text slide under it. Previously this was stolen from the text box while
    // the pill stayed 96px, so the clock just re-centred into a narrower slot.
    constexpr float kPrivacyReserve = 18.0f;

    constexpr float kHeight = 36.0f;

    // The stadium cap is kHeight/2 at each end, so anything below ~2x the height
    // stops reading as a pill. The ceiling keeps a long localized string from
    // turning the island into a bar.
    constexpr float kMinWidth = 72.0f;
    constexpr float kMaxWidth = 280.0f;

    // Measured widths are rounded up to this so sub-pixel text metrics can't
    // resize the layered window every frame.
    constexpr float kWidthQuantum = 2.0f;

}
}

namespace island
{
namespace MediaPillLayout
{

    constexpr float kBaseWidth = 100.0f;
    constexpr float kPrivacyExtraWidth = 22.0f;
    constexpr float kClockPadLeft = 14.0f;  // left edge -> clock text
    constexpr float kSlotGap = 9.0f;        // clock text -> divider
    constexpr float kDividerWidth = 1.0f;
    constexpr float kDividerInsetY = 10.0f;
    constexpr float kWidthQuantum = 2.0f;

}
}

namespace island
{
namespace GameOverlayLayout
{

    struct Metrics {
        float padX;
        float padY;
        float cardW;
        float gap;
        float fpsW;
        float fpsGap;
        float radius;
        float height;
    };

    // Cards are wider than the original 52/62 so a label and a three-digit value
    // sit in separate bands without touching, and the corner radius matches the
    // 9-10px used by every other card instead of the old 16, which on a 44px-tall
    // card was very nearly a stadium.
    constexpr Metrics For(bool compact) {
        return compact ? Metrics{10.0f,  9.0f, 62.0f, 6.0f, 74.0f, 7.0f,  9.0f, 56.0f}
                       : Metrics{12.0f, 11.0f, 74.0f, 7.0f, 88.0f, 8.0f, 10.0f, 68.0f};
    }

    constexpr float kMinWidth = 140.0f;

    constexpr float Width(bool compact, bool showFps, int metricCount) {
        const Metrics m = For(compact);
        float w = m.padX * 2.0f;
        if (showFps) {
            w += m.fpsW + m.fpsGap;
        }
        if (metricCount > 0) {
            w += static_cast<float>(metricCount) * m.cardW +
                 static_cast<float>(metricCount - 1) * m.gap;
        }
        return w < kMinWidth ? kMinWidth : w;
    }

}
}

namespace island
{


// ── Media dashboard hit-test geometry ────────────────────────────────────────
// DrawMedia publishes the exact content-space rect it painted into, together
// with the scale that maps content px to client px. OverlayWndProc converts
// mouse positions through this instead of assuming the pill is centered in the
// client area, which it is not when the notch / border-merged offset, the hover
// scale, or a split (two-pill) layout is in play. Keeping one published source
// of truth is what stops the drawn buttons and their hit boxes from drifting
// apart -- the drift that made prev/next clicks fall through to "open the app".
// The fields are guarded by a seqlock rather than read independently: the render
// thread rewrites them every frame, and during the expand animation the content
// height sweeps ~36 -> 184px, so a reader that caught half of one frame and half
// of the next could misplace a hit box for a click. g_mediaHitSeq is odd while a
// write is in progress; readers retry until they see the same even value twice.
// The payload stays in relaxed atomics so there is no formal data race; the
// seqlock counter is what provides consistency *across* the fields.
inline std::atomic<unsigned> g_mediaHitSeq{0};
inline std::atomic<float> g_mediaHitLeft{0.0f};
inline std::atomic<float> g_mediaHitTop{0.0f};
inline std::atomic<float> g_mediaHitRight{0.0f};
inline std::atomic<float> g_mediaHitBottom{0.0f};
inline std::atomic<float> g_mediaHitScale{1.0f};
inline std::atomic<float> g_mediaHitTrim{0.0f};  // content px of lyrics panel to the right of the player
inline std::atomic<unsigned long long> g_mediaHitStamp{0};

struct MediaContentPoint {
    bool valid = false;
    float x = 0.0f;        // relative to the content rect's left edge
    float y = 0.0f;        // relative to the content rect's top edge
    float width = 0.0f;    // content rect width
    float height = 0.0f;   // content rect height
};

}
