#pragma once

#include "core/common.h"
namespace island
{

std::wstring ReadClipboardText(HWND hwnd);
void CaptureShellNotification(HWND hwnd);
void CaptureClipboard(HWND hwnd);

}
