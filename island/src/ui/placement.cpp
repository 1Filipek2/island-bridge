#include "ui/placement.h"
#include "ui/backdrop.h"
#include "core/constants.h"
#include "core/layout.h"
#include "system/monitor.h"
#include "core/util.h"

namespace island
{




RECT GetAnchorWorkRect(HMONITOR* outMonitor) {
    HMONITOR selectedMonitor = GetAnchorMonitor();
    if (outMonitor) {
        *outMonitor = selectedMonitor;
    }
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(selectedMonitor, &mi);
    return mi.rcWork;
}


namespace
{


// Vertical offset for the island's current size (#83).
//
// One Offset Y forced a compromise: a value that tucks the idle pill up near the
// screen edge leaves the expanded dashboard awkward to reach, and vice versa.
// With the separate offset enabled, Offset Y becomes the collapsed value and
// OffsetYExpanded the expanded one, and the island *eases* between them in step
// with the expansion itself rather than snapping at some threshold.
//
// windowHeight is the layered window's height, which carries kRenderPadY on both
// edges, so the pill's own height is recovered before measuring how expanded it
// is.
int ResolveOffsetY(float windowHeight) {
    if (!g_settings.separateExpandedOffsetY) {
        return g_settings.offsetY;
    }

    const float scale = std::max(0.05f, g_settings.sizeScale);
    const float pillHeight = std::max(0.0f, windowHeight - kRenderPadY * 2.0f) / scale;

    // Collapsed alert pills are ~44px tall in content space; the expanded
    // dashboard is MediaLayout::kExpandedHeight.
    constexpr float kCollapsedHeight = 44.0f;
    const float span = MediaLayout::kExpandedHeight - kCollapsedHeight;
    const float t = (span <= 1.0f) ? 0.0f
                                   : Clamp((pillHeight - kCollapsedHeight) / span, 0.0f, 1.0f);

    const float collapsed = static_cast<float>(g_settings.offsetY);
    const float expanded = static_cast<float>(g_settings.offsetYExpanded);
    return static_cast<int>(std::lround(collapsed + (expanded - collapsed) * t));
}

// Where the layered window's top-left goes for a pill `pillWidth` px wide.
// *subX receives the pill's fractional left edge (0..1) so the renderer can draw
// at the exact float position. Snapping to whole pixels made the pill's centre
// wobble ~1px while its width animated.
POINT ComputeOverlayOrigin(int height, float pillWidth, float* subX) {
    RECT work = GetAnchorWorkRect();
    const bool flushTop = g_settings.notchStyle || g_settings.borderMergedMode;
    const float workW = static_cast<float>(work.right - work.left);

    float pillLeft = work.left + (workW - pillWidth) * 0.5f;
    int y = flushTop ? work.top : (work.top + 8);

    switch (g_settings.position) {
        case Position::TopLeft:
            pillLeft = work.left + 16.0f;
            break;
        case Position::TopRight:
            pillLeft = work.right - pillWidth - 16.0f;
            break;
        case Position::BottomCenter:
            y = work.bottom - height - 40;
            break;
        case Position::BottomLeft:
            pillLeft = work.left + 16.0f;
            y = work.bottom - height - 40;
            break;
        case Position::BottomRight:
            pillLeft = work.right - pillWidth - 16.0f;
            y = work.bottom - height - 40;
            break;
        case Position::TopCenter:
        default:
            break;
    }

    pillLeft += static_cast<float>(g_settings.offsetX);
    const float windowLeft = pillLeft - kRenderPadX;
    const float windowLeftFloor = std::floor(windowLeft);
    if (subX) {
        *subX = windowLeft - windowLeftFloor;
    }

    y += ResolveOffsetY(static_cast<float>(height));
    return POINT{static_cast<LONG>(windowLeftFloor), static_cast<LONG>(y)};
}

}



StageLayout ComputeStageLayout(const Settings& settings, float pillW, float pillH) {
    StageLayout s;
    const bool flushTop = settings.notchStyle || settings.borderMergedMode;
    pillW = std::max(pillW, 0.0f);

    if (!UsesFixedStage(settings)) {
        // Legacy path, identical to what Render() did before.
        s.height = std::max(1, static_cast<int>(std::ceil(pillH + kRenderPadY * 2.0f)));
        float subX = 0.0f;
        s.origin = ComputeOverlayOrigin(s.height, pillW, &subX);
        s.width = std::max(1, static_cast<int>(std::ceil(pillW + subX + kRenderPadX * 2.0f)));
        s.pillLeft = kRenderPadX + subX;
        s.pillTop = flushTop ? 0.0f : kRenderPadY;
        return s;
    }

    const RECT work = GetAnchorWorkRect();
    const int workW = work.right - work.left;
    s.width = static_cast<int>(std::ceil(kStageContentWidth * settings.sizeScale + kRenderPadX * 2.0f));
    s.height = static_cast<int>(std::ceil(kStageContentHeight * settings.sizeScale + kRenderPadY * 2.0f));

    // Horizontal: the stage is anchored, and the pill sits inside it the same way
    // the old window-per-pill code anchored it on screen.
    switch (settings.position) {
        case Position::TopLeft:
        case Position::BottomLeft:
            s.origin.x = work.left + 16 - static_cast<int>(kRenderPadX);
            s.pillLeft = kRenderPadX;
            break;
        case Position::TopRight:
        case Position::BottomRight:
            s.origin.x = work.right - 16 + static_cast<int>(kRenderPadX) - s.width;
            s.pillLeft = static_cast<float>(s.width) - kRenderPadX - pillW;
            break;
        default:  // centered
            s.origin.x = work.left + (workW - s.width) / 2;
            s.pillLeft = (static_cast<float>(s.width) - pillW) * 0.5f;
            break;
    }
    s.origin.x += settings.offsetX;

    // Vertical. Bottom positions keep the pill's bottom edge fixed, so it grows upward.
    const int offsetY = ResolveOffsetY(pillH + kRenderPadY * 2.0f);
    if (IsBottomPosition(settings.position)) {
        s.origin.y = work.bottom - s.height - 40 + offsetY;
        s.pillTop = static_cast<float>(s.height) - kRenderPadY - pillH;
    } else {
        s.origin.y = (flushTop ? work.top : work.top + 8) + offsetY;
        s.pillTop = flushTop ? 0.0f : kRenderPadY;
    }
    return s;
}

// z-order / owner / backdrop-region upkeep only. Position and size are NOT touched
// here: UpdateLayeredWindow sets them together with the new pixels, so the window
// never moves or resizes in a separate step from its content.
void ApplyOverlayZOrder(HWND hwnd, int width, int height) {
    HWND zOrder = g_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;

    // Yield to the screen-clip overlay.
    const bool yieldToClipUi = Backdrop::ClipUiActiveCached();
    SetWindowPos(hwnd, yieldToClipUi ? nullptr : zOrder, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     (yieldToClipUi ? SWP_NOZORDER : SWP_SHOWWINDOW));
    if (!yieldToClipUi) {
        DwmBackdrop::EnsureBehind(hwnd);
    }
}


namespace
{


// Windows' own shell surfaces are all WS_EX_TOPMOST, and they are *supposed* to
// sit above the island. Raising ourselves over the Start menu, the notification
// centre, the volume OSD, Task View, a popup menu or a tooltip would be a bug,
// not a fix, so these classes are never treated as something to reclaim from.
bool IsShellOwnedWindow(HWND hwnd) {
    wchar_t className[128] = {};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) <= 0) {
        // Unknown, so treat it as shell-owned and leave the z-order alone.
        return true;
    }

    static const wchar_t* const kShellClasses[] = {
        L"Windows.UI.Core.CoreWindow",              // Start, Action Center, Search
        L"Xaml_WindowedPopupClass",                 // WinUI flyouts and popups
        L"Shell_TrayWnd",                           // taskbar
        L"Shell_SecondaryTrayWnd",                  // taskbar on other monitors
        L"TopLevelWindowForOverflowXamlIsland",     // taskbar overflow
        L"MultitaskingViewFrame",                   // Task View
        L"ForegroundStaging",                       // Task View staging
        L"#32768",                                  // popup menus
        L"tooltips_class32",                        // tooltips
        L"NarratorHelperWindow",
    };

    for (const wchar_t* candidate : kShellClasses) {
        if (_wcsicmp(className, candidate) == 0) {
            return true;
        }
    }
    return false;
}

}



// Re-asserts the island's place in the topmost band when another always-on-top
// window has been raised over it (PowerToys' bar, taskbar mods, other
// overlays). PositionOverlayWindow only runs on a resize or an explicit layout
// change, so without this the island stayed buried until the next one.
//
// Deliberately conservative: shell surfaces are ignored, a slight clip does not
// count, and we only act when we are still in the topmost band ourselves. That
// keeps this from turning into a z-order tug-of-war or from stealing focus
// surfaces away from Windows.
void EnsureTopmost(HWND hwnd, const RECT* pillScreenRect) {
    if (!g_settings.alwaysOnTop || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return;
    }
    if (Backdrop::ClipUiActiveCached()) {
        return;
    }

    // If we are not topmost at all, PositionOverlayWindow owns fixing that;
    // walking the whole z-order from a demoted position would be expensive.
    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        return;
    }

    RECT islandRect = {};
    if (pillScreenRect && !IsRectEmpty(pillScreenRect)) {
        islandRect = *pillScreenRect;  // the stage is mostly empty, so test the pill only
    } else if (!GetWindowRect(hwnd, &islandRect)) {
        return;
    }

    const long islandArea = std::max(1L, (islandRect.right - islandRect.left) *
                                             (islandRect.bottom - islandRect.top));
    // Require a real overlap, not a one-pixel graze.
    const long minOverlapArea = std::max(256L, islandArea / 8);

    bool covered = false;
    for (HWND above = GetWindow(hwnd, GW_HWNDPREV); above != nullptr;
         above = GetWindow(above, GW_HWNDPREV)) {
        if (above == hwnd) {
            continue;
        }
        // Everything above a topmost window is itself topmost, so reaching a
        // non-topmost window means we are already at the top of that band.
        if ((GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
            break;
        }
        if (!IsWindowVisible(above) || IsIconic(above) || IsShellOwnedWindow(above)) {
            continue;
        }

        RECT otherRect = {};
        RECT intersection = {};
        if (GetWindowRect(above, &otherRect) &&
            IntersectRect(&intersection, &islandRect, &otherRect)) {
            const long overlap = (intersection.right - intersection.left) *
                                 (intersection.bottom - intersection.top);
            if (overlap >= minOverlapArea) {
                covered = true;
                break;
            }
        }
    }

    if (covered) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        DwmBackdrop::EnsureBehind(hwnd);
    }
}

RECT GetIslandDockRect() {
    RECT work = GetAnchorWorkRect();
    const float scale = g_settings.sizeScale;
    const int dockWidth = static_cast<int>(std::max(280.0f, 340.0f * scale));
    const int dockHeight = static_cast<int>(std::max(48.0f, 56.0f * scale));
    const bool flushTop = g_settings.notchStyle || g_settings.borderMergedMode;

    int x = work.left + (work.right - work.left - dockWidth) / 2;
    int y = flushTop ? work.top : (work.top + 8);

    switch (g_settings.position) {
        case Position::TopLeft:
            x = work.left + 16;
            y = flushTop ? work.top : (work.top + 8);
            break;
        case Position::TopRight:
            x = work.right - dockWidth - 16;
            y = flushTop ? work.top : (work.top + 8);
            break;
        case Position::BottomCenter:
            x = work.left + (work.right - work.left - dockWidth) / 2;
            y = work.bottom - dockHeight - 40;
            break;
        case Position::BottomLeft:
            x = work.left + 16;
            y = work.bottom - dockHeight - 40;
            break;
        case Position::BottomRight:
            x = work.right - dockWidth - 16;
            y = work.bottom - dockHeight - 40;
            break;
        case Position::TopCenter:
        default:
            break;
    }

    x += g_settings.offsetX;
    // The dock rect is the hover target for a hidden island, which is always in
    // its collapsed state, so it follows the collapsed offset.
    y += g_settings.offsetY;

    RECT r;
    r.left = x;
    r.right = x + dockWidth;
    if (IsBottomPosition(g_settings.position)) {
        r.top = y;
        r.bottom = std::max(static_cast<int>(work.bottom), y + dockHeight + 20);
    } else {
        r.top = std::min(y, static_cast<int>(work.top));
        r.bottom = y + dockHeight;
    }
    return r;
}


}
