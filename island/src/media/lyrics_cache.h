#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{
// Lyrics cache: memory, then disk, then network. Runs on the lyrics thread only; the menu touches just
// these atomics.
inline std::atomic<bool> g_lyrics_cache_clear_requested{false};
inline std::atomic<bool> g_lyrics_cache_counts_valid{false}; // false until the first folder scan
inline std::atomic<int> g_lyrics_cache_file_count{0};
inline std::atomic<std::uint64_t> g_lyrics_cache_bytes{0};

}

namespace island
{
namespace lyrics_cache
{
enum class Status
{
    Found,
    NotFound
};

struct Keys
{
    std::wstring loose; // normalized artist|title
    std::wstring full;  // normalized artist|title|album|duration_sec
    int duration_sec = 0;
    std::uint64_t loose_hash = 0;
    std::uint64_t full_hash = 0;
};

std::string wide_to_utf8(const std::wstring& w);
std::wstring utf8_to_wide(const std::string& s);
Keys build_keys(const std::wstring& title, const std::wstring& artist, const std::wstring& album, std::int64_t end_ticks);
bool write_file_atomic(const std::wstring& final_path, const std::string& data);
void maybe_cleanup(bool disk_on, std::uint64_t cap_bytes) noexcept;
void clear_all() noexcept;

// Created at the top of each lyrics loop iteration: handles a pending clear, then runs housekeeping
// when the iteration ends.
class IterationGuard
{
public:
    IterationGuard(bool disk_on, std::uint64_t cap_bytes) : disk_on_(disk_on), cap_bytes_(cap_bytes)
    {
        if (g_lyrics_cache_clear_requested.exchange(false))
        {
            clear_all();
        }
    }

    ~IterationGuard()
    {
        maybe_cleanup(disk_on_, cap_bytes_);
    }

    IterationGuard(const IterationGuard&) = delete;
    IterationGuard& operator=(const IterationGuard&) = delete;

private:
    bool disk_on_;
    std::uint64_t cap_bytes_;
};

bool lookup(const Keys& k, bool disk_on, const std::wstring& title, const std::wstring& artist, LyricsSnapshot* out);
void store(const Keys& k, Status status, const LyricsSnapshot& snap, bool disk_on);

}
}
