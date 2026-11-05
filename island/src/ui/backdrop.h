#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{

void enable_blur_behind(HWND hwnd);

}

namespace island
{
namespace backdrop
{

bool clip_ui_active_cached();

}
}

namespace island
{
namespace dwm_backdrop
{

constexpr int max_pills = 2; // split / combined layouts still use one silhouette each
bool available();
void hide_all();
void destroy_all();
void ensure_behind(HWND island);
void sync(HWND island, POINT origin, const D2D1_RECT_F* rects, const float* radii, int count, bool notch,
          BackdropMaterial material, const D2D1_COLOR_F& tint, float tint_alpha);

}
}

namespace island
{

void apply_backdrop_material(HWND);
void apply_hide_show_hotkey();

}
