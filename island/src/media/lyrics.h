#pragma once

#include "core/common.h"
namespace island
{

std::string UrlEncodeComponent(const std::wstring& value);
std::wstring AsciiToWide(const std::string& s);
std::wstring JsonStringUnescape(const std::string& raw);
bool ExtractJsonStringField(const std::string& json, const char* fieldKey, std::string* outRaw);
std::vector<std::string> SplitJsonObjectArray(const std::string& json);
DWORD WINAPI LyricsThreadProc(void*);

}
