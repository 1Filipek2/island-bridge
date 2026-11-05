#pragma once

#include "core/common.h"
namespace island
{

std::wstring FormatWithName(const wchar_t* format, const std::wstring& name);

// ── Localization (#35) ───────────────────────────────────────────────────────
// Every user-visible string in the island goes through Loc(). The English text
// doubles as the lookup key, so an untranslated string degrades to readable
// English rather than to a raw identifier, and adding a language means adding
// one column -- no key bookkeeping.
enum class UiLanguage {
    English,
    French,
    Spanish,
    German,
    Portuguese,
    Italian,
    Russian,
    Turkish,
    Hindi,
    ChineseSimplified,
    Japanese,
    Korean,
    Count,
};

inline std::atomic<int> g_uiLanguage{static_cast<int>(UiLanguage::English)};
UiLanguage LanguageFromTag(std::wstring_view tag);
UiLanguage DetectSystemLanguage();
std::wstring FormatIslandTime(const SYSTEMTIME& local, bool followSystem, bool use24Hour,
                              bool showSeconds);
std::wstring FormatIslandDate(const SYSTEMTIME& local, const std::wstring& customFormat,
                              const wchar_t* fallbackPattern);
const wchar_t* Loc(const wchar_t* english);

}
