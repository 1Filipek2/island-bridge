#include "media/lyrics.h"
#include "services/http.h"
#include "ui/icons.h"
#include "media/lyrics_cache.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{

// ── Lyrics (LRCLIB, https://lrclib.net) ──────────────────────────────────────
// User-Agent sent to LRCLIB.
constexpr wchar_t lyrics_user_agent[] =
    L"DynamicIslandForWindows/" ISLAND_VERSION L" (https://github.com/devcode90/Dynamic-Island-for-Windows)";

bool is_url_safe_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

}

std::string url_encode_component(const std::wstring& value)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (len > 0)
    {
        utf8.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    }

    std::string out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : utf8)
    {
        if (is_url_safe_char(c))
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::wstring ascii_to_wide(const std::string& s)
{
    std::wstring w;
    w.reserve(s.size());
    for (char c : s)
    {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return w;
}

// Unescapes a raw JSON string value (still containing \n, \", \uXXXX etc.)
// and decodes it from UTF-8 into a wide string.
std::wstring json_string_unescape(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size())
        {
            char next = raw[i + 1];
            switch (next)
            {
                case 'n':
                    out.push_back('\n');
                    ++i;
                    break;
                case 'r':
                    out.push_back('\r');
                    ++i;
                    break;
                case 't':
                    out.push_back('\t');
                    ++i;
                    break;
                case '"':
                    out.push_back('"');
                    ++i;
                    break;
                case '\\':
                    out.push_back('\\');
                    ++i;
                    break;
                case '/':
                    out.push_back('/');
                    ++i;
                    break;
                case 'u':
                {
                    if (i + 5 < raw.size())
                    {
                        std::string hex_narrow = raw.substr(i + 2, 4);
                        std::wstring hex_wide = ascii_to_wide(hex_narrow);
                        wchar_t code = static_cast<wchar_t>(wcstoul(hex_wide.c_str(), nullptr, 16));
                        char utf8buf[4] = {};
                        int out_len =
                            WideCharToMultiByte(CP_UTF8, 0, &code, 1, utf8buf, sizeof(utf8buf), nullptr, nullptr);
                        if (out_len > 0)
                        {
                            out.append(utf8buf, out_len);
                        }
                        i += 5;
                    }
                    break;
                }
                default:
                    out.push_back(next);
                    ++i;
                    break;
            }
        }
        else
        {
            out.push_back(c);
        }
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
    std::wstring wout;
    if (wlen > 0)
    {
        wout.resize(wlen - 1);
        MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, wout.data(), wlen);
    }
    return wout;
}

// Finds "fieldName":"....." in a JSON blob and returns the raw (still
// escaped) contents between the quotes. Returns false if missing or null.
bool extract_json_string_field(const std::string& json, const char* field_key, std::string* out_raw)
{
    const std::string key = std::string("\"") + field_key + "\":\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos)
    {
        return false;
    }
    pos += key.size();

    std::string result;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i)
    {
        char c = json[i];
        if (escaped)
        {
            result.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\')
        {
            result.push_back(c);
            escaped = true;
            continue;
        }
        if (c == '"')
        {
            *out_raw = result;
            return true;
        }
        result.push_back(c);
    }
    return false;
}

// Splits a top-level JSON array "[ {...}, {...} ]" into the substrings for
// each object, respecting nested braces and quoted strings.
std::vector<std::string> split_json_object_array(const std::string& json)
{
    std::vector<std::string> objects;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    size_t object_start = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i)
    {
        char c = json[i];
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
            if (depth == 0)
            {
                object_start = i;
            }
            ++depth;
        }
        else if (c == '}')
        {
            --depth;
            if (depth == 0 && object_start != std::string::npos)
            {
                objects.push_back(json.substr(object_start, i - object_start + 1));
                object_start = std::string::npos;
            }
        }
    }

    return objects;
}

namespace
{

std::vector<LyricsLine> parse_synced_lyrics(const std::wstring& text)
{
    std::vector<LyricsLine> lines;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos)
        {
            end = text.size();
        }
        std::wstring line = text.substr(start, end - start);

        if (line.size() >= 3 && line[0] == L'[')
        {
            const size_t close_bracket = line.find(L']');
            if (close_bracket != std::wstring::npos)
            {
                int minutes = 0, seconds = 0, hundredths = 0;
                const std::wstring ts = line.substr(1, close_bracket - 1);
                if (swscanf_s(ts.c_str(), L"%d:%d.%d", &minutes, &seconds, &hundredths) >= 2)
                {
                    std::wstring lyric_text = line.substr(close_bracket + 1);
                    while (!lyric_text.empty() && lyric_text.front() == L' ')
                    {
                        lyric_text.erase(lyric_text.begin());
                    }
                    LyricsLine ll;
                    ll.time_ms = static_cast<int64_t>(minutes) * 60000 + static_cast<int64_t>(seconds) * 1000 +
                                 static_cast<int64_t>(hundredths) * 10;
                    ll.text = std::move(lyric_text);
                    lines.push_back(std::move(ll));
                }
            }
        }

        if (end == text.size())
        {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::vector<LyricsLine> parse_plain_lyrics(const std::wstring& text)
{
    std::vector<LyricsLine> lines;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos)
        {
            end = text.size();
        }
        std::wstring line = text.substr(start, end - start);
        if (!line.empty())
        {
            LyricsLine ll;
            ll.time_ms = -1;
            ll.text = std::move(line);
            lines.push_back(std::move(ll));
        }
        if (end == text.size())
        {
            break;
        }
        start = end + 1;
    }
    return lines;
}

// ── Lyrics lookup helpers ────────────────────────────────────────────────────
std::wstring trim_lyrics_text(const std::wstring& s)
{
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos)
    {
        return std::wstring();
    }
    const size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool has_lyrics_noise_word(const std::wstring& text)
{
    static const wchar_t* const k_noise[] = {
        L"feat",  L"ft.",   L"featuring", L"with ",  L"remaster", L"version",    L"edit",       L"mix",
        L"live",  L"from ", L"official",  L"video",  L"audio",    L"lyric",      L"explicit",   L"deluxe",
        L"bonus", L"mono",  L"stereo",    L"single", L"radio",    L"soundtrack", L"anniversary"};
    const std::wstring lower = to_lower_copy(text);
    for (const wchar_t* w : k_noise)
    {
        if (lower.find(w) != std::wstring::npos)
        {
            return true;
        }
    }
    return false;
}

// "Song (feat. X) - Remastered 2011 | Channel" -> "Song"
std::wstring clean_lyrics_title(std::wstring title)
{
    for (size_t i = 0; i < title.size();)
    {
        const wchar_t open = title[i];
        if (open == L'(' || open == L'[')
        {
            const wchar_t close = (open == L'(') ? L')' : L']';
            const size_t end = title.find(close, i + 1);
            if (end != std::wstring::npos && has_lyrics_noise_word(title.substr(i + 1, end - i - 1)))
            {
                size_t from = i;
                while (from > 0 && title[from - 1] == L' ')
                {
                    --from;
                }
                title.erase(from, end - from + 1);
                i = from;
                continue;
            }
        }
        ++i;
    }
    const size_t bar = title.find(L" | ");
    if (bar != std::wstring::npos && bar > 0)
    {
        title.resize(bar);
    }
    for (;;)
    {
        const size_t dash = title.rfind(L" - ");
        if (dash == std::wstring::npos || dash == 0)
        {
            break;
        }
        if (!has_lyrics_noise_word(title.substr(dash + 3)))
        {
            break;
        }
        title.resize(dash);
    }
    return trim_lyrics_text(title);
}

// "A, B" / "A feat. B" / "A - Topic" -> "A"
std::wstring primary_lyrics_artist(std::wstring artist)
{
    artist = trim_lyrics_text(artist);
    auto ends_with = [](const std::wstring& lower, const wchar_t* suffix)
    {
        const size_t n = wcslen(suffix);
        return lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0;
    };
    std::wstring lower = to_lower_copy(artist);
    if (ends_with(lower, L" - topic"))
    {
        artist.resize(artist.size() - 8);
    }
    else if (ends_with(lower, L"vevo"))
    {
        artist.resize(artist.size() - 4);
    }
    lower = to_lower_copy(artist);

    static const wchar_t* const seps[] = {L",",    L";",           L"/",   L" feat",   L" ft.",
                                          L" ft ", L" featuring ", L" x ", L" \u00d7 "};
    size_t cut = std::wstring::npos;
    for (const wchar_t* sep : seps)
    {
        const size_t pos = lower.find(sep);
        if (pos != std::wstring::npos && pos > 0)
        {
            cut = std::min(cut, pos);
        }
    }
    if (cut != std::wstring::npos)
    {
        artist.resize(cut);
    }
    return trim_lyrics_text(artist);
}

std::wstring normalize_lyrics_match(const std::wstring& in)
{
    std::wstring out;
    bool space = false;
    for (wchar_t c : in)
    {
        if (iswalnum(c))
        {
            if (space && !out.empty())
            {
                out.push_back(L' ');
            }
            space = false;
            out.push_back(static_cast<wchar_t>(towlower(c)));
        }
        else
        {
            space = true;
        }
    }
    return out;
}

// 2 = same, 1 = one contains the other, 0 = unrelated
int lyrics_text_match(const std::wstring& a, const std::wstring& b)
{
    const std::wstring na = normalize_lyrics_match(a);
    const std::wstring nb = normalize_lyrics_match(b);
    if (na.empty() || nb.empty())
    {
        return 0;
    }
    if (na == nb)
    {
        return 2;
    }
    if (na.find(nb) != std::wstring::npos || nb.find(na) != std::wstring::npos)
    {
        return 1;
    }
    return 0;
}

bool extract_json_number_field(const std::string& json, const char* key, double* out)
{
    const std::string k = std::string("\"") + key + "\":";
    size_t pos = json.find(k);
    if (pos == std::string::npos)
    {
        return false;
    }
    pos += k.size();
    while (pos < json.size() && json[pos] == ' ')
    {
        ++pos;
    }
    if (pos >= json.size() || (json[pos] != '-' && (json[pos] < '0' || json[pos] > '9')))
    {
        return false;
    }
    *out = strtod(json.c_str() + pos, nullptr);
    return true;
}

struct LyricsPick
{
    bool found = false;
    bool synced = false;
    int score = -1;
    std::vector<LyricsLine> lines;
};

// Scores every candidate in an LRCLIB response (single object or array) and keeps
// the best one in *best. `trusted` = exact /api/get answer, so title checks are skipped.
void consider_lyrics_body(const std::string& body, const std::wstring& want_title, const std::wstring& want_artist,
                          int want_duration_sec, bool trusted, LyricsPick* best)
{
    if (body.empty())
    {
        return;
    }

    std::vector<std::string> candidates;
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && body[first] == '[')
    {
        candidates = split_json_object_array(body);
    }
    else
    {
        candidates.push_back(body);
    }

    for (const std::string& c : candidates)
    {
        if (c.find("\"instrumental\":true") != std::string::npos)
        {
            continue;
        }

        std::string synced_raw, plain_raw, name_raw, artist_raw;
        const bool has_synced = extract_json_string_field(c, "syncedLyrics", &synced_raw) && !synced_raw.empty();
        const bool has_plain = extract_json_string_field(c, "plainLyrics", &plain_raw) && !plain_raw.empty();
        if (!has_synced && !has_plain)
        {
            continue;
        }

        std::vector<LyricsLine> lines;
        bool synced = false;
        if (has_synced)
        {
            lines = parse_synced_lyrics(json_string_unescape(synced_raw));
            synced = !lines.empty();
        }
        if (!synced && has_plain)
        {
            lines = parse_plain_lyrics(json_string_unescape(plain_raw));
        }
        if (lines.empty())
        {
            continue;
        }

        extract_json_string_field(c, "trackName", &name_raw);
        extract_json_string_field(c, "artistName", &artist_raw);
        const std::wstring cand_title = json_string_unescape(name_raw);
        const std::wstring cand_artist = json_string_unescape(artist_raw);

        int dur_diff = -1;
        double cand_dur = 0.0;
        if (want_duration_sec > 0 && extract_json_number_field(c, "duration", &cand_dur) && cand_dur > 0.0)
        {
            dur_diff = std::abs(static_cast<int>(cand_dur + 0.5) - want_duration_sec);
        }

        int score = 0;
        if (!trusted)
        {
            const int title_match = lyrics_text_match(cand_title, want_title);
            if (title_match == 0)
            {
                continue; // different song
            }
            if (dur_diff > 10)
            {
                continue; // different cut: sync would be off
            }
            const int artist_match = want_artist.empty() ? 1 : lyrics_text_match(cand_artist, want_artist);
            if (artist_match == 0 && !(dur_diff >= 0 && dur_diff <= 3))
            {
                continue; // maybe a cover
            }
            score += title_match * 30 + artist_match * 20;
        }
        if (dur_diff >= 0)
        {
            score += dur_diff <= 2 ? 40 : (dur_diff <= 5 ? 25 : 5);
        }
        if (synced)
        {
            score += 20;
        }

        if (!best->found || score > best->score)
        {
            best->found = true;
            best->synced = synced;
            best->score = score;
            best->lines = std::move(lines);
        }
    }
}

// Tries exact lookups first, then tolerant searches. Returns true if usable lyrics
// were found. *definitive becomes false when a request failed (timeout / 429 / 5xx),
// meaning "no lyrics" can't be trusted and the caller should retry later.
bool fetch_lyrics_from_lrclib(const std::wstring& title, const std::wstring& artist, const std::wstring& album,
                              int duration_sec, LyricsPick* pick, bool* definitive)
{
    *definitive = true;

    const std::wstring clean_title = clean_lyrics_title(title);
    const std::wstring main_artist = primary_lyrics_artist(artist);
    const std::wstring search_title = clean_title.empty() ? title : clean_title;
    const std::wstring search_artist = main_artist.empty() ? artist : main_artist;

    auto enc = [](const std::wstring& s)
    {
        return ascii_to_wide(url_encode_component(s));
    };

    // One quick retry on a transport failure before giving up on this request.
    auto request = [&](const std::wstring& path) -> HttpResult
    {
        HttpResult r = http_get_ex(L"lrclib.net", path.c_str(), true, lyrics_user_agent);
        if (!r.is_definitive())
        {
            if (WaitForSingleObject(g_stop_event, 400) != WAIT_TIMEOUT)
            {
                return r;
            }
            r = http_get_ex(L"lrclib.net", path.c_str(), true, lyrics_user_agent);
        }
        return r;
    };
    auto handle = [&](const HttpResult& r, bool trusted) -> bool
    {
        if (!r.is_definitive())
        {
            *definitive = false;
            return false;
        }
        if (r.status == 200)
        {
            consider_lyrics_body(r.body, search_title, search_artist, duration_sec, trusted, pick);
        }
        return true;
    };
    auto have_synced = [&]()
    {
        return pick->found && pick->synced;
    };

    // 1) exact lookup with the metadata exactly as reported
    {
        std::wstring path = L"/api/get?track_name=" + enc(title) + L"&artist_name=" + enc(artist);
        if (!album.empty())
        {
            path += L"&album_name=" + enc(album);
        }
        if (duration_sec > 0)
        {
            path += L"&duration=" + std::to_wstring(duration_sec);
        }
        if (!handle(request(path), true))
        {
            return pick->found;
        }
    }

    // 2) exact lookup with a cleaned title and the main artist only
    if (!have_synced() && (search_title != title || search_artist != artist))
    {
        std::wstring path = L"/api/get?track_name=" + enc(search_title) + L"&artist_name=" + enc(search_artist);
        if (duration_sec > 0)
        {
            path += L"&duration=" + std::to_wstring(duration_sec);
        }
        if (!handle(request(path), true))
        {
            return pick->found;
        }
    }

    // 3) tolerant free-text search, results scored and filtered
    if (!have_synced())
    {
        const std::wstring path = L"/api/search?q=" + enc(search_title + L" " + search_artist);
        if (!handle(request(path), false))
        {
            return pick->found;
        }
    }

    // 4) title-only search (artist spelled differently, or wrong artist in metadata)
    if (!have_synced())
    {
        const std::wstring path = L"/api/search?track_name=" + enc(search_title);
        if (!handle(request(path), false))
        {
            return pick->found;
        }
    }

    return pick->found;
}

}

DWORD WINAPI lyrics_thread_proc(void*)
{
    WaitForSingleObject(g_stop_event, 4000);

    std::wstring last_title;
    std::wstring last_artist;
    double retry_at_sec = 0.0;
    int fail_count = 0;

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        bool lyrics_enabled = true;
        bool disk_cache_on = false;
        uint64_t disk_cap_bytes = 0;
        {
            std::lock_guard lock(g_settings_mutex);
            lyrics_enabled = (g_settings.collapsed_lyrics || g_settings.media_tab_lyrics) && g_settings.media;
            disk_cache_on = g_settings.lyrics_cache && g_settings.lyrics_cache_max_mb > 0;
            disk_cap_bytes = static_cast<uint64_t>(std::max(0, g_settings.lyrics_cache_max_mb)) * 1024ull * 1024ull;
        }
        lyrics_cache::IterationGuard cache_guard(disk_cache_on, disk_cap_bytes);
        const bool lyrics_view_visible =
            (now_seconds() - g_last_lyrics_view_time.load(std::memory_order_relaxed)) < 3.0;
        if (!lyrics_enabled || !lyrics_view_visible)
        {
            WaitForSingleObject(g_stop_event, 1000);
            continue;
        }

        bool available = false;
        std::wstring title, artist, album;
        int64_t end_ticks = 0;
        auto read_media = [&]()
        {
            std::lock_guard lock(g_state_mutex);
            available = g_state.media.available;
            title = g_state.media.title;
            artist = g_state.media.artist;
            album = g_state.media.album_title;
            end_ticks = g_state.media.end_ticks;
        };
        read_media();

        if (!available || title.empty())
        {
            if (!last_title.empty())
            {
                last_title.clear();
                last_artist.clear();
                retry_at_sec = 0.0;
                fail_count = 0;
                std::lock_guard lock(g_state_mutex);
                g_state.lyrics = LyricsSnapshot{};
            }
            WaitForSingleObject(g_stop_event, 1000);
            continue;
        }

        const bool same_track = (title == last_title && artist == last_artist);
        if (same_track)
        {
            // Already handled. Only continue if a failed fetch is due for a retry.
            if (retry_at_sec <= 0.0 || now_seconds() < retry_at_sec)
            {
                WaitForSingleObject(g_stop_event, 1000);
                continue;
            }
        }
        else
        {
            fail_count = 0;
            // Title / artist / duration don't always update together. Let them settle.
            if (WaitForSingleObject(g_stop_event, 1500) != WAIT_TIMEOUT)
            {
                break;
            }
            read_media();
            if (!available || title.empty())
            {
                continue;
            }
        }
        retry_at_sec = 0.0;
        last_title = title;
        last_artist = artist;

        const lyrics_cache::Keys cache_keys = lyrics_cache::build_keys(title, artist, album, end_ticks);
        {
            LyricsSnapshot cached;
            if (lyrics_cache::lookup(cache_keys, disk_cache_on, title, artist, &cached))
            {
                host::log(L"Lyrics: cache hit for %s - %s", artist.c_str(), title.c_str());
                std::lock_guard lock(g_state_mutex);
                if (g_state.media.title == title && g_state.media.artist == artist)
                {
                    g_state.lyrics = std::move(cached);
                }
                continue;
            }
        }

        if (!same_track)
        {
            std::lock_guard lock(g_state_mutex);
            g_state.lyrics = LyricsSnapshot{};
            g_state.lyrics.fetching = true;
            g_state.lyrics.matched_title = title;
            g_state.lyrics.matched_artist = artist;
        }

        const int duration_sec = end_ticks > 0 ? static_cast<int>(end_ticks / 10000000.0 + 0.5) : 0;
        host::log(L"Lyrics: requesting %s - %s (attempt %d)", artist.c_str(), title.c_str(), fail_count + 1);

        LyricsPick pick;
        bool definitive = true;
        fetch_lyrics_from_lrclib(title, artist, album, duration_sec, &pick, &definitive);

        LyricsSnapshot snap;
        snap.matched_title = title;
        snap.matched_artist = artist;

        if (pick.found)
        {
            snap.has_data = true;
            snap.synced = pick.synced;
            snap.lines = LyricsLines(std::move(pick.lines));
            fail_count = 0;
            lyrics_cache::store(cache_keys, lyrics_cache::Status::Found, snap, disk_cache_on);
        }
        else if (definitive)
        {
            snap.not_found = true; // LRCLIB really has nothing for this track
            fail_count = 0;
            host::log(L"Lyrics: genuinely no lyrics for %s - %s", artist.c_str(), title.c_str());
        }
        else
        {
            // Network / 429 / 5xx: NOT "not found". Back off and retry.
            ++fail_count;
            static const double backoff[] = {4.0, 10.0, 30.0, 60.0};
            retry_at_sec = now_seconds() + backoff[std::min<size_t>(fail_count - 1, 3)];
            snap.fetching = (fail_count < 3);
            snap.not_found = (fail_count >= 3);
            host::log(L"Lyrics: request FAILED for %s - %s (fail #%d), retrying later.", artist.c_str(), title.c_str(),
                      fail_count);
        }

        {
            std::lock_guard lock(g_state_mutex);
            if (g_state.media.title == title && g_state.media.artist == artist)
            {
                g_state.lyrics = std::move(snap);
            }
        }
        WaitForSingleObject(g_stop_event, 500);
    }
    return 0;
}

}
