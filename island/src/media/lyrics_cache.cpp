#include "media/lyrics_cache.h"
#include "core/util.h"

namespace island
{
namespace lyrics_cache
{
namespace
{
constexpr std::int64_t found_ttl_sec = 90LL * 24 * 3600;    // found lyrics: refetched after 90 days
constexpr std::int64_t not_found_ttl_sec = 3LL * 24 * 3600; // "not found": retried after 3 days
constexpr std::size_t memory_capacity = 30;
constexpr int loose_duration_tolerance_sec = 3;   // browsers disagree by a second or two
constexpr std::uint64_t max_entry_bytes = 1024 * 1024; // anything bigger is treated as corrupt
constexpr double cleanup_min_interval_sec = 60.0;

struct Entry
{
    Status status = Status::Found;
    bool synced = false;
    std::int64_t saved_at = 0; // unix seconds
    int duration_sec = 0;
    std::wstring loose_key;
    std::wstring full_key;
    std::uint64_t loose_hash = 0;
    std::uint64_t full_hash = 0;
    std::vector<LyricsLine> lines;
};

}
}

namespace lyrics_cache
{
std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty())
    {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
    {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty())
    {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0)
    {
        return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

}

namespace lyrics_cache
{
namespace
{
std::int64_t unix_now()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u = {};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<std::int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::uint64_t fnv1a64(const std::string& bytes)
{
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : bytes)
    {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::uint64_t hash_key(const std::wstring& key)
{
    return fnv1a64(wide_to_utf8(key));
}

bool ends_with(const std::wstring& s, const wchar_t* suffix)
{
    const std::size_t n = std::wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// Lowercase, trim, collapse whitespace; '|' is replaced so a title cannot forge the key separator.
std::wstring normalize_part(const std::wstring& in)
{
    std::wstring out;
    out.reserve(in.size());
    bool pending_space = false;
    for (wchar_t ch : in)
    {
        if (std::iswspace(ch) || ch == L'|')
        {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space)
        {
            out.push_back(L' ');
            pending_space = false;
        }
        out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return out;
}

}
}

namespace lyrics_cache
{
Keys build_keys(const std::wstring& title, const std::wstring& artist, const std::wstring& album, std::int64_t end_ticks)
{
    Keys k;
    k.duration_sec = end_ticks > 0 ? static_cast<int>(static_cast<double>(end_ticks) / 10000000.0 + 0.5) : 0;
    k.loose = normalize_part(artist) + L"|" + normalize_part(title);
    k.full = k.loose + L"|" + normalize_part(album) + L"|" + std::to_wstring(k.duration_sec);
    k.loose_hash = hash_key(k.loose);
    k.full_hash = hash_key(k.full);
    return k;
}

}

namespace lyrics_cache
{
namespace
{
std::wstring file_name_for(std::uint64_t loose_hash, std::uint64_t full_hash)
{
    wchar_t name[48] = {};
    swprintf_s(name, L"%016llx-%016llx.lrc", static_cast<unsigned long long>(loose_hash),
               static_cast<unsigned long long>(full_hash));
    return name;
}

bool duration_compatible(int a, int b)
{
    if (a <= 0 || b <= 0)
    {
        return true;
    }
    return std::abs(a - b) <= loose_duration_tolerance_sec;
}

bool entry_expired(const Entry& e, std::int64_t now_unix)
{
    if (e.saved_at <= 0 || e.saved_at > now_unix + 86400)
    {
        return true;
    }
    return (now_unix - e.saved_at) > (e.status == Status::Found ? found_ttl_sec : not_found_ttl_sec);
}

void counters_adjust(int d_count, std::int64_t d_bytes)
{
    // Single writer (the lyrics thread); the menu only reads.
    int c = g_lyrics_cache_file_count.load(std::memory_order_relaxed) + d_count;
    if (c < 0)
    {
        c = 0;
    }
    g_lyrics_cache_file_count.store(c, std::memory_order_relaxed);
    std::int64_t b = static_cast<std::int64_t>(g_lyrics_cache_bytes.load(std::memory_order_relaxed)) + d_bytes;
    if (b < 0)
    {
        b = 0;
    }
    g_lyrics_cache_bytes.store(static_cast<std::uint64_t>(b), std::memory_order_relaxed);
}

// cache folder: <island storage>\lyrics.
const std::wstring& cache_dir(bool create)
{
    static std::wstring dir;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        wchar_t storage_path[MAX_PATH] = {};
        if (host::storage_path(storage_path, ARRAYSIZE(storage_path)))
        {
            dir = std::wstring(storage_path) + L"\\lyrics";
        }
    }
    if (create && !dir.empty())
    {
        const std::wstring parent = dir.substr(0, dir.find_last_of(L'\\'));
        CreateDirectoryW(parent.c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

// memory layer: MRU at the back, 30 entries.
std::vector<Entry>& memory()
{
    static std::vector<Entry> mem;
    return mem;
}

void put_memory(Entry e)
{
    auto& mem = memory();
    mem.erase(std::remove_if(mem.begin(), mem.end(),
                             [&](const Entry& other)
                             {
                                 return other.full_key == e.full_key;
                             }),
              mem.end());
    mem.push_back(std::move(e));
    while (mem.size() > memory_capacity)
    {
        mem.erase(mem.begin());
    }
}

const Entry* find_in_memory(const Keys& k, std::int64_t now_unix)
{
    auto& mem = memory();
    int exact_idx = -1;
    int loose_idx = -1;
    int loose_score = 1 << 30;
    for (int i = static_cast<int>(mem.size()) - 1; i >= 0; --i)
    {
        const Entry& e = mem[static_cast<std::size_t>(i)];
        if (entry_expired(e, now_unix))
        {
            continue;
        }
        if (e.full_key == k.full)
        {
            exact_idx = i;
            break;
        }
        if (e.loose_key == k.loose && duration_compatible(e.duration_sec, k.duration_sec))
        {
            const int delta =
                (e.duration_sec > 0 && k.duration_sec > 0) ? std::abs(e.duration_sec - k.duration_sec) : 0;
            const int score = delta + (e.status == Status::NotFound ? 1000 : 0);
            if (score < loose_score)
            {
                loose_score = score;
                loose_idx = i;
            }
        }
    }
    const int idx = exact_idx >= 0 ? exact_idx : loose_idx;
    if (idx < 0)
    {
        return nullptr;
    }
    std::rotate(mem.begin() + idx, mem.begin() + idx + 1, mem.end());
    return &mem.back();
}

// File I/O. Touching (last-write = now) drives LRU eviction; expiry uses the saved stamp in the file.
void touch_file(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

}
}

namespace lyrics_cache
{
// Write <name>.tmp, then rename: a reader never sees a half-written file.
bool write_file_atomic(const std::wstring& final_path, const std::string& data)
{
    const std::wstring tmp = final_path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0;
    const BOOL wrote = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    if (!wrote || written != data.size())
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}

namespace lyrics_cache
{
namespace
{
std::wstring single_line(std::wstring s)
{
    s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
    std::replace(s.begin(), s.end(), L'\n', L' ');
    return s;
}

// File layout (UTF-8):
//   DIWLC 1            magic + format version (anything else = miss + delete)
//   saved=<unix>       (peek_header relies on saved/status being lines 2 and 3)
//   status=found|notfound
//   synced=1|0
//   dur=<sec>
//   lkey=<normalized artist|title>
//   fkey=<normalized artist|title|album|dur>
//   title=... / artist=...   (informational only)
//   ---
//   body: synced -> "<time_ms>\t<text>\n" per line, plain -> "<text>\n" per line
std::string serialize_entry(const Entry& e, const std::wstring& title, const std::wstring& artist)
{
    std::string out;
    out += "DIWLC 1\n";
    out += "saved=" + std::to_string(e.saved_at) + "\n";
    out += (e.status == Status::Found) ? "status=found\n" : "status=notfound\n";
    out += e.synced ? "synced=1\n" : "synced=0\n";
    out += "dur=" + std::to_string(e.duration_sec) + "\n";
    out += "lkey=" + wide_to_utf8(e.loose_key) + "\n";
    out += "fkey=" + wide_to_utf8(e.full_key) + "\n";
    out += "title=" + wide_to_utf8(single_line(title)) + "\n";
    out += "artist=" + wide_to_utf8(single_line(artist)) + "\n";
    out += "---\n";

    if (e.status == Status::Found)
    {
        std::wstring body;
        for (const LyricsLine& line : e.lines)
        {
            if (e.synced)
            {
                body += std::to_wstring(line.time_ms);
                body += L'\t';
            }
            body += single_line(line.text);
            body += L'\n';
        }
        out += wide_to_utf8(body);
    }
    return out;
}

bool parse_entry(const std::string& data, Entry* out)
{
    std::size_t pos = 0;
    auto next_line = [&](std::string* line) -> bool
    {
        if (pos >= data.size())
        {
            return false;
        }
        const std::size_t end = data.find('\n', pos);
        if (end == std::string::npos)
        {
            *line = data.substr(pos);
            pos = data.size();
        }
        else
        {
            *line = data.substr(pos, end - pos);
            pos = end + 1;
        }
        return true;
    };

    std::string line;
    if (!next_line(&line) || line != "DIWLC 1")
    {
        return false;
    }

    Entry e;
    bool have_saved = false, have_status = false, have_loose = false, have_full = false, header_ended = false;
    while (next_line(&line))
    {
        if (line == "---")
        {
            header_ended = true;
            break;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "saved")
        {
            e.saved_at = static_cast<std::int64_t>(strtoll(val.c_str(), nullptr, 10));
            have_saved = e.saved_at > 0;
        }
        else if (key == "status")
        {
            if (val == "found")
            {
                e.status = Status::Found;
                have_status = true;
            }
            else if (val == "notfound")
            {
                e.status = Status::NotFound;
                have_status = true;
            }
        }
        else if (key == "synced")
        {
            e.synced = (val == "1");
        }
        else if (key == "dur")
        {
            e.duration_sec = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
        }
        else if (key == "lkey")
        {
            e.loose_key = utf8_to_wide(val);
            have_loose = !e.loose_key.empty();
        }
        else if (key == "fkey")
        {
            e.full_key = utf8_to_wide(val);
            have_full = !e.full_key.empty();
        }
    }
    if (!header_ended || !have_saved || !have_status || !have_loose || !have_full)
    {
        return false;
    }

    if (e.status == Status::Found)
    {
        const std::wstring body = utf8_to_wide(data.substr(pos));
        std::size_t start = 0;
        while (start < body.size())
        {
            std::size_t end = body.find(L'\n', start);
            if (end == std::wstring::npos)
            {
                end = body.size();
            }
            const std::wstring row = body.substr(start, end - start);
            start = end + 1;
            if (row.empty())
            {
                continue;
            }

            LyricsLine ll;
            if (e.synced)
            {
                const std::size_t tab = row.find(L'\t');
                if (tab == std::wstring::npos || tab == 0)
                {
                    return false;
                }
                ll.time_ms = static_cast<std::int64_t>(wcstoll(row.c_str(), nullptr, 10));
                ll.text = row.substr(tab + 1);
            }
            else
            {
                ll.time_ms = -1;
                ll.text = row;
            }
            e.lines.push_back(std::move(ll));
        }
        if (e.lines.empty())
        {
            return false;
        }
    }

    e.loose_hash = hash_key(e.loose_key);
    e.full_hash = hash_key(e.full_key);
    *out = std::move(e);
    return true;
}

enum class LoadResult
{
    Ok,
    Missing,
    Invalid,
    Expired
};

LoadResult load_entry(const std::wstring& path, std::int64_t now_unix, Entry* out, std::uint64_t* size_out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return LoadResult::Missing;
    }

    LARGE_INTEGER size = {};
    std::string data;
    bool read_ok =
        GetFileSizeEx(h, &size) && size.QuadPart > 0 && static_cast<std::uint64_t>(size.QuadPart) <= max_entry_bytes;
    if (read_ok)
    {
        data.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        read_ok = ReadFile(h, data.data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                  got == static_cast<DWORD>(size.QuadPart);
    }
    CloseHandle(h);
    if (size_out)
    {
        *size_out = size.QuadPart > 0 ? static_cast<std::uint64_t>(size.QuadPart) : 0;
    }

    Entry e;
    if (!read_ok || !parse_entry(data, &e))
    {
        return LoadResult::Invalid;
    }
    if (e.status != Status::Found)
    {
        return LoadResult::Invalid;
    }
    if (entry_expired(e, now_unix))
    {
        return LoadResult::Expired;
    }
    *out = std::move(e);
    return LoadResult::Ok;
}

void remove_entry_file(const std::wstring& path, std::uint64_t known_size)
{
    if (DeleteFileW(path.c_str()))
    {
        counters_adjust(-1, -static_cast<std::int64_t>(known_size));
    }
}

// Disk layer
bool lookup_disk(const Keys& k, std::int64_t now_unix, Entry* out)
{
    const std::wstring& dir = cache_dir(false);
    if (dir.empty())
    {
        return false;
    }

    // 1) Exact key: one direct open, no directory scan.
    const std::wstring exact_name = file_name_for(k.loose_hash, k.full_hash);
    {
        const std::wstring path = dir + L"\\" + exact_name;
        Entry e;
        std::uint64_t size = 0;
        const LoadResult r = load_entry(path, now_unix, &e, &size);
        if (r == LoadResult::Ok && e.full_key == k.full)
        { // key check also guards hash collisions
            touch_file(path);
            *out = std::move(e);
            return true;
        }
        if (r == LoadResult::Invalid || r == LoadResult::Expired)
        {
            remove_entry_file(path, size);
        }
    }

    // 2) Loose key: same artist|title, duration within tolerance.
    wchar_t pattern[40] = {};
    swprintf_s(pattern, L"%016llx-*.lrc", static_cast<unsigned long long>(k.loose_hash));
    std::vector<std::wstring> candidates;
    WIN32_FIND_DATAW fd = {};
    HANDLE find = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            if (_wcsicmp(fd.cFileName, exact_name.c_str()) == 0)
            {
                continue;
            }
            if (candidates.size() < 16)
            {
                candidates.push_back(dir + L"\\" + fd.cFileName);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    bool have_best = false;
    int best_score = 1 << 30;
    Entry best;
    std::wstring best_path;
    for (const std::wstring& path : candidates)
    {
        Entry e;
        std::uint64_t size = 0;
        const LoadResult r = load_entry(path, now_unix, &e, &size);
        if (r == LoadResult::Invalid || r == LoadResult::Expired)
        {
            remove_entry_file(path, size);
            continue;
        }
        if (r != LoadResult::Ok)
        {
            continue;
        }
        if (e.loose_key != k.loose || !duration_compatible(e.duration_sec, k.duration_sec))
        {
            continue;
        }

        const int delta = (e.duration_sec > 0 && k.duration_sec > 0) ? std::abs(e.duration_sec - k.duration_sec) : 0;
        const int score = delta + (e.status == Status::NotFound ? 1000 : 0); // prefer real lyrics
        if (score < best_score)
        {
            best_score = score;
            best = std::move(e);
            best_path = path;
            have_best = true;
        }
    }
    if (!have_best)
    {
        return false;
    }
    touch_file(best_path);
    *out = std::move(best);
    return true;
}

bool g_cleanup_due = false;
bool g_scanned_once = false;
double g_last_cleanup = -1e9;
std::uint64_t g_last_cap = 0;

void write_entry_to_disk(const Entry& e, const std::wstring& title, const std::wstring& artist)
{
    const std::wstring& dir = cache_dir(true);
    if (dir.empty())
    {
        return;
    }

    const std::wstring path = dir + L"\\" + file_name_for(e.loose_hash, e.full_hash);
    const std::string data = serialize_entry(e, title, artist);

    WIN32_FILE_ATTRIBUTE_DATA old = {};
    const bool had_old = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &old) != 0;
    const std::uint64_t old_size = had_old ? ((static_cast<std::uint64_t>(old.nFileSizeHigh) << 32) | old.nFileSizeLow) : 0;

    if (!write_file_atomic(path, data))
    {
        host::log(L"Lyrics cache: could not write %s (error %lu).", path.c_str(), GetLastError());
        return;
    }
    if (had_old)
    {
        counters_adjust(0, static_cast<std::int64_t>(data.size()) - static_cast<std::int64_t>(old_size));
    }
    else
    {
        counters_adjust(1, static_cast<std::int64_t>(data.size()));
    }
    g_cleanup_due = true;
}

bool peek_header(const std::wstring& path, std::int64_t* saved_at, bool* found)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    char buf[256];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 24)
    {
        return false;
    }
    buf[got] = '\0';

    static const char magic_line[] = "DIWLC 1\n";
    if (std::strncmp(buf, magic_line, sizeof(magic_line) - 1) != 0)
    {
        return false;
    }
    const char* p = buf + sizeof(magic_line) - 1;
    if (std::strncmp(p, "saved=", 6) != 0)
    {
        return false;
    }
    *saved_at = static_cast<std::int64_t>(strtoll(p + 6, nullptr, 10));
    const char* nl = std::strchr(p, '\n');
    if (!nl)
    {
        return false;
    }
    p = nl + 1;
    if (std::strncmp(p, "status=found", 12) == 0)
    {
        *found = true;
    }
    else if (std::strncmp(p, "status=notfound", 15) == 0)
    {
        *found = false;
    }
    else
    {
        return false;
    }
    return *saved_at > 0;
}

// One pass: sweep .tmp files, delete corrupt or expired entries, recount, then evict least recently
// used files down to 90% of the cap. Files not ending in .lrc are never touched.
void cleanup_pass(std::uint64_t cap_bytes)
{
    struct FileInfo
    {
        std::wstring path;
        std::uint64_t size;
        ULONGLONG last_write;
    };

    std::vector<FileInfo> files;
    std::uint64_t total_bytes = 0;
    const std::int64_t now_unix = unix_now();
    const std::wstring& dir = cache_dir(false);

    if (!dir.empty())
    {
        WIN32_FIND_DATAW fd = {};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                const std::wstring name = fd.cFileName;
                const std::wstring path = dir + L"\\" + name;
                if (ends_with(name, L".tmp"))
                {
                    DeleteFileW(path.c_str());
                    continue;
                }
                if (!ends_with(name, L".lrc"))
                {
                    continue;
                }

                std::int64_t saved_at = 0;
                bool found = false;
                bool drop = !peek_header(path, &saved_at, &found);
                if (!drop)
                {
                    drop = !found || saved_at > now_unix + 86400 || (now_unix - saved_at) > found_ttl_sec;
                }
                if (drop)
                {
                    DeleteFileW(path.c_str());
                    continue;
                }

                ULARGE_INTEGER lw = {};
                lw.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                lw.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                const std::uint64_t size = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                files.push_back(FileInfo{path, size, lw.QuadPart});
                total_bytes += size;
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }

    std::size_t remaining = files.size();
    if (cap_bytes > 0 && total_bytes > cap_bytes)
    {
        std::sort(files.begin(), files.end(),
                  [](const FileInfo& a, const FileInfo& b)
                  {
                      return a.last_write < b.last_write;
                  });
        const std::uint64_t target = cap_bytes - cap_bytes / 10;
        for (std::size_t i = 0; i < files.size() && total_bytes > target; ++i)
        {
            if (DeleteFileW(files[i].path.c_str()))
            {
                total_bytes -= files[i].size;
                --remaining;
            }
        }
    }

    g_lyrics_cache_file_count.store(static_cast<int>(remaining), std::memory_order_relaxed);
    g_lyrics_cache_bytes.store(total_bytes, std::memory_order_relaxed);
    g_lyrics_cache_counts_valid.store(true, std::memory_order_relaxed);
}

}
}

namespace lyrics_cache
{
void maybe_cleanup(bool disk_on, std::uint64_t cap_bytes) noexcept
{
    if (!disk_on)
    {
        return;
    }
    try
    {
        const double now = now_seconds();
        const bool cap_changed = g_scanned_once && cap_bytes != g_last_cap;
        const bool due = g_cleanup_due && (now - g_last_cleanup) >= cleanup_min_interval_sec;
        if (!g_scanned_once || cap_changed || due)
        {
            cleanup_pass(cap_bytes);
            g_scanned_once = true;
            g_cleanup_due = false;
            g_last_cleanup = now;
            g_last_cap = cap_bytes;
        }
    }
    catch (...)
    {
    }
}

void clear_all() noexcept
{
    try
    {
        memory().clear();
        const std::wstring& dir = cache_dir(false);
        if (!dir.empty())
        {
            WIN32_FIND_DATAW fd = {};
            HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        continue;
                    }
                    const std::wstring name = fd.cFileName;
                    if (ends_with(name, L".lrc") || ends_with(name, L".tmp"))
                    {
                        DeleteFileW((dir + L"\\" + name).c_str());
                    }
                } while (FindNextFileW(find, &fd));
                FindClose(find);
            }
        }
        g_lyrics_cache_file_count.store(0, std::memory_order_relaxed);
        g_lyrics_cache_bytes.store(0, std::memory_order_relaxed);
        g_scanned_once = false;
        g_cleanup_due = false;
        host::log(L"Lyrics cache cleared.");
    }
    catch (...)
    {
    }
}

}

namespace lyrics_cache
{
namespace
{
// Used by the lyrics thread
void fill_snapshot(const Entry& e, const std::wstring& title, const std::wstring& artist, LyricsSnapshot* out)
{
    *out = LyricsSnapshot{};
    out->matched_title = title;
    out->matched_artist = artist;
    if (e.status == Status::Found)
    {
        out->has_data = true;
        out->synced = e.synced;
        out->lines = LyricsLines(std::vector<LyricsLine>(e.lines));
    }
    else
    {
        out->not_found = true;
    }
}

}
}

namespace lyrics_cache
{
bool lookup(const Keys& k, bool disk_on, const std::wstring& title, const std::wstring& artist, LyricsSnapshot* out)
{
    const std::int64_t now_unix = unix_now();

    if (const Entry* m = find_in_memory(k, now_unix))
    {
        fill_snapshot(*m, title, artist, out);
        if (disk_on)
        {
            const std::wstring& dir = cache_dir(false);
            if (!dir.empty())
            {
                touch_file(dir + L"\\" + file_name_for(m->loose_hash, m->full_hash));
            }
        }
        return true;
    }

    if (!disk_on)
    {
        return false;
    }
    Entry e;
    if (!lookup_disk(k, now_unix, &e))
    {
        return false;
    }
    fill_snapshot(e, title, artist, out);
    put_memory(std::move(e));
    return true;
}

void store(const Keys& k, Status status, const LyricsSnapshot& snap, bool disk_on)
{
    // Only tracks that have lyrics are cached; "not found" is never remembered.
    if (status != Status::Found)
    {
        return;
    }

    Entry e;
    e.status = status;
    e.synced = (status == Status::Found) && snap.synced;
    e.saved_at = unix_now();
    e.duration_sec = k.duration_sec;
    e.loose_key = k.loose;
    e.full_key = k.full;
    e.loose_hash = k.loose_hash;
    e.full_hash = k.full_hash;
    if (status == Status::Found)
    {
        e.lines = snap.lines.vec();
    }

    if (disk_on)
    {
        write_entry_to_disk(e, snap.matched_title, snap.matched_artist);
    }
    put_memory(std::move(e));
}

}

}
