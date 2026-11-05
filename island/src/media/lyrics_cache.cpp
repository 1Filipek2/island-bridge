#include "media/lyrics_cache.h"
#include "core/util.h"

namespace island
{
namespace LyricsCache
{
namespace
{


constexpr int64_t kFoundTtlSec = 90LL * 24 * 3600;     // found lyrics: refetched after 90 days
constexpr int64_t kNotFoundTtlSec = 3LL * 24 * 3600;   // "not found": retried after 3 days
constexpr size_t kMemoryCapacity = 30;
constexpr int kLooseDurationToleranceSec = 3;          // browsers disagree by a second or two
constexpr uint64_t kMaxEntryBytes = 1024 * 1024;       // anything bigger is treated as corrupt
constexpr double kCleanupMinIntervalSec = 60.0;

struct Entry {
    Status status = Status::Found;
    bool synced = false;
    int64_t savedAt = 0;     // unix seconds
    int durationSec = 0;
    std::wstring looseKey;
    std::wstring fullKey;
    uint64_t looseHash = 0;
    uint64_t fullHash = 0;
    std::vector<LyricsLine> lines;
};

}
}

namespace LyricsCache
{


// ---- small helpers ----------------------------------------------------------
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

}

namespace LyricsCache
{
namespace
{


int64_t UnixNow() {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u = {};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

uint64_t Fnv1a64(const std::string& bytes) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t HashKey(const std::wstring& key) {
    return Fnv1a64(WideToUtf8(key));
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// Lowercase, trim, collapse whitespace; '|' is replaced so a title can never
// forge the key separator.
std::wstring NormalizePart(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    bool pendingSpace = false;
    for (wchar_t ch : in) {
        if (iswspace(ch) || ch == L'|') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return out;
}

}
}

namespace LyricsCache
{


Keys BuildKeys(const std::wstring& title, const std::wstring& artist,
               const std::wstring& album, int64_t endTicks) {
    Keys k;
    k.durationSec = endTicks > 0 ? static_cast<int>(static_cast<double>(endTicks) / 10000000.0 + 0.5) : 0;
    k.loose = NormalizePart(artist) + L"|" + NormalizePart(title);
    k.full = k.loose + L"|" + NormalizePart(album) + L"|" + std::to_wstring(k.durationSec);
    k.looseHash = HashKey(k.loose);
    k.fullHash = HashKey(k.full);
    return k;
}

}

namespace LyricsCache
{
namespace
{


std::wstring FileNameFor(uint64_t looseHash, uint64_t fullHash) {
    wchar_t name[48] = {};
    swprintf_s(name, L"%016llx-%016llx.lrc", static_cast<unsigned long long>(looseHash),
               static_cast<unsigned long long>(fullHash));
    return name;
}

bool DurationCompatible(int a, int b) {
    if (a <= 0 || b <= 0) return true;  // unknown on either side: don't reject
    return std::abs(a - b) <= kLooseDurationToleranceSec;
}

bool EntryExpired(const Entry& e, int64_t nowUnix) {
    if (e.savedAt <= 0 || e.savedAt > nowUnix + 86400) return true;  // missing or future-dated
    return (nowUnix - e.savedAt) > (e.status == Status::Found ? kFoundTtlSec : kNotFoundTtlSec);
}

void CountersAdjust(int dCount, int64_t dBytes) {
    // Single writer (the lyrics thread), so load/store is race-free; the menu only reads.
    int c = g_lyricsCacheFileCount.load(std::memory_order_relaxed) + dCount;
    if (c < 0) c = 0;
    g_lyricsCacheFileCount.store(c, std::memory_order_relaxed);
    int64_t b = static_cast<int64_t>(g_lyricsCacheBytes.load(std::memory_order_relaxed)) + dBytes;
    if (b < 0) b = 0;
    g_lyricsCacheBytes.store(static_cast<uint64_t>(b), std::memory_order_relaxed);
}

// ---- cache folder -------------------------------------------------------------
// Windhawk mod storage directory \lyrics. Strictly kept inside mod storage.
const std::wstring& CacheDir(bool create) {
    static std::wstring dir;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        wchar_t storagePath[MAX_PATH] = {};
        if (host::storage_path(storagePath, ARRAYSIZE(storagePath))) {
            dir = std::wstring(storagePath) + L"\\lyrics";
        }
    }
    if (create && !dir.empty()) {
        // Cheap and idempotent; done on every write so a folder deleted by hand
        // while the mod runs is simply recreated.
        const std::wstring parent = dir.substr(0, dir.find_last_of(L'\\'));
        CreateDirectoryW(parent.c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

// ---- memory layer ---------------------------------------------------------------
// MRU at the back. 30 entries, so linear scans are free.
std::vector<Entry>& Memory() {
    static std::vector<Entry> mem;
    return mem;
}

void PutMemory(Entry e) {
    auto& mem = Memory();
    mem.erase(std::remove_if(mem.begin(), mem.end(),
                             [&](const Entry& other) { return other.fullKey == e.fullKey; }),
              mem.end());
    mem.push_back(std::move(e));
    while (mem.size() > kMemoryCapacity) {
        mem.erase(mem.begin());
    }
}

// Exact key first, otherwise the best loose match. Moves the hit to the MRU slot;
// the returned pointer is valid until the next modification of the vector.
const Entry* FindInMemory(const Keys& k, int64_t nowUnix) {
    auto& mem = Memory();
    int exactIdx = -1;
    int looseIdx = -1;
    int looseScore = 1 << 30;
    for (int i = static_cast<int>(mem.size()) - 1; i >= 0; --i) {
        const Entry& e = mem[static_cast<size_t>(i)];
        if (EntryExpired(e, nowUnix)) continue;
        if (e.fullKey == k.full) {
            exactIdx = i;
            break;
        }
        if (e.looseKey == k.loose && DurationCompatible(e.durationSec, k.durationSec)) {
            const int delta = (e.durationSec > 0 && k.durationSec > 0) ? std::abs(e.durationSec - k.durationSec) : 0;
            const int score = delta + (e.status == Status::NotFound ? 1000 : 0);
            if (score < looseScore) {
                looseScore = score;
                looseIdx = i;
            }
        }
    }
    const int idx = exactIdx >= 0 ? exactIdx : looseIdx;
    if (idx < 0) return nullptr;
    std::rotate(mem.begin() + idx, mem.begin() + idx + 1, mem.end());
    return &mem.back();
}

// ---- file I/O -------------------------------------------------------------------
// "Touching" (last-write time = now) is what makes least-recently-USED eviction
// work. Expiry uses the saved-at stamp inside the file, which is not touched.
void TouchFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

}
}

namespace LyricsCache
{


// Write to <name>.tmp, then rename over the final name: a reader never sees a
// half-written file, and a crash leaves at worst a stray .tmp (swept by cleanup).
bool WriteFileAtomic(const std::wstring& finalPath, const std::string& data) {
    const std::wstring tmp = finalPath + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL wrote = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    if (!wrote || written != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}

namespace LyricsCache
{
namespace
{


std::wstring SingleLine(std::wstring s) {
    s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
    std::replace(s.begin(), s.end(), L'\n', L' ');
    return s;
}

// File layout (UTF-8):
//   DIWLC 1            <- magic + format version (anything else = miss + delete)
//   saved=<unix>       <- (PeekHeader relies on saved/status being lines 2 and 3)
//   status=found|notfound
//   synced=1|0
//   dur=<sec>
//   lkey=<normalized artist|title>
//   fkey=<normalized artist|title|album|dur>
//   title=... / artist=...   (informational only)
//   ---
//   body: synced -> "<timeMs>\t<text>\n" per line, plain -> "<text>\n" per line
std::string SerializeEntry(const Entry& e, const std::wstring& title, const std::wstring& artist) {
    std::string out;
    out += "DIWLC 1\n";
    out += "saved=" + std::to_string(e.savedAt) + "\n";
    out += (e.status == Status::Found) ? "status=found\n" : "status=notfound\n";
    out += e.synced ? "synced=1\n" : "synced=0\n";
    out += "dur=" + std::to_string(e.durationSec) + "\n";
    out += "lkey=" + WideToUtf8(e.looseKey) + "\n";
    out += "fkey=" + WideToUtf8(e.fullKey) + "\n";
    out += "title=" + WideToUtf8(SingleLine(title)) + "\n";
    out += "artist=" + WideToUtf8(SingleLine(artist)) + "\n";
    out += "---\n";

    if (e.status == Status::Found) {
        std::wstring body;
        for (const LyricsLine& line : e.lines) {
            if (e.synced) {
                body += std::to_wstring(line.timeMs);
                body += L'\t';
            }
            body += SingleLine(line.text);
            body += L'\n';
        }
        out += WideToUtf8(body);
    }
    return out;
}

// Returns false for anything that is not a complete, current-version entry.
bool ParseEntry(const std::string& data, Entry* out) {
    size_t pos = 0;
    auto nextLine = [&](std::string* line) -> bool {
        if (pos >= data.size()) return false;
        const size_t end = data.find('\n', pos);
        if (end == std::string::npos) {
            *line = data.substr(pos);
            pos = data.size();
        } else {
            *line = data.substr(pos, end - pos);
            pos = end + 1;
        }
        return true;
    };

    std::string line;
    if (!nextLine(&line) || line != "DIWLC 1") return false;

    Entry e;
    bool haveSaved = false, haveStatus = false, haveLoose = false, haveFull = false, headerEnded = false;
    while (nextLine(&line)) {
        if (line == "---") {
            headerEnded = true;
            break;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "saved") {
            e.savedAt = static_cast<int64_t>(strtoll(val.c_str(), nullptr, 10));
            haveSaved = e.savedAt > 0;
        } else if (key == "status") {
            if (val == "found") { e.status = Status::Found; haveStatus = true; }
            else if (val == "notfound") { e.status = Status::NotFound; haveStatus = true; }
        } else if (key == "synced") {
            e.synced = (val == "1");
        } else if (key == "dur") {
            e.durationSec = static_cast<int>(strtol(val.c_str(), nullptr, 10));
        } else if (key == "lkey") {
            e.looseKey = Utf8ToWide(val);
            haveLoose = !e.looseKey.empty();
        } else if (key == "fkey") {
            e.fullKey = Utf8ToWide(val);
            haveFull = !e.fullKey.empty();
        }
    }
    if (!headerEnded || !haveSaved || !haveStatus || !haveLoose || !haveFull) return false;

    if (e.status == Status::Found) {
        const std::wstring body = Utf8ToWide(data.substr(pos));
        size_t start = 0;
        while (start < body.size()) {
            size_t end = body.find(L'\n', start);
            if (end == std::wstring::npos) end = body.size();
            const std::wstring row = body.substr(start, end - start);
            start = end + 1;
            if (row.empty()) continue;

            LyricsLine ll;
            if (e.synced) {
                const size_t tab = row.find(L'\t');
                if (tab == std::wstring::npos || tab == 0) return false;  // corrupt row
                ll.timeMs = static_cast<int64_t>(wcstoll(row.c_str(), nullptr, 10));
                ll.text = row.substr(tab + 1);
            } else {
                ll.timeMs = -1;
                ll.text = row;
            }
            e.lines.push_back(std::move(ll));
        }
        if (e.lines.empty()) return false;
    }

    e.looseHash = HashKey(e.looseKey);
    e.fullHash = HashKey(e.fullKey);
    *out = std::move(e);
    return true;
}

enum class LoadResult { Ok, Missing, Invalid, Expired };

LoadResult LoadEntry(const std::wstring& path, int64_t nowUnix, Entry* out, uint64_t* sizeOut) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return LoadResult::Missing;

    LARGE_INTEGER size = {};
    std::string data;
    bool readOk = GetFileSizeEx(h, &size) && size.QuadPart > 0 &&
                  static_cast<uint64_t>(size.QuadPart) <= kMaxEntryBytes;
    if (readOk) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        readOk = ReadFile(h, data.data(), static_cast<DWORD>(size.QuadPart), &got, nullptr) &&
                 got == static_cast<DWORD>(size.QuadPart);
    }
    CloseHandle(h);
    if (sizeOut) *sizeOut = size.QuadPart > 0 ? static_cast<uint64_t>(size.QuadPart) : 0;

    Entry e;
    if (!readOk || !ParseEntry(data, &e)) return LoadResult::Invalid;
    if (e.status != Status::Found) return LoadResult::Invalid;  // legacy "no lyrics" file: miss + delete
    if (EntryExpired(e, nowUnix)) return LoadResult::Expired;
    *out = std::move(e);
    return LoadResult::Ok;
}

void RemoveEntryFile(const std::wstring& path, uint64_t knownSize) {
    if (DeleteFileW(path.c_str())) {
        CountersAdjust(-1, -static_cast<int64_t>(knownSize));
    }
}

// ---- disk layer ------------------------------------------------------------------
bool LookupDisk(const Keys& k, int64_t nowUnix, Entry* out) {
    const std::wstring& dir = CacheDir(false);
    if (dir.empty()) return false;

    // 1) Exact key: one direct open, no directory scan.
    const std::wstring exactName = FileNameFor(k.looseHash, k.fullHash);
    {
        const std::wstring path = dir + L"\\" + exactName;
        Entry e;
        uint64_t size = 0;
        const LoadResult r = LoadEntry(path, nowUnix, &e, &size);
        if (r == LoadResult::Ok && e.fullKey == k.full) {  // key check also guards hash collisions
            TouchFile(path);
            *out = std::move(e);
            return true;
        }
        if (r == LoadResult::Invalid || r == LoadResult::Expired) {
            RemoveEntryFile(path, size);  // corrupt / wrong version / expired: miss + delete
        }
    }

    // 2) Loose key: same artist|title, duration within tolerance (album may differ).
    wchar_t pattern[40] = {};
    swprintf_s(pattern, L"%016llx-*.lrc", static_cast<unsigned long long>(k.looseHash));
    std::vector<std::wstring> candidates;
    WIN32_FIND_DATAW fd = {};
    HANDLE find = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (_wcsicmp(fd.cFileName, exactName.c_str()) == 0) continue;
            if (candidates.size() < 16) candidates.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    bool haveBest = false;
    int bestScore = 1 << 30;
    Entry best;
    std::wstring bestPath;
    for (const std::wstring& path : candidates) {
        Entry e;
        uint64_t size = 0;
        const LoadResult r = LoadEntry(path, nowUnix, &e, &size);
        if (r == LoadResult::Invalid || r == LoadResult::Expired) {
            RemoveEntryFile(path, size);
            continue;
        }
        if (r != LoadResult::Ok) continue;
        if (e.looseKey != k.loose || !DurationCompatible(e.durationSec, k.durationSec)) continue;

        const int delta = (e.durationSec > 0 && k.durationSec > 0) ? std::abs(e.durationSec - k.durationSec) : 0;
        const int score = delta + (e.status == Status::NotFound ? 1000 : 0);  // prefer real lyrics
        if (score < bestScore) {
            bestScore = score;
            best = std::move(e);
            bestPath = path;
            haveBest = true;
        }
    }
    if (!haveBest) return false;
    TouchFile(bestPath);
    *out = std::move(best);
    return true;
}

// Cleanup bookkeeping (lyrics thread only).
bool g_cleanupDue = false;
bool g_scannedOnce = false;
double g_lastCleanup = -1e9;
uint64_t g_lastCap = 0;

void WriteEntryToDisk(const Entry& e, const std::wstring& title, const std::wstring& artist) {
    const std::wstring& dir = CacheDir(true);
    if (dir.empty()) return;

    const std::wstring path = dir + L"\\" + FileNameFor(e.looseHash, e.fullHash);
    const std::string data = SerializeEntry(e, title, artist);

    WIN32_FILE_ATTRIBUTE_DATA old = {};
    const bool hadOld = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &old) != 0;
    const uint64_t oldSize = hadOld ? ((static_cast<uint64_t>(old.nFileSizeHigh) << 32) | old.nFileSizeLow) : 0;

    if (!WriteFileAtomic(path, data)) {
        host::log(L"Lyrics cache: could not write %s (error %lu).", path.c_str(), GetLastError());
        return;
    }
    if (hadOld) {
        CountersAdjust(0, static_cast<int64_t>(data.size()) - static_cast<int64_t>(oldSize));
    } else {
        CountersAdjust(1, static_cast<int64_t>(data.size()));
    }
    g_cleanupDue = true;
}

// Reads just enough of a file to know its saved-at stamp and status.
bool PeekHeader(const std::wstring& path, int64_t* savedAt, bool* found) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[256];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 24) return false;
    buf[got] = '\0';

    static const char kMagicLine[] = "DIWLC 1\n";
    if (strncmp(buf, kMagicLine, sizeof(kMagicLine) - 1) != 0) return false;
    const char* p = buf + sizeof(kMagicLine) - 1;
    if (strncmp(p, "saved=", 6) != 0) return false;
    *savedAt = static_cast<int64_t>(strtoll(p + 6, nullptr, 10));
    const char* nl = strchr(p, '\n');
    if (!nl) return false;
    p = nl + 1;
    if (strncmp(p, "status=found", 12) == 0) {
        *found = true;
    } else if (strncmp(p, "status=notfound", 15) == 0) {
        *found = false;
    } else {
        return false;
    }
    return *savedAt > 0;
}

// One pass over the folder: sweep stray .tmp files, delete corrupt / wrong-version
// / expired entries, recompute the counters, then evict least-recently-used files
// down to 90% of the cap if it is exceeded. Files that don't end in .lrc are
// foreign and never touched.
void CleanupPass(uint64_t capBytes) {
    struct FileInfo {
        std::wstring path;
        uint64_t size;
        ULONGLONG lastWrite;
    };
    std::vector<FileInfo> files;
    uint64_t totalBytes = 0;
    const int64_t nowUnix = UnixNow();
    const std::wstring& dir = CacheDir(false);

    if (!dir.empty()) {
        WIN32_FIND_DATAW fd = {};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring name = fd.cFileName;
                const std::wstring path = dir + L"\\" + name;
                if (EndsWith(name, L".tmp")) {
                    DeleteFileW(path.c_str());
                    continue;
                }
                if (!EndsWith(name, L".lrc")) continue;

                int64_t savedAt = 0;
                bool found = false;
                bool drop = !PeekHeader(path, &savedAt, &found);
                if (!drop) {
                    // Not-found entries are no longer kept at all.
                    drop = !found || savedAt > nowUnix + 86400 || (nowUnix - savedAt) > kFoundTtlSec;
                }
                if (drop) {
                    DeleteFileW(path.c_str());
                    continue;
                }

                ULARGE_INTEGER lw = {};
                lw.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                lw.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                files.push_back(FileInfo{path, size, lw.QuadPart});
                totalBytes += size;
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }

    size_t remaining = files.size();
    if (capBytes > 0 && totalBytes > capBytes) {
        std::sort(files.begin(), files.end(),
                  [](const FileInfo& a, const FileInfo& b) { return a.lastWrite < b.lastWrite; });
        const uint64_t target = capBytes - capBytes / 10;  // evict down to 90% so it doesn't thrash
        for (size_t i = 0; i < files.size() && totalBytes > target; ++i) {
            if (DeleteFileW(files[i].path.c_str())) {
                totalBytes -= files[i].size;
                --remaining;
            }
        }
    }

    g_lyricsCacheFileCount.store(static_cast<int>(remaining), std::memory_order_relaxed);
    g_lyricsCacheBytes.store(totalBytes, std::memory_order_relaxed);
    g_lyricsCacheCountsValid.store(true, std::memory_order_relaxed);
}

}
}

namespace LyricsCache
{


void MaybeCleanup(bool diskOn, uint64_t capBytes) noexcept {
    if (!diskOn) return;
    try {
        const double now = NowSeconds();
        const bool capChanged = g_scannedOnce && capBytes != g_lastCap;  // applies a shrunk cap at once
        const bool due = g_cleanupDue && (now - g_lastCleanup) >= kCleanupMinIntervalSec;
        if (!g_scannedOnce || capChanged || due) {
            CleanupPass(capBytes);
            g_scannedOnce = true;
            g_cleanupDue = false;
            g_lastCleanup = now;
            g_lastCap = capBytes;
        }
    } catch (...) {
    }
}

// Deletes every cache file and empties the memory layer. Runs on the lyrics thread
// (the menu only sets a flag). It does not touch g_state.lyrics, so lyrics already
// on screen stay; the track simply isn't cached until it is fetched again.
void ClearAll() noexcept {
    try {
        Memory().clear();
        const std::wstring& dir = CacheDir(false);
        if (!dir.empty()) {
            WIN32_FIND_DATAW fd = {};
            HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const std::wstring name = fd.cFileName;
                    if (EndsWith(name, L".lrc") || EndsWith(name, L".tmp")) {
                        DeleteFileW((dir + L"\\" + name).c_str());
                    }
                } while (FindNextFileW(find, &fd));
                FindClose(find);
            }
        }
        g_lyricsCacheFileCount.store(0, std::memory_order_relaxed);
        g_lyricsCacheBytes.store(0, std::memory_order_relaxed);
        g_scannedOnce = false;  // next housekeeping pass rescans for exact numbers
        g_cleanupDue = false;
        host::log(L"Lyrics cache cleared.");
    } catch (...) {
    }
}

}

namespace LyricsCache
{
namespace
{


// ---- public API used by the lyrics thread -------------------------------------------
void FillSnapshot(const Entry& e, const std::wstring& title, const std::wstring& artist,
                  LyricsSnapshot* out) {
    *out = LyricsSnapshot{};
    out->matchedTitle = title;
    out->matchedArtist = artist;
    if (e.status == Status::Found) {
        out->hasData = true;
        out->synced = e.synced;
        out->lines = LyricsLines(std::vector<LyricsLine>(e.lines));
    } else {
        out->notFound = true;
    }
}

}
}

namespace LyricsCache
{


// True on a hit (memory or disk); *out is then ready to publish: hasData for found
// lyrics, notFound for a cached "no lyrics", fetching never set.
bool Lookup(const Keys& k, bool diskOn, const std::wstring& title, const std::wstring& artist,
            LyricsSnapshot* out) {
    const int64_t nowUnix = UnixNow();

    if (const Entry* m = FindInMemory(k, nowUnix)) {
        FillSnapshot(*m, title, artist, out);
        if (diskOn) {
            // A song replayed from memory must still look "recently used" on disk.
            const std::wstring& dir = CacheDir(false);
            if (!dir.empty()) TouchFile(dir + L"\\" + FileNameFor(m->looseHash, m->fullHash));
        }
        return true;
    }

    if (!diskOn) return false;
    Entry e;
    if (!LookupDisk(k, nowUnix, &e)) return false;
    FillSnapshot(e, title, artist, out);
    PutMemory(std::move(e));
    return true;
}

// Stores a definitive outcome. Memory always; disk only when enabled.
void Store(const Keys& k, Status status, const LyricsSnapshot& snap, bool diskOn) {
    // Only songs that actually have lyrics are ever cached. "Not found" results
    // (YouTube videos, podcasts, obscure tracks) are never remembered.
    if (status != Status::Found) return;

    Entry e;
    e.status = status;
    e.synced = (status == Status::Found) && snap.synced;
    e.savedAt = UnixNow();
    e.durationSec = k.durationSec;
    e.looseKey = k.loose;
    e.fullKey = k.full;
    e.looseHash = k.looseHash;
    e.fullHash = k.fullHash;
    if (status == Status::Found) e.lines = snap.lines.vec();

    if (diskOn) WriteEntryToDisk(e, snap.matchedTitle, snap.matchedArtist);
    PutMemory(std::move(e));
}

}

}
