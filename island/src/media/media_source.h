#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{

int ActiveMediaSourceIndex(const SharedState& state);
std::wstring GetPreferredMediaSource();
void RequestMediaSource(const std::wstring& aumid);
void LockMediaSource(const std::wstring& aumid);
void RequestMediaSourceSwitch(const std::wstring& aumid);

}
