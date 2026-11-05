#pragma once

#include "core/common.h"
#include "core/model.h"
#include "core/pixels.h"
namespace island
{

PixelsRef IconToPixelsRef(HICON icon, UINT size);
bool ProcessImageNameForPid(DWORD pid, std::wstring* imageName);
std::wstring ToLowerCopy(std::wstring value);
std::wstring BaseNameFromPath(std::wstring path);
std::wstring StripExtension(std::wstring value);
bool ProcessImageNameForWindow(HWND hwnd, std::wstring* imageName);
std::wstring FriendlyMediaSourceName(std::wstring_view source);
std::wstring MediaSourceBadge(std::wstring_view sourceName);
bool MediaExpandBlocked(const MediaSnapshot& media);
bool IsBrowserMediaSource(std::wstring_view source);
PixelsRef GetWindowIconPixels(HWND hwnd, UINT size);
bool IsIgnorableForegroundWindow(HWND hwnd, const std::wstring& title);
std::wstring FindBrowserMediaSiteBadge(const std::wstring& sourceAppUserModelId);
PixelsRef FindMediaSourceIcon(const std::wstring& sourceAppUserModelId);
wchar_t notification_initial(const std::wstring& app);
PixelsRef FindAppIconByName(const std::wstring& appName, UINT size);
std::vector<uint8_t> ReadWinRtStreamBytes(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& reference);

}
