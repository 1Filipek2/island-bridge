#pragma once

#include "core/common.h"
namespace island
{

bool EqualsNoCase(std::wstring_view a, std::wstring_view b);
D2D1_COLOR_F ColorFromHex(std::wstring text, D2D1_COLOR_F fallback);
double RelativeLuminance(D2D1_COLOR_F c);
D2D1_COLOR_F EnsureContrastAgainstBackground(D2D1_COLOR_F candidate, D2D1_COLOR_F bgColor);

}
