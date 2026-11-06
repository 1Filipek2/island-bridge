#include "ui/render/renderer_impl.h"

namespace island
{

void Renderer::draw_tray_bin_button(D2D1_RECT_F box, bool enabled, bool hovered)
{
    const float s = box.right - box.left;
    const D2D1_POINT_2F c = D2D1::Point2F((box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f);
    const D2D1_COLOR_F danger = D2D1::ColorF(1.0f, 0.38f, 0.36f, 1.0f);

    if (enabled && hovered)
    {
        ComPtr<ID2D1SolidColorBrush> bg;
        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(danger, 0.20f * settings_opacity_), &bg)) && bg)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(box, s * 0.32f, s * 0.32f), bg.Get());
        }
    }

    D2D1_COLOR_F ink =
        enabled ? (hovered ? danger : material_.text_secondary) : with_alpha(material_.text_secondary, 0.35f);
    ink.a *= settings_opacity_;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush)
    {
        return;
    }

    ComPtr<ID2D1StrokeStyle> round;
    d2d_factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &round);

    const float stroke = std::max(1.2f, 0.075f * s);

    target_->DrawLine(D2D1::Point2F(c.x - 0.31f * s, c.y - 0.17f * s), D2D1::Point2F(c.x + 0.31f * s, c.y - 0.17f * s),
                      brush.Get(), stroke, round.Get());
    target_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.30f * s, c.x + 0.11f * s, c.y - 0.17f * s), 0.04f * s,
                          0.04f * s),
        brush.Get(), stroke, round.Get());

    ComPtr<ID2D1PathGeometry> body;
    if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&body)) && body)
    {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(body->Open(&sink)) && sink)
        {
            sink->BeginFigure(D2D1::Point2F(c.x - 0.23f * s, c.y - 0.07f * s), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(c.x - 0.18f * s, c.y + 0.30f * s));
            sink->AddLine(D2D1::Point2F(c.x + 0.18f * s, c.y + 0.30f * s));
            sink->AddLine(D2D1::Point2F(c.x + 0.23f * s, c.y - 0.07f * s));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            target_->DrawGeometry(body.Get(), brush.Get(), stroke, round.Get());
        }
    }

    for (int i = -1; i <= 1; i += 2)
    {
        const float x = c.x + static_cast<float>(i) * 0.08f * s;
        target_->DrawLine(D2D1::Point2F(x, c.y + 0.03f * s), D2D1::Point2F(x, c.y + 0.20f * s), brush.Get(),
                          stroke * 0.85f, round.Get());
    }
}

void Renderer::draw_tray_paste_button(D2D1_RECT_F box, bool hovered)
{
    const float s = box.right - box.left;
    const D2D1_POINT_2F c = D2D1::Point2F((box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f);

    if (hovered)
    {
        ComPtr<ID2D1SolidColorBrush> bg;
        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.20f * settings_opacity_), &bg)) &&
            bg)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(box, s * 0.32f, s * 0.32f), bg.Get());
        }
    }

    D2D1_COLOR_F ink = hovered ? material_.accent : material_.text_secondary;
    ink.a *= settings_opacity_;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush)
    {
        return;
    }

    ComPtr<ID2D1StrokeStyle> round;
    d2d_factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &round);

    const float stroke = std::max(1.2f, 0.075f * s);

    target_->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(c.x - 0.25f * s, c.y - 0.24f * s, c.x + 0.25f * s, c.y + 0.31f * s), 0.07f * s,
                          0.07f * s),
        brush.Get(), stroke, round.Get());

    target_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.32f * s, c.x + 0.11f * s, c.y - 0.15f * s), 0.05f * s,
                          0.05f * s),
        brush.Get());

    target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.00f * s), D2D1::Point2F(c.x + 0.12f * s, c.y + 0.00f * s),
                      brush.Get(), stroke * 0.85f, round.Get());
    target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.14f * s), D2D1::Point2F(c.x + 0.05f * s, c.y + 0.14f * s),
                      brush.Get(), stroke * 0.85f, round.Get());
}

void Renderer::draw_tray_remove_button(D2D1_POINT_2F c, float size, bool hovered, bool row_hovered)
{
    const D2D1_COLOR_F danger = D2D1::ColorF(1.0f, 0.38f, 0.36f, 1.0f);

    if (hovered)
    {
        ComPtr<ID2D1SolidColorBrush> bg;
        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(danger, 0.22f * settings_opacity_), &bg)) && bg)
        {
            target_->FillEllipse(D2D1::Ellipse(c, size * 0.5f, size * 0.5f), bg.Get());
        }
    }

    D2D1_COLOR_F ink = hovered ? danger : with_alpha(material_.text_secondary, row_hovered ? 0.85f : 0.55f);
    ink.a *= settings_opacity_;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush)
    {
        return;
    }

    ComPtr<ID2D1StrokeStyle> round;
    d2d_factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &round);

    const float r = size * 0.22f;
    const float stroke = std::max(1.2f, size * 0.085f);
    target_->DrawLine(D2D1::Point2F(c.x - r, c.y - r), D2D1::Point2F(c.x + r, c.y + r), brush.Get(), stroke,
                      round.Get());
    target_->DrawLine(D2D1::Point2F(c.x - r, c.y + r), D2D1::Point2F(c.x + r, c.y - r), brush.Get(), stroke,
                      round.Get());
}

// File Tray: a shelf for files dropped on the island, with the real Explorer icons.
void Renderer::draw_file_tray_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                                        float scale)
{
    const float pad_x = file_tray_layout::pad_x * scale;

    const D2D1_RECT_F header_rect =
        D2D1::RectF(rect.left + pad_x, rect.top + 18.0f * scale, rect.right - pad_x, rect.top + 38.0f * scale);
    text_brush_->SetOpacity(0.96f);
    if (bold_text_format_)
    {
        bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const wchar_t* title = loc(L"File Tray");
        target_->DrawTextW(title, static_cast<UINT32>(std::wcslen(title)), bold_text_format_.Get(), header_rect,
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    const std::size_t total = state.file_tray_items.size();

    const float clear_size = file_tray_layout::clear_btn_size * scale;
    const float clear_top = rect.top + file_tray_layout::clear_btn_top * scale;
    const D2D1_RECT_F clear_box =
        D2D1::RectF(rect.right - pad_x - clear_size, clear_top, rect.right - pad_x, clear_top + clear_size);
    const float paste_gap = file_tray_layout::paste_btn_gap * scale;
    const D2D1_RECT_F paste_box = D2D1::RectF(clear_box.left - paste_gap - clear_size, clear_top,
                                              clear_box.left - paste_gap, clear_top + clear_size);

    if (total > 0 && small_text_format_)
    {
        wchar_t count_buf[48] = {};
        swprintf_s(count_buf, L"%zu %s", total, loc(total == 1 ? L"item" : L"items"));
        const D2D1_RECT_F count_rect =
            D2D1::RectF(header_rect.left, header_rect.top, paste_box.left - 8.0f * scale, header_rect.bottom);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        muted_brush_->SetOpacity(0.75f);
        target_->DrawTextW(count_buf, static_cast<UINT32>(std::wcslen(count_buf)), small_text_format_.Get(), count_rect,
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    draw_tray_paste_button(paste_box, g_hovered_file_tray_action.load(std::memory_order_relaxed) == -3);
    draw_tray_bin_button(clear_box, total > 0, g_hovered_file_tray_action.load(std::memory_order_relaxed) == -2);

    const float list_top = rect.top + file_tray_layout::list_top * scale;
    const float list_bottom = rect.bottom - file_tray_layout::list_bottom_inset * scale;

    if (state.file_tray_items.empty())
    {
        const D2D1_RECT_F drop_rect = D2D1::RectF(rect.left + pad_x, list_top, rect.right - pad_x, list_bottom);
        ComPtr<ID2D1SolidColorBrush> dash;
        if (SUCCEEDED(
                target_->CreateSolidColorBrush(with_alpha(material_.hairline, material_.hairline.a * 1.6f), &dash)) &&
            dash)
        {
            ComPtr<ID2D1StrokeStyle> dash_style;
            D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
            props.dashStyle = D2D1_DASH_STYLE_DASH;
            props.dashCap = D2D1_CAP_STYLE_ROUND;
            d2d_factory_->CreateStrokeStyle(props, nullptr, 0, &dash_style);
            target_->DrawRoundedRectangle(D2D1::RoundedRect(drop_rect, 12.0f * scale, 12.0f * scale), dash.Get(), 1.4f,
                                          dash_style.Get());
        }

        if (text_format_)
        {
            text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            const wchar_t* hint = loc(L"Drag files here");
            muted_brush_->SetOpacity(0.80f);
            target_->DrawTextW(hint, static_cast<UINT32>(std::wcslen(hint)), text_format_.Get(), drop_rect,
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        muted_brush_->SetOpacity(0.75f);
        return;
    }

    const float row_h = file_tray_layout::row_height * scale;
    const float row_gap = file_tray_layout::row_gap * scale;
    const int capacity = file_tray_layout::visible_row_capacity((rect.bottom - rect.top) / scale);

    int drawn = 0;
    for (auto it = state.file_tray_items.rbegin(); it != state.file_tray_items.rend() && drawn < capacity;
         ++it, ++drawn)
    {
        const FileTrayItem& item = *it;
        const float row_top = list_top + drawn * (row_h + row_gap);
        const D2D1_RECT_F row = D2D1::RectF(rect.left + pad_x, row_top, rect.right - pad_x, row_top + row_h);

        const bool hovered = (g_hovered_file_tray_row.load(std::memory_order_relaxed) == drawn);
        draw_card(row, 9.0f * scale, hovered);

        const D2D1_RECT_F icon_rect = D2D1::RectF(row.left + 8.0f * scale, row.top + 6.0f * scale,
                                                  row.left + 26.0f * scale, row.bottom - 6.0f * scale);
        if (item.icon)
        {
            draw_bitmap_pixels(*item.icon, icon_rect, file_tray_icon_bitmap_, file_tray_icon_generation_, 0.98f);
        }
        else if (icon_format_)
        {
            const wchar_t* glyph = item.is_text ? L"\uE8D2" : (item.is_directory ? L"\uE8B7" : L"\uE7C3");
            accent_brush_->SetOpacity(0.85f);
            target_->DrawTextW(glyph, 1, icon_format_.Get(), icon_rect, accent_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
            accent_brush_->SetOpacity(1.0f);
        }

        wchar_t size_buf[40] = {};
        if (item.is_text)
        {
            swprintf_s(size_buf, L"%zu ch", item.text.size());
        }
        else if (item.is_directory)
        {
            wcscpy_s(size_buf, L"-");
        }
        else if (item.size_bytes >= 1073741824ull)
        {
            swprintf_s(size_buf, L"%.1f GB", static_cast<double>(item.size_bytes) / 1073741824.0);
        }
        else if (item.size_bytes >= 1048576ull)
        {
            swprintf_s(size_buf, L"%.1f MB", static_cast<double>(item.size_bytes) / 1048576.0);
        }
        else if (item.size_bytes >= 1024ull)
        {
            swprintf_s(size_buf, L"%.0f KB", static_cast<double>(item.size_bytes) / 1024.0);
        }
        else
        {
            swprintf_s(size_buf, L"%llu B", static_cast<unsigned long long>(item.size_bytes));
        }

        const float remove_size = file_tray_layout::remove_btn_size * scale;
        const float remove_margin = file_tray_layout::remove_btn_margin * scale;
        const float remove_space = remove_size + remove_margin + 4.0f * scale;
        const bool remove_hovered = (g_hovered_file_tray_action.load(std::memory_order_relaxed) == drawn);
        draw_tray_remove_button(
            D2D1::Point2F(row.right - remove_margin - remove_size * 0.5f, (row.top + row.bottom) * 0.5f), remove_size,
            remove_hovered, hovered);

        const float size_w = 66.0f * scale;
        const D2D1_RECT_F name_rect = D2D1::RectF(row.left + 32.0f * scale, row.top,
                                                  row.right - remove_space - size_w - 4.0f * scale, row.bottom);
        if (small_text_format_)
        {
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text_brush_->SetOpacity(0.94f);
            target_->DrawTextW(item.name.c_str(), static_cast<UINT32>(item.name.size()), small_text_format_.Get(),
                               name_rect, text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            const D2D1_RECT_F size_rect =
                D2D1::RectF(row.right - remove_space - size_w, row.top, row.right - remove_space, row.bottom);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            muted_brush_->SetOpacity(0.70f);
            target_->DrawTextW(size_buf, static_cast<UINT32>(std::wcslen(size_buf)), small_text_format_.Get(),
                               size_rect, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }

    if (static_cast<int>(total) > capacity && small_text_format_)
    {
        wchar_t more_buf[32] = {};
        swprintf_s(more_buf, L"+%d", static_cast<int>(total) - capacity);
        const D2D1_RECT_F more_rect =
            D2D1::RectF(rect.left + pad_x, list_bottom - 12.0f * scale, rect.right - pad_x, list_bottom);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        muted_brush_->SetOpacity(0.65f);
        target_->DrawTextW(more_buf, static_cast<UINT32>(std::wcslen(more_buf)), small_text_format_.Get(), more_rect,
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    text_brush_->SetOpacity(0.96f);
    muted_brush_->SetOpacity(0.75f);
}

std::size_t Renderer::notification_center_slot(std::uint64_t generation)
{
    for (std::size_t slot = 0; slot < notification_center_generations_.size(); ++slot)
    {
        if (generation != 0 && notification_center_generations_[slot] == generation &&
            notification_center_bitmaps_[slot])
        {
            return slot;
        }
    }
    const std::size_t slot = notification_center_next_slot_;
    notification_center_next_slot_ = (notification_center_next_slot_ + 1) % notification_center_generations_.size();
    notification_center_generations_[slot] = 0;
    return slot;
}

void Renderer::draw_notification_center_icon(const notification_center::Group& group, D2D1_RECT_F badge, float radius)
{
    ComPtr<ID2D1SolidColorBrush> plate;
    if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raised_strong, &plate)) && plate)
    {
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, radius, radius), plate.Get());
    }

    if (group.icon)
    {
        const std::size_t slot = notification_center_slot(group.icon->generation);
        draw_rounded_bitmap_pixels(*group.icon, badge, radius, notification_center_bitmaps_[slot],
                                   notification_center_generations_[slot], 1.0f);
        return;
    }

    const wchar_t initial = notification_initial(group.app);
    if (initial == 0 || !bold_text_format_)
    {
        return;
    }
    const DWRITE_TEXT_ALIGNMENT text_alignment = bold_text_format_->GetTextAlignment();
    const DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment = bold_text_format_->GetParagraphAlignment();
    bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    bold_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_brush_->SetOpacity(0.95f);
    target_->DrawTextW(&initial, 1, bold_text_format_.Get(), badge, text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    bold_text_format_->SetTextAlignment(text_alignment);
    bold_text_format_->SetParagraphAlignment(paragraph_alignment);
}

void Renderer::draw_notification_center_back(D2D1_RECT_F box, float scale, bool hovered)
{
    if (hovered)
    {
        draw_card(box, 7.0f * scale, true);
    }
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float arm = 4.5f * scale;
    const float stroke = 1.8f * scale;
    text_brush_->SetOpacity(hovered ? 1.0f : 0.85f);
    target_->DrawLine(D2D1::Point2F(cx + arm * 0.5f, cy - arm), D2D1::Point2F(cx - arm * 0.5f, cy), text_brush_.Get(),
                      stroke);
    target_->DrawLine(D2D1::Point2F(cx - arm * 0.5f, cy), D2D1::Point2F(cx + arm * 0.5f, cy + arm), text_brush_.Get(),
                      stroke);
}

void Renderer::draw_notification_center_detail(const notification_center::Group& group, std::size_t offset,
                                               D2D1_RECT_F rect, float scale)
{
    const int capacity = notification_center::visible_rows((rect.bottom - rect.top) / scale);
    g_notification_center_detail_rows.store(capacity, std::memory_order_relaxed);
    const std::size_t rows = static_cast<std::size_t>(capacity);
    const std::size_t count = group.entries.size();
    const std::size_t limit = notification_center::max_offset(count, capacity);
    const std::size_t first = std::min(offset, limit);
    const int hovered_action = g_hovered_notification_action.load(std::memory_order_relaxed);

    const float pad_x = notification_center::pad_x * scale;
    const float head_top = rect.top + notification_center::clear_top * scale;
    const float clear_size = notification_center::clear_size * scale;
    const float back_size = notification_center::back_size * scale;
    const D2D1_RECT_F clear_box =
        D2D1::RectF(rect.right - pad_x - clear_size, head_top, rect.right - pad_x, head_top + clear_size);
    const D2D1_RECT_F back_box =
        D2D1::RectF(rect.left + pad_x, head_top, rect.left + pad_x + back_size, head_top + back_size);
    draw_notification_center_back(back_box, scale, hovered_action == notification_center_back);

    const float icon_size = 20.0f * scale;
    const float icon_top = (back_box.top + back_box.bottom - icon_size) * 0.5f;
    const D2D1_RECT_F badge = D2D1::RectF(back_box.right + 6.0f * scale, icon_top,
                                          back_box.right + 6.0f * scale + icon_size, icon_top + icon_size);
    draw_notification_center_icon(group, badge, 6.0f * scale);

    const float text_top = rect.top + 18.0f * scale;
    const float text_bottom = rect.top + 38.0f * scale;
    float heading_right = clear_box.left - 8.0f * scale;

    if (count > rows && small_text_format_)
    {
        wchar_t range_buf[48] = {};
        swprintf_s(range_buf, L"%zu\u2013%zu / %zu", first + 1, std::min(count, first + rows), count);
        const D2D1_RECT_F range_rect = D2D1::RectF(heading_right - 72.0f * scale, text_top, heading_right, text_bottom);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        muted_brush_->SetOpacity(0.75f);
        target_->DrawTextW(range_buf, static_cast<UINT32>(std::wcslen(range_buf)), small_text_format_.Get(), range_rect,
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        heading_right = range_rect.left;
    }

    if (bold_text_format_)
    {
        std::wstring heading = group.app;
        if (count > 1)
        {
            heading += L" \u00B7 " + std::to_wstring(count);
        }
        const DWRITE_TEXT_ALIGNMENT alignment = bold_text_format_->GetTextAlignment();
        bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        text_brush_->SetOpacity(0.96f);
        target_->DrawTextW(heading.c_str(), static_cast<UINT32>(heading.size()), bold_text_format_.Get(),
                           D2D1::RectF(badge.right + 8.0f * scale, text_top, heading_right - 6.0f * scale, text_bottom),
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        bold_text_format_->SetTextAlignment(alignment);
    }

    draw_tray_bin_button(clear_box, true, hovered_action == notification_center_clear);

    const float list_top = rect.top + notification_center::list_top * scale;
    const float row_h = notification_center::row_height * scale;
    const float row_gap = notification_center::row_gap * scale;
    const float time_w = 56.0f * scale;

    if (small_text_format_)
    {
        const DWRITE_WORD_WRAPPING wrapping = small_text_format_->GetWordWrapping();
        for (std::size_t index = 0; index < rows; ++index)
        {
            const notification_center::Entry* entry = notification_center::entry_at(group, first, index);
            if (!entry)
            {
                break;
            }
            const float row_top = list_top + static_cast<float>(index) * (row_h + row_gap);
            const D2D1_RECT_F row = D2D1::RectF(rect.left + pad_x, row_top, rect.right - pad_x, row_top + row_h);
            draw_card(row, 9.0f * scale, false);

            const float text_left = row.left + 10.0f * scale;
            const float text_right = row.right - 10.0f * scale;

            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            muted_brush_->SetOpacity(0.65f);
            target_->DrawTextW(
                entry->time.c_str(), static_cast<UINT32>(entry->time.size()), small_text_format_.Get(),
                D2D1::RectF(text_right - time_w, row.top + 3.0f * scale, text_right, row.top + 19.0f * scale),
                muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

            small_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            text_brush_->SetOpacity(0.90f);
            target_->DrawTextW(entry->preview.c_str(), static_cast<UINT32>(entry->preview.size()),
                               small_text_format_.Get(),
                               D2D1::RectF(text_left, row.top + 3.0f * scale, text_right - time_w - 4.0f * scale,
                                           row.bottom - 2.0f * scale),
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetWordWrapping(wrapping);
        }
    }

    if (count > rows && limit > 0)
    {
        const float track_top = list_top;
        const float track_height = static_cast<float>(rows) * (row_h + row_gap) - row_gap;
        const float thumb_height =
            std::max(10.0f * scale, track_height * static_cast<float>(rows) / static_cast<float>(count));
        const float thumb_top =
            track_top + (track_height - thumb_height) * static_cast<float>(first) / static_cast<float>(limit);
        const float track_x = rect.right - pad_x + 7.0f * scale;
        const float thumb_width = 3.0f * scale;
        muted_brush_->SetOpacity(0.55f);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(track_x, thumb_top, track_x + thumb_width, thumb_top + thumb_height),
                              thumb_width * 0.5f, thumb_width * 0.5f),
            muted_brush_.Get());
    }

    text_brush_->SetOpacity(0.96f);
    muted_brush_->SetOpacity(0.75f);
}

void Renderer::draw_notification_center_dashboard(const SharedState& state, D2D1_RECT_F rect, float scale)
{
    g_notification_center_unread.store(false, std::memory_order_relaxed);

    if (const notification_center::Group* open = open_notification_group(state))
    {
        draw_notification_center_detail(*open, state.notification_center_view.offset, rect, scale);
        return;
    }

    const float pad_x = notification_center::pad_x * scale;
    const D2D1_RECT_F header_rect =
        D2D1::RectF(rect.left + pad_x, rect.top + 18.0f * scale, rect.right - pad_x, rect.top + 38.0f * scale);
    text_brush_->SetOpacity(0.96f);
    if (bold_text_format_)
    {
        bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const wchar_t* title = loc(L"Notifications");
        target_->DrawTextW(title, static_cast<UINT32>(std::wcslen(title)), bold_text_format_.Get(), header_rect,
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    const notification_center::History* history = state.notification_center.get();
    const std::size_t group_count = history ? history->groups().size() : 0;
    const int capacity = notification_center::visible_rows((rect.bottom - rect.top) / scale);

    const float clear_size = notification_center::clear_size * scale;
    const float clear_top = rect.top + notification_center::clear_top * scale;
    const D2D1_RECT_F clear_box =
        D2D1::RectF(rect.right - pad_x - clear_size, clear_top, rect.right - pad_x, clear_top + clear_size);

    if (group_count > 0 && small_text_format_)
    {
        wchar_t count_buf[48] = {};
        const int hidden = static_cast<int>(group_count) - capacity;
        if (hidden > 0)
        {
            swprintf_s(count_buf, L"%zu \u00B7 +%d", history->total(), hidden);
        }
        else
        {
            swprintf_s(count_buf, L"%zu", history->total());
        }
        const D2D1_RECT_F count_rect =
            D2D1::RectF(header_rect.left, header_rect.top, clear_box.left - 8.0f * scale, header_rect.bottom);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        muted_brush_->SetOpacity(0.75f);
        target_->DrawTextW(count_buf, static_cast<UINT32>(std::wcslen(count_buf)), small_text_format_.Get(), count_rect,
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    draw_tray_bin_button(clear_box, group_count > 0,
                         g_hovered_notification_action.load(std::memory_order_relaxed) == -2);

    const float list_top = rect.top + notification_center::list_top * scale;
    const float list_bottom = rect.bottom - notification_center::list_bottom_inset * scale;

    if (group_count == 0)
    {
        if (text_format_)
        {
            const D2D1_RECT_F empty_rect = D2D1::RectF(rect.left + pad_x, list_top, rect.right - pad_x, list_bottom);
            text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const wchar_t* hint = loc(L"No notifications");
            muted_brush_->SetOpacity(0.80f);
            target_->DrawTextW(hint, static_cast<UINT32>(std::wcslen(hint)), text_format_.Get(), empty_rect,
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        muted_brush_->SetOpacity(0.75f);
        return;
    }

    const float row_h = notification_center::row_height * scale;
    const float row_gap = notification_center::row_gap * scale;
    const float remove_size = notification_center::remove_size * scale;
    const float remove_margin = notification_center::remove_margin * scale;
    const float remove_space = remove_size + remove_margin + 4.0f * scale;
    const float time_w = 56.0f * scale;
    const int hovered_row = g_hovered_notification_row.load(std::memory_order_relaxed);
    const int hovered_action = g_hovered_notification_action.load(std::memory_order_relaxed);

    int drawn = 0;
    for (const notification_center::Group& group : history->groups())
    {
        if (drawn >= capacity)
        {
            break;
        }
        const float row_top = list_top + static_cast<float>(drawn) * (row_h + row_gap);
        const D2D1_RECT_F row = D2D1::RectF(rect.left + pad_x, row_top, rect.right - pad_x, row_top + row_h);
        const bool hovered = hovered_row == drawn;
        draw_card(row, 9.0f * scale, hovered);

        const float icon_size = 24.0f * scale;
        const float icon_top = (row.top + row.bottom - icon_size) * 0.5f;
        const D2D1_RECT_F badge =
            D2D1::RectF(row.left + 7.0f * scale, icon_top, row.left + 7.0f * scale + icon_size, icon_top + icon_size);
        draw_notification_center_icon(group, badge, 7.0f * scale);

        draw_tray_remove_button(
            D2D1::Point2F(row.right - remove_margin - remove_size * 0.5f, (row.top + row.bottom) * 0.5f), remove_size,
            hovered_action == drawn, hovered);

        if (small_text_format_)
        {
            const float text_left = badge.right + 9.0f * scale;
            const float text_right = row.right - remove_space;
            const float mid_y = (row.top + row.bottom) * 0.5f;

            std::wstring heading = group.app;
            if (group.entries.size() > 1)
            {
                heading += L" \u00B7 " + std::to_wstring(group.entries.size());
            }
            text_brush_->SetOpacity(0.94f);
            target_->DrawTextW(
                heading.c_str(), static_cast<UINT32>(heading.size()), small_text_format_.Get(),
                D2D1::RectF(text_left, row.top + 3.0f * scale, text_right - time_w, mid_y + 1.0f * scale),
                text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            muted_brush_->SetOpacity(0.65f);
            const notification_center::Entry& latest = group.latest();
            target_->DrawTextW(
                latest.time.c_str(), static_cast<UINT32>(latest.time.size()), small_text_format_.Get(),
                D2D1::RectF(text_right - time_w, row.top + 3.0f * scale, text_right, mid_y + 1.0f * scale),
                muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

            muted_brush_->SetOpacity(0.78f);
            target_->DrawTextW(latest.preview.c_str(), static_cast<UINT32>(latest.preview.size()),
                               small_text_format_.Get(),
                               D2D1::RectF(text_left, mid_y, text_right, row.bottom - 2.0f * scale), muted_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        ++drawn;
    }

    text_brush_->SetOpacity(0.96f);
    muted_brush_->SetOpacity(0.75f);
}

}
