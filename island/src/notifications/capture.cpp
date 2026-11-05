#include "notifications/capture.h"
#include "core/constants.h"
#include "ui/icons.h"
#include "core/model.h"
#include "core/pixels.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{


std::wstring ReadClipboardText(HWND hwnd) {
    std::wstring text;
    if (!OpenClipboard(hwnd)) {
        return text;
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        auto* locked = static_cast<const wchar_t*>(GlobalLock(data));
        if (locked) {
            text = locked;
            GlobalUnlock(data);
        }
    }

    CloseClipboard();
    return text;
}


namespace
{


// Decodes whatever bitmap is currently on the clipboard into a small BGRA
// thumbnail, aspect-preserving and capped at maxDim on the longer side.
// Requesting CF_BITMAP works regardless of which bitmap format the source
// app actually placed on the clipboard (CF_DIB/CF_DIBV5) — Windows
// synthesizes CF_BITMAP from those automatically.
bool ReadClipboardImagePixels(HWND hwnd, BitmapPixels* outPixels, UINT maxDim) {
    if (!outPixels || !OpenClipboard(hwnd)) {
        return false;
    }

    HBITMAP sourceBitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (!sourceBitmap) {
        CloseClipboard();
        return false;
    }

    BITMAP bm = {};
    if (!GetObject(sourceBitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        CloseClipboard();
        return false;
    }

    UINT srcW = static_cast<UINT>(bm.bmWidth);
    UINT srcH = static_cast<UINT>(bm.bmHeight);
    UINT dstW = srcW;
    UINT dstH = srcH;
    if (srcW > maxDim || srcH > maxDim) {
        const float scale = static_cast<float>(maxDim) / static_cast<float>(std::max(srcW, srcH));
        dstW = std::max<UINT>(1, static_cast<UINT>(srcW * scale));
        dstH = std::max<UINT>(1, static_cast<UINT>(srcH * scale));
    }

    HDC screen = GetDC(nullptr);
    HDC srcDc = CreateCompatibleDC(screen);
    HDC dstDc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!srcDc || !dstDc) {
        if (srcDc) DeleteDC(srcDc);
        if (dstDc) DeleteDC(dstDc);
        CloseClipboard();
        return false;
    }

    // Selecting the clipboard's own HBITMAP into a scratch DC to read from it
    // is the MSDN-documented way to do this; we must not DeleteObject it.
    HGDIOBJ oldSrc = SelectObject(srcDc, sourceBitmap);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(dstW);
    bi.bmiHeader.biHeight = -static_cast<LONG>(dstH);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dstDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        SelectObject(srcDc, oldSrc);
        DeleteDC(srcDc);
        DeleteDC(dstDc);
        CloseClipboard();
        return false;
    }

    HGDIOBJ oldDst = SelectObject(dstDc, dib);
    SetStretchBltMode(dstDc, HALFTONE);
    SetBrushOrgEx(dstDc, 0, 0, nullptr);
    StretchBlt(dstDc, 0, 0, static_cast<int>(dstW), static_cast<int>(dstH),
               srcDc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);

    BitmapPixels pixels;
    pixels.width = dstW;
    pixels.height = dstH;
    pixels.bgra.resize(static_cast<size_t>(dstW) * dstH * 4);
    memcpy(pixels.bgra.data(), bits, pixels.bgra.size());

    // GDI blits never populate an alpha channel; without this the D2D bitmap
    // (which expects premultiplied alpha) would render fully invisible.
    for (size_t i = 3; i < pixels.bgra.size(); i += 4) {
        pixels.bgra[i] = 255;
    }

    pixels.generation = ++g_artGenerationCounter;
    *outPixels = std::move(pixels);

    SelectObject(dstDc, oldDst);
    DeleteObject(dib);
    SelectObject(srcDc, oldSrc);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    CloseClipboard();
    return true;
}

bool IsLikelyToastWindow(HWND hwnd, const wchar_t* className, const wchar_t* title) {
    if (hwnd == g_hwnd || !hwnd) {
        return false;
    }

    if (GetWindow(hwnd, GW_OWNER)) {
        return false;
    }

    std::wstring image;
    if (ProcessImageNameForWindow(hwnd, &image)) {
        const std::wstring base = ToLowerCopy(BaseNameFromPath(image));
        if (base == L"powershell.exe" || base == L"pwsh.exe" || base == L"conhost.exe" || base == L"cmd.exe") {
            return false;
        }
    }

    const std::wstring cls = ToLowerCopy(className ? className : L"");
    const std::wstring text = ToLowerCopy(title ? title : L"");

    // Classic Windows 10 toasts have clear class names
    if (cls.find(L"notification") != std::wstring::npos ||
        cls.find(L"toast") != std::wstring::npos ||
        cls.find(L"windows.ui.notifications") != std::wstring::npos) {
        return true;
    }

    // Windows 11 toasts use generic XAML or CoreWindow classes, usually hosted by
    // explorer.exe, sihost.exe, or ShellExperienceHost.exe.
    // Importantly, their title is often empty at the exact moment of creation!
    if (cls.find(L"xaml_windowedpopupclass") != std::wstring::npos ||
        cls.find(L"windows.ui.core.corewindow") != std::wstring::npos) {

        std::wstring image;
        if (ProcessImageNameForWindow(hwnd, &image)) {
            const std::wstring base = ToLowerCopy(BaseNameFromPath(image));
            if (base == L"explorer.exe" || base == L"sihost.exe" || base == L"shellexperiencehost.exe") {
                // Ensure it's not the start menu, search, or action center main panel
                if (text != L"start" && text != L"action center" && text != L"search" && text != L"task view") {
                    return true;
                }
            }
        }
    }

    return false;
}

}



void CaptureShellNotification(HWND hwnd) {
    // Grace period: ignore notifications that fire in the first 3 seconds after
    // the mod starts. sihost.exe gets injected while system windows are still
    // settling, which causes false positives (e.g. Snipping Tool windows).
    if (NowSeconds() < 3.0) {
        return;
    }

    wchar_t className[128] = {};
    wchar_t title[192] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));

    if (!IsLikelyToastWindow(hwnd, className, title)) {
        return;
    }

    NotificationSnapshot notification;
    notification.active = true;
    notification.app = L"Notification";
    notification.title = title;
    notification.expiresAt = NowSeconds() + 4.0;
    // Fetch a 64px icon to ensure crisp rendering inside the pill
    notification.icon = GetWindowIconPixels(hwnd, 64);

    if (notification.title.size() > 96) {
        notification.body = notification.title.substr(64);
        notification.title.resize(64);
        notification.title += L"...";
    }

    {
        std::lock_guard lock(g_stateMutex);
        g_state.notification = std::move(notification);
    }
    TriggerNudge();

    // Spawn a background thread to extract the full rich text body of the toast using UI Automation.
    // Modern Windows Toasts often only provide the App Name via GetWindowTextW, leaving the body hidden in the XAML tree.
    std::thread([hwnd]() {
        try {
            Sleep(400); // Give the heavy UWP XAML tree enough time to fully construct the text nodes
            if (!IsWindow(hwnd)) return;
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(hr)) {
                IUIAutomation* uia = nullptr;
                hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
                if (SUCCEEDED(hr) && uia) {
                    IUIAutomation2* uia2 = nullptr;
                    if (SUCCEEDED(uia->QueryInterface(__uuidof(IUIAutomation2), (void**)&uia2)) && uia2) {
                        uia2->put_TransactionTimeout(500);
                        uia2->put_ConnectionTimeout(500);
                        uia2->Release();
                    }
                    IUIAutomationElement* windowEl = nullptr;
                    if (SUCCEEDED(uia->ElementFromHandle(hwnd, &windowEl)) && windowEl) {
                        IUIAutomationCondition* cond = nullptr;
                        uia->CreateTrueCondition(&cond);
                        IUIAutomationElementArray* elements = nullptr;
                        if (SUCCEEDED(windowEl->FindAll(TreeScope_Descendants, cond, &elements)) && elements) {
                            int count = 0;
                            elements->get_Length(&count);
                            std::wstring appName;
                            std::wstring fullText;
                            for (int i = 0; i < count; ++i) {
                                IUIAutomationElement* el = nullptr;
                                if (SUCCEEDED(elements->GetElement(i, &el)) && el) {
                                    BSTR name = nullptr;
                                    el->get_CurrentName(&name);
                                    if (name && wcslen(name) > 0) {
                                        std::wstring chunk = name;
                                        // Skip generic screen-reader labels often found in toasts
                                        if (chunk != L"Notification" && chunk != L"New notification") {
                                            if (appName.empty()) {
                                                appName = chunk;
                                            } else {
                                                if (!fullText.empty()) fullText += L"  -  ";
                                                fullText += chunk;
                                            }
                                        }
                                    }
                                    if (name) SysFreeString(name);
                                    el->Release();
                                }
                            }
                            elements->Release();

                            if (fullText.empty() && !appName.empty()) {
                                fullText = appName;
                                appName = L"Notification";
                            }

                            if (!fullText.empty()) {
                                PixelsRef resolvedIcon;
                                if (!appName.empty() && appName != L"Notification") {
                                    resolvedIcon = FindAppIconByName(appName, 64);
                                }
                                std::lock_guard lock(g_stateMutex);
                                if (g_state.notification.active) {
                                    if (!appName.empty()) {
                                        g_state.notification.app = appName;
                                        if (resolvedIcon) {
                                            g_state.notification.icon = std::move(resolvedIcon);
                                        }
                                    }
                                    g_state.notification.title = fullText;
                                }
                            }
                        }
                        if (cond) cond->Release();
                        windowEl->Release();
                    }
                    uia->Release();
                }
                CoUninitialize();
            }
        } catch (...) {
        }
    }).detach();
}

void CaptureClipboard(HWND hwnd) {
    ClipboardSnapshot clip;
    clip.expiresAt = NowSeconds() + 2.5;
    HWND owner = GetClipboardOwner();
    if (!owner) {
        owner = GetForegroundWindow();
    }
    wchar_t ownerTitle[80] = {};
    if (owner) {
        GetWindowTextW(owner, ownerTitle, ARRAYSIZE(ownerTitle));
    }
    if (owner && !IsIgnorableForegroundWindow(owner, ownerTitle)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(owner, &pid);
        // Fetch at 64px for crisp rendering — 18px/32px is often too small for icon APIs and returns empty.
        clip.appIcon = GetWindowIconPixels(owner, 64);

        clip.appName = ownerTitle;
        if (clip.appName.empty()) {
            std::wstring path;
            if (ProcessImageNameForPid(pid, &path)) {
                clip.appName = StripExtension(BaseNameFromPath(path));
            }
        }
        if (clip.appName.size() > 24) {
            clip.appName.resize(24);
            clip.appName += L"...";
        }
    }

    std::wstring text = ReadClipboardText(hwnd);
    if (!text.empty()) {
        constexpr size_t maxChars = 96;
        std::replace(text.begin(), text.end(), L'\r', L' ');
        std::replace(text.begin(), text.end(), L'\n', L' ');
        if (text.size() > maxChars) {
            text.resize(maxChars);
            text += L"...";
        }
        clip.text = text;
        clip.image = false;
        clip.active = true;
    } else {
        BitmapPixels imagePixels;
        if (ReadClipboardImagePixels(hwnd, &imagePixels, kClipboardImageThumbMaxDim)) {
            clip.text = L"Image copied";
            clip.image = true;
            clip.active = true;
            clip.imagePreview = PixelsRef(std::move(imagePixels));
        }
    }

    if (clip.active) {
        {
            std::lock_guard lock(g_stateMutex);
            g_state.clipboard = std::move(clip);
        }
    }
}


}
