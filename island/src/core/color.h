#pragma once

#include "core/common.h"
namespace island
{

bool equals_no_case(std::wstring_view a, std::wstring_view b);
D2D1_COLOR_F color_from_hex(std::wstring text, D2D1_COLOR_F fallback);
double relative_luminance(D2D1_COLOR_F c);
D2D1_COLOR_F ensure_contrast_against_background(D2D1_COLOR_F candidate, D2D1_COLOR_F bg_color);

}
