#pragma once

#include "core/common.h"
namespace island
{

void set_click_through(HWND hwnd, bool click_through);
void handle_status_click_at_point(HWND hwnd, LPARAM l_param);
void start_focus_timer(int minutes, bool is_break);
void toggle_timer_pause();
void stop_focus_timer();
void toggle_endpoint_mute();
void seek_media_to_ticks(int64_t target_ticks);
void send_media_transport_command(int cmd);
void open_relevant_app();
void dismiss_transient_state();

}
