#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{

int active_media_source_index(const SharedState& state);
std::wstring get_preferred_media_source();
void request_media_source(const std::wstring& aumid);
void lock_media_source(const std::wstring& aumid);
void request_media_source_switch(const std::wstring& aumid);

}
