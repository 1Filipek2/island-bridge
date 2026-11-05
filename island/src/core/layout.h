#pragma once

#include "core/common.h"
namespace island
{
namespace media_layout
{

    constexpr float expanded_width = 380.0f;
    constexpr float expanded_height = 184.0f;

    // Extra width added on the right of the expanded media tab when "Lyrics in the
    // media tab" is on. The normal 380px player is left untouched.
    constexpr float side_lyrics_width = 220.0f;

    constexpr float k_scrubber_y = 114.0f;
    constexpr float scrub_margin = 24.0f;
    constexpr float scrub_bar_left_inset = 48.0f;
    constexpr float scrub_bar_right_inset = 48.0f;

    constexpr float scrub_hit_half_height = 14.0f;
    constexpr float scrub_hit_pad_x = 6.0f;

    // Distance from the content rect's left/right edge to each end of the
    // scrubber bar. Used by the content-space hit test so the bar's clickable
    // span follows the actual pill width instead of assuming 380px.
    constexpr float scrub_inset_left = scrub_margin + scrub_bar_left_inset;
    constexpr float scrub_inset_right = scrub_margin + scrub_bar_right_inset;

    // Transport controls exactly as DrawMedia paints them: y is measured down
    // from the content rect's top edge, x as an offset from the content rect's
    // horizontal center. The hit test derives its boxes from these same values.
    constexpr float controls_y = 148.0f;
    constexpr float control_spacing = 64.0f;
    constexpr float nav_button_radius = 16.0f;
    constexpr float play_button_radius = 22.0f;
    // Extra slack around each button so the round targets are comfortable to
    // hit. Kept small enough that prev/next never overlap play/pause:
    // prev spans -86..-42, play spans -28..+28.
    constexpr float control_hit_pad = 6.0f;

    // Expanded-layout album art, as drawn by DrawMedia.
    constexpr float art_inset_x = 24.0f;
    constexpr float art_inset_y = 20.0f;
    constexpr float k_art_size = 64.0f;

    // The expanded dashboard is faded in by DrawMedia via
    // expandedAlpha = clamp((contentHeight - 60) / 60), so anything at or below
    // this height is still the collapsed pill and must not be hit-tested.
    constexpr float expanded_min_height = 60.0f;

    // ── Source dock ──────────────────────────────────────────────────────────
    // Widest layout is 3 slots = 86px, so it ends at x=106, clear of the prev
    // button (~110). Draw code and hit test both read these.
    constexpr float dock_left = 20.0f;
    constexpr float k_dock_height = 30.0f;
    constexpr float dock_slot = 26.0f;
    constexpr float dock_gap = 2.0f;
    constexpr float dock_pad = 2.0f;
    constexpr float dock_icon = 16.0f;
    constexpr float dock_hit_min = 28.0f;
    constexpr int dock_max_slots = 3;            // 3 sources, or 2 sources + "+N"
    constexpr double dock_slide_sec = 0.30;

    // Source-switch animation
    constexpr float switch_shift = 14.0f;
    constexpr double switch_out_sec = 0.17;
    constexpr double switch_in_sec = 0.24;
    constexpr double switch_hold_timeout_sec = 1.5;

    constexpr float dock_width(int slots) {
        return slots <= 0 ? 0.0f
                          : dock_pad * 2.0f + slots * dock_slot + (slots - 1) * dock_gap;
    }
    constexpr float dock_slot_left(int slot) {
        return dock_left + dock_pad + slot * (dock_slot + dock_gap);
    }

}
}

namespace island
{
namespace immersive_layout
{

    // Lyrics toggle button on the controls row, right-aligned (content space).
    // Drawn inside the scaled player block, so these are design-space values.
    constexpr float btn_inset_right = 24.0f;
    constexpr float btn_radius = 14.0f;
    constexpr float btn_hit_pad = 6.0f;

    // Small cover circle + one-line header. The left margin lines up with the
    // scaled player block below (dock / scrubber start at roughly x=37..40).
    constexpr float art_left = 38.0f;
    constexpr float art_top = 12.0f;
    constexpr float k_art_size = 26.0f;
    constexpr float header_text_left = art_left + k_art_size + 10.0f;
    constexpr float header_right_pad = 38.0f;

    // Lyrics zone. Sits between the header (ends ~y=38) and the scaled
    // scrubber (starts ~y=116), with an equal gap above and below.
    constexpr float lyrics_top = 42.0f;
    constexpr float lyrics_bottom = 112.0f;
    constexpr float k_pad_x = 34.0f;
    constexpr float line_pitch = 25.0f;       // centre -> neighbour (was 19: lines touched)
    constexpr float outer_pitch = 14.0f;      // beyond the neighbour, lines are fading out

    // The transport block (scrubber, time labels, controls, source dock and the
    // lyrics button) is drawn at this scale about (content centre, pivotY).
    // Hit tests undo exactly the same transform (see MediaPlayerZonePoint).
    constexpr float player_scale = 0.90f;
    constexpr float player_pivot_y = 176.0f;

    constexpr double fade_sec = 0.32;         // cross-fade between normal and lyrics view
    constexpr float spring_stiffness = 190.0f;
    constexpr float spring_damping = 26.0f;   // ~critically damped: no wobble on line change

}
}

namespace island
{

inline std::atomic<bool> g_immersive_lyrics{false};   // lyrics view requested
inline std::atomic<bool> g_hovered_lyrics_btn{false};
inline std::atomic<float> g_immersive_mix_pub{0.0f};   // eased 0..1 mix, published by DrawMedia for hit tests

}

namespace island
{
namespace file_tray_layout
{

    constexpr float k_pad_x = 26.0f;
    constexpr float k_list_top = 46.0f;
    constexpr float k_list_bottom_inset = 16.0f;
    constexpr float k_row_height = 30.0f;
    constexpr float k_row_gap = 6.0f;

    constexpr float remove_btn_size = 18.0f;    // cut button at the right end of each row
    constexpr float remove_btn_margin = 7.0f;   // gap between the cut button and the row's right edge
    constexpr float clear_btn_size = 24.0f;     // bin button in the header
    constexpr float clear_btn_top = 16.0f;      // bin top edge, measured from the content top
    constexpr float paste_btn_gap = 4.0f;       // gap between the paste button and the bin

    constexpr int visible_row_capacity(float content_height) {
        const float span = content_height - k_list_bottom_inset - k_list_top + k_row_gap;
        const int capacity = static_cast<int>(span / (k_row_height + k_row_gap));
        return capacity < 1 ? 1 : capacity;
    }

}
}

namespace island
{
namespace idle_strip_layout
{

    constexpr float k_pad_x = 14.0f;         // inner padding at each end
    constexpr float slot_gap = 9.0f;       // clock <-> divider <-> weather
    constexpr float divider_width = 1.0f;
    constexpr float divider_inset_y = 9.0f;

    // Room reserved on the right for the mic/camera dot. DrawPrivacyDots anchors
    // it at rect.right - 20 with a 4px radius, so 18px clears it without letting
    // the text slide under it. Previously this was stolen from the text box while
    // the pill stayed 96px, so the clock just re-centred into a narrower slot.
    constexpr float k_privacy_reserve = 18.0f;

    constexpr float k_height = 36.0f;

    // The stadium cap is kHeight/2 at each end, so anything below ~2x the height
    // stops reading as a pill. The ceiling keeps a long localized string from
    // turning the island into a bar.
    constexpr float min_width = 72.0f;
    constexpr float k_max_width = 280.0f;

    // Measured widths are rounded up to this so sub-pixel text metrics can't
    // resize the layered window every frame.
    constexpr float width_quantum = 2.0f;

}
}

namespace island
{
namespace media_pill_layout
{

    constexpr float base_width = 100.0f;
    constexpr float privacy_extra_width = 22.0f;
    constexpr float clock_pad_left = 14.0f;  // left edge -> clock text
    constexpr float slot_gap = 9.0f;        // clock text -> divider
    constexpr float divider_width = 1.0f;
    constexpr float divider_inset_y = 10.0f;
    constexpr float width_quantum = 2.0f;

}
}

namespace island
{
namespace game_overlay_layout
{

    struct Metrics {
        float pad_x;
        float pad_y;
        float card_w;
        float gap;
        float fps_w;
        float fps_gap;
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

    constexpr float min_width = 140.0f;

    constexpr float width(bool compact, bool show_fps, int metric_count) {
        const Metrics m = For(compact);
        float w = m.pad_x * 2.0f;
        if (show_fps) {
            w += m.fps_w + m.fps_gap;
        }
        if (metric_count > 0) {
            w += static_cast<float>(metric_count) * m.card_w +
                 static_cast<float>(metric_count - 1) * m.gap;
        }
        return w < min_width ? min_width : w;
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
inline std::atomic<unsigned> g_media_hit_seq{0};
inline std::atomic<float> g_media_hit_left{0.0f};
inline std::atomic<float> g_media_hit_top{0.0f};
inline std::atomic<float> g_media_hit_right{0.0f};
inline std::atomic<float> g_media_hit_bottom{0.0f};
inline std::atomic<float> g_media_hit_scale{1.0f};
inline std::atomic<float> g_media_hit_trim{0.0f};  // content px of lyrics panel to the right of the player
inline std::atomic<unsigned long long> g_media_hit_stamp{0};

struct MediaContentPoint {
    bool valid = false;
    float x = 0.0f;        // relative to the content rect's left edge
    float y = 0.0f;        // relative to the content rect's top edge
    float width = 0.0f;    // content rect width
    float height = 0.0f;   // content rect height
};

}
