#include "ui/render/renderer_impl.h"

namespace island
{

// Content height the lookup card needs, so the island animates to exactly what draw_lookup paints.
float Renderer::measure_lookup_card(const SharedState& state, const Settings& settings)
{
    const LookupSnapshot& lk = state.lookup;
    if (lk.status == LookupStatus::Search)
    {
        const std::vector<LookupRow> rows = build_lookup_rows(state.lookup_recent, g_lookup_ui);
        return lookup_layout::search_height(static_cast<int>(rows.size()), lookup_has_header(g_lookup_ui, rows));
    }
    if (lk.status == LookupStatus::Found)
    {
        const float card = lookup_card_height(lk, ensure_lookup_fit(lk, settings), lookup_grow(settings));
        return clamp(card + lookup_layout::field_h + lookup_layout::field_gap, lookup_layout::simple_height,
                     lookup_layout::max_height);
    }
    return lookup_layout::status_height();
}

float Renderer::lookup_grow(const Settings& settings) const
{
    return std::max(1.0f, clamp(settings.text_scale, 0.7f, 1.6f));
}

float Renderer::lookup_card_height(const LookupSnapshot& lk, const LookupFit& fit, float grow) const
{
    float inner = fit.body_h;
    if (!fit.example.empty())
    {
        inner += lookup_layout::example_gap + fit.example_h;
    }
    const float h = lookup_layout::pad_top + lookup_layout::title_h * grow +
                    (lk.subtitle.empty() ? 2.0f : lookup_layout::subtitle_h * grow) + lookup_layout::card_gap + inner +
                    lookup_layout::card_pad * 2.0f + lookup_layout::pad_bottom;
    return clamp(h, lookup_layout::simple_height, lookup_layout::max_height);
}

void Renderer::ensure_lookup_formats(const Settings& settings)
{
    const float ts = clamp(settings.text_scale, 0.7f, 1.6f);
    if (lookup_body_format_ && lookup_example_format_ && std::fabs(lookup_format_scale_ - ts) < 0.001f &&
        lookup_format_family_ == settings.font_family)
    {
        return;
    }
    lookup_body_format_.Reset();
    lookup_example_format_.Reset();
    lookup_fit_.valid = false;

    const wchar_t* family = settings.font_family.empty() ? L"Segoe UI Variable Small" : settings.font_family.c_str();
    auto make = [&](DWRITE_FONT_STYLE style, float size, ComPtr<IDWriteTextFormat>& out)
    {
        HRESULT hr = dwrite_factory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, style,
                                                       DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
        if (FAILED(hr) || !out)
        {
            out.Reset();
            dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, style,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
        }
        if (out)
        {
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    };
    make(DWRITE_FONT_STYLE_NORMAL, 12.0f * ts, lookup_body_format_);
    make(DWRITE_FONT_STYLE_ITALIC, 11.5f * ts, lookup_example_format_);
    lookup_format_scale_ = ts;
    lookup_format_family_ = settings.font_family;
}

std::wstring Renderer::fit_text_to_lines(const std::wstring& text, IDWriteTextFormat* fmt, float width, int max_lines,
                                         float* out_height)
{
    *out_height = 0.0f;
    if (text.empty() || !fmt || !dwrite_factory_)
    {
        return text;
    }

    auto measure = [&](const std::wstring& s, UINT32* lines, float* height) -> bool
    {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_factory_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, width, 4096.0f,
                                                     &layout)) ||
            !layout)
        {
            return false;
        }
        DWRITE_TEXT_METRICS tm = {};
        if (FAILED(layout->GetMetrics(&tm)))
        {
            return false;
        }
        *lines = tm.lineCount;
        *height = tm.height;
        return true;
    };

    UINT32 line_count = 0;
    float height = 0.0f;
    if (!measure(text, &line_count, &height))
    {
        return text;
    }
    if (static_cast<int>(line_count) <= max_lines)
    {
        *out_height = height;
        return text;
    }
    const float one_line = line_count > 0 ? height / static_cast<float>(line_count) : 0.0f;

    std::size_t keep = text.size();
    {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, width,
                                                        4096.0f, &layout)) &&
            layout)
        {
            UINT32 n = 0;
            layout->GetLineMetrics(nullptr, 0, &n);
            std::vector<DWRITE_LINE_METRICS> lines(n);
            if (n > 0 && SUCCEEDED(layout->GetLineMetrics(lines.data(), n, &n)))
            {
                std::size_t total = 0;
                for (UINT32 i = 0; i < n && i < static_cast<UINT32>(max_lines); ++i)
                {
                    total += lines[i].length;
                }
                keep = std::min(total, text.size());
            }
        }
    }

    std::wstring cut = text.substr(0, keep);
    for (int attempt = 0; attempt < 60 && !cut.empty(); ++attempt)
    {
        while (!cut.empty() && (std::iswspace(cut.back()) || cut.back() == L',' || cut.back() == L';'))
        {
            cut.pop_back();
        }
        const std::wstring candidate = cut + L"\u2026";
        UINT32 lines = 0;
        float h = 0.0f;
        if (measure(candidate, &lines, &h) && static_cast<int>(lines) <= max_lines)
        {
            *out_height = h;
            return candidate;
        }
        if (cut.empty())
        {
            break;
        }
        cut.pop_back();
        if (!cut.empty() && IS_HIGH_SURROGATE(cut.back()))
        {
            cut.pop_back();
        }
    }
    *out_height = one_line;
    return L"\u2026";
}

// Cached per result, not per frame.
const Renderer::LookupFit& Renderer::ensure_lookup_fit(const LookupSnapshot& lk, const Settings& settings)
{
    ensure_lookup_formats(settings);
    const std::wstring key = lk.body + L"\x1F" + lk.example;
    if (lookup_fit_.valid && lookup_fit_.key == key)
    {
        return lookup_fit_;
    }
    const float text_w = lookup_layout::width - lookup_layout::pad_x * 2.0f - lookup_layout::card_pad * 2.0f;
    LookupFit fit;
    fit.key = key;
    fit.body =
        fit_text_to_lines(lk.body, lookup_body_format_.Get(), text_w, lookup_layout::max_body_lines, &fit.body_h);
    fit.example = fit_text_to_lines(lk.example, lookup_example_format_.Get(), text_w, lookup_layout::max_example_lines,
                                    &fit.example_h);
    fit.valid = true;
    lookup_fit_ = std::move(fit);
    return lookup_fit_;
}

ComPtr<IDWriteTextLayout> Renderer::make_line_layout(const std::wstring& text, IDWriteTextFormat* fmt, float max_width,
                                                     DWRITE_TEXT_METRICS* metrics, bool ellipsis)
{
    ComPtr<IDWriteTextLayout> layout;
    if (!fmt || !dwrite_factory_)
    {
        return layout;
    }
    if (FAILED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                                 std::max(1.0f, max_width), 200.0f, &layout)) ||
        !layout)
    {
        layout.Reset();
        return layout;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (ellipsis)
    {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(layout.Get(), &sign)) && sign)
        {
            DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, sign.Get());
        }
    }
    if (metrics)
    {
        layout->GetMetrics(metrics);
    }
    return layout;
}

ID2D1StrokeStyle* Renderer::round_stroke()
{
    if (!round_stroke_ && d2d_factory_)
    {
        d2d_factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                    D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                        nullptr, 0, &round_stroke_);
    }
    return round_stroke_.Get();
}

// kind: 0 magnifier, 1 history Clock, 2 clipboard, 3 return arrow.
void Renderer::draw_lookup_glyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush)
{
    ID2D1StrokeStyle* round = round_stroke();
    const float stroke = std::max(1.3f, 0.10f * s);
    auto p = [](float x, float y)
    {
        return D2D1::Point2F(x, y);
    };

    switch (kind)
    {
        case 0:
        {
            const float r = 0.30f * s;
            const D2D1_POINT_2F lens = p(c.x - 0.08f * s, c.y - 0.08f * s);
            target_->DrawEllipse(D2D1::Ellipse(lens, r, r), brush, stroke, round);
            target_->DrawLine(p(lens.x + r * 0.72f, lens.y + r * 0.72f), p(c.x + 0.40f * s, c.y + 0.40f * s), brush,
                              stroke, round);
            break;
        }
        case 1:
        {
            const float r = 0.40f * s;
            target_->DrawEllipse(D2D1::Ellipse(c, r, r), brush, stroke, round);
            target_->DrawLine(p(c.x, c.y - 0.22f * s), c, brush, stroke, round);
            target_->DrawLine(c, p(c.x + 0.17f * s, c.y + 0.10f * s), brush, stroke, round);
            break;
        }
        case 2:
        {
            target_->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(c.x - 0.30f * s, c.y - 0.28f * s, c.x + 0.30f * s, c.y + 0.40f * s),
                                  0.08f * s, 0.08f * s),
                brush, stroke, round);
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(c.x - 0.13f * s, c.y - 0.40f * s, c.x + 0.13f * s, c.y - 0.20f * s),
                                  0.05f * s, 0.05f * s),
                brush);
            target_->DrawLine(p(c.x - 0.14f * s, c.y + 0.06f * s), p(c.x + 0.14f * s, c.y + 0.06f * s), brush,
                              stroke * 0.85f, round);
            target_->DrawLine(p(c.x - 0.14f * s, c.y + 0.22f * s), p(c.x + 0.05f * s, c.y + 0.22f * s), brush,
                              stroke * 0.85f, round);
            break;
        }
        default:
        {
            const D2D1_POINT_2F tip = p(c.x - 0.36f * s, c.y + 0.08f * s);
            target_->DrawLine(p(c.x + 0.34f * s, c.y - 0.28f * s), p(c.x + 0.34f * s, c.y + 0.08f * s), brush, stroke,
                              round);
            target_->DrawLine(p(c.x + 0.34f * s, c.y + 0.08f * s), tip, brush, stroke, round);
            target_->DrawLine(tip, p(c.x - 0.14f * s, c.y - 0.12f * s), brush, stroke, round);
            target_->DrawLine(tip, p(c.x - 0.14f * s, c.y + 0.28f * s), brush, stroke, round);
            break;
        }
    }
}

void Renderer::draw_lookup_field(D2D1_RECT_F rect, double now)
{
    const LookupUiState& ui = g_lookup_ui;
    const D2D1_RECT_F field =
        D2D1::RectF(rect.left + lookup_layout::pad_x, rect.top + lookup_layout::field_top,
                    rect.right - lookup_layout::pad_x, rect.top + lookup_layout::field_top + lookup_layout::field_h);
    draw_card(field, 12.0f, true);
    const float mid_y = (field.top + field.bottom) * 0.5f;

    accent_brush_->SetOpacity(0.95f);
    draw_lookup_glyph(D2D1::Point2F(field.left + 19.0f, mid_y), 16.0f, 0, accent_brush_.Get());
    accent_brush_->SetOpacity(1.0f);

    const D2D1_RECT_F chip = D2D1::RectF(field.right - 44.0f, mid_y - 10.0f, field.right - 12.0f, mid_y + 10.0f);
    ComPtr<ID2D1SolidColorBrush> chip_fill;
    if (SUCCEEDED(target_->CreateSolidColorBrush(material_.hairline, &chip_fill)) && chip_fill)
    {
        target_->FillRoundedRectangle(D2D1::RoundedRect(chip, 6.0f, 6.0f), chip_fill.Get());
    }
    muted_brush_->SetOpacity(0.75f);
    if (ui.text.empty())
    {
        if (small_text_format_)
        {
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(L"Esc", 3, small_text_format_.Get(), chip, muted_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }
    else
    {
        draw_lookup_glyph(D2D1::Point2F((chip.left + chip.right) * 0.5f, mid_y), 12.0f, 3, muted_brush_.Get());
    }

    const float area_left = field.left + 38.0f;
    const float area_right = chip.left - 10.0f;
    const float area_w = std::max(8.0f, area_right - area_left);
    float caret_x = 0.0f;
    float scroll = 0.0f;

    target_->PushAxisAlignedClip(D2D1::RectF(area_left, field.top, area_right, field.bottom),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    DWRITE_TEXT_METRICS tm{};
    if (ui.text.empty())
    {
        auto hint = make_line_layout(loc(L"Search a word or phrase"), text_format_.Get(), 4096.0f, &tm);
        if (hint)
        {
            muted_brush_->SetOpacity(0.50f);
            target_->DrawTextLayout(D2D1::Point2F(area_left + 6.0f, mid_y - tm.height * 0.5f), hint.Get(),
                                    muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
    else if (auto layout = make_line_layout(ui.text, text_format_.Get(), 4096.0f, &tm))
    {
        const UINT32 caret_idx = static_cast<UINT32>(std::min(ui.caret, ui.text.size()));
        float caret_y = 0.0f;
        DWRITE_HIT_TEST_METRICS hm{};
        layout->HitTestTextPosition(caret_idx, FALSE, &caret_x, &caret_y, &hm);
        if (caret_x > area_w - 6.0f)
        {
            scroll = caret_x - (area_w - 6.0f);
        }

        const float ox = area_left - scroll;
        if (ui.select_all)
        {
            ComPtr<ID2D1SolidColorBrush> sel;
            if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.32f), &sel)) && sel)
            {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(ox - 2.0f, mid_y - 10.0f,
                                                  ox + tm.widthIncludingTrailingWhitespace + 2.0f, mid_y + 10.0f),
                                      4.0f, 4.0f),
                    sel.Get());
            }
        }
        text_brush_->SetOpacity(0.98f);
        target_->DrawTextLayout(D2D1::Point2F(ox, mid_y - tm.height * 0.5f), layout.Get(), text_brush_.Get(),
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    if (!ui.select_all && lookup_caret_visible(now, ui.caret_reset_at))
    {
        const float cx = area_left + 1.0f + caret_x - scroll;
        accent_brush_->SetOpacity(0.95f);
        target_->DrawLine(D2D1::Point2F(cx, mid_y - 9.0f), D2D1::Point2F(cx, mid_y + 9.0f), accent_brush_.Get(), 1.6f,
                          round_stroke());
        accent_brush_->SetOpacity(1.0f);
    }
    target_->PopAxisAlignedClip();
    muted_brush_->SetOpacity(0.75f);
}

void Renderer::draw_lookup_rows(D2D1_RECT_F rect, const std::vector<LookupRow>& rows, bool header)
{
    const LookupUiState& ui = g_lookup_ui;
    const float left = rect.left + lookup_layout::pad_x;
    const float right = rect.right - lookup_layout::pad_x;
    float y = rect.top + lookup_layout::below_field;

    if (rows.empty())
    {
        DWRITE_TEXT_METRICS tm{};
        auto hint = make_line_layout(loc(L"Type a word or phrase, then press Enter"), small_text_format_.Get(),
                                     right - left - 8.0f, &tm, true);
        if (hint)
        {
            muted_brush_->SetOpacity(0.60f);
            target_->DrawTextLayout(D2D1::Point2F(left + 4.0f, y + (lookup_layout::status_h - tm.height) * 0.5f),
                                    hint.Get(), muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        muted_brush_->SetOpacity(0.75f);
        return;
    }

    if (header && small_text_format_)
    {
        const D2D1_RECT_F band = D2D1::RectF(left + 4.0f, y, right - 4.0f, y + lookup_layout::section_h);
        small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        muted_brush_->SetOpacity(0.55f);
        const wchar_t* title = loc(L"Recent");
        target_->DrawTextW(title, static_cast<UINT32>(std::wcslen(title)), small_text_format_.Get(), band,
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (lookup_has_recent_rows(rows))
        {
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            const wchar_t* clear = loc(L"Clear");
            target_->DrawTextW(clear, static_cast<UINT32>(std::wcslen(clear)), small_text_format_.Get(), band,
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        y += lookup_layout::section_h;
    }

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const LookupRow& row = rows[i];
        const D2D1_RECT_F rr = D2D1::RectF(left, y, right, y + lookup_layout::row_h);
        const bool selected = static_cast<int>(i) == ui.selected;
        const float mid_y = y + lookup_layout::row_h * 0.5f;
        if (selected)
        {
            draw_card(rr, 10.0f, false);
        }

        const int glyph = row.kind == LookupRowKind::Typed ? 0 : row.kind == LookupRowKind::Clipboard ? 2 : 1;
        ID2D1SolidColorBrush* glyph_brush =
            (row.kind == LookupRowKind::Recent) ? muted_brush_.Get() : accent_brush_.Get();
        glyph_brush->SetOpacity(row.kind == LookupRowKind::Recent ? 0.70f : 0.92f);
        draw_lookup_glyph(D2D1::Point2F(rr.left + 17.0f, mid_y), 14.0f, glyph, glyph_brush);
        glyph_brush->SetOpacity(row.kind == LookupRowKind::Recent ? 0.75f : 1.0f);

        const float x0 = rr.left + 34.0f;
        const float x1 = rr.right - (selected ? 34.0f : 10.0f);
        const float area_w = std::max(10.0f, x1 - x0);

        const std::wstring label = row.kind == LookupRowKind::Typed
                                       ? std::wstring(loc(L"Look up")) + L" \u201c" + row.label + L"\u201d"
                                       : row.label;

        DWRITE_TEXT_METRICS lm{};
        make_line_layout(label, text_format_.Get(), 4096.0f, &lm);
        const float label_w =
            std::min(lm.widthIncludingTrailingWhitespace, area_w * (row.detail.empty() ? 1.0f : 0.62f));
        if (auto label_layout = make_line_layout(label, text_format_.Get(), label_w + 0.5f, &lm, true))
        {
            text_brush_->SetOpacity(0.96f);
            target_->DrawTextLayout(D2D1::Point2F(x0, mid_y - lm.height * 0.5f), label_layout.Get(), text_brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        if (!row.detail.empty())
        {
            const float dx = x0 + label_w + 10.0f;
            const float dw = x1 - dx;
            if (dw > 40.0f)
            {
                DWRITE_TEXT_METRICS dm{};
                if (auto detail = make_line_layout(row.detail, small_text_format_.Get(), dw, &dm, true))
                {
                    ID2D1SolidColorBrush* b =
                        (row.kind == LookupRowKind::Clipboard) ? accent_brush_.Get() : muted_brush_.Get();
                    b->SetOpacity(row.kind == LookupRowKind::Clipboard ? 0.85f : 0.62f);
                    target_->DrawTextLayout(D2D1::Point2F(dx, mid_y - dm.height * 0.5f), detail.Get(), b,
                                            D2D1_DRAW_TEXT_OPTIONS_NONE);
                    b->SetOpacity(b == accent_brush_.Get() ? 1.0f : 0.75f);
                }
            }
        }

        if (selected)
        {
            muted_brush_->SetOpacity(0.80f);
            draw_lookup_glyph(D2D1::Point2F(rr.right - 17.0f, mid_y), 12.0f, 3, muted_brush_.Get());
            muted_brush_->SetOpacity(0.75f);
        }
        y += lookup_layout::row_h + lookup_layout::row_gap;
    }
    text_brush_->SetOpacity(0.96f);
}

void Renderer::draw_lookup_status(D2D1_RECT_F rect, const LookupSnapshot& lk)
{
    const float left = rect.left + lookup_layout::pad_x + 4.0f;
    const float right = rect.right - lookup_layout::pad_x - 4.0f;
    const float y = rect.top + lookup_layout::below_field;
    const wchar_t* msg = lk.status == LookupStatus::Loading ? loc(L"Looking up...") : loc(L"No result found");
    DWRITE_TEXT_METRICS tm{};
    if (auto layout = make_line_layout(msg, text_format_.Get(), right - left, &tm, true))
    {
        muted_brush_->SetOpacity(0.75f);
        target_->DrawTextLayout(D2D1::Point2F(left, y + (lookup_layout::status_h - tm.height) * 0.5f), layout.Get(),
                                muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

void Renderer::draw_lookup_result(const SharedState& state, D2D1_RECT_F rect, const Settings& settings)
{
    const LookupSnapshot& lk = state.lookup;
    const LookupFit& fit = ensure_lookup_fit(lk, settings);
    const float grow = lookup_grow(settings);
    const float title_h = lookup_layout::title_h * grow;
    const float left = rect.left + lookup_layout::pad_x;
    const float right = rect.right - lookup_layout::pad_x;
    float y = rect.top + lookup_layout::below_field;

    constexpr float tag_w = 74.0f;
    text_brush_->SetOpacity(0.97f);
    target_->DrawTextW(lk.title.c_str(), static_cast<UINT32>(lk.title.size()), text_format_.Get(),
                       D2D1::RectF(left, y, right - tag_w - 6.0f, y + title_h), text_brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (!lk.source.empty() && small_text_format_)
    {
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        accent_brush_->SetOpacity(0.90f);
        target_->DrawTextW(lk.source.c_str(), static_cast<UINT32>(lk.source.size()), small_text_format_.Get(),
                           D2D1::RectF(right - tag_w, y + 3.0f, right, y + title_h), accent_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        accent_brush_->SetOpacity(1.0f);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    y += title_h;

    if (!lk.subtitle.empty())
    {
        const float sub_h = lookup_layout::subtitle_h * grow;
        muted_brush_->SetOpacity(0.78f);
        target_->DrawTextW(lk.subtitle.c_str(), static_cast<UINT32>(lk.subtitle.size()), small_text_format_.Get(),
                           D2D1::RectF(left, y - 2.0f, right, y + sub_h - 2.0f), muted_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        y += sub_h;
    }
    else
    {
        y += 2.0f;
    }
    y += lookup_layout::card_gap;

    float inner = fit.body_h;
    if (!fit.example.empty())
    {
        inner += lookup_layout::example_gap + fit.example_h;
    }
    const float pad = lookup_layout::card_pad;
    const D2D1_RECT_F card = D2D1::RectF(left, y, right, y + inner + pad * 2.0f);
    draw_card(card, 10.0f);

    float ty = card.top + pad;
    text_brush_->SetOpacity(0.92f);
    target_->DrawTextW(fit.body.c_str(), static_cast<UINT32>(fit.body.size()), lookup_body_format_.Get(),
                       D2D1::RectF(card.left + pad, ty, card.right - pad, ty + fit.body_h + 2.0f), text_brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    ty += fit.body_h;

    if (!fit.example.empty())
    {
        ty += lookup_layout::example_gap;
        muted_brush_->SetOpacity(0.70f);
        target_->DrawTextW(fit.example.c_str(), static_cast<UINT32>(fit.example.size()), lookup_example_format_.Get(),
                           D2D1::RectF(card.left + pad, ty, card.right - pad, ty + fit.example_h + 2.0f),
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Renderer::draw_lookup(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now)
{
    const LookupSnapshot& lk = state.lookup;
    const float h = rect.bottom - rect.top;
    const float w = rect.right - rect.left;
    if (h < 24.0f || w < 100.0f)
    {
        return;
    }

    publish_content_geometry(rect);

    const float need = measure_lookup_card(state, settings);
    const float alpha = smooth_step01((h - need * 0.55f) / (need * 0.45f));
    if (alpha <= 0.01f)
    {
        return;
    }

    ComPtr<ID2D1Geometry> mask = create_island_mask_geometry(rect, content_island_radius(h), settings.notch_style);
    ID2D1Layer* layer = acquire_layer();
    const bool masked = mask && layer;
    if (masked)
    {
        target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS_NONE),
                           layer);
    }
    else
    {
        target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    draw_lookup_field(rect, now);

    if (lk.status == LookupStatus::Search)
    {
        const std::vector<LookupRow> rows = build_lookup_rows(state.lookup_recent, g_lookup_ui);
        draw_lookup_rows(rect, rows, lookup_has_header(g_lookup_ui, rows));
    }
    else if (lk.status == LookupStatus::Found)
    {
        draw_lookup_result(state, rect, settings);
    }
    else
    {
        draw_lookup_status(rect, lk);
    }

    if (masked)
    {
        target_->PopLayer();
    }
    else
    {
        target_->PopAxisAlignedClip();
    }
    text_brush_->SetOpacity(0.96f);
    muted_brush_->SetOpacity(0.75f);
}

}
