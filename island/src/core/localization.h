#pragma once

#include "core/common.h"
namespace island
{

std::wstring format_with_name(const wchar_t* format, const std::wstring& name);

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

inline std::atomic<int> g_ui_language{static_cast<int>(UiLanguage::English)};
UiLanguage language_from_tag(std::wstring_view tag);
UiLanguage detect_system_language();
std::wstring format_island_time(const SYSTEMTIME& local, bool follow_system, bool use24_hour,
                              bool show_seconds);
std::wstring format_island_date(const SYSTEMTIME& local, const std::wstring& custom_format,
                              const wchar_t* fallback_pattern);
const wchar_t* loc(const wchar_t* english);

}
