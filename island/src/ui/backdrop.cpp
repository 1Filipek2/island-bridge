#include "ui/backdrop.h"
#include "core/constants.h"
#include "services/quick_lookup.h"
#include "core/state.h"

namespace island
{


void EnableBlurBehind(HWND hwnd) {
    DWM_BLURBEHIND blur = {};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = FALSE;
    DwmEnableBlurBehindWindow(hwnd, &blur);
}


namespace Backdrop
{
namespace
{


// True while the foreground window belongs to the Snipping Tool / screen clip UI.
bool ScreenClipUiActive() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    bool hit = false;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        hit = _wcsicmp(name, L"SnippingTool.exe") == 0 ||
              _wcsicmp(name, L"ScreenClippingHost.exe") == 0;
    }
    CloseHandle(h);
    return hit;
}

}
}

namespace Backdrop
{


// While the Snipping Tool overlay is foreground the island must not raise or
// show itself. Cached for 100 ms since this is checked every frame.
bool ClipUiActiveCached() {
    static std::atomic<ULONGLONG> lastCheck{0};
    static std::atomic<bool> last{false};
    const ULONGLONG now = GetTickCount64();
    if (now - lastCheck.load() >= 100) {
        last.store(ScreenClipUiActive());
        lastCheck.store(now);
    }
    return last.load();
}

}

namespace DwmBackdrop
{
namespace
{


struct AccentPolicy {
    int accentState;
    int accentFlags;
    DWORD gradientColor;  // ABGR: alpha = tint opacity
    DWORD animationId;
};

struct CompositionAttribData {
    int attrib;
    PVOID data;
    SIZE_T size;
};

using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, CompositionAttribData*);

constexpr int kAccentDisabled = 0;
constexpr int kAccentEnableBlurBehind = 3;
constexpr int kAccentEnableAcrylicBlurBehind = 4;
constexpr int kWcaAccentPolicy = 19;
constexpr int kWcaTransitionsForcedisabled = 3;
constexpr wchar_t kClassName[] = L"DynamicIslandBackdrop";

struct Slot {
    HWND hwnd = nullptr;
    int x = 0, y = 0, w = 0, h = 0;  // last placed rect (screen px)
    int radius = -1;                 // last region corner radius
    int lift = -1;                   // last region top lift (notch: flat top)
    bool visible = false;
    bool topMost = false;
    int accentState = -1;
    DWORD accentColor = 0;
};

Slot g_slots[kMaxPills];
SetWindowCompositionAttribute_t g_setWca = nullptr;
bool g_resolved = false;
bool g_supported = false;  // API present and Windows 10 1803 or newer
bool g_classRegistered = false;

// Last Sync signature so idle presents that redraw content skip SetWindowPos work.
struct SyncFingerprint {
    HWND island = nullptr;
    POINT origin = {0, 0};
    int count = 0;
    int material = -1;
    DWORD color = 0;
    int state = -1;
    bool notch = false;
    int x[kMaxPills] = {};
    int y[kMaxPills] = {};
    int w[kMaxPills] = {};
    int h[kMaxPills] = {};
    int radius[kMaxPills] = {};
    int lift[kMaxPills] = {};
};
SyncFingerprint g_lastSync = {};

bool WindowsBuildAtLeast(DWORD build) {
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<RtlGetVersion_t>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    if (!fn) return false;
    OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    return fn(&vi) == 0 && vi.dwBuildNumber >= build;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // belt-and-braces with WS_EX_TRANSPARENT
        case WM_ERASEBKGND:
            return 1;  // accent owns the pixels; avoid GDI flash on show/resize
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Resolve() {
    if (g_resolved) return;
    g_resolved = true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    g_setWca = user32 ? reinterpret_cast<SetWindowCompositionAttribute_t>(
                            GetProcAddress(user32, "SetWindowCompositionAttribute"))
                      : nullptr;
    g_supported = g_setWca && WindowsBuildAtLeast(17134);  // acrylic: 1803+
}

}
}

namespace DwmBackdrop
{


// False only on Windows too old for the accent; the island then keeps its solid fill.
bool Available() {
    Resolve();
    return g_supported;
}

}

namespace DwmBackdrop
{
namespace
{


DWORD ColorToAccentAbgr(const D2D1_COLOR_F& tint, float tintAlpha) {
    auto to8 = [](float v) {
        return static_cast<DWORD>(std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255));
    };
    DWORD alpha = to8(tintAlpha);
    if (alpha == 0) alpha = 1;  // acrylic rejects a fully transparent tint
    return (alpha << 24) | (to8(tint.b) << 16) | (to8(tint.g) << 8) | to8(tint.r);
}

void DisableTransitions(HWND hwnd) {
    BOOL disable = TRUE;
    CompositionAttribData data = {kWcaTransitionsForcedisabled, &disable, sizeof(disable)};
    g_setWca(hwnd, &data);
}

void ApplyAccent(Slot& s, int state, DWORD color) {
    if (!s.hwnd || !g_setWca) return;
    if (s.accentState == state && s.accentColor == color) return;
    AccentPolicy policy = {};
    policy.accentState = state;
    policy.accentFlags = 0x10;  // clip the accent to the window region
    policy.gradientColor = color;
    CompositionAttribData data = {kWcaAccentPolicy, &policy, sizeof(policy)};
    g_setWca(s.hwnd, &data);
    s.accentState = state;
    s.accentColor = color;
}

void ClearAccent(Slot& s) {
    if (!s.hwnd || !g_setWca) {
        s.accentState = -1;
        s.accentColor = 0;
        return;
    }
    if (s.accentState == kAccentDisabled) return;
    AccentPolicy policy = {};
    policy.accentState = kAccentDisabled;
    CompositionAttribData data = {kWcaAccentPolicy, &policy, sizeof(policy)};
    g_setWca(s.hwnd, &data);
    s.accentState = kAccentDisabled;
    s.accentColor = 0;
}

HWND CreateBackdropWindow(HWND owner) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!g_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));  // black = clear for accent
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
        g_classRegistered = true;
    }
    // No WS_EX_LAYERED (it would kill the accent); WS_EX_TRANSPARENT passes the mouse.
    // Unowned (hWndParent = nullptr): In Win32, an owned popup window is strictly forced
    // above its owner in the z-order upon owner activation. If owned by the island, calling
    // SetForegroundWindow(island) for the right-click context menu (or Quick Lookup) would
    // bring the backdrop window in front of the island, completely covering the island's
    // content (text, icons, album art) with blur/acrylic. Unowned popups stay behind the island.
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    if (g_settings.alwaysOnTop) exStyle |= WS_EX_TOPMOST;
    HWND hwnd = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
                                inst, nullptr);
    if (!hwnd) return nullptr;
    DisableTransitions(hwnd);
    return hwnd;
}

void HideSlot(Slot& s) {
    if (s.hwnd && s.visible) {
        ClearAccent(s);  // drop GPU blur work while hidden
        ShowWindow(s.hwnd, SW_HIDE);
    }
    s.visible = false;
    // Force a fresh accent apply on the next show — DWM can drop it while hidden.
    s.accentState = -1;
}

}
}

namespace DwmBackdrop
{


void HideAll() {
    for (Slot& s : g_slots) HideSlot(s);
    g_lastSync = {};
}

void DestroyAll() {
    for (Slot& s : g_slots) {
        if (s.hwnd && IsWindow(s.hwnd)) {
            DestroyWindow(s.hwnd);
        }
        s = Slot{};
    }
    g_lastSync = {};
    if (g_classRegistered) {
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
        g_classRegistered = false;
    }
}

}

namespace DwmBackdrop
{
namespace
{


// Clips the window to the pill; `lift` raises the top so a notch pill gets a flat top.
void UpdateRegion(Slot& s, int w, int h, int radius, int lift) {
    if (s.radius == radius && s.lift == lift && s.w == w && s.h == h) return;
    const int diameter = radius * 2;
    HRGN region = CreateRoundRectRgn(0, -lift, w + 1, h + 1, diameter, diameter);
    if (region && SetWindowRgn(s.hwnd, region, FALSE) == 0) {
        DeleteObject(region);  // the system owns it only when SetWindowRgn succeeds
    }
    s.radius = radius;
    s.lift = lift;
}

void EnsureTopMost(Slot& s, bool wantTopMost) {
    if (!s.hwnd || s.topMost == wantTopMost) return;
    // Style-bit edits alone do not move a window between the topmost bands;
    // SetWindowPos with HWND_TOPMOST / HWND_NOTOPMOST is required.
    SetWindowPos(s.hwnd, wantTopMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    s.topMost = wantTopMost;
}

}
}

namespace DwmBackdrop
{


// Keeps all active backdrop windows placed immediately behind the island window in the z-order.
// Called around context menu popups and foreground activation so the blur never covers the island.
void EnsureBehind(HWND island) {
    if (!island) return;
    HWND prev = island;
    for (Slot& s : g_slots) {
        if (s.hwnd && s.visible) {
            SetWindowPos(s.hwnd, prev, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            prev = s.hwnd;
        }
    }
}

}

namespace DwmBackdrop
{
namespace
{


// Places one backdrop window below `prev`. Only moves it when something changed:
// per-frame SetWindowPos lags on some Windows 10 builds.
void PlaceSlot(Slot& s, HWND prev, int x, int y, int w, int h, int radius, int lift,
               bool wantTopMost) {
    EnsureTopMost(s, wantTopMost);
    UpdateRegion(s, w, h, radius, lift);
    const bool moved = x != s.x || y != s.y || w != s.w || h != s.h;
    const bool stacked = GetWindow(prev, GW_HWNDNEXT) == s.hwnd;
    if (moved || !stacked || !s.visible) {
        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (stacked) flags |= SWP_NOZORDER;
        if (!s.visible) flags |= SWP_SHOWWINDOW;
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

namespace DwmBackdrop
{


// Called after every presented frame with the island's screen origin and its pills
// (client px, including hover scale) with corner radii.
void Sync(HWND island, POINT origin, const D2D1_RECT_F* rects, const float* radii, int count,
          bool notch, BackdropMaterial material, const D2D1_COLOR_F& tint, float tintAlpha) {
    if (material == BackdropMaterial::None || count <= 0 || !island || !IsWindowVisible(island) ||
        !Available() || Backdrop::ClipUiActiveCached()) {
        HideAll();
        return;
    }
    count = std::min(count, kMaxPills);

    int state = kAccentEnableBlurBehind;
    DWORD color = ColorToAccentAbgr(tint, tintAlpha * 0.22f);  // soft wash so plain blur isn't harsh
    if (material == BackdropMaterial::Acrylic) {
        state = kAccentEnableAcrylicBlurBehind;
        color = ColorToAccentAbgr(tint, tintAlpha);
    }

    // Precompute geometry once so we can early-out when nothing moved.
    struct Geom {
        int x, y, w, h, radius, lift;
        bool valid;
    } geom[kMaxPills] = {};
    for (int i = 0; i < count; ++i) {
        const D2D1_RECT_F& r = rects[i];
        // Inset 1px so the region's hard edge stays hidden behind the island contour.
        constexpr int inset = 1;
        const int x = static_cast<int>(std::lround(origin.x + r.left)) + inset;
        const int y = static_cast<int>(std::lround(origin.y + r.top)) + (notch ? 0 : inset);
        const int right = static_cast<int>(std::lround(origin.x + r.right)) - inset;
        const int bottom = static_cast<int>(std::lround(origin.y + r.bottom)) - inset;
        const int w = right - x;
        const int h = bottom - y;
        if (w < 4 || h < 4) {
            geom[i].valid = false;
            continue;
        }
        int radius = static_cast<int>(std::lround(radii[i])) - inset;
        radius = std::clamp(radius, 0, std::min(w, h) / 2);
        geom[i] = {x, y, w, h, radius, notch ? radius : 0, true};
    }

    const bool wantTopMost = g_settings.alwaysOnTop;
    bool unchanged = g_lastSync.island == island && g_lastSync.origin.x == origin.x &&
                     g_lastSync.origin.y == origin.y && g_lastSync.count == count &&
                     g_lastSync.material == static_cast<int>(material) &&
                     g_lastSync.color == color && g_lastSync.state == state &&
                     g_lastSync.notch == notch;
    if (unchanged && count > 0 && g_slots[0].hwnd && g_slots[0].visible) {
        // If another always-on-top window slipped between the island and its backdrop, restack.
        if (GetWindow(island, GW_HWNDNEXT) != g_slots[0].hwnd) {
            unchanged = false;
        }
    }
    if (unchanged) {
        for (int i = 0; i < count; ++i) {
            if (!geom[i].valid) {
                if (g_slots[i].visible) {
                    unchanged = false;
                    break;
                }
                continue;
            }
            if (g_lastSync.x[i] != geom[i].x || g_lastSync.y[i] != geom[i].y ||
                g_lastSync.w[i] != geom[i].w || g_lastSync.h[i] != geom[i].h ||
                g_lastSync.radius[i] != geom[i].radius || g_lastSync.lift[i] != geom[i].lift ||
                !g_slots[i].visible || g_slots[i].topMost != wantTopMost) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged) {
        // Accent can still need a refresh after a settings tick that didn't move geometry.
        for (int i = 0; i < count; ++i) {
            if (geom[i].valid && g_slots[i].hwnd) {
                ApplyAccent(g_slots[i], state, color);
            }
        }
        return;
    }

    HWND prev = island;
    for (int i = 0; i < count; ++i) {
        Slot& s = g_slots[i];
        if (!geom[i].valid) {
            HideSlot(s);
            continue;
        }
        if (!s.hwnd) {
            s.hwnd = CreateBackdropWindow(island);
            s.topMost = wantTopMost;
            if (!s.hwnd) continue;
        }
        ApplyAccent(s, state, color);
        PlaceSlot(s, prev, geom[i].x, geom[i].y, geom[i].w, geom[i].h, geom[i].radius,
                  geom[i].lift, wantTopMost);
        prev = s.hwnd;
    }
    for (int i = count; i < kMaxPills; ++i) HideSlot(g_slots[i]);

    g_lastSync.island = island;
    g_lastSync.origin = origin;
    g_lastSync.count = count;
    g_lastSync.material = static_cast<int>(material);
    g_lastSync.color = color;
    g_lastSync.state = state;
    g_lastSync.notch = notch;
    for (int i = 0; i < kMaxPills; ++i) {
        if (i < count && geom[i].valid) {
            g_lastSync.x[i] = geom[i].x;
            g_lastSync.y[i] = geom[i].y;
            g_lastSync.w[i] = geom[i].w;
            g_lastSync.h[i] = geom[i].h;
            g_lastSync.radius[i] = geom[i].radius;
            g_lastSync.lift[i] = geom[i].lift;
        } else {
            g_lastSync.x[i] = g_lastSync.y[i] = g_lastSync.w[i] = g_lastSync.h[i] = 0;
            g_lastSync.radius[i] = g_lastSync.lift[i] = 0;
        }
    }
}

}



// The backdrop picks up a material or tint change on the next frame; turning it
// off hides the DWM windows immediately so the blur does not linger a frame.
void ApplyBackdropMaterial(HWND) {
    if (g_settings.backdropMaterial == BackdropMaterial::None) {
        DwmBackdrop::HideAll();
    }
    g_layoutDirty = true;
}

// Always unregisters before registering so a settings change never leaks the
// previous hotkey. Safe to call before the overlay window exists (no-ops).
void ApplyHideShowHotkey() {
    // Quick Lookup shares this entry point so both hotkeys are registered,
    // re-registered and released in the same places (render thread only).
    ApplyLookupHotkey();
    if (!g_hwnd) {
        return;
    }

    if (g_hotkeyRegistered) {
        UnregisterHotKey(g_hwnd, ID_HIDE_SHOW_HOTKEY);
        g_hotkeyRegistered = false;
    }

    if (g_settings.hideShowHotkeyEnabled) {
        if (RegisterHotKey(g_hwnd, ID_HIDE_SHOW_HOTKEY, g_settings.hideShowModifiers,
                           g_settings.hideShowVk)) {
            g_registeredHotkeyModifiers = g_settings.hideShowModifiers;
            g_registeredHotkeyVk = g_settings.hideShowVk;
            g_hotkeyRegistered = true;
        } else {
            host::log(L"Failed to register hide/show hotkey (error %lu).", GetLastError());
        }
    }
}


}
