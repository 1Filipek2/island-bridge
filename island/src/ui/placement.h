#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{

RECT GetAnchorWorkRect(HMONITOR* outMonitor = nullptr);

// ── Fixed stage ──────────────────────────────────────────────────────────────
// The layered window is one fixed size (the largest pill the island can draw) and
// the pill animates inside it. The window never resizes or moves while animating.
// Split layouts (two alert pills) are the widest case, Quick Lookup the tallest.
constexpr float kStageContentWidth = 880.0f;   // content px, multiplied by sizeScale
constexpr float kStageContentHeight = 320.0f;

// Set to false to force a full repaint every frame (useful if you ever see ghosting).
constexpr bool kUseDirtyRect = true;

struct StageLayout {
    POINT origin = {0, 0};   // screen position of the window
    int width = 0;           // window size in px
    int height = 0;
    float pillLeft = 0.0f;   // pill's left edge inside the window (fractional)
    float pillTop = 0.0f;    // pill's top edge inside the window, before the nudge
};

// Where the pill was last drawn, in client px. Written by Renderer::Render and read
// by WM_NCHITTEST. Both run on the render thread, so no lock is needed.
struct PillClientRect {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    bool valid = false;
};
inline PillClientRect g_pillClientRect;

// The island always uses the fixed stage; the backdrop is painted by separate DWM
// windows (see DwmBackdrop) and does not depend on the window size.
inline bool UsesFixedStage(const Settings&) {
    return true;
}
StageLayout ComputeStageLayout(const Settings& settings, float pillW, float pillH);
void ApplyOverlayZOrder(HWND hwnd, int width, int height);
void EnsureTopmost(HWND hwnd, const RECT* pillScreenRect = nullptr);
RECT GetIslandDockRect();

}
