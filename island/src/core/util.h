#pragma once

#include "core/common.h"
namespace island
{


constexpr GUID kSubTypeIeeeFloat = {
    0x00000003,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};
double NowSeconds();
float Clamp(float v, float lo, float hi);
int ClampInt(int v, int lo, int hi);
constexpr int kLyricsOffsetLimitMs = 3000;
void PublishLyricsOffsets(int settingMs, int settingBtMs);
double LyricsLookaheadSec();
float SmoothStep01(float t);

}
