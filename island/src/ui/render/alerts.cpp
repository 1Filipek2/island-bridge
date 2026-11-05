#include "ui/render/renderer_impl.h"

namespace island
{

// Level banner (volume and brightness)
void Renderer::draw_level_banner(D2D1_RECT_F rect, const wchar_t* glyph, const std::wstring& label,
                                 const wchar_t* value, float pct, bool drained)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f)
    {
        return;
    }
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14, cy - badge_sz * 0.5f, rect.left + 14 + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), raised_strong_brush_.Get());

    text_brush_->SetOpacity(0.95f);

    if (icon_format_)
    {
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    const float tx = badge.right + 14;
    D2D1_RECT_F label_rect = D2D1::RectF(tx, cy - 13.0f, rect.right - 58, cy + 3.0f);
    muted_brush_->SetOpacity(0.50f);
    target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), small_text_format_.Get(), label_rect,
                       muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    D2D1_RECT_F value_rect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
    target_->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), small_text_format_.Get(), value_rect,
                       text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    text_brush_->SetOpacity(0.90f);

    D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
    if (drained)
    {
        ComPtr<ID2D1SolidColorBrush> track_brush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raised_strong, &track_brush)) && track_brush)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f, 2.5f), track_brush.Get());
        }
        D2D1_RECT_F fill =
            D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * pct, track.bottom);
        ComPtr<ID2D1SolidColorBrush> dim;
        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.text_secondary, 0.35f), &dim)) && dim)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.5f, 2.5f), dim.Get());
        }
    }
    else
    {
        draw_accent_track(track, pct, 2.5f);
    }
    accent_brush_->SetOpacity(1.0f);
    muted_brush_->SetOpacity(0.58f);
}

void Renderer::draw_brightness(const SharedState& state, D2D1_RECT_F rect)
{
    wchar_t value[32] = {};
    swprintf_s(value, L"%d%%", state.brightness.percent);
    draw_level_banner(rect, L"\uE706", loc(L"Brightness"), value, clamp(state.brightness.percent / 100.0f, 0.0f, 1.0f),
                      false);
}

void Renderer::draw_marquee_text(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                                 ID2D1Brush* brush, double now, float speed, MarqueeLayoutCache& cache)
{
    if (!format || !brush || text.empty())
    {
        return;
    }

    const float wrap_height = rect.bottom - rect.top;
    // The layout is rebuilt only when text, format or height change; the scroll offset is applied
    // through the draw origin, so the marquee never rebuilds per frame.
    if (cache.text != text || cache.format != format || std::fabs(cache.wrap_width - wrap_height) > 0.01f ||
        !cache.layout)
    {
        cache.layout.Reset();
        dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 2000.0f, wrap_height,
                                          &cache.layout);
        cache.text = text;
        cache.format = format;
        cache.wrap_width = wrap_height;
        cache.metrics = {};
        if (cache.layout)
        {
            cache.layout->GetMetrics(&cache.metrics);
        }
    }

    if (!cache.layout)
    {
        return;
    }

    const float available = rect.right - rect.left;

    D2D1_RECT_F clip_rect = rect;
    clip_rect.top -= 10.0f;
    clip_rect.bottom += 10.0f;
    target_->PushAxisAlignedClip(clip_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (cache.metrics.widthIncludingTrailingWhitespace <= available)
    {
        target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), cache.layout.Get(), brush,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    else
    {
        const float cycle = cache.metrics.widthIncludingTrailingWhitespace + 38.0f;
        const float offset = std::fmod(static_cast<float>(now) * speed, cycle);
        target_->DrawTextLayout(D2D1::Point2F(rect.left - offset, rect.top), cache.layout.Get(), brush,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
        target_->DrawTextLayout(D2D1::Point2F(rect.left - offset + cycle, rect.top), cache.layout.Get(), brush,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    target_->PopAxisAlignedClip();
}

float Renderer::playing_indicator_level(int bar, double now) const
{
    const double t = std::fmod(now, 3600.0);
    const double slow = std::sin(6.283185307179586 * (0.62 + 0.09 * bar) * t + 1.9 * bar);
    const double fast = std::sin(6.283185307179586 * (1.03 + 0.05 * bar) * t + 0.7 * bar * bar);
    const float wave = static_cast<float>(0.6 * slow + 0.4 * fast);
    return smooth_step01(0.5f + 0.5f * wave);
}

float Renderer::playing_indicator_glow(double now) const
{
    const float breath = 0.5f + 0.5f * static_cast<float>(std::sin(6.283185307179586 * 0.45 * std::fmod(now, 3600.0)));
    return 0.22f + 0.14f * breath;
}

void Renderer::draw_playing_indicator(D2D1_RECT_F rect, bool playing, double now)
{
    const bool recently_drawn = indicator_last_drawn_ >= 0.0 && (now - indicator_last_drawn_) < 0.25;
    const float dt = recently_drawn ? clamp(static_cast<float>(now - indicator_last_drawn_), 0.0f, 0.05f) : 0.0f;
    indicator_last_drawn_ = now;
    const float target = playing ? 1.0f : 0.0f;
    if (!recently_drawn)
    {
        indicator_mix_ = target;
    }
    else
    {
        indicator_mix_ += (target - indicator_mix_) * (1.0f - std::exp(-dt / 0.22f));
        if (std::fabs(target - indicator_mix_) < 0.002f)
        {
            indicator_mix_ = target;
        }
        else
        {
            g_layout_dirty = true;
        }
    }

    const float gap = 2.5f;
    const float dot = 2.4f;
    const float available_w = rect.right - rect.left;
    const int count = std::clamp(static_cast<int>((available_w + gap) / (2.0f + gap)), 1, 32);
    const float slot_width = (available_w - gap * (count - 1)) / count;
    const float center_y = (rect.top + rect.bottom) * 0.5f;
    const float max_h = (rect.bottom - rect.top) * 0.78f;
    const float mix = smooth_step01(indicator_mix_);
    const float width = dot + (slot_width - dot) * mix;
    const float t = spec_t_;

    for (int i = 0; i < count; ++i)
    {
        const float level = 0.28f + 0.72f * playing_indicator_level(i, now);
        float h = dot + (std::max(width, level * max_h) - dot) * mix;
        if (t >= 0.0f && count > 1 && playing)
        {
            float f = static_cast<float>(i) / static_cast<float>(count - 1);
            if (spec_dir_ < 0.0f)
            {
                f = 1.0f - f;
            }
            const float u = clamp((t - 0.34f * f) / 0.66f, 0.0f, 1.0f);
            const float dip = std::sin(3.14159265f * u);
            h = std::max(width, h * (1.0f - 0.6f * dip * dip));
        }

        const float cx = rect.left + i * (slot_width + gap) + slot_width * 0.5f;
        const D2D1_RECT_F bar =
            D2D1::RectF(cx - width * 0.5f, center_y - h * 0.5f, cx + width * 0.5f, center_y + h * 0.5f);
        const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(bar, width * 0.5f, width * 0.5f);
        if (mix < 0.999f)
        {
            muted_brush_->SetOpacity(0.5f * (1.0f - mix));
            target_->FillRoundedRectangle(shape, muted_brush_.Get());
        }
        if (mix > 0.001f)
        {
            accent_brush_->SetOpacity(mix * (0.6f + 0.3f * level));
            target_->FillRoundedRectangle(shape, accent_brush_.Get());
        }
    }
    accent_brush_->SetOpacity(1.0f);
}

void Renderer::draw_countdown_progress(float left, float right, float bottom, float progress)
{
    if (!g_settings.status_countdown_progress)
    {
        return;
    }
    const float h = 2.5f;
    D2D1_RECT_F track = D2D1::RectF(left, bottom - h, right, bottom);
    ComPtr<ID2D1SolidColorBrush> track_brush;
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), &track_brush);
    target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f, 1.25f), track_brush.Get());
    D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                   track.left + (track.right - track.left) * clamp(progress, 0.0f, 1.0f), track.bottom);
    accent_brush_->SetOpacity(0.70f);
    target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 1.25f, 1.25f), accent_brush_.Get());
    accent_brush_->SetOpacity(1.0f);
}

void Renderer::draw_clipboard(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 40.0f || rect.right - rect.left < 100.0f)
    {
        return;
    }
    const double now = now_seconds();
    const float ttl = 2.5f;
    const float remaining = clamp(static_cast<float>(state.clipboard.expires_at - now), 0.0f, ttl);
    const float progress = remaining / ttl;

    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14.0f, cy - badge_sz * 0.5f, rect.left + 14.0f + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    if (state.clipboard.image && state.clipboard.image_preview)
    {
        draw_cover_fit_bitmap_pixels(*state.clipboard.image_preview, badge, br, clipboard_image_bitmap_,
                                     clipboard_image_generation_, 1.0f);
    }
    else if (state.clipboard.app_icon)
    {
        draw_rounded_bitmap_pixels(*state.clipboard.app_icon, badge, br, clipboard_icon_bitmap_,
                                   clipboard_icon_generation_, 0.96f);
    }
    else
    {
        const wchar_t* glyph = state.clipboard.image ? (using_fluent_icons_ ? L"\uE91B" : L"\uE114")
                                                     : (using_fluent_icons_ ? L"\uF0E3" : L"\uE8C8");
        text_brush_->SetOpacity(0.95f);

        if (icon_format_)
        {
            icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), icon_format_.Get(), badge,
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        text_brush_->SetOpacity(0.90f);
    }

    const float tx = badge.right + 14.0f;
    const bool show_bar = g_settings.status_countdown_progress;
    D2D1_RECT_F title_rect = show_bar ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
                                      : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
    muted_brush_->SetOpacity(0.48f);
    const std::wstring clip_title =
        state.clipboard.app_name.empty()
            ? (state.clipboard.image ? std::wstring(L"Image copied") : std::wstring(L"Clipboard"))
            : state.clipboard.app_name + L"  \u00b7  Clipboard";
    target_->DrawTextW(clip_title.c_str(), static_cast<UINT32>(clip_title.size()), small_text_format_.Get(), title_rect,
                       muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    D2D1_RECT_F text_rect = show_bar ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
                                     : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
    draw_marquee_text(state.clipboard.text.empty() ? std::wstring(loc(L"Copied")) : state.clipboard.text, text_rect,
                      text_format_.Get(), text_brush_.Get(), now, 34.0f, marquee_clipboard_cache_);

    draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
    muted_brush_->SetOpacity(0.58f);
}

void Renderer::draw_notification(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 48.0f || rect.right - rect.left < 120.0f)
    {
        return;
    }
    const double now = now_seconds();
    const float ttl = 4.0f;
    const float remaining = clamp(static_cast<float>(state.notification.expires_at - now), 0.0f, ttl);
    const float progress = remaining / ttl;

    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float icon_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - icon_sz * 0.5f, rect.left + 14 + icon_sz, cy + icon_sz * 0.5f);
    const float br = icon_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> plate_brush;
    target_->CreateSolidColorBrush(material_.raised_strong, &plate_brush);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), plate_brush.Get());

    if (state.notification.icon)
    {
        draw_rounded_bitmap_pixels(*state.notification.icon, badge, br, notification_icon_bitmap_,
                                   notification_icon_generation_, 1.0f);

        ComPtr<ID2D1SolidColorBrush> badge_color;
        target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f), &badge_color);

        const float dot_r = icon_sz * 0.13f;
        const float dot_x = badge.right - dot_r * 0.5f;
        const float dot_y = badge.top + dot_r * 0.5f;

        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_x, dot_y), dot_r, dot_r), badge_color.Get());

        ComPtr<ID2D1SolidColorBrush> badge_border;
        target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &badge_border);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(dot_x, dot_y), dot_r, dot_r), badge_border.Get(), 0.9f);
    }
    else if (const wchar_t initial = notification_initial(state.notification.app); initial != 0 && bold_text_format_)
    {
        const DWRITE_TEXT_ALIGNMENT text_alignment = bold_text_format_->GetTextAlignment();
        const DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment = bold_text_format_->GetParagraphAlignment();
        bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        bold_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text_brush_->SetOpacity(0.95f);
        target_->DrawTextW(&initial, 1, bold_text_format_.Get(), badge, text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        bold_text_format_->SetTextAlignment(text_alignment);
        bold_text_format_->SetParagraphAlignment(paragraph_alignment);
        text_brush_->SetOpacity(0.90f);
    }
    else
    {
        const wchar_t* glyph = using_fluent_icons_ ? L"\uEA8F" : L"\uE7E7";
        text_brush_->SetOpacity(0.95f);
        if (icon_format_)
        {
            icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), icon_format_.Get(), badge,
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        text_brush_->SetOpacity(0.90f);
    }

    const float tx = badge.right + 14.0f;
    const bool show_bar = g_settings.status_countdown_progress;
    D2D1_RECT_F app_rect = show_bar ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
                                    : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
    muted_brush_->SetOpacity(0.75f);
    target_->DrawTextW(state.notification.app.c_str(), static_cast<UINT32>(state.notification.app.size()),
                       small_text_format_.Get(), app_rect, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    D2D1_RECT_F title_rect = show_bar ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
                                      : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
    text_brush_->SetOpacity(0.95f);
    draw_marquee_text(state.notification.title.empty() ? L"Notification" : state.notification.title, title_rect,
                      text_format_.Get(), text_brush_.Get(), now, 28.0f, marquee_notification_cache_);
    text_brush_->SetOpacity(0.90f);

    draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
    muted_brush_->SetOpacity(0.50f);
}

void Renderer::draw_volume(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f)
    {
        return;
    }
    const bool muted = state.volume.muted || state.volume.percent == 0;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14, cy - badge_sz * 0.5f, rect.left + 14 + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    const wchar_t* glyph = muted ? L"\uE74F" : (using_fluent_icons_ ? L"\uE767" : L"\uE993");
    text_brush_->SetOpacity(0.95f);

    if (icon_format_)
    {
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    const float tx = badge.right + 14;
    D2D1_RECT_F label_rect = D2D1::RectF(tx, cy - 13.0f, rect.right - 58, cy + 3.0f);
    muted_brush_->SetOpacity(0.50f);
    const std::wstring device_label =
        state.volume.device_name.empty() ? std::wstring(loc(L"Volume")) : state.volume.device_name;
    target_->DrawTextW(device_label.c_str(), static_cast<UINT32>(device_label.size()), small_text_format_.Get(),
                       label_rect, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    wchar_t value[32] = {};
    if (muted)
    {
        wcscpy_s(value, ARRAYSIZE(value), loc(L"Muted"));
    }
    else
    {
        swprintf_s(value, L"%d%%", state.volume.percent);
    }
    D2D1_RECT_F value_rect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
    target_->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), small_text_format_.Get(), value_rect,
                       text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    text_brush_->SetOpacity(0.90f);

    D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
    const float pct = clamp(state.volume.display_percent / 100.0f, 0.0f, 1.0f);
    if (muted)
    {
        ComPtr<ID2D1SolidColorBrush> track_brush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raised_strong, &track_brush)) && track_brush)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f, 2.5f), track_brush.Get());
        }
        D2D1_RECT_F fill =
            D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * pct, track.bottom);
        ComPtr<ID2D1SolidColorBrush> dim;
        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.text_secondary, 0.35f), &dim)) && dim)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.5f, 2.5f), dim.Get());
        }
    }
    else
    {
        draw_accent_track(track, pct, 2.5f);
    }
    accent_brush_->SetOpacity(1.0f);
    muted_brush_->SetOpacity(0.58f);
}

void Renderer::draw_timer(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f)
    {
        return;
    }
    const double now = now_seconds();
    double remaining = state.timer.just_finished ? 0.0
                                                 : (state.timer.running ? std::max(0.0, state.timer.ends_at - now)
                                                                        : state.timer.remaining_at_pause);
    const int total_sec = state.timer.total_seconds > 0 ? state.timer.total_seconds : 1;
    const float progress =
        state.timer.just_finished ? 1.0f : clamp(1.0f - static_cast<float>(remaining / total_sec), 0.0f, 1.0f);

    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14, cy - badge_sz * 0.5f, rect.left + 14 + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    const float ring_r = badge_sz * 0.30f;
    D2D1_POINT_2F ring_center = D2D1::Point2F((badge.left + badge.right) * 0.5f, (badge.top + badge.bottom) * 0.5f);
    ComPtr<ID2D1SolidColorBrush> ring_track;
    target_->CreateSolidColorBrush(material_.raised_strong, &ring_track);
    target_->DrawEllipse(D2D1::Ellipse(ring_center, ring_r, ring_r), ring_track.Get(), 2.0f);

    ComPtr<ID2D1PathGeometry> geometry;
    d2d_factory_->CreatePathGeometry(&geometry);
    ComPtr<ID2D1GeometrySink> sink;
    geometry->Open(&sink);
    const float start = -3.14159265f * 0.5f;
    const float sweep = 2.0f * 3.14159265f * progress;
    const int segments = std::max(2, static_cast<int>(40 * progress));
    auto point_at = [&](float a)
    {
        return D2D1::Point2F(ring_center.x + std::cos(a) * ring_r, ring_center.y + std::sin(a) * ring_r);
    };
    sink->BeginFigure(point_at(start), D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i <= segments; ++i)
    {
        sink->AddLine(point_at(start + sweep * i / segments));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    ComPtr<ID2D1SolidColorBrush> ring_fg;
    D2D1_COLOR_F fg_color =
        state.timer.is_break ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f) : D2D1::ColorF(1.0f, 0.58f, 0.0f, 1.0f);
    if (state.timer.just_finished)
    {
        fg_color = D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f);
    }
    target_->CreateSolidColorBrush(fg_color, &ring_fg);
    target_->DrawGeometry(geometry.Get(), ring_fg.Get(), 2.4f);

    if (state.timer.active && !state.timer.running)
    {
        ComPtr<ID2D1SolidColorBrush> pause_brush;
        target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &pause_brush);
        const float h = ring_r * 0.7f;
        target_->FillRectangle(
            D2D1::RectF(ring_center.x - 2.6f, ring_center.y - h * 0.5f, ring_center.x - 0.8f, ring_center.y + h * 0.5f),
            pause_brush.Get());
        target_->FillRectangle(
            D2D1::RectF(ring_center.x + 0.8f, ring_center.y - h * 0.5f, ring_center.x + 2.6f, ring_center.y + h * 0.5f),
            pause_brush.Get());
    }

    const float tx = badge.right + 14;
    muted_brush_->SetOpacity(0.50f);
    std::wstring label = state.timer.just_finished ? (state.timer.is_break ? L"Break Complete" : L"Focus Complete")
                                                   : (state.timer.is_break ? L"Break Timer" : L"Focus Timer");
    D2D1_RECT_F label_rect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
    target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), small_text_format_.Get(), label_rect,
                       muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    wchar_t value[32] = {};
    int rem = static_cast<int>(std::ceil(remaining));
    swprintf_s(value, L"%d:%02d", rem / 60, rem % 60);
    D2D1_RECT_F value_rect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
    text_brush_->SetOpacity(0.95f);
    target_->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), text_format_.Get(), value_rect,
                       text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    text_brush_->SetOpacity(0.90f);
    muted_brush_->SetOpacity(0.58f);
}

void Renderer::draw_caps_lock(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 110.0f)
    {
        return;
    }
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14, cy - badge_sz * 0.5f, rect.left + 14 + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    const wchar_t* glyph = nullptr;
    std::wstring label;
    bool is_on = false;

    if (state.caps_lock.is_num_event)
    {
        glyph = L"1";
        label = loc(L"Num Lock");
        is_on = state.caps_lock.num_on;
    }
    else
    {
        glyph = L"A";
        label = loc(L"Caps Lock");
        is_on = state.caps_lock.caps_on;
    }

    text_brush_->SetOpacity(0.95f);
    target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), clock_format_.Get(), badge, text_brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);

    ComPtr<ID2D1SolidColorBrush> led_brush;
    D2D1_COLOR_F led_color = is_on ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f);
    target_->CreateSolidColorBrush(led_color, &led_brush);

    const float led_r = 2.2f;
    D2D1_POINT_2F led_center = D2D1::Point2F(badge.right - 5.5f, badge.top + 5.5f);
    target_->FillEllipse(D2D1::Ellipse(led_center, led_r, led_r), led_brush.Get());

    const float tx = badge.right + 14.0f;
    D2D1_RECT_F label_rect = D2D1::RectF(tx, cy - 9.0f, rect.right - 46.0f, cy + 11.0f);
    text_brush_->SetOpacity(0.95f);
    target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), text_format_.Get(), label_rect,
                       text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    std::wstring status = is_on ? loc(L"On") : loc(L"Off");
    D2D1_RECT_F status_rect = D2D1::RectF(rect.right - 44.0f, cy - 9.0f, rect.right - 14.0f, cy + 11.0f);
    if (is_on)
    {
        text_brush_->SetOpacity(0.95f);
        target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), text_format_.Get(), status_rect,
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    else
    {
        muted_brush_->SetOpacity(0.75f);
        target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), text_format_.Get(), status_rect,
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Renderer::draw_device(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 100.0f)
    {
        return;
    }
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const bool connected = (state.device.event_type == DeviceEventType::Connected);

    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14, cy - badge_sz * 0.5f, rect.left + 14 + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    const wchar_t* glyph = using_fluent_icons_ ? L"\uECF0" : L"\uE88E";
    text_brush_->SetOpacity(0.95f);
    if (icon_format_)
    {
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    text_brush_->SetOpacity(0.90f);

    ComPtr<ID2D1SolidColorBrush> dot_brush;
    D2D1_COLOR_F dot_color =
        connected ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f) : D2D1::ColorF(1.0f, 0.27f, 0.22f, 1.0f);
    target_->CreateSolidColorBrush(dot_color, &dot_brush);

    D2D1_POINT_2F dot_center = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
    target_->FillEllipse(D2D1::Ellipse(dot_center, 4.5f, 4.5f), dot_brush.Get());

    const float tx = badge.right + 14.0f;
    const bool show_bar = g_settings.status_countdown_progress;
    const double now = now_seconds();
    const float ttl = 3.0f;
    const float remaining = clamp(static_cast<float>(state.device.expires_at - now), 0.0f, ttl);
    const float progress = remaining / ttl;

    muted_brush_->SetOpacity(0.50f);
    std::wstring label = connected ? L"Device Connected" : L"Device Removed";
    D2D1_RECT_F label_rect = show_bar ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
                                      : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
    target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), small_text_format_.Get(), label_rect,
                       muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    text_brush_->SetOpacity(0.95f);
    const std::wstring& name =
        state.device.device_name.empty()
            ? (state.device.is_bluetooth_like ? std::wstring(L"Bluetooth") : std::wstring(L"USB Device"))
            : state.device.device_name;
    D2D1_RECT_F name_rect = show_bar ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
                                     : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
    target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()), text_format_.Get(), name_rect, text_brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    text_brush_->SetOpacity(0.90f);

    draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
    muted_brush_->SetOpacity(0.58f);
}

void Renderer::draw_do_not_disturb(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 110.0f)
    {
        return;
    }
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14.0f, cy - badge_sz * 0.5f, rect.left + 14.0f + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    const bool is_on = state.do_not_disturb.enabled;
    const wchar_t* glyph = is_on ? L"\uE7ED" : L"\uEA8F";
    text_brush_->SetOpacity(0.95f);
    if (icon_format_)
    {
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    ComPtr<ID2D1SolidColorBrush> led_brush;
    D2D1_COLOR_F led_color = is_on ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f);
    target_->CreateSolidColorBrush(led_color, &led_brush);

    const float led_r = 2.2f;
    D2D1_POINT_2F led_center = D2D1::Point2F(badge.right - 5.5f, badge.top + 5.5f);
    target_->FillEllipse(D2D1::Ellipse(led_center, led_r, led_r), led_brush.Get());

    const float tx = badge.right + 14.0f;
    D2D1_RECT_F label_rect = D2D1::RectF(tx, cy - 9.0f, rect.right - 46.0f, cy + 11.0f);
    text_brush_->SetOpacity(0.95f);
    std::wstring label = loc(L"Do Not Disturb");
    target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), text_format_.Get(), label_rect,
                       text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    std::wstring status = is_on ? loc(L"On") : loc(L"Off");
    D2D1_RECT_F status_rect = D2D1::RectF(rect.right - 44.0f, cy - 9.0f, rect.right - 14.0f, cy + 11.0f);
    if (is_on)
    {
        text_brush_->SetOpacity(0.95f);
        target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), text_format_.Get(), status_rect,
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    else
    {
        muted_brush_->SetOpacity(0.75f);
        target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), text_format_.Get(), status_rect,
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const double now = now_seconds();
    const float ttl = 3.0f;
    const float remaining = clamp(static_cast<float>(state.do_not_disturb.expires_at - now), 0.0f, ttl);
    const float progress = remaining / ttl;
    draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 4.0f, progress);
    muted_brush_->SetOpacity(0.58f);
    text_brush_->SetOpacity(0.90f);
}

void Renderer::draw_bluetooth_category_icon(D2D1_RECT_F badge, BluetoothDeviceCategory category)
{
    const wchar_t* glyph = L"\uE702";
    switch (category)
    {
        case BluetoothDeviceCategory::Headphones:
            glyph = L"\uE7F6";
            break;
        case BluetoothDeviceCategory::Speaker:
            glyph = L"\uE7F5";
            break;
        case BluetoothDeviceCategory::Mouse:
            glyph = L"\uE962";
            break;
        case BluetoothDeviceCategory::Keyboard:
            glyph = L"\uE92E";
            break;
        case BluetoothDeviceCategory::Phone:
            glyph = L"\uE8EA";
            break;
        case BluetoothDeviceCategory::Generic:
        default:
            glyph = L"\uE702";
            break;
    }

    text_brush_->SetOpacity(0.95f);
    if (icon_format_)
    {
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        target_->DrawTextW(glyph, static_cast<UINT32>(std::wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    text_brush_->SetOpacity(0.90f);
}

void Renderer::draw_bluetooth_device(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f)
    {
        return;
    }
    const double now = now_seconds();
    const float ttl = 4.0f;
    const float remaining = clamp(static_cast<float>(state.bluetooth_device.expires_at - now), 0.0f, ttl);
    const float progress = remaining / ttl;

    const bool connected = state.bluetooth_device.connected;
    const int battery = state.bluetooth_device.battery_percent;
    const bool has_battery = g_settings.bluetooth_show_battery && battery >= 0;

    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14, cy - badge_sz * 0.5f, rect.left + 14 + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    draw_bluetooth_category_icon(badge, state.bluetooth_device.category);

    ComPtr<ID2D1SolidColorBrush> dot_brush;
    D2D1_COLOR_F dot_color =
        connected ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f) : D2D1::ColorF(1.0f, 0.27f, 0.22f, 1.0f);
    target_->CreateSolidColorBrush(dot_color, &dot_brush);
    D2D1_POINT_2F dot_center = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
    target_->FillEllipse(D2D1::Ellipse(dot_center, 4.5f, 4.5f), dot_brush.Get());

    const float tx = badge.right + 14.0f;
    const bool show_bar = g_settings.status_countdown_progress;
    const float right_edge = has_battery ? rect.right - 62.0f : rect.right - 14.0f;

    muted_brush_->SetOpacity(0.50f);
    std::wstring label =
        std::wstring(loc(L"Bluetooth")) + L" " + (connected ? loc(L"Connected") : loc(L"Disconnected"));
    D2D1_RECT_F label_rect = show_bar ? D2D1::RectF(tx, cy - 18.0f, right_edge, cy - 2.0f)
                                      : D2D1::RectF(tx, cy - 16.0f, right_edge, cy - 1.0f);
    target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), small_text_format_.Get(), label_rect,
                       muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    text_brush_->SetOpacity(0.95f);
    const std::wstring& name = state.bluetooth_device.device_name.empty() ? std::wstring(L"Bluetooth Device")
                                                                          : state.bluetooth_device.device_name;
    D2D1_RECT_F name_rect = show_bar ? D2D1::RectF(tx, cy - 2.0f, right_edge, cy + 15.0f)
                                     : D2D1::RectF(tx, cy - 1.0f, right_edge, cy + 16.0f);
    target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()), text_format_.Get(), name_rect, text_brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);

    if (has_battery)
    {
        const float col_center = rect.right - 31.0f;
        const float bw = 16.0f;
        const float bh = 8.5f;
        const float nub_w = 1.6f;
        const float total_w = bw + nub_w;
        const float bat_left = col_center - total_w * 0.5f;
        const float bat_y = show_bar ? cy - 10.0f : cy - 9.0f;

        D2D1_RECT_F bat_rect = D2D1::RectF(bat_left, bat_y - bh * 0.5f, bat_left + bw, bat_y + bh * 0.5f);
        ComPtr<ID2D1SolidColorBrush> bat_border;
        target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.75f), &bat_border);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(bat_rect, 1.5f, 1.5f), bat_border.Get(), 1.2f);
        D2D1_RECT_F nub = D2D1::RectF(bat_rect.right, bat_y - 2.0f, bat_rect.right + nub_w, bat_y + 2.0f);
        target_->FillRectangle(nub, bat_border.Get());

        const float pct = clamp(battery / 100.0f, 0.0f, 1.0f);
        D2D1_RECT_F fill = D2D1::RectF(bat_rect.left + 1.5f, bat_rect.top + 1.5f,
                                       bat_rect.left + 1.5f + (bw - 3.0f) * pct, bat_rect.bottom - 1.5f);
        ComPtr<ID2D1SolidColorBrush> fill_brush;
        D2D1_COLOR_F fill_color = battery <= 20 ? D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f) : D2D1::ColorF(1, 1, 1, 0.92f);
        target_->CreateSolidColorBrush(fill_color, &fill_brush);
        target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 0.8f, 0.8f), fill_brush.Get());

        wchar_t pct_buf[16] = {};
        swprintf_s(pct_buf, L"%d%%", battery);
        D2D1_RECT_F pct_rect = show_bar ? D2D1::RectF(col_center - 25.0f, cy - 2.0f, col_center + 25.0f, cy + 14.0f)
                                        : D2D1::RectF(col_center - 25.0f, cy - 1.0f, col_center + 25.0f, cy + 15.0f);
        text_brush_->SetOpacity(0.85f);
        if (small_text_format_)
        {
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            target_->DrawTextW(pct_buf, static_cast<UINT32>(std::wcslen(pct_buf)), small_text_format_.Get(), pct_rect,
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
    muted_brush_->SetOpacity(0.58f);
    text_brush_->SetOpacity(0.90f);
}

void Renderer::draw_battery(const SharedState& state, D2D1_RECT_F rect)
{
    if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f)
    {
        return;
    }
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float badge_sz = (rect.bottom - rect.top) - 16.0f;
    D2D1_RECT_F badge =
        D2D1::RectF(rect.left + 14, cy - badge_sz * 0.5f, rect.left + 14 + badge_sz, cy + badge_sz * 0.5f);
    const float br = badge_sz * 0.35f;

    ComPtr<ID2D1SolidColorBrush> badge_bg;
    target_->CreateSolidColorBrush(material_.raised_strong, &badge_bg);
    target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badge_bg.Get());

    const float bx = badge.left + badge_sz * 0.25f;
    const float by = cy - badge_sz * 0.22f;
    const float bw = badge_sz * 0.45f;
    const float bh = badge_sz * 0.44f;
    D2D1_RECT_F bat_rect = D2D1::RectF(bx, by, bx + bw, by + bh);

    ComPtr<ID2D1SolidColorBrush> bat_border;
    target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.85f), &bat_border);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(bat_rect, 2, 2), bat_border.Get(), 1.5f);

    D2D1_RECT_F nub_rect = D2D1::RectF(bat_rect.right, cy - 3, bat_rect.right + 2.5f, cy + 3);
    target_->FillRectangle(nub_rect, bat_border.Get());

    const float pct = clamp(state.battery.percent / 100.0f, 0.0f, 1.0f);
    D2D1_RECT_F fill_rect =
        D2D1::RectF(bat_rect.left + 2, bat_rect.top + 2, bat_rect.left + 2 + (bw - 4) * pct, bat_rect.bottom - 2);

    ComPtr<ID2D1SolidColorBrush> bat_fill;
    if (state.battery.low)
    {
        target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f), &bat_fill);
    }
    else if (state.battery.charging)
    {
        target_->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f), &bat_fill);
    }
    else
    {
        target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), &bat_fill);
    }
    target_->FillRoundedRectangle(D2D1::RoundedRect(fill_rect, 1, 1), bat_fill.Get());

    const float tx = badge.right + 14;
    D2D1_RECT_F label_rect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
    muted_brush_->SetOpacity(0.50f);
    std::wstring label = state.battery.charging ? L"Power Connected" : L"Battery Alert";
    target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), small_text_format_.Get(), label_rect,
                       muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    wchar_t value[128] = {};
    if (state.battery.seconds_remaining != BATTERY_LIFE_UNKNOWN && !state.battery.charging)
    {
        const DWORD minutes = state.battery.seconds_remaining / 60;
        swprintf_s(value, ARRAYSIZE(value), L"%d%% \u2022 %luh %02lum left", state.battery.percent, minutes / 60,
                   minutes % 60);
    }
    else
    {
        swprintf_s(value, ARRAYSIZE(value), L"%d%%", state.battery.percent);
    }

    D2D1_RECT_F value_rect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
    text_brush_->SetOpacity(0.95f);
    target_->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), text_format_.Get(), value_rect,
                       text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    text_brush_->SetOpacity(0.90f);
}

void Renderer::draw_progress(const SharedState& state, D2D1_RECT_F rect)
{
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"Progress %d%%", state.progress.percent);
    IDWriteTextFormat* fmt = idle_text_format_ ? idle_text_format_.Get() : text_format_.Get();
    target_->DrawTextW(buffer, static_cast<UINT32>(std::wcslen(buffer)), fmt, rect, text_brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::draw_progress_ring(D2D1_RECT_F rect, int percent)
{
    ComPtr<ID2D1PathGeometry> geometry;
    d2d_factory_->CreatePathGeometry(&geometry);
    ComPtr<ID2D1GeometrySink> sink;
    geometry->Open(&sink);

    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float rx = (rect.right - rect.left) * 0.5f + 6.0f;
    const float ry = (rect.bottom - rect.top) * 0.5f + 6.0f;
    const float start = -3.14159265f * 0.5f;
    const float sweep = 2.0f * 3.14159265f * clamp(percent / 100.0f, 0.0f, 1.0f);
    const int segments = std::max(2, static_cast<int>(48 * percent / 100.0f));

    auto point_at = [&](float a)
    {
        return D2D1::Point2F(cx + std::cos(a) * rx, cy + std::sin(a) * ry);
    };

    sink->BeginFigure(point_at(start), D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i <= segments; ++i)
    {
        const float a = start + sweep * i / segments;
        sink->AddLine(point_at(a));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    accent_brush_->SetOpacity(0.92f);
    target_->DrawGeometry(geometry.Get(), accent_brush_.Get(), 3.0f);
    accent_brush_->SetOpacity(1.0f);
}

}
