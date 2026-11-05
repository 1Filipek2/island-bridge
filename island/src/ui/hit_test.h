#pragma once

#include "core/common.h"
#include "core/layout.h"
#include "core/model.h"
#include "notifications/notification_center.h"

namespace island
{
MediaContentPoint media_content_from_client(int client_x, int client_y);
int media_transport_hit_test(const MediaContentPoint& raw_pt);
float media_scrub_fraction_from_content(const MediaContentPoint& raw_pt);
float media_scrub_fraction_unbounded(const MediaContentPoint& raw_pt);
int file_tray_row_at_content_point(const MediaContentPoint& pt, int item_count);
bool file_tray_clear_hit_test(const MediaContentPoint& pt);
bool file_tray_paste_hit_test(const MediaContentPoint& pt);
bool file_tray_remove_hit_test(const MediaContentPoint& pt, int row);

struct NotificationCenterHit
{
    int row = -1;
    int action = -1;
};

constexpr int notification_center_back = -3;
constexpr int notification_center_clear = -2;
const notification_center::Group* open_notification_group(const SharedState& state);
NotificationCenterHit notification_center_hit_test(const MediaContentPoint& pt);
void close_notification_center_detail();
void handle_notification_center_click(const NotificationCenterHit& hit);
bool scroll_notification_center_detail(int step);
bool media_art_hit_test(const MediaContentPoint& pt);
bool media_lyrics_button_hit_test(const MediaContentPoint& raw_pt);

// Source dock
struct DockSlots
{
    int count = 0;                      // slots drawn
    int source_index[3] = {-1, -1, -1}; // index into media_sources; -1 = the "+N" slot
    int overflow = 0;                   // N in "+N" (0 = none)
};

DockSlots compute_dock_slots(int source_count, int active_index);

struct DockHit
{
    int slot = -1;
    bool overflow = false;
    bool is_active = false;
    std::wstring aumid;
};

DockHit resolve_source_dock_hit(const MediaContentPoint& pt);

}
