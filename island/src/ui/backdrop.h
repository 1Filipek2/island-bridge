#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{

void EnableBlurBehind(HWND hwnd);

}

namespace island
{
namespace Backdrop
{

bool ClipUiActiveCached();

}
}

namespace island
{
namespace DwmBackdrop
{

constexpr int kMaxPills = 2;  // split / combined layouts still use one silhouette each
bool Available();
void HideAll();
void DestroyAll();
void EnsureBehind(HWND island);
void Sync(HWND island, POINT origin, const D2D1_RECT_F* rects, const float* radii, int count,
          bool notch, BackdropMaterial material, const D2D1_COLOR_F& tint, float tintAlpha);

}
}

namespace island
{

void ApplyBackdropMaterial(HWND);
void ApplyHideShowHotkey();

}
