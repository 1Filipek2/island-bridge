#include "core/util.h"
#include "core/state.h"

namespace island
{




double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

int ClampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}


namespace
{


// ── Lyrics sync offset ───────────────────────────────────────────────────────
// Convention: positive offset = lyrics appear EARLIER, negative = LATER.
// Draw code only reads the atomics, so nothing here takes a lock or polls anything.
constexpr double kLyricsBaseLookaheadSec = 0.49;  // built-in baseline; the user offset is added on top

}



// Effective offset = Mod Settings value + right-click nudge, clamped. Called from
// LoadSettings() and from the right-click menu.
void PublishLyricsOffsets(int settingMs, int settingBtMs) {
    g_lyricsEffectiveOffsetMs.store(
        ClampInt(settingMs + host::int_value(L"LyricsNudgeMs", 0),
                 -kLyricsOffsetLimitMs, kLyricsOffsetLimitMs),
        std::memory_order_relaxed);
    g_lyricsEffectiveOffsetBtMs.store(
        ClampInt(settingBtMs + host::int_value(L"LyricsNudgeBtMs", 0),
                 -kLyricsOffsetLimitMs, kLyricsOffsetLimitMs),
        std::memory_order_relaxed);
}

// Seconds added to the reported playback position for every lyric surface.
double LyricsLookaheadSec() {
    const int offsetMs = g_defaultOutputIsBluetooth.load(std::memory_order_relaxed)
                             ? g_lyricsEffectiveOffsetBtMs.load(std::memory_order_relaxed)
                             : g_lyricsEffectiveOffsetMs.load(std::memory_order_relaxed);
    return kLyricsBaseLookaheadSec + static_cast<double>(offsetMs) / 1000.0;
}

// Smoothstep easing (ease in / ease out). Used by the Lyrics tab.
float SmoothStep01(float t) {
    t = Clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}


}
