#pragma once

#include "core/common.h"
namespace island
{

std::wstring read_clipboard_text(HWND hwnd);
void capture_shell_notification(HWND hwnd);
void capture_clipboard(HWND hwnd);

}
