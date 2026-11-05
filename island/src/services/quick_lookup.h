#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{


// ── Quick Lookup panel: rows + hit testing ───────────────────────────────────
// Shared by the renderer (what to draw) and the window procedure (what a click or
// Enter means) so the two can never disagree about which row is which.
enum class LookupRowKind { Typed, Clipboard, Recent };

struct LookupRow {
    LookupRowKind kind = LookupRowKind::Recent;
    std::wstring query;   // what Enter / a click looks up
    std::wstring label;
    std::wstring detail;
};
std::wstring LookupTypedQuery(const LookupUiState& ui);
std::vector<LookupRow> BuildLookupRows(const std::vector<LookupRecent>& recents,
                                       const LookupUiState& ui);

// The "Recent / Clear" header only exists while the box is empty.
inline bool LookupHasHeader(const LookupUiState& ui, const std::vector<LookupRow>& rows) {
    return !rows.empty() && LookupTypedQuery(ui).empty();
}

inline bool LookupHasRecentRows(const std::vector<LookupRow>& rows) {
    return std::any_of(rows.begin(), rows.end(),
                       [](const LookupRow& r) { return r.kind == LookupRowKind::Recent; });
}

inline bool LookupCaretVisible(double now, double resetAt) {
    return std::fmod(std::max(0.0, now - resetAt), LookupLayout::kCaretPeriod) <
           LookupLayout::kCaretOn;
}

}

namespace island
{
namespace QuickLookup
{


constexpr double kPanelIdleSeconds = 25.0;       // panel closes this long after the last key / mouse move
constexpr size_t kMaxQueryChars = 80;            // longer text is never sent anywhere
std::wstring CollapseWhitespace(const std::wstring& in, size_t maxChars = std::wstring::npos);
std::wstring NormalizeQuery(const std::wstring& raw);

}
}

namespace island
{

void CloseLookupPanel(HWND hwnd);
void StartQuickLookup(HWND hwnd);
bool HandleLookupKeyDown(HWND hwnd, WPARAM vk);
bool HandleLookupChar(wchar_t ch);
bool HandleLookupClick(int x, int y);
void HandleLookupMouseMove(int x, int y);
LPCWSTR LookupCursorAt(int x, int y);
void ApplyLookupHotkey();

}
