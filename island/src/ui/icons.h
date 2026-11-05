#pragma once

#include "core/common.h"
#include "core/model.h"
#include "core/pixels.h"

namespace island
{

PixelsRef icon_to_pixels_ref(HICON icon, UINT size);
bool process_image_name_for_pid(DWORD pid, std::wstring* image_name);
std::wstring to_lower_copy(std::wstring value);
std::wstring base_name_from_path(std::wstring path);
std::wstring strip_extension(std::wstring value);
bool process_image_name_for_window(HWND hwnd, std::wstring* image_name);
std::wstring friendly_media_source_name(std::wstring_view source);
std::wstring media_source_badge(std::wstring_view source_name);
bool media_expand_blocked(const MediaSnapshot& media);
bool is_browser_media_source(std::wstring_view source);
PixelsRef get_window_icon_pixels(HWND hwnd, UINT size);
bool is_ignorable_foreground_window(HWND hwnd, const std::wstring& title);
std::wstring find_browser_media_site_badge(const std::wstring& source_app_user_model_id);
PixelsRef find_media_source_icon(const std::wstring& source_app_user_model_id);
wchar_t notification_initial(const std::wstring& app);
PixelsRef find_app_icon_by_name(const std::wstring& app_name, UINT size);
std::vector<std::uint8_t> read_win_rt_stream_bytes(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& reference);

}
