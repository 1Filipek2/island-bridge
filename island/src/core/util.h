#pragma once

#include "core/common.h"
namespace island
{


constexpr GUID sub_type_ieee_float = {
    0x00000003,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};
double now_seconds();
float clamp(float v, float lo, float hi);
int clamp_int(int v, int lo, int hi);
constexpr int lyrics_offset_limit_ms = 3000;
void publish_lyrics_offsets(int setting_ms, int setting_bt_ms);
double lyrics_lookahead_sec();
float smooth_step01(float t);

}
