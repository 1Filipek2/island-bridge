#include "ui/hit_test.h"
#include "media/media_source.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
// Media dashboard hit testing (geometry published by draw_media, see layout.h)

MediaContentPoint media_content_from_client(int client_x, int client_y)
{
    MediaContentPoint out;

    // Seqlock read: all fields come from the same frame.
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f, scale = 1.0f;
    float trim = 0.0f;
    unsigned long long stamp = 0;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const unsigned before = g_media_hit_seq.load(std::memory_order_relaxed);
        if (before & 1u)
        {
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);

        left = g_media_hit_left.load(std::memory_order_relaxed);
        top = g_media_hit_top.load(std::memory_order_relaxed);
        right = g_media_hit_right.load(std::memory_order_relaxed);
        bottom = g_media_hit_bottom.load(std::memory_order_relaxed);
        scale = g_media_hit_scale.load(std::memory_order_relaxed);
        trim = g_media_hit_trim.load(std::memory_order_relaxed);
        stamp = g_media_hit_stamp.load(std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_media_hit_seq.load(std::memory_order_relaxed) == before)
        {
            break;
        }
        stamp = 0;
    }

    if (stamp == 0 || GetTickCount64() - stamp > 500)
    {
        return out;
    }

    if (scale < 0.05f)
    {
        scale = 1.0f;
    }

    const float width = right - left - std::max(0.0f, trim);
    const float height = bottom - top;
    if (width <= 1.0f || height <= 1.0f)
    {
        return out;
    }

    const float center_x = (left + right) * 0.5f;
    const float center_y = (top + bottom) * 0.5f;
    const float content_x = (static_cast<float>(client_x) - center_x) / scale + center_x;
    const float content_y = (static_cast<float>(client_y) - center_y) / scale + center_y;

    out.valid = true;
    out.x = content_x - left;
    out.y = content_y - top;
    out.width = width;
    out.height = height;
    return out;
}

namespace
{
// Maps a content point into the transport block's design space (the immersive view draws it smaller).
MediaContentPoint media_player_zone_point(const MediaContentPoint& pt)
{
    const float mix = g_immersive_mix_pub.load(std::memory_order_relaxed);
    if (!pt.valid || mix <= 0.001f)
    {
        return pt;
    }
    const float s = 1.0f + (immersive_layout::player_scale - 1.0f) * mix;
    const float cx = pt.width * 0.5f;
    MediaContentPoint out = pt;
    out.x = cx + (pt.x - cx) / s;
    out.y = immersive_layout::player_pivot_y + (pt.y - immersive_layout::player_pivot_y) / s;
    return out;
}

}

// Returns 0 = previous, 1 = play/pause, 2 = next, or -1 for no button.
int media_transport_hit_test(const MediaContentPoint& raw_pt)
{
    const MediaContentPoint pt = media_player_zone_point(raw_pt);
    if (!pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return -1;
    }

    const float center_x = pt.width * 0.5f;
    const float dy = pt.y - media_layout::controls_y;

    struct Target
    {
        float offset_x;
        float radius;
        int command;
    };

    const Target targets[] = {
        {-media_layout::control_spacing, media_layout::nav_button_radius, 0},
        {0.0f, media_layout::play_button_radius, 1},
        {media_layout::control_spacing, media_layout::nav_button_radius, 2},
    };

    for (const Target& t : targets)
    {
        const float reach = t.radius + media_layout::control_hit_pad;
        if (std::fabs(dy) <= reach && std::fabs(pt.x - (center_x + t.offset_x)) <= reach)
        {
            return t.command;
        }
    }
    return -1;
}

float media_scrub_fraction_from_content(const MediaContentPoint& raw_pt)
{
    const MediaContentPoint pt = media_player_zone_point(raw_pt);
    if (!pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return -1.0f;
    }

    const float bar_left = media_layout::scrub_inset_left;
    const float bar_right = pt.width - media_layout::scrub_inset_right;
    if (bar_right - bar_left <= 1.0f)
    {
        return -1.0f;
    }

    if (std::fabs(pt.y - media_layout::scrubber_y) > media_layout::scrub_hit_half_height ||
        pt.x < bar_left - media_layout::scrub_hit_pad_x || pt.x > bar_right + media_layout::scrub_hit_pad_x)
    {
        return -1.0f;
    }

    return clamp((clamp(pt.x, bar_left, bar_right) - bar_left) / (bar_right - bar_left), 0.0f, 1.0f);
}

float media_scrub_fraction_unbounded(const MediaContentPoint& raw_pt)
{
    const MediaContentPoint pt = media_player_zone_point(raw_pt);
    if (!pt.valid)
    {
        return -1.0f;
    }
    const float bar_left = media_layout::scrub_inset_left;
    const float bar_right = pt.width - media_layout::scrub_inset_right;
    if (bar_right - bar_left <= 1.0f)
    {
        return -1.0f;
    }
    return clamp((clamp(pt.x, bar_left, bar_right) - bar_left) / (bar_right - bar_left), 0.0f, 1.0f);
}

int file_tray_row_at_content_point(const MediaContentPoint& pt, int item_count)
{
    if (!pt.valid || item_count <= 0 || pt.height <= media_layout::expanded_min_height)
    {
        return -1;
    }
    if (pt.x < file_tray_layout::pad_x || pt.x > pt.width - file_tray_layout::pad_x)
    {
        return -1;
    }

    const float list_bottom = pt.height - file_tray_layout::list_bottom_inset;
    if (pt.y < file_tray_layout::list_top || pt.y > list_bottom)
    {
        return -1;
    }

    const float stride = file_tray_layout::row_height + file_tray_layout::row_gap;
    const int index = static_cast<int>((pt.y - file_tray_layout::list_top) / stride);
    const float row_top = file_tray_layout::list_top + index * stride;
    if (pt.y > row_top + file_tray_layout::row_height)
    {
        return -1;
    }

    const int capacity = file_tray_layout::visible_row_capacity(pt.height);
    if (index < 0 || index >= capacity || index >= item_count)
    {
        return -1;
    }
    return index;
}

bool file_tray_clear_hit_test(const MediaContentPoint& pt)
{
    if (!pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return false;
    }
    constexpr float row_slop = 3.0f;
    const float right = pt.width - file_tray_layout::pad_x;
    const float left = right - file_tray_layout::clear_btn_size;
    const float top = file_tray_layout::clear_btn_top;
    const float bottom = top + file_tray_layout::clear_btn_size;
    return pt.x >= left - row_slop && pt.x <= right + row_slop && pt.y >= top - row_slop && pt.y <= bottom + row_slop;
}

bool file_tray_paste_hit_test(const MediaContentPoint& pt)
{
    if (!pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return false;
    }
    constexpr float row_slop = 3.0f;
    const float right =
        pt.width - file_tray_layout::pad_x - file_tray_layout::clear_btn_size - file_tray_layout::paste_btn_gap;
    const float left = right - file_tray_layout::clear_btn_size;
    const float top = file_tray_layout::clear_btn_top;
    const float bottom = top + file_tray_layout::clear_btn_size;
    return pt.x >= left - row_slop && pt.x <= right + row_slop && pt.y >= top - row_slop && pt.y <= bottom + row_slop;
}

bool file_tray_remove_hit_test(const MediaContentPoint& pt, int row)
{
    if (!pt.valid || row < 0 || pt.height <= media_layout::expanded_min_height)
    {
        return false;
    }
    constexpr float row_slop = 3.0f;
    const float half = file_tray_layout::remove_btn_size * 0.5f + row_slop;
    const float center_x = pt.width - file_tray_layout::pad_x - file_tray_layout::remove_btn_margin -
                           file_tray_layout::remove_btn_size * 0.5f;
    const float center_y = file_tray_layout::list_top +
                           static_cast<float>(row) * (file_tray_layout::row_height + file_tray_layout::row_gap) +
                           file_tray_layout::row_height * 0.5f;
    return std::fabs(pt.x - center_x) <= half && std::fabs(pt.y - center_y) <= half;
}

const notification_center::Group* open_notification_group(const SharedState& state)
{
    if (!state.notification_center_view.open() || !state.notification_center)
    {
        return nullptr;
    }
    return state.notification_center->find(state.notification_center_view.key);
}

NotificationCenterHit notification_center_hit_test(const MediaContentPoint& pt)
{
    NotificationCenterHit hit;
    if (!pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return hit;
    }
    std::lock_guard lock(g_state_mutex);
    if (open_notification_group(g_state))
    {
        if (notification_center::clear_hit(pt.x, pt.y, pt.width))
        {
            hit.action = notification_center_clear;
        }
        else if (notification_center::back_hit(pt.x, pt.y, pt.width))
        {
            hit.action = notification_center_back;
        }
        return hit;
    }
    const int group_count =
        g_state.notification_center ? static_cast<int>(g_state.notification_center->groups().size()) : 0;
    hit.row = notification_center::row_at(pt.x, pt.y, pt.width, pt.height, group_count);
    if (group_count > 0 && notification_center::clear_hit(pt.x, pt.y, pt.width))
    {
        hit.action = notification_center_clear;
    }
    else if (notification_center::remove_hit(pt.x, pt.y, pt.width, hit.row))
    {
        hit.action = hit.row;
    }
    return hit;
}

namespace
{
void set_notification_center_view(notification_center::View view)
{
    g_notification_center_detail_open.store(view.open(), std::memory_order_relaxed);
    g_state.notification_center_view = std::move(view);
}

}

void close_notification_center_detail()
{
    if (!g_notification_center_detail_open.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard lock(g_state_mutex);
    set_notification_center_view({});
}

void handle_notification_center_click(const NotificationCenterHit& hit)
{
    std::lock_guard lock(g_state_mutex);
    const notification_center::History* history = g_state.notification_center.get();
    if (!history)
    {
        set_notification_center_view({});
        return;
    }

    if (const notification_center::Group* open = open_notification_group(g_state))
    {
        if (hit.action == notification_center_clear)
        {
            auto next = std::make_shared<notification_center::History>(*history);
            next->remove(open->key);
            g_state.notification_center = std::move(next);
        }
        if (hit.action == notification_center_clear || hit.action == notification_center_back)
        {
            set_notification_center_view({});
        }
        return;
    }

    if (hit.action == notification_center_clear)
    {
        g_state.notification_center.reset();
        set_notification_center_view({});
    }
    else if (hit.action >= 0)
    {
        auto next = std::make_shared<notification_center::History>(*history);
        if (next->remove(static_cast<std::size_t>(hit.action)))
        {
            g_state.notification_center = std::move(next);
        }
    }
    else if (hit.row >= 0 && static_cast<std::size_t>(hit.row) < history->groups().size())
    {
        set_notification_center_view(
            notification_center::View{history->groups()[static_cast<std::size_t>(hit.row)].key, 0});
    }
}

bool scroll_notification_center_detail(int step)
{
    if (!g_notification_center_detail_open.load(std::memory_order_relaxed))
    {
        return false;
    }
    std::lock_guard lock(g_state_mutex);
    const notification_center::Group* open = open_notification_group(g_state);
    if (!open)
    {
        return false;
    }
    notification_center::View& view = g_state.notification_center_view;
    const std::size_t next = notification_center::scrolled(
        view.offset, step, open->entries.size(), g_notification_center_detail_rows.load(std::memory_order_relaxed));
    if (next != view.offset)
    {
        view.offset = next;
        g_layout_dirty = true;
    }
    return true;
}

bool media_art_hit_test(const MediaContentPoint& pt)
{
    if (!pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return false;
    }
    if (g_immersive_mix_pub.load(std::memory_order_relaxed) > 0.5f)
    {
        return pt.x >= immersive_layout::art_left && pt.x <= immersive_layout::art_left + immersive_layout::art_size &&
               pt.y >= immersive_layout::art_top && pt.y <= immersive_layout::art_top + immersive_layout::art_size;
    }
    return pt.x >= media_layout::art_inset_x && pt.x <= media_layout::art_inset_x + media_layout::art_size &&
           pt.y >= media_layout::art_inset_y && pt.y <= media_layout::art_inset_y + media_layout::art_size;
}

bool media_lyrics_button_hit_test(const MediaContentPoint& raw_pt)
{
    const MediaContentPoint pt = media_player_zone_point(raw_pt);
    if (!pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return false;
    }
    const float cx = pt.width - immersive_layout::btn_inset_right - immersive_layout::btn_radius;
    const float reach = immersive_layout::btn_radius + immersive_layout::btn_hit_pad;
    return std::fabs(pt.x - cx) <= reach && std::fabs(pt.y - media_layout::controls_y) <= reach;
}

DockSlots compute_dock_slots(int source_count, int active_index)
{
    DockSlots d;
    if (source_count < 2)
    {
        return d;
    }
    if (source_count <= media_layout::dock_max_slots)
    {
        d.count = source_count;
        for (int i = 0; i < source_count; ++i)
        {
            d.source_index[i] = i;
        }
    }
    else
    {
        d.count = 3;
        d.source_index[0] = 0;
        d.source_index[1] = active_index >= 2 ? active_index : 1;
        d.source_index[2] = -1;
        d.overflow = source_count - 2;
    }
    return d;
}

namespace
{
// Boundaries sit in the middle of the gaps, so the slots tile the pill with no dead space.
int media_source_dock_hit_test(const MediaContentPoint& raw_pt, int slot_count)
{
    const MediaContentPoint pt = media_player_zone_point(raw_pt);
    if (slot_count < 2 || !pt.valid || pt.height <= media_layout::expanded_min_height)
    {
        return -1;
    }
    const float half_h = std::max(media_layout::dock_hit_min, media_layout::dock_height) * 0.5f;
    if (std::fabs(pt.y - media_layout::controls_y) > half_h)
    {
        return -1;
    }

    const float left = media_layout::dock_left;
    const float right = left + media_layout::dock_width(slot_count);
    if (pt.x < left || pt.x > right)
    {
        return -1;
    }

    const float stride = media_layout::dock_slot + media_layout::dock_gap;
    const float origin = left + media_layout::dock_pad - media_layout::dock_gap * 0.5f;
    const int index = static_cast<int>(std::floor((pt.x - origin) / stride));
    return clamp_int(index, 0, slot_count - 1);
}

}

DockHit resolve_source_dock_hit(const MediaContentPoint& pt)
{
    DockHit hit;
    std::lock_guard lock(g_state_mutex);
    const int count = static_cast<int>(g_state.media_sources.size());
    const int active = active_media_source_index(g_state);
    const DockSlots slots = compute_dock_slots(count, active);
    const int slot = media_source_dock_hit_test(pt, slots.count);
    if (slot < 0)
    {
        return hit;
    }

    hit.slot = slot;
    const int idx = slots.source_index[slot];
    if (idx < 0)
    {
        hit.overflow = true;
        return hit;
    }
    hit.aumid = g_state.media_sources[idx].aumid;
    hit.is_active = (idx == active);
    return hit;
}

}
