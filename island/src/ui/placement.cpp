#include "ui/placement.h"
#include "ui/backdrop.h"
#include "core/constants.h"
#include "core/layout.h"
#include "system/monitor.h"
#include "core/util.h"

namespace island
{
RECT get_anchor_work_rect(HMONITOR* out_monitor)
{
    HMONITOR selected_monitor = get_anchor_monitor();
    if (out_monitor)
    {
        *out_monitor = selected_monitor;
    }
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(selected_monitor, &mi);
    return mi.rcWork;
}

namespace
{
// Vertical offset: with the separate expanded offset on, the island eases between the collapsed and
// the expanded value together with the expansion.
int resolve_offset_y(float window_height)
{
    if (!g_settings.separate_expanded_offset_y)
    {
        return g_settings.offset_y;
    }

    const float scale = std::max(0.05f, g_settings.size_scale);
    const float pill_height = std::max(0.0f, window_height - render_pad_y * 2.0f) / scale;

    constexpr float collapsed_height = 44.0f;
    const float span = media_layout::expanded_height - collapsed_height;
    const float t = (span <= 1.0f) ? 0.0f : clamp((pill_height - collapsed_height) / span, 0.0f, 1.0f);

    const float collapsed = static_cast<float>(g_settings.offset_y);
    const float expanded = static_cast<float>(g_settings.offset_y_expanded);
    return static_cast<int>(std::lround(collapsed + (expanded - collapsed) * t));
}

// *sub_x receives the fractional left edge so the pill is drawn at the exact float position (whole
// pixels made it wobble while the width animated).
POINT compute_overlay_origin(int height, float pill_width, float* sub_x)
{
    RECT work = get_anchor_work_rect();
    const bool flush_top = g_settings.notch_style || g_settings.border_merged_mode;
    const float work_w = static_cast<float>(work.right - work.left);

    float pill_left = work.left + (work_w - pill_width) * 0.5f;
    int y = flush_top ? work.top : (work.top + 8);

    switch (g_settings.position)
    {
        case Position::TopLeft:
            pill_left = work.left + 16.0f;
            break;
        case Position::TopRight:
            pill_left = work.right - pill_width - 16.0f;
            break;
        case Position::BottomCenter:
            y = work.bottom - height - 40;
            break;
        case Position::BottomLeft:
            pill_left = work.left + 16.0f;
            y = work.bottom - height - 40;
            break;
        case Position::BottomRight:
            pill_left = work.right - pill_width - 16.0f;
            y = work.bottom - height - 40;
            break;
        case Position::TopCenter:
        default:
            break;
    }

    pill_left += static_cast<float>(g_settings.offset_x);
    const float window_left = pill_left - render_pad_x;
    const float window_left_floor = std::floor(window_left);
    if (sub_x)
    {
        *sub_x = window_left - window_left_floor;
    }

    y += resolve_offset_y(static_cast<float>(height));
    return POINT{static_cast<LONG>(window_left_floor), static_cast<LONG>(y)};
}

}

StageLayout compute_stage_layout(const Settings& settings, float pill_w, float pill_h)
{
    StageLayout s;
    const bool flush_top = settings.notch_style || settings.border_merged_mode;
    pill_w = std::max(pill_w, 0.0f);

    if (!uses_fixed_stage(settings))
    {
        s.height = std::max(1, static_cast<int>(std::ceil(pill_h + render_pad_y * 2.0f)));
        float sub_x = 0.0f;
        s.origin = compute_overlay_origin(s.height, pill_w, &sub_x);
        s.width = std::max(1, static_cast<int>(std::ceil(pill_w + sub_x + render_pad_x * 2.0f)));
        s.pill_left = render_pad_x + sub_x;
        s.pill_top = flush_top ? 0.0f : render_pad_y;
        return s;
    }

    const RECT work = get_anchor_work_rect();
    const int work_w = work.right - work.left;
    s.width = static_cast<int>(std::ceil(stage_content_width * settings.size_scale + render_pad_x * 2.0f));
    s.height = static_cast<int>(std::ceil(stage_content_height * settings.size_scale + render_pad_y * 2.0f));

    switch (settings.position)
    {
        case Position::TopLeft:
        case Position::BottomLeft:
            s.origin.x = work.left + 16 - static_cast<int>(render_pad_x);
            s.pill_left = render_pad_x;
            break;
        case Position::TopRight:
        case Position::BottomRight:
            s.origin.x = work.right - 16 + static_cast<int>(render_pad_x) - s.width;
            s.pill_left = static_cast<float>(s.width) - render_pad_x - pill_w;
            break;
        default:
            s.origin.x = work.left + (work_w - s.width) / 2;
            s.pill_left = (static_cast<float>(s.width) - pill_w) * 0.5f;
            break;
    }
    s.origin.x += settings.offset_x;

    const int offset_y = resolve_offset_y(pill_h + render_pad_y * 2.0f);
    if (is_bottom_position(settings.position))
    {
        s.origin.y = work.bottom - s.height - 40 + offset_y;
        s.pill_top = static_cast<float>(s.height) - render_pad_y - pill_h;
    }
    else
    {
        s.origin.y = (flush_top ? work.top : work.top + 8) + offset_y;
        s.pill_top = flush_top ? 0.0f : render_pad_y;
    }
    return s;
}

// Z-order and owner upkeep only: UpdateLayeredWindow sets position and size together with the pixels.
void apply_overlay_z_order(HWND hwnd, int width, int height)
{
    HWND z_order = g_settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST;

    const bool yield_to_clip_ui = backdrop::clip_ui_active_cached();
    SetWindowPos(hwnd, yield_to_clip_ui ? nullptr : z_order, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     (yield_to_clip_ui ? SWP_NOZORDER : SWP_SHOWWINDOW));
    if (!yield_to_clip_ui)
    {
        dwm_backdrop::ensure_behind(hwnd);
    }
}

namespace
{
// Shell surfaces (Start, flyouts, OSDs, Task View, menus, tooltips) are meant to sit above the island.
bool is_shell_owned_window(HWND hwnd)
{
    wchar_t class_name[128] = {};
    if (GetClassNameW(hwnd, class_name, ARRAYSIZE(class_name)) <= 0)
    {
        return true;
    }

    static const wchar_t* const shell_classes[] = {
        L"Windows.UI.Core.CoreWindow",          // Start, Action Center, Search
        L"Xaml_WindowedPopupClass",             // WinUI flyouts and popups
        L"Shell_TrayWnd",                       // taskbar
        L"Shell_SecondaryTrayWnd",              // taskbar on other monitors
        L"TopLevelWindowForOverflowXamlIsland", // taskbar overflow
        L"MultitaskingViewFrame",               // Task View
        L"ForegroundStaging",                   // Task View staging
        L"#32768",                              // popup menus
        L"tooltips_class32",                    // tooltips
        L"NarratorHelperWindow",
    };

    for (const wchar_t* candidate : shell_classes)
    {
        if (_wcsicmp(class_name, candidate) == 0)
        {
            return true;
        }
    }
    return false;
}

}

// Re-asserts the topmost band when another always-on-top window covers the island. Conservative:
// shell surfaces are ignored, small overlaps do not count, and nothing happens unless the island is
// still topmost itself.
void ensure_topmost(HWND hwnd, const RECT* pill_screen_rect)
{
    if (!g_settings.always_on_top || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
    {
        return;
    }
    if (backdrop::clip_ui_active_cached())
    {
        return;
    }

    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
    {
        return;
    }

    RECT island_rect = {};
    if (pill_screen_rect && !IsRectEmpty(pill_screen_rect))
    {
        island_rect = *pill_screen_rect;
    }
    else if (!GetWindowRect(hwnd, &island_rect))
    {
        return;
    }

    const long island_area =
        std::max(1L, (island_rect.right - island_rect.left) * (island_rect.bottom - island_rect.top));
    const long min_overlap_area = std::max(256L, island_area / 8);

    bool covered = false;
    for (HWND above = GetWindow(hwnd, GW_HWNDPREV); above != nullptr; above = GetWindow(above, GW_HWNDPREV))
    {
        if (above == hwnd)
        {
            continue;
        }
        if ((GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
        {
            break;
        }
        if (!IsWindowVisible(above) || IsIconic(above) || is_shell_owned_window(above))
        {
            continue;
        }

        RECT other_rect = {};
        RECT intersection = {};
        if (GetWindowRect(above, &other_rect) && IntersectRect(&intersection, &island_rect, &other_rect))
        {
            const long overlap = (intersection.right - intersection.left) * (intersection.bottom - intersection.top);
            if (overlap >= min_overlap_area)
            {
                covered = true;
                break;
            }
        }
    }

    if (covered)
    {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        dwm_backdrop::ensure_behind(hwnd);
    }
}

RECT get_island_dock_rect()
{
    RECT work = get_anchor_work_rect();
    const float scale = g_settings.size_scale;
    const int dock_width = static_cast<int>(std::max(280.0f, 340.0f * scale));
    const int dock_height = static_cast<int>(std::max(48.0f, 56.0f * scale));
    const bool flush_top = g_settings.notch_style || g_settings.border_merged_mode;

    int x = work.left + (work.right - work.left - dock_width) / 2;
    int y = flush_top ? work.top : (work.top + 8);

    switch (g_settings.position)
    {
        case Position::TopLeft:
            x = work.left + 16;
            y = flush_top ? work.top : (work.top + 8);
            break;
        case Position::TopRight:
            x = work.right - dock_width - 16;
            y = flush_top ? work.top : (work.top + 8);
            break;
        case Position::BottomCenter:
            x = work.left + (work.right - work.left - dock_width) / 2;
            y = work.bottom - dock_height - 40;
            break;
        case Position::BottomLeft:
            x = work.left + 16;
            y = work.bottom - dock_height - 40;
            break;
        case Position::BottomRight:
            x = work.right - dock_width - 16;
            y = work.bottom - dock_height - 40;
            break;
        case Position::TopCenter:
        default:
            break;
    }

    x += g_settings.offset_x;
    y += g_settings.offset_y;

    RECT r;
    r.left = x;
    r.right = x + dock_width;
    if (is_bottom_position(g_settings.position))
    {
        r.top = y;
        r.bottom = std::max(static_cast<int>(work.bottom), y + dock_height + 20);
    }
    else
    {
        r.top = std::min(y, static_cast<int>(work.top));
        r.bottom = y + dock_height;
    }
    return r;
}

}
