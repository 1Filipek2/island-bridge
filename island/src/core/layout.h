#pragma once

#include "core/common.h"

namespace island
{
namespace media_layout
{
constexpr float expanded_width = 380.0f;
constexpr float expanded_height = 184.0f;

// Extra width on the right of the expanded media tab when lyrics in the media tab are on.
constexpr float side_lyrics_width = 220.0f;

constexpr float scrubber_y = 114.0f;
constexpr float scrub_margin = 24.0f;
constexpr float scrub_bar_left_inset = 48.0f;
constexpr float scrub_bar_right_inset = 48.0f;

constexpr float scrub_hit_half_height = 14.0f;
constexpr float scrub_hit_pad_x = 6.0f;

constexpr float scrub_inset_left = scrub_margin + scrub_bar_left_inset;
constexpr float scrub_inset_right = scrub_margin + scrub_bar_right_inset;

// Transport controls as draw_media paints them: y from the content top, x from the content center.
// The hit test uses the same values.
constexpr float controls_y = 148.0f;
constexpr float control_spacing = 64.0f;
constexpr float nav_button_radius = 16.0f;
constexpr float play_button_radius = 22.0f;
constexpr float control_hit_pad = 6.0f;

constexpr float art_inset_x = 24.0f;
constexpr float art_inset_y = 20.0f;
constexpr float art_size = 64.0f;

// Below this content height the island is still the collapsed pill: no hit testing.
constexpr float expanded_min_height = 60.0f;

// Source dock
constexpr float dock_left = 20.0f;
constexpr float dock_height = 30.0f;
constexpr float dock_slot = 26.0f;
constexpr float dock_gap = 2.0f;
constexpr float dock_pad = 2.0f;
constexpr float dock_icon = 16.0f;
constexpr float dock_hit_min = 28.0f;
constexpr int dock_max_slots = 3; // 3 sources, or 2 sources + "+N"
constexpr double dock_slide_sec = 0.30;

// Source switch animation
constexpr float switch_shift = 14.0f;
constexpr double switch_out_sec = 0.17;
constexpr double switch_in_sec = 0.24;
constexpr double switch_hold_timeout_sec = 1.5;

constexpr float dock_width(int slots)
{
    return slots <= 0 ? 0.0f : dock_pad * 2.0f + slots * dock_slot + (slots - 1) * dock_gap;
}

constexpr float dock_slot_left(int slot)
{
    return dock_left + dock_pad + slot * (dock_slot + dock_gap);
}

}
}

namespace island
{
namespace immersive_layout
{
// Lyrics toggle button on the controls row (design space, inside the scaled player block).
constexpr float btn_inset_right = 24.0f;
constexpr float btn_radius = 14.0f;
constexpr float btn_hit_pad = 6.0f;

constexpr float art_left = 38.0f;
constexpr float art_top = 12.0f;
constexpr float art_size = 26.0f;
constexpr float header_text_left = art_left + art_size + 10.0f;
constexpr float header_right_pad = 38.0f;

constexpr float lyrics_top = 42.0f;
constexpr float lyrics_bottom = 112.0f;
constexpr float pad_x = 34.0f;
constexpr float line_pitch = 25.0f;
constexpr float outer_pitch = 14.0f;

// The transport block is drawn at this scale; media_player_zone_point undoes the same transform.
constexpr float player_scale = 0.90f;
constexpr float player_pivot_y = 176.0f;

constexpr double fade_sec = 0.32; // cross-fade between normal and lyrics view
constexpr float spring_stiffness = 190.0f;
constexpr float spring_damping = 26.0f;

}
}

namespace island
{
inline std::atomic<bool> g_immersive_lyrics{false}; // lyrics view requested
inline std::atomic<bool> g_hovered_lyrics_btn{false};
inline std::atomic<float> g_immersive_mix_pub{0.0f}; // eased 0..1 mix, published by draw_media for hit tests

}

namespace island
{
namespace file_tray_layout
{
constexpr float pad_x = 26.0f;
constexpr float list_top = 46.0f;
constexpr float list_bottom_inset = 16.0f;
constexpr float row_height = 30.0f;
constexpr float row_gap = 6.0f;

constexpr float remove_btn_size = 18.0f;  // cut button at the right end of each row
constexpr float remove_btn_margin = 7.0f; // gap between the cut button and the row's right edge
constexpr float clear_btn_size = 24.0f;   // bin button in the header
constexpr float clear_btn_top = 16.0f;    // bin top edge, measured from the content top
constexpr float paste_btn_gap = 4.0f;

constexpr int visible_row_capacity(float content_height)
{
    const float span = content_height - list_bottom_inset - list_top + row_gap;
    const int capacity = static_cast<int>(span / (row_height + row_gap));
    return capacity < 1 ? 1 : capacity;
}

}
}

namespace island
{
namespace idle_strip_layout
{
constexpr float pad_x = 14.0f;
constexpr float slot_gap = 9.0f; // Clock <-> divider <-> weather
constexpr float divider_width = 1.0f;
constexpr float divider_inset_y = 9.0f;

// Room on the right for the mic/camera dot.
constexpr float privacy_reserve = 18.0f;

constexpr float height = 36.0f;

// Below ~2x the height the stadium stops reading as a pill; the ceiling stops long strings from making a bar.
constexpr float min_width = 72.0f;
constexpr float max_width = 280.0f;

// Widths round up to this so sub-pixel text metrics cannot resize the window every frame.
constexpr float width_quantum = 2.0f;

}
}

namespace island
{
namespace media_pill_layout
{
constexpr float base_width = 100.0f;
constexpr float privacy_extra_width = 22.0f;
constexpr float clock_pad_left = 14.0f; // left edge -> Clock text
constexpr float slot_gap = 9.0f;        // Clock text -> divider
constexpr float divider_width = 1.0f;
constexpr float divider_inset_y = 10.0f;
constexpr float width_quantum = 2.0f;

}
}

namespace island
{
namespace game_overlay_layout
{
struct Metrics
{
    float pad_x;
    float pad_y;
    float card_w;
    float gap;
    float fps_w;
    float fps_gap;
    float radius;
    float height;
};

constexpr Metrics metrics_for(bool compact)
{
    return compact ? Metrics{10.0f, 9.0f, 62.0f, 6.0f, 74.0f, 7.0f, 9.0f, 56.0f}
                   : Metrics{12.0f, 11.0f, 74.0f, 7.0f, 88.0f, 8.0f, 10.0f, 68.0f};
}

constexpr float min_width = 140.0f;

constexpr float width(bool compact, bool show_fps, int metric_count)
{
    const Metrics m = metrics_for(compact);
    float w = m.pad_x * 2.0f;
    if (show_fps)
    {
        w += m.fps_w + m.fps_gap;
    }
    if (metric_count > 0)
    {
        w += static_cast<float>(metric_count) * m.card_w + static_cast<float>(metric_count - 1) * m.gap;
    }
    return w < min_width ? min_width : w;
}

}
}

namespace island
{
// Media hit-test geometry: draw_media publishes the content rect it painted and its scale, the window
// procedure maps the mouse through it. Seqlock: g_media_hit_seq is odd during a write, readers retry
// until they see the same even value twice.
inline std::atomic<unsigned> g_media_hit_seq{0};
inline std::atomic<float> g_media_hit_left{0.0f};
inline std::atomic<float> g_media_hit_top{0.0f};
inline std::atomic<float> g_media_hit_right{0.0f};
inline std::atomic<float> g_media_hit_bottom{0.0f};
inline std::atomic<float> g_media_hit_scale{1.0f};
inline std::atomic<float> g_media_hit_trim{0.0f}; // content px of lyrics panel to the right of the player
inline std::atomic<unsigned long long> g_media_hit_stamp{0};

struct MediaContentPoint
{
    bool valid = false;
    float x = 0.0f; // relative to the content rect's left edge
    float y = 0.0f; // relative to the content rect's top edge
    float width = 0.0f;
    float height = 0.0f;
};

}
