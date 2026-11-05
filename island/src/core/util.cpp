#include "core/util.h"
#include "core/state.h"

namespace island
{




double now_seconds() {
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

float clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

int clamp_int(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}


namespace
{


// ── Lyrics sync offset ───────────────────────────────────────────────────────
// Convention: positive offset = lyrics appear EARLIER, negative = LATER.
// Draw code only reads the atomics, so nothing here takes a lock or polls anything.
constexpr double lyrics_base_lookahead_sec = 0.49;  // built-in baseline; the user offset is added on top

}



// Effective offset = Mod Settings value + right-click nudge, clamped. Called from
// LoadSettings() and from the right-click menu.
void publish_lyrics_offsets(int setting_ms, int setting_bt_ms) {
    g_lyrics_effective_offset_ms.store(
        clamp_int(setting_ms + host::int_value(L"LyricsNudgeMs", 0),
                 -lyrics_offset_limit_ms, lyrics_offset_limit_ms),
        std::memory_order_relaxed);
    g_lyrics_effective_offset_bt_ms.store(
        clamp_int(setting_bt_ms + host::int_value(L"LyricsNudgeBtMs", 0),
                 -lyrics_offset_limit_ms, lyrics_offset_limit_ms),
        std::memory_order_relaxed);
}

// Seconds added to the reported playback position for every lyric surface.
double lyrics_lookahead_sec() {
    const int offset_ms = g_default_output_is_bluetooth.load(std::memory_order_relaxed)
                             ? g_lyrics_effective_offset_bt_ms.load(std::memory_order_relaxed)
                             : g_lyrics_effective_offset_ms.load(std::memory_order_relaxed);
    return lyrics_base_lookahead_sec + static_cast<double>(offset_ms) / 1000.0;
}

// Smoothstep easing (ease in / ease out). Used by the Lyrics tab.
float smooth_step01(float t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}


}
