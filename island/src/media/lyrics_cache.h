#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{


// ── Lyrics cache ─────────────────────────────────────────────────────────────
// Lookup order is memory -> disk -> network; a network result is written to both.
// Everything below runs on the lyrics thread ONLY, so the memory cache and the
// directory bookkeeping need no locks. The only cross-thread state is the four
// atomics here: the right-click menu reads the counters and sets the clear flag,
// and never touches the disk itself.
inline std::atomic<bool> g_lyricsCacheClearRequested{false};
inline std::atomic<bool> g_lyricsCacheCountsValid{false};   // false until the first folder scan
inline std::atomic<int> g_lyricsCacheFileCount{0};
inline std::atomic<uint64_t> g_lyricsCacheBytes{0};

}

namespace island
{
namespace LyricsCache
{


enum class Status { Found, NotFound };

struct Keys {
    std::wstring loose;      // normalized artist|title
    std::wstring full;       // normalized artist|title|album|durationSec
    int durationSec = 0;
    uint64_t looseHash = 0;
    uint64_t fullHash = 0;
};
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
Keys BuildKeys(const std::wstring& title, const std::wstring& artist,
               const std::wstring& album, int64_t endTicks);
bool WriteFileAtomic(const std::wstring& finalPath, const std::string& data);
void MaybeCleanup(bool diskOn, uint64_t capBytes) noexcept;
void ClearAll() noexcept;

// Constructed at the top of every lyrics-loop iteration: the constructor services
// a pending "clear" request, the destructor runs housekeeping once the iteration
// is done (including on `continue`). So the first folder scan happens AFTER the
// first track's lyrics were handled, never in front of them.
class IterationGuard {
   public:
    IterationGuard(bool diskOn, uint64_t capBytes) : diskOn_(diskOn), capBytes_(capBytes) {
        if (g_lyricsCacheClearRequested.exchange(false)) {
            ClearAll();
        }
    }
    ~IterationGuard() { MaybeCleanup(diskOn_, capBytes_); }
    IterationGuard(const IterationGuard&) = delete;
    IterationGuard& operator=(const IterationGuard&) = delete;

   private:
    bool diskOn_;
    uint64_t capBytes_;
};
bool Lookup(const Keys& k, bool diskOn, const std::wstring& title, const std::wstring& artist,
            LyricsSnapshot* out);
void Store(const Keys& k, Status status, const LyricsSnapshot& snap, bool diskOn);

}
}
