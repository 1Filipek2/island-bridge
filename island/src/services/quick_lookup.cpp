#include "services/quick_lookup.h"
#include "ui/backdrop.h"
#include "notifications/capture.h"
#include "core/constants.h"
#include "ui/hit_test.h"
#include "services/http.h"
#include "ui/icons.h"
#include "core/layout.h"
#include "core/localization.h"
#include "media/lyrics.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{

std::wstring lookup_typed_query(const LookupUiState& ui)
{
    const size_t a = ui.text.find_first_not_of(L" \t");
    if (a == std::wstring::npos)
    {
        return std::wstring();
    }
    const size_t b = ui.text.find_last_not_of(L" \t");
    return ui.text.substr(a, b - a + 1);
}

std::vector<LookupRow> build_lookup_rows(const std::vector<LookupRecent>& recents, const LookupUiState& ui)
{
    std::vector<LookupRow> rows;
    const size_t cap = static_cast<size_t>(lookup_layout::max_rows);
    const std::wstring typed = lookup_typed_query(ui);

    if (typed.empty())
    {
        const std::wstring clip_key = to_lower_copy(ui.clipboard_query);
        if (!ui.clipboard_query.empty())
        {
            rows.push_back({LookupRowKind::Clipboard, ui.clipboard_query, ui.clipboard_query, loc(L"Clipboard")});
        }
        for (const LookupRecent& r : recents)
        {
            if (rows.size() >= cap)
            {
                break;
            }
            if (!clip_key.empty() && to_lower_copy(r.query) == clip_key)
            {
                continue;
            }
            rows.push_back({LookupRowKind::Recent, r.query, r.title.empty() ? r.query : r.title, r.detail});
        }
        return rows;
    }

    rows.push_back({LookupRowKind::Typed, typed, typed, std::wstring()});
    const std::wstring needle = to_lower_copy(typed);
    for (const LookupRecent& r : recents)
    {
        if (rows.size() >= cap)
        {
            break;
        }
        const std::wstring q = to_lower_copy(r.query);
        if (q == needle)
        {
            continue; // the Typed row already covers it
        }
        if (q.find(needle) == std::wstring::npos && to_lower_copy(r.title).find(needle) == std::wstring::npos)
        {
            continue;
        }
        rows.push_back({LookupRowKind::Recent, r.query, r.title.empty() ? r.query : r.title, r.detail});
    }
    return rows;
}

namespace
{

inline bool lookup_field_hit_test(const MediaContentPoint& pt)
{
    return pt.valid && pt.x >= lookup_layout::k_pad_x && pt.x <= pt.width - lookup_layout::k_pad_x &&
           pt.y >= lookup_layout::field_top && pt.y <= lookup_layout::field_top + lookup_layout::field_h;
}

inline bool lookup_clear_hit_test(const MediaContentPoint& pt)
{
    return pt.valid && pt.x >= pt.width - lookup_layout::k_pad_x - 64.0f && pt.x <= pt.width - lookup_layout::k_pad_x &&
           pt.y >= lookup_layout::below_field - 3.0f &&
           pt.y <= lookup_layout::below_field + lookup_layout::section_h + 3.0f;
}

int lookup_row_at_content_point(const MediaContentPoint& pt, int row_count, bool header)
{
    if (!pt.valid || row_count <= 0)
    {
        return -1;
    }
    if (pt.x < lookup_layout::k_pad_x || pt.x > pt.width - lookup_layout::k_pad_x)
    {
        return -1;
    }
    const float y = pt.y - lookup_layout::rows_top(header);
    if (y < 0.0f)
    {
        return -1;
    }
    const float stride = lookup_layout::k_row_h + lookup_layout::k_row_gap;
    const int index = static_cast<int>(y / stride);
    if (index >= row_count)
    {
        return -1;
    }
    if (y - static_cast<float>(index) * stride > lookup_layout::k_row_h)
    {
        return -1;
    }
    return index;
}

// ── Quick Lookup ─────────────────────────────────────────────────────────────
// Press the Quick Lookup hotkey to look up whatever text is on the clipboard.

// ── Quick Lookup ─────────────────────────────────────────────────────────────
// Press the Quick Lookup hotkey to look up whatever text is on the clipboard.
//
//   hotkey (render thread) -> StartQuickLookup()
//        1. read + normalise the clipboard text
//        2. in-memory cache hit?  -> publish the result at once
//        3. otherwise publish a "Looking up..." card and spawn ONE worker thread
//   worker thread          -> LookupText() -> publish the result if still wanted
//
// Privacy: nothing is looked up when the clipboard changes. Text is only sent over
// the network when the hotkey is pressed, only if it is a short word or phrase, and
// it is never written to the log or to disk.
//
// Adding a source later = write one function with the signature
//     Outcome LookupXxx(const std::wstring& query, Result* out)
// and add one line to LookupText().

// Wikimedia asks API clients to send a descriptive User-Agent with contact info.
constexpr wchar_t lookup_user_agent[] =
    L"DynamicIslandForWindows"
    L"/" ISLAND_VERSION L" (https://github.com/devcode90/Dynamic-Island-for-Windows) QuickLookup";

// Bumped for every new request AND when the card is dismissed. A worker only
// publishes if its id is still current, so a slow reply can never resurrect a card
// the user already closed. Only changed or compared while holding g_stateMutex.
std::atomic<uint64_t> g_lookup_request_seq{0};
bool g_lookup_hotkey_registered = false;

}

namespace quick_lookup
{
namespace
{

constexpr double result_seconds = panel_idle_seconds;
constexpr double message_seconds = panel_idle_seconds;
constexpr size_t recent_capacity = 12;
constexpr double loading_timeout_seconds = 30.0; // upper bound for "Looking up..."
constexpr size_t k_max_body_chars = 360;         // pre-cap on a definition
constexpr size_t max_extract_sentences = 3;
constexpr size_t max_extract_chars = 320;
constexpr size_t cache_capacity = 40;

struct Result
{
    std::wstring title;
    std::wstring subtitle;
    std::wstring body;
    std::wstring example;
    std::wstring source;
};

// Failed means "could not get a real answer" (timeout, 429, 5xx, no network).
// Only Found and NotFound are cached, so a flaky connection is never remembered.
enum class Outcome
{
    Found,
    NotFound,
    Failed
};

}
}

namespace quick_lookup
{

// ---- text helpers -------------------------------------------------------------
// Collapses runs of whitespace (including newlines and NBSP) into single spaces.
// Stops once the result is longer than maxChars, so a huge clipboard stays cheap.
std::wstring collapse_whitespace(const std::wstring& in, size_t max_chars)
{
    std::wstring out;
    bool pending_space = false;
    for (wchar_t ch : in)
    {
        if (iswspace(ch) || ch == 0x00A0 || ch == L'\0')
        {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space)
        {
            out.push_back(L' ');
            pending_space = false;
        }
        out.push_back(ch);
        if (out.size() > max_chars)
        {
            break;
        }
    }
    return out;
}

}

namespace quick_lookup
{
namespace
{

bool is_edge_punct(wchar_t c)
{
    return c != 0 && wcschr(L".,;:!?\"'()[]{}<>\u201c\u201d\u2018\u2019\u00ab\u00bb", c) != nullptr;
}

}
}

namespace quick_lookup
{

// Clipboard text -> lookup query. Too-long text is returned still too long (one
// char over the limit) so the caller can reject it instead of silently truncating.
std::wstring normalize_query(const std::wstring& raw)
{
    std::wstring text = collapse_whitespace(raw, max_query_chars);
    if (text.size() > max_query_chars)
    {
        return text;
    }
    // "hello," / (hello) / “hello” -> hello
    while (!text.empty() && (is_edge_punct(text.front()) || text.front() == L' '))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && (is_edge_punct(text.back()) || text.back() == L' '))
    {
        text.pop_back();
    }
    return text;
}

}

namespace quick_lookup
{
namespace
{

// Cuts to maxChars on a word boundary and appends an ellipsis.
std::wstring clip_to_chars(std::wstring text, size_t max_chars)
{
    if (text.size() <= max_chars)
    {
        return text;
    }
    text.resize(max_chars);
    const size_t space = text.find_last_of(L' ');
    if (space != std::wstring::npos && space > max_chars / 2)
    {
        text.resize(space);
    }
    while (!text.empty() && (iswspace(text.back()) || text.back() == L',' || text.back() == L';' ||
                             text.back() == L':' || text.back() == L'-'))
    {
        text.pop_back();
    }
    if (!text.empty() && IS_HIGH_SURROGATE(text.back()))
    {
        text.pop_back();
    }
    text += L"\u2026";
    return text;
}

std::wstring title_case(std::wstring text)
{
    bool word_start = true;
    for (wchar_t& c : text)
    {
        if (c == L' ')
        {
            word_start = true;
        }
        else
        {
            if (word_start)
            {
                c = static_cast<wchar_t>(towupper(c));
            }
            word_start = false;
        }
    }
    return text;
}

bool is_sentence_closer(wchar_t c)
{
    return c == L'"' || c == L'\'' || c == L')' || c == L'\u201d' || c == L'\u2019';
}

// A '.', '!' or '?' ends a sentence only when whitespace and then a capital, digit
// or opening quote follow, and (for '.') the word before it isn't an initial or a
// common abbreviation. Good enough for encyclopedia text, not a full tokenizer.
bool is_sentence_end(const std::wstring& t, size_t i)
{
    const wchar_t c = t[i];
    if (c != L'.' && c != L'!' && c != L'?')
    {
        return false;
    }
    size_t j = i + 1;
    while (j < t.size() && is_sentence_closer(t[j]))
    {
        ++j;
    }
    if (j >= t.size())
    {
        return true;
    }
    if (!iswspace(t[j]))
    {
        return false;
    }
    while (j < t.size() && iswspace(t[j]))
    {
        ++j;
    }
    if (j >= t.size())
    {
        return true;
    }
    if (!(iswupper(t[j]) || iswdigit(t[j]) || t[j] == L'"' || t[j] == L'\u201c' || t[j] == L'('))
    {
        return false;
    }
    if (c != L'.')
    {
        return true;
    }

    size_t s = i;
    while (s > 0 && iswalpha(t[s - 1]))
    {
        --s;
    }
    const std::wstring word = to_lower_copy(t.substr(s, i - s));
    if (word.size() == 1)
    {
        return false; // initials: "J. R. R. Tolkien"
    }
    static const wchar_t* const k_abbrev[] = {L"mr",  L"mrs", L"ms",  L"dr",  L"prof", L"st",  L"jr",     L"sr",
                                              L"vs",  L"inc", L"ltd", L"co",  L"no",   L"ca",  L"approx", L"mt",
                                              L"gen", L"col", L"lt",  L"sgt", L"rev",  L"fig", L"vol"};
    for (const wchar_t* abbrev : k_abbrev)
    {
        if (word == abbrev)
        {
            return false;
        }
    }
    return true;
}

// First `maxSentences` sentences, but stops early rather than exceed maxChars
// (the first sentence is always kept, clipped if it alone is too long).
std::wstring first_sentences(const std::wstring& text, size_t max_sentences, size_t max_chars)
{
    size_t sentences = 0;
    size_t accepted_end = 0;
    for (size_t i = 0; i < text.size() && sentences < max_sentences; ++i)
    {
        if (!is_sentence_end(text, i))
        {
            continue;
        }
        size_t end = i + 1;
        while (end < text.size() && is_sentence_closer(text[end]))
        {
            ++end;
        }
        if (sentences > 0 && end > max_chars)
        {
            break;
        }
        accepted_end = end;
        ++sentences;
        i = end - 1;
    }
    if (accepted_end == 0)
    {
        accepted_end = text.size(); // no sentence boundary found: use it all, clipped
    }
    std::wstring out = text.substr(0, accepted_end);
    while (!out.empty() && iswspace(out.back()))
    {
        out.pop_back();
    }
    return clip_to_chars(out, max_chars);
}

// ---- JSON helper (same string-find style as the lyrics code) --------------------
// Returns the balanced {...} object that starts at `open`, string- and escape-aware.
std::string balanced_object_at(const std::string& json, size_t open)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = open; i < json.size(); ++i)
    {
        const char c = json[i];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (c == '"')
        {
            in_string = true;
        }
        else if (c == '{')
        {
            ++depth;
        }
        else if (c == '}')
        {
            if (--depth == 0)
            {
                return json.substr(open, i - open + 1);
            }
        }
    }
    return std::string();
}

// ---- in-memory cache (session only, MRU at the back) ----------------------------
struct CacheEntry
{
    std::wstring key;
    bool found = false;
    Result result;
};

std::mutex g_cache_mutex;

std::vector<CacheEntry>& lookup_cache()
{
    static std::vector<CacheEntry> cache;
    return cache;
}

bool cache_lookup(const std::wstring& key, bool* found, Result* out)
{
    std::lock_guard lock(g_cache_mutex);
    auto& cache = lookup_cache();
    for (size_t i = cache.size(); i-- > 0;)
    {
        if (cache[i].key == key)
        {
            *found = cache[i].found;
            *out = cache[i].result;
            std::rotate(cache.begin() + static_cast<std::ptrdiff_t>(i),
                        cache.begin() + static_cast<std::ptrdiff_t>(i) + 1, cache.end());
            return true;
        }
    }
    return false;
}

void cache_store(const std::wstring& key, bool found, const Result& result)
{
    std::lock_guard lock(g_cache_mutex);
    auto& cache = lookup_cache();
    cache.erase(std::remove_if(cache.begin(), cache.end(),
                               [&](const CacheEntry& e)
                               {
                                   return e.key == key;
                               }),
                cache.end());
    CacheEntry entry;
    entry.key = key;
    entry.found = found;
    entry.result = result;
    cache.push_back(std::move(entry));
    while (cache.size() > cache_capacity)
    {
        cache.erase(cache.begin());
    }
}

// ---- source 1: dictionary (single words) ----------------------------------------
// https://api.dictionaryapi.dev/api/v2/entries/en/<word>
// Shape: [{"word":..,"phonetic":..,"phonetics":[{"text":..}],
//          "meanings":[{"partOfSpeech":..,"definitions":[{"definition":..,"example":..}]}]}]
Outcome lookup_dictionary(const std::wstring& word, Result* out)
{
    const std::wstring path = L"/api/v2/entries/en/" + ascii_to_wide(url_encode_component(to_lower_copy(word)));
    const HttpResult res = http_get_ex(L"api.dictionaryapi.dev", path.c_str(), true, lookup_user_agent);
    if (!res.is_definitive())
    {
        return Outcome::Failed;
    }
    if (res.status != 200)
    {
        return Outcome::NotFound;
    }

    const std::vector<std::string> entries = split_json_object_array(res.body);
    if (entries.empty())
    {
        return Outcome::NotFound;
    }
    const std::string& entry = entries.front();

    std::string word_raw, phonetic_raw;
    extract_json_string_field(entry, "word", &word_raw);
    if (!extract_json_string_field(entry, "phonetic", &phonetic_raw) || phonetic_raw.empty())
    {
        phonetic_raw.clear();
        extract_json_string_field(entry, "text", &phonetic_raw); // first phonetics[].text
    }

    // First meaning's part of speech, then the first definition object after it.
    const size_t meanings_pos = entry.find("\"meanings\":[");
    if (meanings_pos == std::string::npos)
    {
        return Outcome::NotFound;
    }
    std::string pos_raw;
    const size_t pos_key = entry.find("\"partOfSpeech\":\"", meanings_pos);
    if (pos_key != std::string::npos)
    {
        extract_json_string_field(entry.substr(pos_key), "partOfSpeech", &pos_raw);
    }
    const size_t def_key = entry.find("\"definition\":\"", pos_key != std::string::npos ? pos_key : meanings_pos);
    if (def_key == std::string::npos)
    {
        return Outcome::NotFound;
    }

    // Bound the search to this one definition object so a later definition's
    // "example" is never attributed to the first one.
    const size_t def_open = entry.rfind('{', def_key);
    const std::string def_object =
        def_open != std::string::npos ? balanced_object_at(entry, def_open) : entry.substr(def_key);
    std::string def_raw, example_raw;
    extract_json_string_field(def_object, "definition", &def_raw);
    extract_json_string_field(def_object, "example", &example_raw);

    const std::wstring definition = collapse_whitespace(json_string_unescape(def_raw));
    if (definition.empty())
    {
        return Outcome::NotFound;
    }

    Result r;
    r.title = collapse_whitespace(json_string_unescape(word_raw));
    if (r.title.empty())
    {
        r.title = word;
    }
    r.subtitle = collapse_whitespace(json_string_unescape(phonetic_raw));
    const std::wstring pos = collapse_whitespace(json_string_unescape(pos_raw));
    if (!pos.empty())
    {
        if (!r.subtitle.empty())
        {
            r.subtitle += L"  \u00b7  ";
        }
        r.subtitle += pos;
    }
    r.body = clip_to_chars(definition, k_max_body_chars);
    const std::wstring example = collapse_whitespace(json_string_unescape(example_raw));
    if (!example.empty())
    {
        r.example = L"\u201c" + clip_to_chars(example, 200) + L"\u201d";
    }
    r.source = L"Dictionary";
    *out = std::move(r);
    return Outcome::Found;
}

// ---- source 2: Wikipedia summary (phrases, or words the dictionary lacks) ---------
// https://en.wikipedia.org/api/rest_v1/page/summary/<title>
// Titles are case-sensitive after the first letter, so a miss is retried once in
// Title Case ("machine learning" -> "Machine Learning").
Outcome lookup_wikipedia(const std::wstring& query, Result* out)
{
    const std::wstring candidates[2] = {query, title_case(query)};
    Outcome outcome = Outcome::NotFound;

    for (int i = 0; i < 2; ++i)
    {
        if (i == 1 && candidates[1] == candidates[0])
        {
            break;
        }
        std::wstring title = candidates[i];
        std::replace(title.begin(), title.end(), L' ', L'_');
        const std::wstring path = L"/api/rest_v1/page/summary/" + ascii_to_wide(url_encode_component(title));

        const HttpResult res = http_get_ex(L"en.wikipedia.org", path.c_str(), true, lookup_user_agent);
        if (!res.is_definitive())
        {
            return Outcome::Failed;
        }
        if (res.status != 200)
        {
            continue; // 404: try the next spelling
        }

        std::string extract_raw, title_raw, desc_raw;
        if (!extract_json_string_field(res.body, "extract", &extract_raw) || extract_raw.empty())
        {
            continue;
        }
        extract_json_string_field(res.body, "title", &title_raw);
        extract_json_string_field(res.body, "description", &desc_raw);

        const std::wstring extract = collapse_whitespace(json_string_unescape(extract_raw));
        if (extract.empty())
        {
            continue;
        }

        Result r;
        r.title = collapse_whitespace(json_string_unescape(title_raw));
        if (r.title.empty())
        {
            r.title = query;
        }
        r.subtitle = collapse_whitespace(json_string_unescape(desc_raw));
        r.body = first_sentences(extract, max_extract_sentences, max_extract_chars);
        r.source = L"Wikipedia";
        *out = std::move(r);
        return Outcome::Found;
    }
    return outcome;
}

// ---- source 3: Wiktionary (primary dictionary) --------------------------------------
// https://en.wiktionary.org/api/rest_v1/page/definition/<word>
// Shape: {"en":[{"partOfSpeech":"Noun","language":"English","definitions":[
//          {"definition":"<html>","parsedExamples":[{"example":"<html>"}]}, ...]}, ...],
//         "fr":[...], ...}
// Definitions come back as HTML, so tags are stripped and entities decoded.

// Removes <tags>, decodes common HTML entities, collapses whitespace.
std::wstring strip_html(const std::wstring& in)
{
    std::wstring no_tags;
    no_tags.reserve(in.size());
    bool in_tag = false;
    for (wchar_t c : in)
    {
        if (c == L'<')
        {
            in_tag = true;
            continue;
        }
        if (c == L'>' && in_tag)
        {
            in_tag = false;
            continue;
        }
        if (!in_tag)
        {
            no_tags.push_back(c);
        }
    }

    std::wstring decoded;
    decoded.reserve(no_tags.size());
    for (size_t i = 0; i < no_tags.size(); ++i)
    {
        if (no_tags[i] == L'&')
        {
            const size_t semi = no_tags.find(L';', i);
            if (semi != std::wstring::npos && semi - i <= 9)
            {
                const std::wstring ent = no_tags.substr(i + 1, semi - i - 1);
                wchar_t rep = 0;
                if (ent == L"amp")
                {
                    rep = L'&';
                }
                else if (ent == L"lt")
                {
                    rep = L'<';
                }
                else if (ent == L"gt")
                {
                    rep = L'>';
                }
                else if (ent == L"quot")
                {
                    rep = L'"';
                }
                else if (ent == L"apos")
                {
                    rep = L'\'';
                }
                else if (ent == L"nbsp")
                {
                    rep = L' ';
                }
                else if (ent.size() > 1 && ent[0] == L'#')
                {
                    const unsigned long code = (ent[1] == L'x' || ent[1] == L'X')
                                                   ? wcstoul(ent.c_str() + 2, nullptr, 16)
                                                   : wcstoul(ent.c_str() + 1, nullptr, 10);
                    if (code > 0 && code < 0x10000 && !(code >= 0xD800 && code <= 0xDFFF))
                    {
                        rep = static_cast<wchar_t>(code);
                    }
                }
                if (rep)
                {
                    decoded.push_back(rep);
                    i = semi;
                    continue;
                }
            }
        }
        decoded.push_back(no_tags[i]);
    }
    return collapse_whitespace(decoded);
}

// Reads the "en" section of a Wiktionary definition response.
bool parse_wiktionary_english(const std::string& body, const std::wstring& title, Result* out)
{
    const size_t en_key = body.find("\"en\":[");
    if (en_key == std::string::npos)
    {
        return false; // the word exists, but only in other languages
    }

    auto skip_separators = [](const std::string& s, size_t p)
    {
        while (p < s.size() && (s[p] == ' ' || s[p] == ',' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t'))
        {
            ++p;
        }
        return p;
    };

    std::vector<std::wstring> parts_of_speech;
    std::vector<std::wstring> definitions;
    std::wstring example;

    size_t p = en_key + 6; // just past  "en":[
    while (p < body.size())
    {
        p = skip_separators(body, p);
        if (p >= body.size() || body[p] != '{')
        {
            break;
        }
        const std::string block = balanced_object_at(body, p);
        if (block.empty())
        {
            break;
        }
        p += block.size();

        std::string pos_raw;
        extract_json_string_field(block, "partOfSpeech", &pos_raw);
        const std::wstring pos = to_lower_copy(collapse_whitespace(json_string_unescape(pos_raw)));
        if (!pos.empty() && parts_of_speech.size() < 3 &&
            std::find(parts_of_speech.begin(), parts_of_speech.end(), pos) == parts_of_speech.end())
        {
            parts_of_speech.push_back(pos);
        }

        // Definitions are taken from the first part of speech only.
        if (!definitions.empty())
        {
            continue;
        }
        const size_t defs_key = block.find("\"definitions\":[");
        if (defs_key == std::string::npos)
        {
            continue;
        }
        size_t q = defs_key + 15; // just past  "definitions":[
        while (q < block.size() && definitions.size() < 3)
        {
            q = skip_separators(block, q);
            if (q >= block.size() || block[q] != '{')
            {
                break;
            }
            const std::string def_obj = balanced_object_at(block, q);
            if (def_obj.empty())
            {
                break;
            }
            q += def_obj.size();

            std::string def_raw;
            if (!extract_json_string_field(def_obj, "definition", &def_raw))
            {
                continue;
            }
            const std::wstring text = strip_html(json_string_unescape(def_raw));
            if (text.empty())
            {
                continue;
            }
            definitions.push_back(text);

            // Only the first definition's example is shown.
            if (definitions.size() == 1)
            {
                std::string ex_raw;
                if (extract_json_string_field(def_obj, "example", &ex_raw))
                {
                    example = strip_html(json_string_unescape(ex_raw));
                }
            }
        }
    }

    if (definitions.empty())
    {
        return false;
    }

    Result r;
    r.title = title;
    for (size_t i = 0; i < parts_of_speech.size(); ++i)
    {
        if (i > 0)
        {
            r.subtitle += L"  \u00b7  ";
        }
        r.subtitle += parts_of_speech[i];
    }

    std::wstring body_text;
    for (size_t i = 0; i < definitions.size(); ++i)
    {
        if (i > 0)
        {
            body_text += L"\n";
        }
        if (definitions.size() > 1)
        {
            body_text += std::to_wstring(i + 1) + L". ";
        }
        body_text += clip_to_chars(definitions[i], 170);
    }
    r.body = clip_to_chars(body_text, k_max_body_chars);

    if (!example.empty())
    {
        r.example = L"\u201c" + clip_to_chars(example, 200) + L"\u201d";
    }
    r.source = L"Wiktionary";
    *out = std::move(r);
    return true;
}

// Titles are case-sensitive, so lowercase is tried first, then the text as typed.
Outcome lookup_wiktionary(const std::wstring& word, Result* out)
{
    const std::wstring lower = to_lower_copy(word);
    const std::wstring candidates[2] = {lower, word};

    for (int i = 0; i < 2; ++i)
    {
        if (i == 1 && candidates[1] == candidates[0])
        {
            break;
        }
        const std::wstring path = L"/api/rest_v1/page/definition/" + ascii_to_wide(url_encode_component(candidates[i]));
        const HttpResult res = http_get_ex(L"en.wiktionary.org", path.c_str(), true, lookup_user_agent);
        if (!res.is_definitive())
        {
            return Outcome::Failed; // timeout / 429 / 5xx: never cached as "not found"
        }
        if (res.status != 200)
        {
            continue; // 404: try the next spelling
        }
        if (parse_wiktionary_english(res.body, candidates[i], out))
        {
            return Outcome::Found;
        }
    }
    return Outcome::NotFound;
}

// ---- router -----------------------------------------------------------------------
// Single words:  Wiktionary -> dictionaryapi.dev (backup) -> Wikipedia
// Phrases:       Wikipedia  -> Wiktionary (idioms like "kick the bucket")
// To add a source: write LookupXxx() above and add one attempt() line below.
Outcome lookup_text(const std::wstring& query, Result* out)
{
    const bool single_word = query.find(L' ') == std::wstring::npos;

    bool any_failed = false;
    auto attempt = [&](Outcome outcome) -> bool
    {
        if (outcome == Outcome::Found)
        {
            return true;
        }
        if (outcome == Outcome::Failed)
        {
            any_failed = true;
        }
        return false;
    };

    if (single_word)
    {
        if (attempt(lookup_wiktionary(query, out)))
        {
            return Outcome::Found;
        }
        if (attempt(lookup_dictionary(query, out)))
        {
            return Outcome::Found;
        }
        if (attempt(lookup_wikipedia(query, out)))
        {
            return Outcome::Found;
        }
    }
    else
    {
        if (attempt(lookup_wikipedia(query, out)))
        {
            return Outcome::Found;
        }
        if (attempt(lookup_wiktionary(query, out)))
        {
            return Outcome::Found;
        }
    }

    // "Nothing there" is only cacheable if every source gave a definite answer.
    return any_failed ? Outcome::Failed : Outcome::NotFound;
}

// ---- publishing ---------------------------------------------------------------------
void fill_snapshot(LookupSnapshot& lk, const std::wstring& query, bool found, const Result* r, double now)
{
    lk = LookupSnapshot{};
    lk.active = true;
    lk.query = query;
    if (found && r)
    {
        lk.status = LookupStatus::Found;
        lk.title = r->title;
        lk.subtitle = r->subtitle;
        lk.body = r->body;
        lk.example = r->example;
        lk.source = r->source;
        lk.expires_at = now + result_seconds;
    }
    else
    {
        lk.status = LookupStatus::NoResult;
        lk.expires_at = now + message_seconds;
    }
}

// Moves `query` to the front of the recent list. Caller must hold g_stateMutex.
void push_recent_locked(SharedState& st, const std::wstring& query, const Result& r)
{
    LookupRecent entry;
    entry.query = query;
    entry.title = r.title;
    entry.source = r.source;

    std::wstring detail = r.body;
    std::replace(detail.begin(), detail.end(), L'\n', L' ');
    if (detail.rfind(L"1. ", 0) == 0)
    {
        detail.erase(0, 3);
    }
    entry.detail = clip_to_chars(collapse_whitespace(detail), 60);

    auto& list = st.lookup_recent;
    const std::wstring key = to_lower_copy(query);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const LookupRecent& e)
                              {
                                  return to_lower_copy(e.query) == key;
                              }),
               list.end());
    list.insert(list.begin(), std::move(entry));
    if (list.size() > recent_capacity)
    {
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(recent_capacity), list.end());
    }
}

// Worker thread -> island. Dropped if the request was superseded or dismissed, or
// if its "Looking up..." card has already timed out.
void apply_result(uint64_t id, const std::wstring& query, bool found, const Result& r)
{
    if (!g_running.load())
    {
        return;
    }
    const double now = now_seconds();
    {
        std::lock_guard lock(g_state_mutex);
        LookupSnapshot& lk = g_state.lookup;
        if (g_lookup_request_seq.load() != id || !lk.active || now >= lk.expires_at)
        {
            return;
        }
        fill_snapshot(lk, query, found, &r, now);
        if (found)
        {
            push_recent_locked(g_state, query, r);
        }
    }
    g_layout_dirty = true; // repaint; the springs resize the island to the new card
}

}
}

namespace
{

uint64_t publish_new_lookup(const LookupSnapshot& snap, bool nudge = true)
{
    uint64_t id;
    {
        std::lock_guard lock(g_state_mutex);
        g_state.lookup = snap;
        id = ++g_lookup_request_seq;
    }
    g_layout_dirty = true;
    if (nudge)
    {
        trigger_nudge(); // typing and searching stay quiet; only opening the panel bounces
    }
    return id;
}

// ── Quick Lookup panel: session + input ──────────────────────────────────────
// All of this runs on the render thread. g_state.lookup is the only part worker
// threads also touch, so it only changes under g_stateMutex.

// Keeps the panel alive while the user is interacting with it.
void touch_lookup_panel()
{
    std::lock_guard lock(g_state_mutex);
    if (g_state.lookup.active)
    {
        g_state.lookup.expires_at =
            std::max(g_state.lookup.expires_at, now_seconds() + quick_lookup::panel_idle_seconds);
    }
}

std::vector<LookupRow> current_lookup_rows(LookupStatus* status)
{
    std::lock_guard lock(g_state_mutex);
    if (status)
    {
        *status = g_state.lookup.status;
    }
    return build_lookup_rows(g_state.lookup_recent, g_lookup_ui);
}

void force_foreground(HWND hwnd)
{
    INPUT input{};
    SendInput(1, &input, sizeof(INPUT));
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    dwm_backdrop::ensure_behind(hwnd);
    SetFocus(hwnd);
}

// The island is WS_EX_NOACTIVATE so it never steals focus. While the search box is
// open it has to be able to receive keys, so the flag is dropped for the session.
void begin_lookup_session(HWND hwnd)
{
    HWND previous = GetForegroundWindow();
    g_lookup_ui.prev_foreground = (previous && previous != hwnd) ? previous : nullptr;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE)
    {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    }
    if (!IsWindowVisible(hwnd))
    {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    force_foreground(hwnd);
}

void end_lookup_session(HWND hwnd)
{
    if (!g_lookup_ui.open)
    {
        return;
    }
    HWND previous = g_lookup_ui.prev_foreground;
    const bool had_focus = GetForegroundWindow() == hwnd;
    g_lookup_ui = LookupUiState{}; // open = false first, so the focus change below can't re-enter

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    if (had_focus && previous && IsWindow(previous) && IsWindowVisible(previous))
    {
        SetForegroundWindow(previous);
    }
}

}

void close_lookup_panel(HWND hwnd)
{
    {
        std::lock_guard lock(g_state_mutex);
        g_state.lookup.active = false;
        ++g_lookup_request_seq; // a reply still in flight is now ignored
    }
    g_layout_dirty = true;
    end_lookup_session(hwnd);
}

namespace
{

// Runs a lookup and shows the outcome in the panel.
void submit_lookup(const std::wstring& raw_text)
{
    const std::wstring query = quick_lookup::normalize_query(raw_text);
    if (query.empty() || query.size() > quick_lookup::max_query_chars)
    {
        return;
    }

    const double now = now_seconds();
    LookupUiState& ui = g_lookup_ui;
    ui.text = query;
    ui.caret = query.size();
    ui.select_all = false;
    ui.selected = 0;
    ui.caret_reset_at = now;

    LookupSnapshot snap;
    snap.active = true;
    snap.query = query;

    const std::wstring key = to_lower_copy(query);
    bool cached_found = false;
    quick_lookup::Result cached;
    if (quick_lookup::cache_lookup(key, &cached_found, &cached))
    {
        quick_lookup::fill_snapshot(snap, query, cached_found, &cached, now);
        if (cached_found)
        {
            std::lock_guard lock(g_state_mutex);
            quick_lookup::push_recent_locked(g_state, query, cached); // bump it to the top
        }
        publish_new_lookup(snap, false);
        return;
    }

    snap.status = LookupStatus::Loading;
    snap.expires_at = now + quick_lookup::loading_timeout_seconds;
    const uint64_t id = publish_new_lookup(snap, false);

    // The query text is deliberately not logged.
    host::log(L"Quick Lookup: request started.");
    std::thread(
        [id, query, key]()
        {
            try
            {
                quick_lookup::Result result;
                const quick_lookup::Outcome outcome = quick_lookup::lookup_text(query, &result);
                if (outcome != quick_lookup::Outcome::Failed)
                {
                    quick_lookup::cache_store(key, outcome == quick_lookup::Outcome::Found, result);
                }
                quick_lookup::apply_result(id, query, outcome == quick_lookup::Outcome::Found, result);
                host::log(L"Quick Lookup: finished (outcome %d).", static_cast<int>(outcome));
            }
            catch (...)
            {
                host::log(L"Quick Lookup: lookup threw an exception.");
            }
        })
        .detach();
}

void submit_lookup_selection()
{
    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = current_lookup_rows(&status);
    if (status == LookupStatus::Search && !rows.empty())
    {
        const int idx = clamp_int(g_lookup_ui.selected, 0, static_cast<int>(rows.size()) - 1);
        submit_lookup(rows[static_cast<size_t>(idx)].query);
    }
    else
    {
        submit_lookup(g_lookup_ui.text);
    }
}

// Call after any change to the text. Editing leaves the result view and goes back
// to the list, and drops any reply still in flight.
void lookup_ui_edited()
{
    LookupUiState& ui = g_lookup_ui;
    const double now = now_seconds();
    ui.selected = 0;
    ui.select_all = false;
    ui.caret_reset_at = now;

    bool leave_result = false;
    {
        std::lock_guard lock(g_state_mutex);
        leave_result = g_state.lookup.active && g_state.lookup.status != LookupStatus::Search;
    }
    if (leave_result)
    {
        LookupSnapshot snap;
        snap.active = true;
        snap.status = LookupStatus::Search;
        snap.expires_at = now + quick_lookup::panel_idle_seconds;
        publish_new_lookup(snap, false);
    }
    else
    {
        touch_lookup_panel();
    }
    g_layout_dirty = true;
}

void insert_into_lookup_field(const std::wstring& raw)
{
    LookupUiState& ui = g_lookup_ui;
    std::wstring s;
    s.reserve(raw.size());
    for (wchar_t c : raw)
    {
        s.push_back((c < 0x20 || c == 0x7F) ? L' ' : c); // single-line field
    }
    if (s.empty())
    {
        return;
    }

    if (ui.select_all)
    {
        ui.text.clear();
        ui.caret = 0;
        ui.select_all = false;
    }
    ui.caret = std::min(ui.caret, ui.text.size());

    const size_t max_chars = quick_lookup::max_query_chars;
    const size_t room = max_chars > ui.text.size() ? max_chars - ui.text.size() : 0;
    if (room == 0)
    {
        return;
    }
    if (s.size() > room)
    {
        s.resize(room);
        if (!s.empty() && IS_HIGH_SURROGATE(s.back()))
        {
            s.pop_back();
        }
    }
    ui.text.insert(ui.caret, s);
    ui.caret += s.size();
    lookup_ui_edited();
}

size_t lookup_step_back(const std::wstring& t, size_t i)
{
    if (i == 0)
    {
        return 0;
    }
    --i;
    if (i > 0 && IS_LOW_SURROGATE(t[i]) && IS_HIGH_SURROGATE(t[i - 1]))
    {
        --i;
    }
    return i;
}

size_t lookup_step_forward(const std::wstring& t, size_t i)
{
    if (i >= t.size())
    {
        return t.size();
    }
    return (IS_HIGH_SURROGATE(t[i]) && i + 1 < t.size()) ? i + 2 : i + 1;
}

size_t lookup_prev_word(const std::wstring& t, size_t i)
{
    while (i > 0 && iswspace(t[i - 1]))
    {
        --i;
    }
    while (i > 0 && !iswspace(t[i - 1]))
    {
        --i;
    }
    return i;
}

size_t lookup_next_word(const std::wstring& t, size_t i)
{
    while (i < t.size() && !iswspace(t[i]))
    {
        ++i;
    }
    while (i < t.size() && iswspace(t[i]))
    {
        ++i;
    }
    return i;
}

}

// Called on the render thread when the hotkey fires.
void start_quick_lookup(HWND hwnd)
{
    bool enabled = false;
    {
        std::lock_guard lock(g_settings_mutex);
        enabled = g_settings.quick_lookup;
    }
    // A manually hidden island has nothing to show the panel on.
    if (!enabled || g_manually_hidden.load())
    {
        return;
    }

    // Hotkey again = close.
    if (g_lookup_ui.open)
    {
        close_lookup_panel(hwnd);
        return;
    }

    // Clipboard text is only offered as a suggestion row; nothing is sent anywhere yet.
    std::wstring clip = quick_lookup::normalize_query(read_clipboard_text(hwnd));
    if (clip.size() > quick_lookup::max_query_chars)
    {
        clip.clear();
    }

    const double now = now_seconds();
    g_lookup_ui = LookupUiState{};
    g_lookup_ui.open = true;
    g_lookup_ui.clipboard_query = clip;
    g_lookup_ui.caret_reset_at = now;

    LookupSnapshot snap;
    snap.active = true;
    snap.status = LookupStatus::Search;
    snap.expires_at = now + quick_lookup::panel_idle_seconds;
    publish_new_lookup(snap);

    begin_lookup_session(hwnd);
}

// WM_KEYDOWN. Printable characters arrive separately through WM_CHAR.
bool handle_lookup_key_down(HWND hwnd, WPARAM vk)
{
    LookupUiState& ui = g_lookup_ui;
    if (!ui.open)
    {
        return false;
    }
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool moved = false;

    switch (vk)
    {
        case VK_ESCAPE:
            close_lookup_panel(hwnd);
            return true;
        case VK_RETURN:
            submit_lookup_selection();
            return true;
        case VK_UP:
        case VK_DOWN:
        {
            LookupStatus status = LookupStatus::Search;
            const std::vector<LookupRow> rows = current_lookup_rows(&status);
            if (status == LookupStatus::Search && !rows.empty())
            {
                const int n = static_cast<int>(rows.size());
                ui.selected = (clamp_int(ui.selected, 0, n - 1) + (vk == VK_DOWN ? 1 : n - 1)) % n;
            }
            moved = true;
            break;
        }
        case VK_LEFT:
            ui.caret =
                ui.select_all ? 0 : (ctrl ? lookup_prev_word(ui.text, ui.caret) : lookup_step_back(ui.text, ui.caret));
            ui.select_all = false;
            moved = true;
            break;
        case VK_RIGHT:
            ui.caret = ui.select_all
                           ? ui.text.size()
                           : (ctrl ? lookup_next_word(ui.text, ui.caret) : lookup_step_forward(ui.text, ui.caret));
            ui.select_all = false;
            moved = true;
            break;
        case VK_HOME:
            ui.caret = 0;
            ui.select_all = false;
            moved = true;
            break;
        case VK_END:
            ui.caret = ui.text.size();
            ui.select_all = false;
            moved = true;
            break;
        case VK_BACK:
            if (ui.select_all)
            {
                ui.text.clear();
                ui.caret = 0;
            }
            else if (ui.caret > 0)
            {
                const size_t from = ctrl ? lookup_prev_word(ui.text, ui.caret) : lookup_step_back(ui.text, ui.caret);
                ui.text.erase(from, ui.caret - from);
                ui.caret = from;
            }
            else
            {
                return true;
            }
            lookup_ui_edited();
            return true;
        case VK_DELETE:
            if (ui.select_all)
            {
                ui.text.clear();
                ui.caret = 0;
            }
            else if (ui.caret < ui.text.size())
            {
                const size_t to = lookup_step_forward(ui.text, ui.caret);
                ui.text.erase(ui.caret, to - ui.caret);
            }
            else
            {
                return true;
            }
            lookup_ui_edited();
            return true;
        case 'A':
            if (!ctrl)
            {
                return false;
            }
            ui.select_all = !ui.text.empty();
            moved = true;
            break;
        case 'V':
            if (!ctrl)
            {
                return false;
            }
            insert_into_lookup_field(
                quick_lookup::collapse_whitespace(read_clipboard_text(hwnd), quick_lookup::max_query_chars));
            return true;
        default:
            return false;
    }

    if (moved)
    {
        ui.caret_reset_at = now_seconds();
        touch_lookup_panel();
        g_layout_dirty = true;
    }
    return true;
}

bool handle_lookup_char(wchar_t ch)
{
    if (!g_lookup_ui.open)
    {
        return false;
    }
    if (ch < 0x20 || ch == 0x7F)
    {
        return true; // control keys are handled in WM_KEYDOWN
    }
    insert_into_lookup_field(std::wstring(1, ch));
    return true;
}

// Returns true when the click belonged to the panel (so it must not fall through
// to "open the media app" and friends).
bool handle_lookup_click(int x, int y)
{
    LookupUiState& ui = g_lookup_ui;
    if (!ui.open)
    {
        return false;
    }

    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = current_lookup_rows(&status);
    const MediaContentPoint pt = media_content_from_client(x, y);
    touch_lookup_panel();
    if (!pt.valid)
    {
        return true;
    }

    if (status == LookupStatus::Search)
    {
        const bool header = lookup_has_header(ui, rows);
        if (header && lookup_has_recent_rows(rows) && lookup_clear_hit_test(pt))
        {
            {
                std::lock_guard lock(g_state_mutex);
                g_state.lookup_recent.clear();
            }
            ui.selected = 0;
            g_layout_dirty = true;
            return true;
        }
        const int row = lookup_row_at_content_point(pt, static_cast<int>(rows.size()), header);
        if (row >= 0)
        {
            submit_lookup(rows[static_cast<size_t>(row)].query);
            return true;
        }
    }
    if (lookup_field_hit_test(pt))
    {
        ui.caret = ui.text.size();
        ui.select_all = false;
        ui.caret_reset_at = now_seconds();
        g_layout_dirty = true;
    }
    return true;
}

void handle_lookup_mouse_move(int x, int y)
{
    LookupUiState& ui = g_lookup_ui;
    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = current_lookup_rows(&status);
    touch_lookup_panel();
    if (status != LookupStatus::Search)
    {
        return;
    }

    const int row = lookup_row_at_content_point(media_content_from_client(x, y), static_cast<int>(rows.size()),
                                                lookup_has_header(ui, rows));
    if (row >= 0 && row != ui.selected)
    {
        ui.selected = row;
        g_layout_dirty = true;
    }
}

LPCWSTR lookup_cursor_at(int x, int y)
{
    LookupStatus status = LookupStatus::Search;
    const std::vector<LookupRow> rows = current_lookup_rows(&status);
    const MediaContentPoint pt = media_content_from_client(x, y);
    if (status == LookupStatus::Search)
    {
        const bool header = lookup_has_header(g_lookup_ui, rows);
        if ((header && lookup_has_recent_rows(rows) && lookup_clear_hit_test(pt)) ||
            lookup_row_at_content_point(pt, static_cast<int>(rows.size()), header) >= 0)
        {
            return IDC_HAND;
        }
    }
    return lookup_field_hit_test(pt) ? IDC_IBEAM : IDC_ARROW;
}

// Registers (or releases) the Quick Lookup hotkey. Render thread only, because
// RegisterHotKey must run on the thread that owns the window.
void apply_lookup_hotkey()
{
    HWND hwnd = g_hwnd;
    if (!hwnd)
    {
        return;
    }
    if (g_lookup_hotkey_registered)
    {
        UnregisterHotKey(hwnd, id_lookup_hotkey);
        g_lookup_hotkey_registered = false;
    }

    const Settings settings = get_settings_copy();
    if (!settings.quick_lookup)
    {
        return;
    }
    if (RegisterHotKey(hwnd, id_lookup_hotkey, settings.lookup_modifiers, settings.lookup_vk))
    {
        g_lookup_hotkey_registered = true;
    }
    else
    {
        host::log(L"Failed to register Quick Lookup hotkey (error %lu).", GetLastError());
    }
}

}
