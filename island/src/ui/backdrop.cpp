#include "ui/backdrop.h"
#include "core/constants.h"
#include "services/quick_lookup.h"
#include "core/state.h"

namespace island
{
void enable_blur_behind(HWND hwnd)
{
    DWM_BLURBEHIND blur = {};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = FALSE;
    DwmEnableBlurBehindWindow(hwnd, &blur);
}

namespace backdrop
{
namespace
{
bool screen_clip_ui_active()
{
    HWND fg = GetForegroundWindow();
    if (!fg)
    {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid)
    {
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
    {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    bool hit = false;
    if (QueryFullProcessImageNameW(h, 0, path, &n))
    {
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        hit = _wcsicmp(name, L"SnippingTool.exe") == 0 || _wcsicmp(name, L"ScreenClippingHost.exe") == 0;
    }
    CloseHandle(h);
    return hit;
}

}
}

namespace backdrop
{
// The island must not raise itself over the Snipping Tool overlay. Cached for 100 ms (checked per frame).
bool clip_ui_active_cached()
{
    static std::atomic<ULONGLONG> last_check{0};
    static std::atomic<bool> last{false};
    const ULONGLONG now = GetTickCount64();
    if (now - last_check.load() >= 100)
    {
        last.store(screen_clip_ui_active());
        last_check.store(now);
    }
    return last.load();
}

}

namespace dwm_backdrop
{
namespace
{
struct AccentPolicy
{
    int accent_state;
    int accent_flags;
    DWORD gradient_color; // ABGR: alpha = tint opacity
    DWORD animation_id;
};

struct CompositionAttribData
{
    int attrib;
    PVOID data;
    SIZE_T size;
};

using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, CompositionAttribData*);

constexpr int accent_disabled = 0;
constexpr int accent_enable_blur_behind = 3;
constexpr int accent_enable_acrylic_blur_behind = 4;
constexpr int wca_accent_policy = 19;
constexpr int wca_transitions_forcedisabled = 3;
constexpr wchar_t k_class_name[] = L"DynamicIslandBackdrop";

struct Slot
{
    HWND hwnd = nullptr;
    int x = 0, y = 0, w = 0, h = 0; // last placed rect (screen px)
    int radius = -1;                // last region corner radius
    int lift = -1;                  // last region top lift (notch: flat top)
    bool visible = false;
    bool top_most = false;
    int accent_state = -1;
    DWORD accent_color = 0;
};

Slot g_slots[max_pills];
SetWindowCompositionAttribute_t g_set_wca = nullptr;
bool g_resolved = false;
bool g_supported = false; // API present and Windows 10 1803 or newer
bool g_class_registered = false;

struct SyncFingerprint
{
    HWND island = nullptr;
    POINT origin = {0, 0};
    int count = 0;
    int material = -1;
    DWORD color = 0;
    int state = -1;
    bool notch = false;
    int x[max_pills] = {};
    int y[max_pills] = {};
    int w[max_pills] = {};
    int h[max_pills] = {};
    int radius[max_pills] = {};
    int lift[max_pills] = {};
};

SyncFingerprint g_last_sync = {};

bool windows_build_at_least(DWORD build)
{
    using RtlGetVersionT = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<RtlGetVersionT>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    if (!fn)
    {
        return false;
    }
    OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    return fn(&vi) == 0 && vi.dwBuildNumber >= build;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    switch (msg)
    {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1; // accent owns the pixels; avoid GDI flash on show/resize
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

void resolve()
{
    if (g_resolved)
    {
        return;
    }
    g_resolved = true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    g_set_wca =
        user32
            ? reinterpret_cast<SetWindowCompositionAttribute_t>(GetProcAddress(user32, "SetWindowCompositionAttribute"))
            : nullptr;
    g_supported = g_set_wca && windows_build_at_least(17134); // acrylic: 1803+
}

}
}

namespace dwm_backdrop
{
bool available()
{
    resolve();
    return g_supported;
}

}

namespace dwm_backdrop
{
namespace
{
DWORD color_to_accent_abgr(const D2D1_COLOR_F& tint, float tint_alpha)
{
    auto to8 = [](float v)
    {
        return static_cast<DWORD>(std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255));
    };
    DWORD alpha = to8(tint_alpha);
    if (alpha == 0)
    {
        alpha = 1; // acrylic rejects a fully transparent tint
    }
    return (alpha << 24) | (to8(tint.b) << 16) | (to8(tint.g) << 8) | to8(tint.r);
}

void disable_transitions(HWND hwnd)
{
    BOOL disable = TRUE;
    CompositionAttribData data = {wca_transitions_forcedisabled, &disable, sizeof(disable)};
    g_set_wca(hwnd, &data);
}

void apply_accent(Slot& s, int state, DWORD color)
{
    if (!s.hwnd || !g_set_wca)
    {
        return;
    }
    if (s.accent_state == state && s.accent_color == color)
    {
        return;
    }
    AccentPolicy policy = {};
    policy.accent_state = state;
    policy.accent_flags = 0x10; // clip the accent to the window region
    policy.gradient_color = color;
    CompositionAttribData data = {wca_accent_policy, &policy, sizeof(policy)};
    g_set_wca(s.hwnd, &data);
    s.accent_state = state;
    s.accent_color = color;
}

void clear_accent(Slot& s)
{
    if (!s.hwnd || !g_set_wca)
    {
        s.accent_state = -1;
        s.accent_color = 0;
        return;
    }
    if (s.accent_state == accent_disabled)
    {
        return;
    }
    AccentPolicy policy = {};
    policy.accent_state = accent_disabled;
    CompositionAttribData data = {wca_accent_policy, &policy, sizeof(policy)};
    g_set_wca(s.hwnd, &data);
    s.accent_state = accent_disabled;
    s.accent_color = 0;
}

HWND create_backdrop_window(HWND owner)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!g_class_registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)); // black = clear for accent
        wc.lpszClassName = k_class_name;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return nullptr;
        }
        g_class_registered = true;
    }
    // No WS_EX_LAYERED (it kills the accent). Unowned on purpose: an owned popup is forced above its owner
    // on activation and would cover the island with blur.
    DWORD ex_style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    if (g_settings.always_on_top)
    {
        ex_style |= WS_EX_TOPMOST;
    }
    HWND hwnd = CreateWindowExW(ex_style, k_class_name, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst, nullptr);
    if (!hwnd)
    {
        return nullptr;
    }
    disable_transitions(hwnd);
    return hwnd;
}

void hide_slot(Slot& s)
{
    if (s.hwnd && s.visible)
    {
        clear_accent(s);
        ShowWindow(s.hwnd, SW_HIDE);
    }
    s.visible = false;
    s.accent_state = -1;
}

}
}

namespace dwm_backdrop
{
void hide_all()
{
    for (Slot& s : g_slots)
    {
        hide_slot(s);
    }
    g_last_sync = {};
}

void destroy_all()
{
    for (Slot& s : g_slots)
    {
        if (s.hwnd && IsWindow(s.hwnd))
        {
            DestroyWindow(s.hwnd);
        }
        s = Slot{};
    }
    g_last_sync = {};
    if (g_class_registered)
    {
        UnregisterClassW(k_class_name, GetModuleHandleW(nullptr));
        g_class_registered = false;
    }
}

}

namespace dwm_backdrop
{
namespace
{
void update_region(Slot& s, int w, int h, int radius, int lift)
{
    if (s.radius == radius && s.lift == lift && s.w == w && s.h == h)
    {
        return;
    }
    const int diameter = radius * 2;
    HRGN region = CreateRoundRectRgn(0, -lift, w + 1, h + 1, diameter, diameter);
    if (region && SetWindowRgn(s.hwnd, region, FALSE) == 0)
    {
        DeleteObject(region); // the system owns it only when SetWindowRgn succeeds
    }
    s.radius = radius;
    s.lift = lift;
}

void ensure_top_most(Slot& s, bool want_top_most)
{
    if (!s.hwnd || s.top_most == want_top_most)
    {
        return;
    }
    // Only SetWindowPos with HWND_TOPMOST / HWND_NOTOPMOST moves a window between the topmost bands.
    SetWindowPos(s.hwnd, want_top_most ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    s.top_most = want_top_most;
}

}
}

namespace dwm_backdrop
{
void ensure_behind(HWND island)
{
    if (!island)
    {
        return;
    }
    HWND prev = island;
    for (Slot& s : g_slots)
    {
        if (s.hwnd && s.visible)
        {
            SetWindowPos(s.hwnd, prev, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            prev = s.hwnd;
        }
    }
}

}

namespace dwm_backdrop
{
namespace
{
// Moves the window only when something changed: per-frame SetWindowPos lags on some Windows 10 builds.
void place_slot(Slot& s, HWND prev, int x, int y, int w, int h, int radius, int lift, bool want_top_most)
{
    ensure_top_most(s, want_top_most);
    update_region(s, w, h, radius, lift);
    const bool moved = x != s.x || y != s.y || w != s.w || h != s.h;
    const bool stacked = GetWindow(prev, GW_HWNDNEXT) == s.hwnd;
    if (moved || !stacked || !s.visible)
    {
        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (stacked)
        {
            flags |= SWP_NOZORDER;
        }
        if (!s.visible)
        {
            flags |= SWP_SHOWWINDOW;
        }
        SetWindowPos(s.hwnd, prev, x, y, w, h, flags);
    }
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    s.visible = true;
}

}
}

namespace dwm_backdrop
{
void sync(HWND island, POINT origin, const D2D1_RECT_F* rects, const float* radii, int count, bool notch,
          BackdropMaterial material, const D2D1_COLOR_F& tint, float tint_alpha)
{
    if (material == BackdropMaterial::None || count <= 0 || !island || !IsWindowVisible(island) || !available() ||
        backdrop::clip_ui_active_cached())
    {
        hide_all();
        return;
    }
    count = std::min(count, max_pills);

    int state = accent_enable_blur_behind;
    DWORD color = color_to_accent_abgr(tint, tint_alpha * 0.22f); // soft wash so plain blur isn't harsh
    if (material == BackdropMaterial::Acrylic)
    {
        state = accent_enable_acrylic_blur_behind;
        color = color_to_accent_abgr(tint, tint_alpha);
    }

    struct Geom
    {
        int x, y, w, h, radius, lift;
        bool valid;
    } geom[max_pills] = {};

    for (int i = 0; i < count; ++i)
    {
        const D2D1_RECT_F& r = rects[i];
        constexpr int inset = 1;
        const int x = static_cast<int>(std::lround(origin.x + r.left)) + inset;
        const int y = static_cast<int>(std::lround(origin.y + r.top)) + (notch ? 0 : inset);
        const int right = static_cast<int>(std::lround(origin.x + r.right)) - inset;
        const int bottom = static_cast<int>(std::lround(origin.y + r.bottom)) - inset;
        const int w = right - x;
        const int h = bottom - y;
        if (w < 4 || h < 4)
        {
            geom[i].valid = false;
            continue;
        }
        int radius = static_cast<int>(std::lround(radii[i])) - inset;
        radius = std::clamp(radius, 0, std::min(w, h) / 2);
        geom[i] = {x, y, w, h, radius, notch ? radius : 0, true};
    }

    const bool want_top_most = g_settings.always_on_top;
    bool unchanged = g_last_sync.island == island && g_last_sync.origin.x == origin.x &&
                     g_last_sync.origin.y == origin.y && g_last_sync.count == count &&
                     g_last_sync.material == static_cast<int>(material) && g_last_sync.color == color &&
                     g_last_sync.state == state && g_last_sync.notch == notch;
    if (unchanged && count > 0 && g_slots[0].hwnd && g_slots[0].visible)
    {
        if (GetWindow(island, GW_HWNDNEXT) != g_slots[0].hwnd)
        {
            unchanged = false;
        }
    }
    if (unchanged)
    {
        for (int i = 0; i < count; ++i)
        {
            if (!geom[i].valid)
            {
                if (g_slots[i].visible)
                {
                    unchanged = false;
                    break;
                }
                continue;
            }
            if (g_last_sync.x[i] != geom[i].x || g_last_sync.y[i] != geom[i].y || g_last_sync.w[i] != geom[i].w ||
                g_last_sync.h[i] != geom[i].h || g_last_sync.radius[i] != geom[i].radius ||
                g_last_sync.lift[i] != geom[i].lift || !g_slots[i].visible || g_slots[i].top_most != want_top_most)
            {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged)
    {
        for (int i = 0; i < count; ++i)
        {
            if (geom[i].valid && g_slots[i].hwnd)
            {
                apply_accent(g_slots[i], state, color);
            }
        }
        return;
    }

    HWND prev = island;
    for (int i = 0; i < count; ++i)
    {
        Slot& s = g_slots[i];
        if (!geom[i].valid)
        {
            hide_slot(s);
            continue;
        }
        if (!s.hwnd)
        {
            s.hwnd = create_backdrop_window(island);
            s.top_most = want_top_most;
            if (!s.hwnd)
            {
                continue;
            }
        }
        apply_accent(s, state, color);
        place_slot(s, prev, geom[i].x, geom[i].y, geom[i].w, geom[i].h, geom[i].radius, geom[i].lift, want_top_most);
        prev = s.hwnd;
    }
    for (int i = count; i < max_pills; ++i)
    {
        hide_slot(g_slots[i]);
    }

    g_last_sync.island = island;
    g_last_sync.origin = origin;
    g_last_sync.count = count;
    g_last_sync.material = static_cast<int>(material);
    g_last_sync.color = color;
    g_last_sync.state = state;
    g_last_sync.notch = notch;
    for (int i = 0; i < max_pills; ++i)
    {
        if (i < count && geom[i].valid)
        {
            g_last_sync.x[i] = geom[i].x;
            g_last_sync.y[i] = geom[i].y;
            g_last_sync.w[i] = geom[i].w;
            g_last_sync.h[i] = geom[i].h;
            g_last_sync.radius[i] = geom[i].radius;
            g_last_sync.lift[i] = geom[i].lift;
        }
        else
        {
            g_last_sync.x[i] = g_last_sync.y[i] = g_last_sync.w[i] = g_last_sync.h[i] = 0;
            g_last_sync.radius[i] = g_last_sync.lift[i] = 0;
        }
    }
}

}

void apply_backdrop_material(HWND)
{
    if (g_settings.backdrop_material == BackdropMaterial::None)
    {
        dwm_backdrop::hide_all();
    }
    g_layout_dirty = true;
}

void apply_hide_show_hotkey()
{
    apply_lookup_hotkey();
    if (!g_hwnd)
    {
        return;
    }

    if (g_hotkey_registered)
    {
        UnregisterHotKey(g_hwnd, id_hide_show_hotkey);
        g_hotkey_registered = false;
    }

    if (g_settings.hide_show_hotkey_enabled)
    {
        if (RegisterHotKey(g_hwnd, id_hide_show_hotkey, g_settings.hide_show_modifiers, g_settings.hide_show_vk))
        {
            g_registered_hotkey_modifiers = g_settings.hide_show_modifiers;
            g_registered_hotkey_vk = g_settings.hide_show_vk;
            g_hotkey_registered = true;
        }
        else
        {
            host::log(L"Failed to register hide/show hotkey (error %lu).", GetLastError());
        }
    }
}

}
