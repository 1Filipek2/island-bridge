#pragma once

#include "core/common.h"

namespace island
{

std::string url_encode_component(const std::wstring& value);
std::wstring ascii_to_wide(const std::string& s);
std::wstring json_string_unescape(const std::string& raw);
bool extract_json_string_field(const std::string& json, const char* field_key, std::string* out_raw);
std::vector<std::string> split_json_object_array(const std::string& json);
DWORD WINAPI lyrics_thread_proc(void*);

}
