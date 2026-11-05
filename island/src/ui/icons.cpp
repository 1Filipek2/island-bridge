#include "ui/icons.h"
#include "core/state.h"

namespace island
{
namespace
{


bool icon_to_pixels(HICON icon, UINT size, BitmapPixels* out_pixels) {
    if (!icon || !out_pixels || !size) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) {
        return false;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(size);
    bi.bmiHeader.biHeight = -static_cast<LONG>(size);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        return false;
    }

    // Fill with transparent black (alpha=0) so icon edges don't bleed dark fringe.
    HGDIOBJ old = SelectObject(dc, bitmap);
    RECT fill = {0, 0, static_cast<LONG>(size), static_cast<LONG>(size)};
    // Use NULL_BRUSH (transparent) then manually zero-fill the BGRA buffer after copy.
    FillRect(dc, &fill, reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH)));
    // Zero out the bits so background is fully transparent before drawing icon.
    ZeroMemory(bits, static_cast<size_t>(size) * size * 4);
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);

    BitmapPixels pixels;
    pixels.width = size;
    pixels.height = size;
    pixels.bgra.resize(static_cast<size_t>(size) * size * 4);
    memcpy(pixels.bgra.data(), bits, pixels.bgra.size());

    // Older icons can have no alpha in the color bitmap. Treat black pixels as
    // transparent only when the icon did not write any alpha at all.
    bool has_alpha = false;
    for (size_t i = 3; i < pixels.bgra.size(); i += 4) {
        if (pixels.bgra[i] != 0) {
            has_alpha = true;
            break;
        }
    }
    if (!has_alpha) {
        // No alpha channel: treat near-black as transparent, rest as opaque.
        for (size_t i = 0; i + 3 < pixels.bgra.size(); i += 4) {
            const bool black = pixels.bgra[i] < 4 && pixels.bgra[i + 1] < 4 && pixels.bgra[i + 2] < 4;
            pixels.bgra[i + 3] = black ? 0 : 255;
        }
    } else {
        // Convert to premultiplied alpha so D2D renders edges cleanly without dark fringing.
        for (size_t i = 0; i + 3 < pixels.bgra.size(); i += 4) {
            const uint8_t a = pixels.bgra[i + 3];
            if (a < 255 && a > 0) {
                pixels.bgra[i + 0] = static_cast<uint8_t>(pixels.bgra[i + 0] * a / 255);
                pixels.bgra[i + 1] = static_cast<uint8_t>(pixels.bgra[i + 1] * a / 255);
                pixels.bgra[i + 2] = static_cast<uint8_t>(pixels.bgra[i + 2] * a / 255);
            }
        }
    }

    pixels.generation = ++g_art_generation_counter;
    *out_pixels = std::move(pixels);

    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return true;
}

}



PixelsRef icon_to_pixels_ref(HICON icon, UINT size) {
    BitmapPixels px;
    return icon_to_pixels(icon, size, &px) ? PixelsRef(std::move(px)) : PixelsRef();
}

bool process_image_name_for_pid(DWORD pid, std::wstring* image_name) {
    if (!pid || !image_name) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD size = ARRAYSIZE(path);
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (ok) {
        *image_name = path;
    }
    return ok;
}


namespace
{


HICON copy_window_icon(HWND hwnd, WPARAM icon_type) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, WM_GETICON, icon_type, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &result);
    return result ? CopyIcon(reinterpret_cast<HICON>(result)) : nullptr;
}

HICON get_process_icon(DWORD pid) {
    std::wstring path;
    if (process_image_name_for_pid(pid, &path) && !path.empty()) {
        HICON h_icon = nullptr;
        UINT icon_id = 0;
        // Try to fetch a high-res 64x64 icon first to avoid pixelated icons
        using PrivateExtractIconsWT = UINT(WINAPI*)(LPCWSTR, int, int, int, HICON*, UINT*, UINT, UINT);
        static auto p_private_extract_icons_w = reinterpret_cast<PrivateExtractIconsWT>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "PrivateExtractIconsW"));
        if (p_private_extract_icons_w && p_private_extract_icons_w(path.c_str(), 0, 64, 64, &h_icon, &icon_id, 1, 0) == 1 && h_icon) {
            return h_icon;
        }

        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON)) {
            return sfi.hIcon;
        }

        HICON large = nullptr;
        HICON small_icon = nullptr;
        if (ExtractIconExW(path.c_str(), 0, &large, &small_icon, 1) > 0) {
            if (small_icon) {
                DestroyIcon(small_icon);
            }
            if (large) {
                return large;
            }
        }
    }

    return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
}

HICON get_window_icon(HWND hwnd) {
    if (!hwnd) {
        return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    }

    if (HICON icon = copy_window_icon(hwnd, ICON_BIG)) {
        return icon;
    }
    if (HICON icon = copy_window_icon(hwnd, ICON_SMALL)) {
        return icon;
    }

    if (auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON))) {
        return CopyIcon(icon);
    }
    if (auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM))) {
        return CopyIcon(icon);
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return get_process_icon(pid);
}

}



std::wstring to_lower_copy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring base_name_from_path(std::wstring path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.erase(0, slash + 1);
    }
    return path;
}

std::wstring strip_extension(std::wstring value) {
    const size_t dot = value.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        value.resize(dot);
    }
    return value;
}

bool process_image_name_for_window(HWND hwnd, std::wstring* image_name) {
    if (!image_name) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        return false;
    }

    return process_image_name_for_pid(pid, image_name);
}

std::wstring friendly_media_source_name(std::wstring_view source) {
    const std::wstring lower = to_lower_copy(std::wstring(source));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YouTube";
    if (lower.find(L"spotify") != std::wstring::npos) return L"Spotify";
    if (lower.find(L"chrome") != std::wstring::npos) return L"Chrome";
    if (lower.find(L"msedge") != std::wstring::npos || lower.find(L"edge") != std::wstring::npos) return L"Edge";
    if (lower.find(L"firefox") != std::wstring::npos) return L"Firefox";
    if (lower.find(L"vlc") != std::wstring::npos) return L"VLC";
    if (lower.find(L"wmplayer") != std::wstring::npos) return L"Windows Media";
    if (lower.find(L"zune") != std::wstring::npos || lower.find(L"media") != std::wstring::npos) return L"Media Player";

    std::wstring text(source);
    const size_t bang = text.find(L'!');
    if (bang != std::wstring::npos && bang + 1 < text.size()) {
        text.erase(0, bang + 1);
    }
    const size_t dot = text.find(L'.');
    if (dot != std::wstring::npos) {
        text.resize(dot);
    }
    return text.empty() ? L"Media" : text;
}

std::wstring media_source_badge(std::wstring_view source_name) {
    const std::wstring lower = to_lower_copy(std::wstring(source_name));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YT";
    if (lower.find(L"spotify") != std::wstring::npos) return L"SP";
    if (lower.find(L"chrome") != std::wstring::npos) return L"CH";
    if (lower.find(L"edge") != std::wstring::npos) return L"ED";
    if (lower.find(L"firefox") != std::wstring::npos) return L"FF";
    if (lower.find(L"vlc") != std::wstring::npos) return L"VLC";
    if (source_name.empty()) return L"\u25b6";
    std::wstring badge;
    badge.push_back(static_cast<wchar_t>(towupper(source_name[0])));
    return badge;
}


namespace
{


bool window_looks_like_media_source(HWND hwnd, const std::wstring& source_lower) {
    if (!IsWindowVisible(hwnd) || hwnd == g_hwnd) {
        return false;
    }

    std::wstring image;
    if (!process_image_name_for_window(hwnd, &image)) {
        return false;
    }

    const std::wstring base = to_lower_copy(base_name_from_path(image));
    if (base.empty()) {
        return false;
    }

    const std::wstring stem = strip_extension(base);
    return source_lower.find(base) != std::wstring::npos ||
           (stem.size() >= 4 && source_lower.find(stem) != std::wstring::npos) ||
           (base.find(L"chrome") != std::wstring::npos && source_lower.find(L"chrome") != std::wstring::npos) ||
           (base.find(L"msedge") != std::wstring::npos && source_lower.find(L"edge") != std::wstring::npos) ||
           (base.find(L"vlc") != std::wstring::npos && source_lower.find(L"vlc") != std::wstring::npos);
}

}



// #62: short-form video feeds change "track" every few seconds, which turned
// auto-expand into a constant popup. A blocklist entry is matched loosely
// against the friendly source name, the raw AUMID and the title, so a single
// entry like "tiktok" catches both the desktop app and a browser tab. The
// collapsed pill still updates -- only the expansion is suppressed.
//
// Reads the blocklist under g_settingsMutex rather than taking a Settings
// reference: the caller is the render loop, and walking a std::vector<std::wstring>
// that LoadSettings() may be reassigning is a use-after-free, not just a stale
// read. The haystack is built outside the lock because that part allocates.
bool media_expand_blocked(const MediaSnapshot& media) {
    {
        // Fast path for the overwhelmingly common empty-blocklist case: one
        // uncontended lock, no allocation, no string work.
        std::lock_guard lock(g_settings_mutex);
        if (g_settings.media_expand_blocklist.empty()) {
            return false;
        }
    }

    const std::wstring haystack = to_lower_copy(
        media.source_name + L"\n" + media.source_app_user_model_id + L"\n" + media.title);

    std::lock_guard lock(g_settings_mutex);
    for (const std::wstring& needle : g_settings.media_expand_blocklist) {
        if (!needle.empty() && haystack.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool is_browser_media_source(std::wstring_view source) {
    const std::wstring lower = to_lower_copy(std::wstring(source));
    return lower.find(L"chrome") != std::wstring::npos ||
           lower.find(L"edge") != std::wstring::npos ||
           lower.find(L"msedge") != std::wstring::npos ||
           lower.find(L"firefox") != std::wstring::npos ||
           lower.find(L"youtube") != std::wstring::npos;
}


namespace
{


std::wstring site_badge_from_title(std::wstring_view title) {
    const std::wstring lower = to_lower_copy(std::wstring(title));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YT";
    if (lower.find(L"netflix") != std::wstring::npos) return L"NF";
    if (lower.find(L"prime video") != std::wstring::npos || lower.find(L"amazon") != std::wstring::npos) return L"PV";
    if (lower.find(L"disney") != std::wstring::npos) return L"D+";
    if (lower.find(L"hotstar") != std::wstring::npos) return L"HS";
    if (lower.find(L"spotify") != std::wstring::npos) return L"SP";
    if (lower.find(L"soundcloud") != std::wstring::npos) return L"SC";
    return L"WEB";
}

struct IconCacheEntry {
    DWORD pid = 0;
    std::wstring exe_path;
    PixelsRef pixels;
    uint64_t last_used = 0;
};

std::mutex g_icon_cache_mutex;
std::unordered_map<DWORD, IconCacheEntry> g_icon_cache_by_pid;
uint64_t g_icon_cache_clock = 0;

PixelsRef get_cached_process_icon_pixels(DWORD pid, UINT size) {
    std::wstring exe_path;
    process_image_name_for_pid(pid, &exe_path);

    {
        std::lock_guard lock(g_icon_cache_mutex);
        auto it = g_icon_cache_by_pid.find(pid);
        if (it != g_icon_cache_by_pid.end() && it->second.exe_path == exe_path &&
            it->second.pixels->width == size) {
            it->second.last_used = ++g_icon_cache_clock;
            return it->second.pixels;
        }
    }

    PixelsRef pixels;
    HICON icon = get_process_icon(pid);
    if (icon) {
        pixels = icon_to_pixels_ref(icon, size);
        DestroyIcon(icon);
    }

    if (pixels) {
        std::lock_guard lock(g_icon_cache_mutex);
        g_icon_cache_by_pid[pid] = IconCacheEntry{pid, exe_path, pixels, ++g_icon_cache_clock};
        if (g_icon_cache_by_pid.size() > 32) {
            auto oldest = g_icon_cache_by_pid.begin();
            for (auto it = g_icon_cache_by_pid.begin(); it != g_icon_cache_by_pid.end(); ++it) {
                if (it->second.last_used < oldest->second.last_used) {
                    oldest = it;
                }
            }
            g_icon_cache_by_pid.erase(oldest);
        }
    }

    return pixels;
}

}



PixelsRef get_window_icon_pixels(HWND hwnd, UINT size) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    PixelsRef pixels = get_cached_process_icon_pixels(pid, size);
    if (pixels) {
        return pixels;
    }

    HICON icon = get_window_icon(hwnd);
    if (icon) {
        pixels = icon_to_pixels_ref(icon, size);
        DestroyIcon(icon);
    }
    return pixels;
}

bool is_ignorable_foreground_window(HWND hwnd, const std::wstring& title) {
    wchar_t class_name[128] = {};
    GetClassNameW(hwnd, class_name, ARRAYSIZE(class_name));
    const std::wstring cls = to_lower_copy(class_name);
    const std::wstring lower_title = to_lower_copy(title);

    std::wstring image;
    process_image_name_for_window(hwnd, &image);
    const std::wstring base = to_lower_copy(base_name_from_path(image));

    return title.empty() ||
           hwnd == g_hwnd ||
           cls == L"shell_traywnd" ||
           cls == L"workerw" ||
           cls == L"progman" ||
           lower_title.find(L"windhawk") != std::wstring::npos ||
           base == L"explorer.exe" ||
           base == L"windhawk.exe";
}


namespace
{


BOOL CALLBACK find_media_source_window_proc(HWND hwnd, LPARAM l_param) {
    auto* data = reinterpret_cast<std::pair<const std::wstring*, HWND*>*>(l_param);
    if (window_looks_like_media_source(hwnd, *data->first)) {
        *data->second = hwnd;
        return FALSE;
    }
    return TRUE;
}

}



std::wstring find_browser_media_site_badge(const std::wstring& source_app_user_model_id) {
    const std::wstring source_lower = to_lower_copy(source_app_user_model_id);
    HWND found = nullptr;
    std::pair<const std::wstring*, HWND*> data{&source_lower, &found};
    EnumWindows(find_media_source_window_proc, reinterpret_cast<LPARAM>(&data));
    if (found) {
        wchar_t title[192] = {};
        GetWindowTextW(found, title, ARRAYSIZE(title));
        return site_badge_from_title(title);
    }
    return L"WEB";
}

PixelsRef find_media_source_icon(const std::wstring& source_app_user_model_id) {
    PixelsRef pixels;
    if (is_browser_media_source(source_app_user_model_id)) {
        // Continue searching for the browser window anyway to retrieve
        // the browser's app icon or the PWA's app icon.
    }

    const std::wstring source_lower = to_lower_copy(source_app_user_model_id);
    HWND found = nullptr;
    std::pair<const std::wstring*, HWND*> data{&source_lower, &found};
    EnumWindows(find_media_source_window_proc, reinterpret_cast<LPARAM>(&data));
    if (found) {
        pixels = get_window_icon_pixels(found, 32);
    }
    return pixels;
}


namespace
{


struct FindAppIconData {
    const std::wstring* target_name;
    HWND best_hwnd = nullptr;
    std::unordered_map<DWORD, std::wstring> pid_to_process_name;
};

BOOL CALLBACK find_app_icon_window_proc(HWND hwnd, LPARAM l_param) {
    auto* d = reinterpret_cast<FindAppIconData*>(l_param);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    if (hwnd == g_hwnd) {
        return TRUE;
    }

    // 1. Fast path: Check window title first (lightweight check without opening process handles)
    wchar_t title[128] = {};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    std::wstring title_lower = to_lower_copy(title);
    if (!title_lower.empty() && title_lower.find(*d->target_name) != std::wstring::npos) {
        d->best_hwnd = hwnd;
        return FALSE; // Found via title, stop enumeration
    }

    // 2. Slow path fallback: Check process executable name
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        return TRUE;
    }

    std::wstring base_no_exe;
    auto it = d->pid_to_process_name.find(pid);
    if (it != d->pid_to_process_name.end()) {
        base_no_exe = it->second;
    } else {
        std::wstring image;
        if (process_image_name_for_pid(pid, &image)) {
            std::wstring base = to_lower_copy(base_name_from_path(image));
            base_no_exe = base;
            if (base_no_exe.size() > 4 && base_no_exe.substr(base_no_exe.size() - 4) == L".exe") {
                base_no_exe = base_no_exe.substr(0, base_no_exe.size() - 4);
            }
            d->pid_to_process_name[pid] = base_no_exe;
        } else {
            d->pid_to_process_name[pid] = L"";
        }
    }

    if (!base_no_exe.empty()) {
        if (base_no_exe.find(*d->target_name) != std::wstring::npos ||
            d->target_name->find(base_no_exe) != std::wstring::npos) {
            d->best_hwnd = hwnd;
            return FALSE; // Found via process name, stop enumeration
        }
    }

    return TRUE;
}

}



wchar_t notification_initial(const std::wstring& app)
{
    for (const wchar_t ch : app)
    {
        if (ch >= 0xD800 && ch <= 0xDFFF)
        {
            return 0;
        }

        if (std::iswalnum(ch))
        {
            return static_cast<wchar_t>(std::towupper(ch));
        }
    }

    return 0;
}

PixelsRef find_app_icon_by_name(const std::wstring& app_name, UINT size) {
    PixelsRef pixels;
    if (app_name.empty()) {
        return pixels;
    }

    const std::wstring app_name_lower = to_lower_copy(app_name);
    FindAppIconData data;
    data.target_name = &app_name_lower;

    EnumWindows(find_app_icon_window_proc, reinterpret_cast<LPARAM>(&data));

    if (data.best_hwnd) {
        pixels = get_window_icon_pixels(data.best_hwnd, size);
    }
    return pixels;
}

std::vector<uint8_t> read_win_rt_stream_bytes(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& reference) {
    std::vector<uint8_t> bytes;
    if (!reference) {
        return bytes;
    }

    auto stream = reference.OpenReadAsync().get();
    if (!stream) {
        return bytes;
    }

    const uint64_t size64 = stream.Size();
    if (size64 == 0 || size64 > 8 * 1024 * 1024) {
        return bytes;
    }

    const uint32_t size = static_cast<uint32_t>(size64);
    winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
    reader.LoadAsync(size).get();
    bytes.resize(size);
    reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    return bytes;
}


}
