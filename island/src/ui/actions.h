#pragma once

#include "core/common.h"
namespace island
{

void SetClickThrough(HWND hwnd, bool clickThrough);
void HandleStatusClickAtPoint(HWND hwnd, LPARAM lParam);
void StartFocusTimer(int minutes, bool isBreak);
void ToggleTimerPause();
void StopFocusTimer();
void ToggleEndpointMute();
void SeekMediaToTicks(int64_t targetTicks);
void SendMediaTransportCommand(int cmd);
void OpenRelevantApp();
void DismissTransientState();

}
