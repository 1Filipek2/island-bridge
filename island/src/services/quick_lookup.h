#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{
// Quick lookup rows, shared by the renderer and the window procedure so both agree on what a row is.
enum class LookupRowKind
{
    Typed,
    Clipboard,
    Recent
};

struct LookupRow
{
    LookupRowKind kind = LookupRowKind::Recent;
    std::wstring query;
    std::wstring label;
    std::wstring detail;
};

std::wstring lookup_typed_query(const LookupUiState& ui);
std::vector<LookupRow> build_lookup_rows(const std::vector<LookupRecent>& recents, const LookupUiState& ui);

inline bool lookup_has_header(const LookupUiState& ui, const std::vector<LookupRow>& rows)
{
    return !rows.empty() && lookup_typed_query(ui).empty();
}

inline bool lookup_has_recent_rows(const std::vector<LookupRow>& rows)
{
    return std::any_of(rows.begin(), rows.end(),
                       [](const LookupRow& r)
                       {
                           return r.kind == LookupRowKind::Recent;
                       });
}

inline bool lookup_caret_visible(double now, double reset_at)
{
    return std::fmod(std::max(0.0, now - reset_at), lookup_layout::caret_period) < lookup_layout::caret_on;
}

}

namespace island
{
namespace quick_lookup
{
constexpr double panel_idle_seconds = 25.0; // panel closes this long after the last key / mouse move
constexpr size_t max_query_chars = 80;      // longer text is never sent anywhere
std::wstring collapse_whitespace(const std::wstring& in, size_t max_chars = std::wstring::npos);
std::wstring normalize_query(const std::wstring& raw);

}
}

namespace island
{
void close_lookup_panel(HWND hwnd);
void start_quick_lookup(HWND hwnd);
bool handle_lookup_key_down(HWND hwnd, WPARAM vk);
bool handle_lookup_char(wchar_t ch);
bool handle_lookup_click(int x, int y);
void handle_lookup_mouse_move(int x, int y);
LPCWSTR lookup_cursor_at(int x, int y);
void apply_lookup_hotkey();

}
