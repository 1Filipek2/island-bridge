#include "notifications/capture.h"
#include "core/constants.h"
#include "ui/icons.h"
#include "core/model.h"
#include "core/pixels.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
std::wstring read_clipboard_text(HWND hwnd)
{
    std::wstring text;
    if (!OpenClipboard(hwnd))
    {
        return text;
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data)
    {
        auto* locked = static_cast<const wchar_t*>(GlobalLock(data));
        if (locked)
        {
            text = locked;
            GlobalUnlock(data);
        }
    }

    CloseClipboard();
    return text;
}

namespace
{
// Clipboard bitmap -> small BGRA thumbnail (aspect kept, longer side capped). CF_BITMAP is synthesized
// by Windows from CF_DIB/CF_DIBV5.
bool read_clipboard_image_pixels(HWND hwnd, BitmapPixels* out_pixels, UINT max_dim)
{
    if (!out_pixels || !OpenClipboard(hwnd))
    {
        return false;
    }

    HBITMAP source_bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (!source_bitmap)
    {
        CloseClipboard();
        return false;
    }

    BITMAP bm = {};
    if (!GetObject(source_bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
    {
        CloseClipboard();
        return false;
    }

    UINT src_w = static_cast<UINT>(bm.bmWidth);
    UINT src_h = static_cast<UINT>(bm.bmHeight);
    UINT dst_w = src_w;
    UINT dst_h = src_h;
    if (src_w > max_dim || src_h > max_dim)
    {
        const float scale = static_cast<float>(max_dim) / static_cast<float>(std::max(src_w, src_h));
        dst_w = std::max<UINT>(1, static_cast<UINT>(src_w * scale));
        dst_h = std::max<UINT>(1, static_cast<UINT>(src_h * scale));
    }

    HDC screen = GetDC(nullptr);
    HDC src_dc = CreateCompatibleDC(screen);
    HDC dst_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!src_dc || !dst_dc)
    {
        if (src_dc)
        {
            DeleteDC(src_dc);
        }
        if (dst_dc)
        {
            DeleteDC(dst_dc);
        }
        CloseClipboard();
        return false;
    }

    HGDIOBJ old_src = SelectObject(src_dc, source_bitmap);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(dst_w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(dst_h); // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dst_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
    {
        SelectObject(src_dc, old_src);
        DeleteDC(src_dc);
        DeleteDC(dst_dc);
        CloseClipboard();
        return false;
    }

    HGDIOBJ old_dst = SelectObject(dst_dc, dib);
    SetStretchBltMode(dst_dc, HALFTONE);
    SetBrushOrgEx(dst_dc, 0, 0, nullptr);
    StretchBlt(dst_dc, 0, 0, static_cast<int>(dst_w), static_cast<int>(dst_h), src_dc, 0, 0, bm.bmWidth, bm.bmHeight,
               SRCCOPY);

    BitmapPixels pixels;
    pixels.width = dst_w;
    pixels.height = dst_h;
    pixels.bgra.resize(static_cast<size_t>(dst_w) * dst_h * 4);
    memcpy(pixels.bgra.data(), bits, pixels.bgra.size());

    // GDI blits leave alpha at 0; D2D expects premultiplied alpha, so set it.
    for (size_t i = 3; i < pixels.bgra.size(); i += 4)
    {
        pixels.bgra[i] = 255;
    }

    pixels.generation = ++g_art_generation_counter;
    *out_pixels = std::move(pixels);

    SelectObject(dst_dc, old_dst);
    DeleteObject(dib);
    SelectObject(src_dc, old_src);
    DeleteDC(src_dc);
    DeleteDC(dst_dc);
    CloseClipboard();
    return true;
}

bool is_likely_toast_window(HWND hwnd, const wchar_t* class_name, const wchar_t* title)
{
    if (hwnd == g_hwnd || !hwnd)
    {
        return false;
    }

    if (GetWindow(hwnd, GW_OWNER))
    {
        return false;
    }

    std::wstring image;
    if (process_image_name_for_window(hwnd, &image))
    {
        const std::wstring base = to_lower_copy(base_name_from_path(image));
        if (base == L"powershell.exe" || base == L"pwsh.exe" || base == L"conhost.exe" || base == L"cmd.exe")
        {
            return false;
        }
    }

    const std::wstring cls = to_lower_copy(class_name ? class_name : L"");
    const std::wstring text = to_lower_copy(title ? title : L"");

    if (cls.find(L"notification") != std::wstring::npos || cls.find(L"toast") != std::wstring::npos ||
        cls.find(L"windows.ui.notifications") != std::wstring::npos)
    {
        return true;
    }

    // Windows 11 toasts use generic XAML/CoreWindow classes and often have no title when created.
    if (cls.find(L"xaml_windowedpopupclass") != std::wstring::npos ||
        cls.find(L"windows.ui.core.corewindow") != std::wstring::npos)
    {
        std::wstring image;
        if (process_image_name_for_window(hwnd, &image))
        {
            const std::wstring base = to_lower_copy(base_name_from_path(image));
            if (base == L"explorer.exe" || base == L"sihost.exe" || base == L"shellexperiencehost.exe")
            {
                if (text != L"start" && text != L"action center" && text != L"search" && text != L"task view")
                {
                    return true;
                }
            }
        }
    }

    return false;
}

}

void capture_shell_notification(HWND hwnd)
{
    if (now_seconds() < 3.0)
    {
        return;
    }

    wchar_t class_name[128] = {};
    wchar_t title[192] = {};
    GetClassNameW(hwnd, class_name, ARRAYSIZE(class_name));
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));

    if (!is_likely_toast_window(hwnd, class_name, title))
    {
        return;
    }

    NotificationSnapshot notification;
    notification.active = true;
    notification.app = L"Notification";
    notification.title = title;
    notification.expires_at = now_seconds() + 4.0;
    notification.icon = get_window_icon_pixels(hwnd, 64);

    if (notification.title.size() > 96)
    {
        notification.body = notification.title.substr(64);
        notification.title.resize(64);
        notification.title += L"...";
    }

    {
        std::lock_guard lock(g_state_mutex);
        g_state.notification = std::move(notification);
    }
    trigger_nudge();

    // Toast bodies are only in the XAML tree: read them with UI Automation on a background thread.
    std::thread(
        [hwnd]()
        {
            try
            {
                Sleep(400);
                if (!IsWindow(hwnd))
                {
                    return;
                }
                HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (SUCCEEDED(hr))
                {
                    IUIAutomation* uia = nullptr;
                    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                          __uuidof(IUIAutomation), (void**)&uia);
                    if (SUCCEEDED(hr) && uia)
                    {
                        IUIAutomation2* uia2 = nullptr;
                        if (SUCCEEDED(uia->QueryInterface(__uuidof(IUIAutomation2), (void**)&uia2)) && uia2)
                        {
                            uia2->put_TransactionTimeout(500);
                            uia2->put_ConnectionTimeout(500);
                            uia2->Release();
                        }
                        IUIAutomationElement* window_el = nullptr;
                        if (SUCCEEDED(uia->ElementFromHandle(hwnd, &window_el)) && window_el)
                        {
                            IUIAutomationCondition* cond = nullptr;
                            uia->CreateTrueCondition(&cond);
                            IUIAutomationElementArray* elements = nullptr;
                            if (SUCCEEDED(window_el->FindAll(TreeScope_Descendants, cond, &elements)) && elements)
                            {
                                int count = 0;
                                elements->get_Length(&count);
                                std::wstring app_name;
                                std::wstring full_text;
                                for (int i = 0; i < count; ++i)
                                {
                                    IUIAutomationElement* el = nullptr;
                                    if (SUCCEEDED(elements->GetElement(i, &el)) && el)
                                    {
                                        BSTR name = nullptr;
                                        el->get_CurrentName(&name);
                                        if (name && wcslen(name) > 0)
                                        {
                                            std::wstring chunk = name;
                                            if (chunk != L"Notification" && chunk != L"New notification")
                                            {
                                                if (app_name.empty())
                                                {
                                                    app_name = chunk;
                                                }
                                                else
                                                {
                                                    if (!full_text.empty())
                                                    {
                                                        full_text += L"  -  ";
                                                    }
                                                    full_text += chunk;
                                                }
                                            }
                                        }
                                        if (name)
                                        {
                                            SysFreeString(name);
                                        }
                                        el->Release();
                                    }
                                }
                                elements->Release();

                                if (full_text.empty() && !app_name.empty())
                                {
                                    full_text = app_name;
                                    app_name = L"Notification";
                                }

                                if (!full_text.empty())
                                {
                                    PixelsRef resolved_icon;
                                    if (!app_name.empty() && app_name != L"Notification")
                                    {
                                        resolved_icon = find_app_icon_by_name(app_name, 64);
                                    }
                                    std::lock_guard lock(g_state_mutex);
                                    if (g_state.notification.active)
                                    {
                                        if (!app_name.empty())
                                        {
                                            g_state.notification.app = app_name;
                                            if (resolved_icon)
                                            {
                                                g_state.notification.icon = std::move(resolved_icon);
                                            }
                                        }
                                        g_state.notification.title = full_text;
                                    }
                                }
                            }
                            if (cond)
                            {
                                cond->Release();
                            }
                            window_el->Release();
                        }
                        uia->Release();
                    }
                    CoUninitialize();
                }
            }
            catch (...)
            {
            }
        })
        .detach();
}

void capture_clipboard(HWND hwnd)
{
    ClipboardSnapshot clip;
    clip.expires_at = now_seconds() + 2.5;
    HWND owner = GetClipboardOwner();
    if (!owner)
    {
        owner = GetForegroundWindow();
    }
    wchar_t owner_title[80] = {};
    if (owner)
    {
        GetWindowTextW(owner, owner_title, ARRAYSIZE(owner_title));
    }
    if (owner && !is_ignorable_foreground_window(owner, owner_title))
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(owner, &pid);
        clip.app_icon = get_window_icon_pixels(owner, 64);

        clip.app_name = owner_title;
        if (clip.app_name.empty())
        {
            std::wstring path;
            if (process_image_name_for_pid(pid, &path))
            {
                clip.app_name = strip_extension(base_name_from_path(path));
            }
        }
        if (clip.app_name.size() > 24)
        {
            clip.app_name.resize(24);
            clip.app_name += L"...";
        }
    }

    std::wstring text = read_clipboard_text(hwnd);
    if (!text.empty())
    {
        constexpr size_t max_chars = 96;
        std::replace(text.begin(), text.end(), L'\r', L' ');
        std::replace(text.begin(), text.end(), L'\n', L' ');
        if (text.size() > max_chars)
        {
            text.resize(max_chars);
            text += L"...";
        }
        clip.text = text;
        clip.image = false;
        clip.active = true;
    }
    else
    {
        BitmapPixels image_pixels;
        if (read_clipboard_image_pixels(hwnd, &image_pixels, clipboard_image_thumb_max_dim))
        {
            clip.text = L"Image copied";
            clip.image = true;
            clip.active = true;
            clip.image_preview = PixelsRef(std::move(image_pixels));
        }
    }

    if (clip.active)
    {
        {
            std::lock_guard lock(g_state_mutex);
            g_state.clipboard = std::move(clip);
        }
    }
}

}
