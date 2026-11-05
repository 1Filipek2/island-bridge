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
constexpr wchar_t kLyricsUserAgent[] =
    L"DynamicIslandForWindows/" ISLAND_VERSION L" (https://github.com/devcode90/Dynamic-Island-for-Windows)";

bool IsUrlSafeChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

}



std::string UrlEncodeComponent(const std::wstring& value) {
    int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (len > 0) {
        utf8.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    }

    std::string out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : utf8) {
        if (IsUrlSafeChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::wstring AsciiToWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return w;
}

// Unescapes a raw JSON string value (still containing \n, \", \uXXXX etc.)
// and decodes it from UTF-8 into a wide string.
std::wstring JsonStringUnescape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            switch (next) {
                case 'n': out.push_back('\n'); ++i; break;
                case 'r': out.push_back('\r'); ++i; break;
                case 't': out.push_back('\t'); ++i; break;
                case '"': out.push_back('"'); ++i; break;
                case '\\': out.push_back('\\'); ++i; break;
                case '/': out.push_back('/'); ++i; break;
                case 'u': {
                    if (i + 5 < raw.size()) {
                        std::string hexNarrow = raw.substr(i + 2, 4);
                        std::wstring hexWide = AsciiToWide(hexNarrow);
                        wchar_t code = static_cast<wchar_t>(wcstoul(hexWide.c_str(), nullptr, 16));
                        char utf8buf[4] = {};
                        int outLen = WideCharToMultiByte(CP_UTF8, 0, &code, 1, utf8buf,
                                                         sizeof(utf8buf), nullptr, nullptr);
                        if (outLen > 0) {
                            out.append(utf8buf, outLen);
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
        } else {
            out.push_back(c);
        }
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
    std::wstring wout;
    if (wlen > 0) {
        wout.resize(wlen - 1);
        MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, wout.data(), wlen);
    }
    return wout;
}

// Finds "fieldName":"....." in a JSON blob and returns the raw (still
// escaped) contents between the quotes. Returns false if missing or null.
bool ExtractJsonStringField(const std::string& json, const char* fieldKey, std::string* outRaw) {
    const std::string key = std::string("\"") + fieldKey + "\":\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        return false;
    }
    pos += key.size();

    std::string result;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (escaped) {
            result.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            result.push_back(c);
            escaped = true;
            continue;
        }
        if (c == '"') {
            *outRaw = result;
            return true;
        }
        result.push_back(c);
    }
    return false;
}

// Splits a top-level JSON array "[ {...}, {...} ]" into the substrings for
// each object, respecting nested braces and quoted strings.
std::vector<std::string> SplitJsonObjectArray(const std::string& json) {
    std::vector<std::string> objects;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            if (depth == 0) {
                objectStart = i;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }

    return objects;
}


namespace
{


std::vector<LyricsLine> ParseSyncedLyrics(const std::wstring& text) {
    std::vector<LyricsLine> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);

        if (line.size() >= 3 && line[0] == L'[') {
            const size_t closeBracket = line.find(L']');
            if (closeBracket != std::wstring::npos) {
                int minutes = 0, seconds = 0, hundredths = 0;
                const std::wstring ts = line.substr(1, closeBracket - 1);
                if (swscanf_s(ts.c_str(), L"%d:%d.%d", &minutes, &seconds, &hundredths) >= 2) {
                    std::wstring lyricText = line.substr(closeBracket + 1);
                    while (!lyricText.empty() && lyricText.front() == L' ') {
                        lyricText.erase(lyricText.begin());
                    }
                    LyricsLine ll;
                    ll.timeMs = static_cast<int64_t>(minutes) * 60000 +
                               static_cast<int64_t>(seconds) * 1000 +
                               static_cast<int64_t>(hundredths) * 10;
                    ll.text = std::move(lyricText);
                    lines.push_back(std::move(ll));
                }
            }
        }

        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

std::vector<LyricsLine> ParsePlainLyrics(const std::wstring& text) {
    std::vector<LyricsLine> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty()) {
            LyricsLine ll;
            ll.timeMs = -1;
            ll.text = std::move(line);
            lines.push_back(std::move(ll));
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

// ── Lyrics lookup helpers ────────────────────────────────────────────────────
std::wstring TrimLyricsText(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring();
    const size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool HasLyricsNoiseWord(const std::wstring& text) {
    static const wchar_t* const kNoise[] = {
        L"feat", L"ft.", L"featuring", L"with ", L"remaster", L"version", L"edit", L"mix",
        L"live", L"from ", L"official", L"video", L"audio", L"lyric", L"explicit", L"deluxe",
        L"bonus", L"mono", L"stereo", L"single", L"radio", L"soundtrack", L"anniversary"};
    const std::wstring lower = ToLowerCopy(text);
    for (const wchar_t* w : kNoise) {
        if (lower.find(w) != std::wstring::npos) return true;
    }
    return false;
}

// "Song (feat. X) - Remastered 2011 | Channel" -> "Song"
std::wstring CleanLyricsTitle(std::wstring title) {
    for (size_t i = 0; i < title.size();) {
        const wchar_t open = title[i];
        if (open == L'(' || open == L'[') {
            const wchar_t close = (open == L'(') ? L')' : L']';
            const size_t end = title.find(close, i + 1);
            if (end != std::wstring::npos && HasLyricsNoiseWord(title.substr(i + 1, end - i - 1))) {
                size_t from = i;
                while (from > 0 && title[from - 1] == L' ') --from;
                title.erase(from, end - from + 1);
                i = from;
                continue;
            }
        }
        ++i;
    }
    const size_t bar = title.find(L" | ");
    if (bar != std::wstring::npos && bar > 0) title.resize(bar);
    for (;;) {
        const size_t dash = title.rfind(L" - ");
        if (dash == std::wstring::npos || dash == 0) break;
        if (!HasLyricsNoiseWord(title.substr(dash + 3))) break;
        title.resize(dash);
    }
    return TrimLyricsText(title);
}

// "A, B" / "A feat. B" / "A - Topic" -> "A"
std::wstring PrimaryLyricsArtist(std::wstring artist) {
    artist = TrimLyricsText(artist);
    auto endsWith = [](const std::wstring& lower, const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0;
    };
    std::wstring lower = ToLowerCopy(artist);
    if (endsWith(lower, L" - topic")) artist.resize(artist.size() - 8);
    else if (endsWith(lower, L"vevo")) artist.resize(artist.size() - 4);
    lower = ToLowerCopy(artist);

    static const wchar_t* const kSeps[] = {L",", L";", L"/", L" feat", L" ft.", L" ft ",
                                           L" featuring ", L" x ", L" \u00d7 "};
    size_t cut = std::wstring::npos;
    for (const wchar_t* sep : kSeps) {
        const size_t pos = lower.find(sep);
        if (pos != std::wstring::npos && pos > 0) cut = std::min(cut, pos);
    }
    if (cut != std::wstring::npos) artist.resize(cut);
    return TrimLyricsText(artist);
}

std::wstring NormalizeLyricsMatch(const std::wstring& in) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : in) {
        if (iswalnum(c)) {
            if (space && !out.empty()) out.push_back(L' ');
            space = false;
            out.push_back(static_cast<wchar_t>(towlower(c)));
        } else {
            space = true;
        }
    }
    return out;
}

// 2 = same, 1 = one contains the other, 0 = unrelated
int LyricsTextMatch(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = NormalizeLyricsMatch(a);
    const std::wstring nb = NormalizeLyricsMatch(b);
    if (na.empty() || nb.empty()) return 0;
    if (na == nb) return 2;
    if (na.find(nb) != std::wstring::npos || nb.find(na) != std::wstring::npos) return 1;
    return 0;
}

bool ExtractJsonNumberField(const std::string& json, const char* key, double* out) {
    const std::string k = std::string("\"") + key + "\":";
    size_t pos = json.find(k);
    if (pos == std::string::npos) return false;
    pos += k.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || (json[pos] != '-' && (json[pos] < '0' || json[pos] > '9'))) return false;
    *out = strtod(json.c_str() + pos, nullptr);
    return true;
}

struct LyricsPick {
    bool found = false;
    bool synced = false;
    int score = -1;
    std::vector<LyricsLine> lines;
};

// Scores every candidate in an LRCLIB response (single object or array) and keeps
// the best one in *best. `trusted` = exact /api/get answer, so title checks are skipped.
void ConsiderLyricsBody(const std::string& body, const std::wstring& wantTitle,
                        const std::wstring& wantArtist, int wantDurationSec, bool trusted,
                        LyricsPick* best) {
    if (body.empty()) return;

    std::vector<std::string> candidates;
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && body[first] == '[') {
        candidates = SplitJsonObjectArray(body);
    } else {
        candidates.push_back(body);
    }

    for (const std::string& c : candidates) {
        if (c.find("\"instrumental\":true") != std::string::npos) continue;

        std::string syncedRaw, plainRaw, nameRaw, artistRaw;
        const bool hasSynced = ExtractJsonStringField(c, "syncedLyrics", &syncedRaw) && !syncedRaw.empty();
        const bool hasPlain = ExtractJsonStringField(c, "plainLyrics", &plainRaw) && !plainRaw.empty();
        if (!hasSynced && !hasPlain) continue;

        std::vector<LyricsLine> lines;
        bool synced = false;
        if (hasSynced) {
            lines = ParseSyncedLyrics(JsonStringUnescape(syncedRaw));
            synced = !lines.empty();
        }
        if (!synced && hasPlain) {
            lines = ParsePlainLyrics(JsonStringUnescape(plainRaw));
        }
        if (lines.empty()) continue;

        ExtractJsonStringField(c, "trackName", &nameRaw);
        ExtractJsonStringField(c, "artistName", &artistRaw);
        const std::wstring candTitle = JsonStringUnescape(nameRaw);
        const std::wstring candArtist = JsonStringUnescape(artistRaw);

        int durDiff = -1;
        double candDur = 0.0;
        if (wantDurationSec > 0 && ExtractJsonNumberField(c, "duration", &candDur) && candDur > 0.0) {
            durDiff = std::abs(static_cast<int>(candDur + 0.5) - wantDurationSec);
        }

        int score = 0;
        if (!trusted) {
            const int titleMatch = LyricsTextMatch(candTitle, wantTitle);
            if (titleMatch == 0) continue;                       // different song
            if (durDiff > 10) continue;                          // different cut: sync would be off
            const int artistMatch = wantArtist.empty() ? 1 : LyricsTextMatch(candArtist, wantArtist);
            if (artistMatch == 0 && !(durDiff >= 0 && durDiff <= 3)) continue;  // maybe a cover
            score += titleMatch * 30 + artistMatch * 20;
        }
        if (durDiff >= 0) score += durDiff <= 2 ? 40 : (durDiff <= 5 ? 25 : 5);
        if (synced) score += 20;

        if (!best->found || score > best->score) {
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
bool FetchLyricsFromLrclib(const std::wstring& title, const std::wstring& artist,
                           const std::wstring& album, int durationSec,
                           LyricsPick* pick, bool* definitive) {
    *definitive = true;

    const std::wstring cleanTitle = CleanLyricsTitle(title);
    const std::wstring mainArtist = PrimaryLyricsArtist(artist);
    const std::wstring searchTitle = cleanTitle.empty() ? title : cleanTitle;
    const std::wstring searchArtist = mainArtist.empty() ? artist : mainArtist;

    auto enc = [](const std::wstring& s) { return AsciiToWide(UrlEncodeComponent(s)); };

    // One quick retry on a transport failure before giving up on this request.
    auto request = [&](const std::wstring& path) -> HttpResult {
        HttpResult r = HttpGetEx(L"lrclib.net", path.c_str(), true, kLyricsUserAgent);
        if (!r.IsDefinitive()) {
            if (WaitForSingleObject(g_stopEvent, 400) != WAIT_TIMEOUT) return r;
            r = HttpGetEx(L"lrclib.net", path.c_str(), true, kLyricsUserAgent);
        }
        return r;
    };
    auto handle = [&](const HttpResult& r, bool trusted) -> bool {
        if (!r.IsDefinitive()) {
            *definitive = false;
            return false;
        }
        if (r.status == 200) {
            ConsiderLyricsBody(r.body, searchTitle, searchArtist, durationSec, trusted, pick);
        }
        return true;
    };
    auto haveSynced = [&]() { return pick->found && pick->synced; };

    // 1) exact lookup with the metadata exactly as reported
    {
        std::wstring path = L"/api/get?track_name=" + enc(title) + L"&artist_name=" + enc(artist);
        if (!album.empty()) path += L"&album_name=" + enc(album);
        if (durationSec > 0) path += L"&duration=" + std::to_wstring(durationSec);
        if (!handle(request(path), true)) return pick->found;
    }

    // 2) exact lookup with a cleaned title and the main artist only
    if (!haveSynced() && (searchTitle != title || searchArtist != artist)) {
        std::wstring path = L"/api/get?track_name=" + enc(searchTitle) + L"&artist_name=" + enc(searchArtist);
        if (durationSec > 0) path += L"&duration=" + std::to_wstring(durationSec);
        if (!handle(request(path), true)) return pick->found;
    }

    // 3) tolerant free-text search, results scored and filtered
    if (!haveSynced()) {
        const std::wstring path = L"/api/search?q=" + enc(searchTitle + L" " + searchArtist);
        if (!handle(request(path), false)) return pick->found;
    }

    // 4) title-only search (artist spelled differently, or wrong artist in metadata)
    if (!haveSynced()) {
        const std::wstring path = L"/api/search?track_name=" + enc(searchTitle);
        if (!handle(request(path), false)) return pick->found;
    }

    return pick->found;
}

}



DWORD WINAPI LyricsThreadProc(void*) {
    WaitForSingleObject(g_stopEvent, 4000);

    std::wstring lastTitle;
    std::wstring lastArtist;
    double retryAtSec = 0.0;
    int failCount = 0;

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        bool lyricsEnabled = true;
        bool diskCacheOn = false;
        uint64_t diskCapBytes = 0;
        {
            std::lock_guard lock(g_settingsMutex);
            lyricsEnabled = (g_settings.lyrics || g_settings.collapsedLyrics ||
                             g_settings.mediaTabLyrics) && g_settings.media;
            diskCacheOn = g_settings.lyricsCache && g_settings.lyricsCacheMaxMB > 0;
            diskCapBytes = static_cast<uint64_t>(std::max(0, g_settings.lyricsCacheMaxMB)) * 1024ull * 1024ull;
        }
        LyricsCache::IterationGuard cacheGuard(diskCacheOn, diskCapBytes);
        const bool lyricsViewVisible =
            (NowSeconds() - g_lastLyricsViewTime.load(std::memory_order_relaxed)) < 3.0;
        if (!lyricsEnabled || !lyricsViewVisible) {
            WaitForSingleObject(g_stopEvent, 1000);
            continue;
        }

        bool available = false;
        std::wstring title, artist, album;
        int64_t endTicks = 0;
        auto readMedia = [&]() {
            std::lock_guard lock(g_stateMutex);
            available = g_state.media.available;
            title = g_state.media.title;
            artist = g_state.media.artist;
            album = g_state.media.albumTitle;
            endTicks = g_state.media.endTicks;
        };
        readMedia();

        if (!available || title.empty()) {
            if (!lastTitle.empty()) {
                lastTitle.clear();
                lastArtist.clear();
                retryAtSec = 0.0;
                failCount = 0;
                std::lock_guard lock(g_stateMutex);
                g_state.lyrics = LyricsSnapshot{};
            }
            WaitForSingleObject(g_stopEvent, 1000);
            continue;
        }

        const bool sameTrack = (title == lastTitle && artist == lastArtist);
        if (sameTrack) {
            // Already handled. Only continue if a failed fetch is due for a retry.
            if (retryAtSec <= 0.0 || NowSeconds() < retryAtSec) {
                WaitForSingleObject(g_stopEvent, 1000);
                continue;
            }
        } else {
            failCount = 0;
            // Title / artist / duration don't always update together. Let them settle.
            if (WaitForSingleObject(g_stopEvent, 1500) != WAIT_TIMEOUT) break;
            readMedia();
            if (!available || title.empty()) continue;
        }
        retryAtSec = 0.0;
        lastTitle = title;
        lastArtist = artist;

        const LyricsCache::Keys cacheKeys = LyricsCache::BuildKeys(title, artist, album, endTicks);
        {
            LyricsSnapshot cached;
            if (LyricsCache::Lookup(cacheKeys, diskCacheOn, title, artist, &cached)) {
                host::log(L"Lyrics: cache hit for %s - %s", artist.c_str(), title.c_str());
                std::lock_guard lock(g_stateMutex);
                if (g_state.media.title == title && g_state.media.artist == artist) {
                    g_state.lyrics = std::move(cached);
                }
                continue;
            }
        }

        if (!sameTrack) {
            std::lock_guard lock(g_stateMutex);
            g_state.lyrics = LyricsSnapshot{};
            g_state.lyrics.fetching = true;
            g_state.lyrics.matchedTitle = title;
            g_state.lyrics.matchedArtist = artist;
        }

        const int durationSec = endTicks > 0 ? static_cast<int>(endTicks / 10000000.0 + 0.5) : 0;
        host::log(L"Lyrics: requesting %s - %s (attempt %d)", artist.c_str(), title.c_str(), failCount + 1);

        LyricsPick pick;
        bool definitive = true;
        FetchLyricsFromLrclib(title, artist, album, durationSec, &pick, &definitive);

        LyricsSnapshot snap;
        snap.matchedTitle = title;
        snap.matchedArtist = artist;

        if (pick.found) {
            snap.hasData = true;
            snap.synced = pick.synced;
            snap.lines = LyricsLines(std::move(pick.lines));
            failCount = 0;
            LyricsCache::Store(cacheKeys, LyricsCache::Status::Found, snap, diskCacheOn);
        } else if (definitive) {
            snap.notFound = true;   // LRCLIB really has nothing for this track
            failCount = 0;
            host::log(L"Lyrics: genuinely no lyrics for %s - %s", artist.c_str(), title.c_str());
        } else {
            // Network / 429 / 5xx: NOT "not found". Back off and retry.
            ++failCount;
            static const double kBackoff[] = {4.0, 10.0, 30.0, 60.0};
            retryAtSec = NowSeconds() + kBackoff[std::min<size_t>(failCount - 1, 3)];
            snap.fetching = (failCount < 3);
            snap.notFound = (failCount >= 3);
            host::log(L"Lyrics: request FAILED for %s - %s (fail #%d), retrying later.",
                   artist.c_str(), title.c_str(), failCount);
        }

        {
            std::lock_guard lock(g_stateMutex);
            if (g_state.media.title == title && g_state.media.artist == artist) {
                g_state.lyrics = std::move(snap);
            }
        }
        WaitForSingleObject(g_stopEvent, 500);
    }
    return 0;
}


}
