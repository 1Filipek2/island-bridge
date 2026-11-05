#include "ui/render/renderer_impl.h"

namespace island
{

bool Renderer::initialize(HWND hwnd)
{
    hwnd_ = hwnd;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                   reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (FAILED(hr))
    {
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(hr))
    {
        return false;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

    hr = d2d_factory_->CreateDCRenderTarget(&props, &target_);
    if (FAILED(hr))
    {
        return false;
    }

    const Settings initial_settings = get_settings_copy();
    ensure_text_formats(initial_settings.size_scale, initial_settings.font_family, initial_settings.text_scale);

    return create_backing_bitmap(520, 140);
}

bool Renderer::render(const SharedState& state, const Settings& settings, const Activity& primary,
                      const std::optional<Activity>& secondary, float width, float height, float nudge, bool hover,
                      bool pinned, double now)
{
    ensure_text_formats(settings.size_scale, settings.font_family, settings.text_scale);

    width = std::max(width, 0.0f);
    if (uses_fixed_stage(settings))
    {
        width = std::min(width, stage_content_width * settings.size_scale);
        height = std::min(height, stage_content_height * settings.size_scale);
    }

    const StageLayout stage = compute_stage_layout(settings, width, height);
    const int pixel_width = stage.width;
    const int pixel_height = stage.height;

    const bool flush_top = settings.notch_style || settings.border_merged_mode;
    const float top = stage.pill_top + (flush_top ? std::max(0.0f, nudge) : nudge);
    const float left = stage.pill_left;

    const BYTE alpha = static_cast<BYTE>(clamp(settings.pill_opacity, 0.35f, 1.0f) * 255.0f);
    const bool size_changed = pixel_width != bitmap_width_ || pixel_height != bitmap_height_;
    const bool moved = stage.origin.x != last_origin_.x || stage.origin.y != last_origin_.y;

    // Full repaint when the window itself changed, plus once a second as a safety net.
    const bool full_paint = !use_dirty_rect || !stage_valid_ || size_changed || moved || alpha != last_alpha_ ||
                            (now - last_full_paint_) > 1.0;

    if (size_changed)
    {
        if (!create_backing_bitmap(pixel_width, pixel_height))
        {
            return false;
        }
    }
    else if (g_layout_dirty.exchange(false))
    {
        upkeep_pending_ = true;
    }
    if (upkeep_pending_)
    {
        if (now - last_upkeep_at_ > 0.25)
        {
            upkeep_pending_ = false;
            last_upkeep_at_ = now;
            apply_overlay_z_order(hwnd_, pixel_width, pixel_height);
        }
        else
        {
            g_layout_dirty = true;
        }
    }

    // Pixels that can change this frame: the pill with shadow and nudge slack, plus last frame's area.
    // Split pills are laid out from their own widths, which can reach past the animated total.
    const float split_gap = 12.0f * settings.size_scale;
    const float drawn_width = secondary ? std::max(width, primary.width + split_gap + secondary->width) : width;
    const float drawn_height = secondary ? std::max(height, std::max(primary.height, secondary->height)) : height;

    RECT bounds;
    bounds.left = std::max(0, static_cast<int>(std::floor(left - render_pad_x - 4.0f)));
    bounds.top = std::max(0, static_cast<int>(std::floor(top - render_pad_y - 12.0f)));
    bounds.right = std::min(pixel_width, static_cast<int>(std::ceil(left + drawn_width + render_pad_x + 4.0f)));
    bounds.bottom = std::min(pixel_height, static_cast<int>(std::ceil(top + drawn_height + render_pad_y + 12.0f)));

    RECT dirty = {0, 0, pixel_width, pixel_height};
    if (!full_paint)
    {
        UnionRect(&dirty, &last_bounds_, &bounds);
    }

    if (IsRectEmpty(&dirty))
    {
        dirty = RECT{0, 0, pixel_width, pixel_height};
    }

    // Bind only the dirty region (binding the whole stage copied all of it every frame); the translation
    // restores stage coordinates.
    RECT rc = dirty;
    HRESULT hr = target_->BindDC(mem_dc_, &rc);
    if (FAILED(hr))
    {
        return false;
    }

    pill_count_ = 0; // draw_pill records each pill it draws for the backdrop windows

    target_->BeginDraw();
    target_->SetTransform(
        D2D1::Matrix3x2F::Translation(-static_cast<float>(dirty.left), -static_cast<float>(dirty.top)));
    // Clear() honours the active clip, so this only wipes the dirty region.
    target_->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(dirty.left), static_cast<float>(dirty.top),
                                             static_cast<float>(dirty.right), static_cast<float>(dirty.bottom)),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
    target_->Clear(D2D1::ColorF(0, 0.0f));
    begin_frame_resources();

    ensure_brushes(settings, state, now);
    settings_opacity_ = settings.pill_opacity;

    const bool game_metrics_present =
        primary.kind == IslandKind::Idle && (settings.game_overlay || g_game_overlay_pinned.load());
    island_hiding_ = primary.width < 1.0f && primary.height < 1.0f && !secondary;

    const float hover_scale =
        (settings.expand_on_hover && ((hover && !game_metrics_present) || pinned)) ? 1.025f : 1.0f;
    const float scale = hover_scale;
    hover_scale_ = scale;

    if (width >= 2.0f && height >= 2.0f)
    {
        if (secondary)
        {
            const float gap = split_gap;
            const float max_h = std::max(primary.height, secondary->height);
            // Notch and border-merged islands hang from the top edge.
            const bool hang_from_top = settings.notch_style || settings.border_merged_mode;
            const float p_top = hang_from_top ? top : top + (max_h - primary.height) * 0.5f;
            const float s_top = hang_from_top ? top : top + (max_h - secondary->height) * 0.5f;

            draw_pill(state, settings, primary, D2D1::RectF(left, p_top, left + primary.width, p_top + primary.height),
                      scale, now);
            draw_pill(state, settings, *secondary,
                      D2D1::RectF(left + primary.width + gap, s_top, left + primary.width + gap + secondary->width,
                                  s_top + secondary->height),
                      scale, now);
        }
        else
        {
            draw_pill(state, settings, primary, D2D1::RectF(left, top, left + width, top + height), scale, now);
        }
    }

    target_->PopAxisAlignedClip();
    hr = target_->EndDraw();
    if (FAILED(hr))
    {
        stage_valid_ = false;
        return false;
    }

    POINT src = {0, 0};
    SIZE size = {bitmap_width_, bitmap_height_};
    POINT dst = stage.origin; // constant while the pill animates

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UPDATELAYEREDWINDOWINFO info = {};
    info.cbSize = sizeof(info);
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = mem_dc_;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = full_paint ? nullptr : &dirty; // only this rect is re-composited

    const bool presented = UpdateLayeredWindowIndirect(hwnd_, &info) != FALSE;
    if (presented)
    {
        stage_valid_ = true;
        last_origin_ = stage.origin;
        last_bounds_ = bounds;
        last_alpha_ = alpha;
        if (full_paint)
        {
            last_full_paint_ = now;
        }
        g_pill_client_rect = PillClientRect{left, top, left + width, top + height, true};
    }
    else
    {
        stage_valid_ = false;
    }

    if (presented)
    {
        dwm_backdrop::sync(hwnd_, stage.origin, pill_rects_, pill_radii_, pill_count_, settings.notch_style,
                           settings.backdrop_material, pill_bg_color_, settings.backdrop_tint);
    }
    return presented;
}

void Renderer::shutdown()
{
    release_cached_resources();
    marquee_title_cache_.layout.Reset();
    marquee_artist_cache_.layout.Reset();
    marquee_album_cache_.layout.Reset();
    marquee_clipboard_cache_.layout.Reset();
    marquee_notification_cache_.layout.Reset();
    scratch_color_brush_.Reset();
    karaoke_cache_.layout.Reset();

    art_bitmap_.Reset();
    notification_icon_bitmap_.Reset();
    for (ComPtr<ID2D1Bitmap>& bitmap : notification_center_bitmaps_)
    {
        bitmap.Reset();
    }
    notification_center_generations_.fill(0);
    media_source_icon_bitmap_.Reset();
    clipboard_icon_bitmap_.Reset();
    clipboard_image_bitmap_.Reset();
    accent_brush_.Reset();
    red_brush_.Reset();
    text_brush_.Reset();
    muted_brush_.Reset();
    tint_brush_.Reset();
    shadow_brush_.Reset();
    mic_dot_brush_.Reset();
    unread_dot_brush_.Reset();
    mic_glow_brush_.Reset();
    cam_dot_brush_.Reset();
    cam_glow_brush_.Reset();
    weather_desc_format_.Reset();
    mic_glow_brush_.Reset();
    cam_dot_brush_.Reset();
    cam_glow_brush_.Reset();
    target_.Reset();
    text_format_.Reset();
    small_text_format_.Reset();
    bold_text_format_.Reset();
    huge_text_format_.Reset();
    clock_format_.Reset();
    icon_format_.Reset();
    media_play_icon_format_.Reset();
    media_nav_icon_format_.Reset();
    idle_text_format_.Reset();
    cal_day_large_format_.Reset();
    cal_grid_format_.Reset();
    dwrite_factory_.Reset();
    skip_tri_full_.Reset();
    skip_tri_notch_.Reset();
    title_old_.clear();
    title_new_.clear();
    title_anim_active_ = false;
    round_join_style_.Reset();
    art_flip_old_.Reset();
    pill_art_old_.Reset();
    pill_art_last_.Reset();
    pill_art_start_ = -1.0;
    pill_art_init_ = false;
    swap_artist_.prev_cache.layout.Reset();
    swap_album_.prev_cache.layout.Reset();
    d2d_factory_.Reset();

    if (old_bitmap_)
    {
        SelectObject(mem_dc_, old_bitmap_);
        old_bitmap_ = nullptr;
    }
    if (dib_)
    {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    if (mem_dc_)
    {
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
}

// Sizes the island to the idle text it is about to paint; draw_idle_dashboard uses the same numbers.
IdleStripMetrics Renderer::measure_idle_strip(const SharedState& state, const Settings& settings, double now)
{
    // The text scale drives the idle font size, so the formats must be current before measuring.
    ensure_text_formats(settings.size_scale, settings.font_family, settings.text_scale);

    IdleStripMetrics metrics;
    IDWriteTextFormat* fmt = idle_text_format_ ? idle_text_format_.Get() : small_text_format_.Get();

    SYSTEMTIME local = {};
    GetLocalTime(&local);
    const std::wstring clock =
        format_island_time(local, settings.clock_follow_system, settings.use24_hour_clock, settings.show_seconds);
    metrics.clock_width = measure_text_width_cached(widest_digit_form(clock), fmt, idle_clock_width_cache_);

    metrics.has_weather = settings.weather;
    if (metrics.has_weather)
    {
        const bool has_data = state.weather.has_data && (now - state.weather.last_updated < 3600.0);
        wchar_t label[32] = {};
        if (has_data)
        {
            std::wstring icon = L"\U0001F321\uFE0F";
            std::wstring desc = state.weather.weather_desc;
            get_weather_icon_and_text(state.weather.weather_code, icon, desc);
            swprintf_s(label, L"%s %.0f\x00B0", icon.c_str(), state.weather.temperature);
        }
        else
        {
            wcscpy_s(label, ARRAYSIZE(label), L"\U0001F321\uFE0F --\x00B0");
        }
        metrics.weather_width = measure_text_width_cached(widest_digit_form(label), fmt, idle_weather_width_cache_);
    }

    metrics.has_privacy = (state.system.mic_active && settings.privacy_dots && settings.privacy_dots_mic) ||
                          (state.system.camera_active && settings.privacy_dots && settings.privacy_dots_cam);

    float total = idle_strip_layout::pad_x * 2.0f + metrics.clock_width;
    if (metrics.has_weather)
    {
        total += idle_strip_layout::slot_gap * 2.0f + idle_strip_layout::divider_width + metrics.weather_width;
    }
    metrics.has_unread = notification_center_unread(settings);
    total += idle_dot_reserve(metrics);

    total = std::ceil(total / idle_strip_layout::width_quantum) * idle_strip_layout::width_quantum;
    metrics.total_width = clamp(total, idle_strip_layout::min_width, idle_strip_layout::max_width);
    return metrics;
}

// Collapsed media pill with the optional Clock (content space, measured in the widest-digit form).
MediaPillMetrics Renderer::measure_media_pill(const SharedState& state, const Settings& settings)
{
    (void)state;
    MediaPillMetrics metrics;
    if (!settings.media_pill_clock)
    {
        return metrics;
    }

    ensure_text_formats(settings.size_scale, settings.font_family, settings.text_scale);
    IDWriteTextFormat* fmt = idle_text_format_ ? idle_text_format_.Get() : small_text_format_.Get();

    SYSTEMTIME local = {};
    GetLocalTime(&local);
    const std::wstring clock =
        format_island_time(local, settings.clock_follow_system, settings.use24_hour_clock, settings.show_seconds);
    metrics.clock_width = measure_text_width_cached(widest_digit_form(clock), fmt, idle_clock_width_cache_);

    float section = media_pill_layout::clock_pad_left + metrics.clock_width + media_pill_layout::slot_gap +
                    media_pill_layout::divider_width;
    section = std::ceil(section / media_pill_layout::width_quantum) * media_pill_layout::width_quantum;
    metrics.section_width = section;
    metrics.total_width = media_pill_layout::base_width + section;
    return metrics;
}

float Renderer::measure_text_width_cached(const std::wstring& text, IDWriteTextFormat* fmt, TextWidthCache& cache)
{
    if (cache.valid && cache.key == text)
    {
        return cache.width;
    }

    float width = 0.0f;
    if (fmt && dwrite_factory_ && !text.empty())
    {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 4096.0f,
                                                        idle_strip_layout::height, &layout)) &&
            layout)
        {
            DWRITE_TEXT_METRICS tm = {};
            if (SUCCEEDED(layout->GetMetrics(&tm)))
            {
                width = std::max(tm.width, tm.widthIncludingTrailingWhitespace);
            }
        }
    }

    cache.key = text;
    cache.width = width;
    cache.valid = true;
    return width;
}

bool Renderer::create_backing_bitmap(int width, int height)
{
    const int logical_w = width;
    const int logical_h = height;
    // Reuse the DIB while it is big enough; UpdateLayeredWindow reads only (0, 0, width, height).
    if (dib_ && width <= bitmap_cap_w_ && height <= bitmap_cap_h_)
    {
        bitmap_width_ = width;
        bitmap_height_ = height;
        return true;
    }

    width = ((width + 63) / 64) * 64 + 64;
    height = ((height + 31) / 32) * 32 + 32;

    if (old_bitmap_)
    {
        SelectObject(mem_dc_, old_bitmap_);
        old_bitmap_ = nullptr;
    }
    if (dib_)
    {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    if (!mem_dc_)
    {
        HDC screen = GetDC(nullptr);
        mem_dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!mem_dc_)
        {
            return false;
        }
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib_)
    {
        return false;
    }

    old_bitmap_ = static_cast<HBITMAP>(SelectObject(mem_dc_, dib_));
    bitmap_cap_w_ = width;
    bitmap_cap_h_ = height;
    bitmap_width_ = logical_w;
    bitmap_height_ = logical_h;
    return true;
}

// text_scale changes only the type; size_scale magnifies the whole island through a transform.
void Renderer::ensure_text_formats(float scale, const std::wstring& font_family, float text_scale)
{
    if (std::abs(scale - last_font_scale_) < 0.001f && font_family == last_font_family_ &&
        std::abs(text_scale - last_text_scale_) < 0.001f)
    {
        return;
    }
    last_text_scale_ = text_scale;
    const float ts = clamp(text_scale, 0.7f, 1.6f);

    idle_clock_width_cache_.valid = false;
    idle_weather_width_cache_.valid = false;

    text_format_ = nullptr;
    small_text_format_ = nullptr;
    clock_format_ = nullptr;
    bold_text_format_ = nullptr;
    huge_text_format_ = nullptr;
    icon_format_ = nullptr;
    media_play_icon_format_ = nullptr;
    media_nav_icon_format_ = nullptr;
    idle_text_format_ = nullptr;
    cal_day_large_format_ = nullptr;
    cal_grid_format_ = nullptr;
    time_dashboard_format_ = nullptr;
    date_dashboard_format_ = nullptr;

    const wchar_t* default_display = L"Segoe UI Variable Display";
    const wchar_t* default_small = L"Segoe UI Variable Small";
    const wchar_t* main_family = font_family.empty() ? default_display : font_family.c_str();
    const wchar_t* small_family = font_family.empty() ? default_small : font_family.c_str();

    auto create_format = [&](const wchar_t* family, const wchar_t* fallback, DWRITE_FONT_WEIGHT weight,
                             DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, float size,
                             ComPtr<IDWriteTextFormat>& out)
    {
        HRESULT hr = dwrite_factory_->CreateTextFormat(family, nullptr, weight, style, stretch, size, L"", &out);
        if (FAILED(hr) || !out)
        {
            if (fallback && std::wcscmp(family, fallback) != 0)
            {
                hr = dwrite_factory_->CreateTextFormat(fallback, nullptr, weight, style, stretch, size, L"", &out);
            }
            if (FAILED(hr) || !out)
            {
                dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, weight, style, stretch, size, L"", &out);
            }
        }
    };

    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 13.5f * ts, text_format_);
    create_format(small_family, default_small, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 11.0f * ts, small_text_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 18.0f * ts, clock_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 12.0f * ts, bold_text_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 42.0f * ts, huge_text_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 13.0f * ts, idle_text_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 63.0f * ts, cal_day_large_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 14.4f * ts, cal_grid_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 56.0f * ts, time_dashboard_format_);
    create_format(main_family, default_display, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                  DWRITE_FONT_STRETCH_NORMAL, 16.0f * ts, date_dashboard_format_);

    using_fluent_icons_ = false;
    ComPtr<IDWriteFontCollection> sys_fonts;
    if (dwrite_factory_ && SUCCEEDED(dwrite_factory_->GetSystemFontCollection(&sys_fonts, FALSE)) && sys_fonts)
    {
        UINT32 font_idx = 0;
        BOOL font_found = FALSE;
        if (SUCCEEDED(sys_fonts->FindFamilyName(L"Segoe Fluent Icons", &font_idx, &font_found)) && font_found)
        {
            using_fluent_icons_ = true;
        }
    }

    const wchar_t* icon_font_family = using_fluent_icons_ ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

    HRESULT hr_icon = dwrite_factory_->CreateTextFormat(icon_font_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f,
                                                        L"", &icon_format_);
    if (FAILED(hr_icon) || !icon_format_)
    {
        dwrite_factory_->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"",
                                          &icon_format_);
    }

    HRESULT hr_play = dwrite_factory_->CreateTextFormat(icon_font_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f,
                                                        L"", &media_play_icon_format_);
    if (FAILED(hr_play) || !media_play_icon_format_)
    {
        dwrite_factory_->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"",
                                          &media_play_icon_format_);
    }

    HRESULT hr_nav = dwrite_factory_->CreateTextFormat(icon_font_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"",
                                                       &media_nav_icon_format_);
    if (FAILED(hr_nav) || !media_nav_icon_format_)
    {
        dwrite_factory_->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"",
                                          &media_nav_icon_format_);
    }

    if (text_format_)
    {
        text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (small_text_format_)
    {
        small_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (bold_text_format_)
    {
        bold_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        bold_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (huge_text_format_)
    {
        huge_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        huge_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        huge_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (clock_format_)
    {
        clock_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        clock_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        clock_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (idle_text_format_)
    {
        idle_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        idle_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        idle_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (cal_day_large_format_)
    {
        cal_day_large_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        cal_day_large_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        cal_day_large_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (cal_grid_format_)
    {
        cal_grid_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        cal_grid_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        cal_grid_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (time_dashboard_format_)
    {
        time_dashboard_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        time_dashboard_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        time_dashboard_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (date_dashboard_format_)
    {
        date_dashboard_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        date_dashboard_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        date_dashboard_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (icon_format_)
    {
        icon_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (media_play_icon_format_)
    {
        media_play_icon_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        media_play_icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        media_play_icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (media_nav_icon_format_)
    {
        media_nav_icon_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        media_nav_icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        media_nav_icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (font_family != last_font_family_)
    {
        marquee_title_cache_.layout.Reset();
        marquee_artist_cache_.layout.Reset();
        marquee_album_cache_.layout.Reset();
        marquee_clipboard_cache_.layout.Reset();
        marquee_notification_cache_.layout.Reset();
        weather_desc_format_.Reset();
        weather_desc_format_size_ = -1.0f;
    }

    karaoke_cache_.layout.Reset();
    karaoke_cache_.valid = false;

    collapsed_lyric_cur_ = CollapsedLyricLine{};
    collapsed_lyric_prev_ = CollapsedLyricLine{};
    collapsed_lyric_idx_ = -2;

    last_font_scale_ = scale;
    last_font_family_ = font_family;
}

void Renderer::ensure_brushes(const Settings& settings, const SharedState& state, double now)
{
    D2D1_COLOR_F target_accent = settings.custom_accent;
    if (settings.accent_mode == AccentMode::System)
    {
        target_accent = get_system_accent_color();
    }
    else if (settings.accent_mode == AccentMode::Auto && state.media.art)
    {
        target_accent = state.media.art->sampled_accent;
    }
    else
    {
        // A custom or system accent must also reach 3:1 against the island background (light themes).
        target_accent = ensure_contrast_against_background(target_accent, settings.pill_bg_color);
    }

    // Accent eases toward the target; the first frame snaps.
    if (last_accent_time_ < 0.0)
    {
        current_accent_ = target_accent;
        last_accent_time_ = now;
    }
    else
    {
        const double dt = std::max(0.0, std::min(now - last_accent_time_, 0.1));
        last_accent_time_ = now;
        const float k = 1.0f - static_cast<float>(std::exp(-dt / 0.18));
        current_accent_.r += (target_accent.r - current_accent_.r) * k;
        current_accent_.g += (target_accent.g - current_accent_.g) * k;
        current_accent_.b += (target_accent.b - current_accent_.b) * k;
        current_accent_.a = 1.0f;
    }

    const D2D1_COLOR_F accent = current_accent_;

    if (!accent_brush_)
    {
        target_->CreateSolidColorBrush(accent, &accent_brush_);
    }
    else
    {
        accent_brush_->SetColor(accent);
    }

    if (!red_brush_)
    {
        target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.27f, 0.27f, 1.0f), &red_brush_);
    }

    D2D1_COLOR_F primary = settings.text_primary_color;
    primary.a = 0.98f;
    if (!text_brush_)
    {
        target_->CreateSolidColorBrush(primary, &text_brush_);
    }
    else
    {
        text_brush_->SetColor(primary);
    }

    D2D1_COLOR_F secondary = settings.text_secondary_color;
    secondary.a = 0.90f;
    if (!muted_brush_)
    {
        target_->CreateSolidColorBrush(secondary, &muted_brush_);
    }
    else
    {
        muted_brush_->SetColor(secondary);
    }

    // Keeps the authored alpha: #RRGGBBAA is how a translucent island is requested.
    pill_bg_color_ = settings.pill_bg_color;

    D2D1_COLOR_F tint_color = D2D1::ColorF(0.010f, 0.010f, 0.012f, settings.tint_opacity);
    if (!tint_brush_)
    {
        target_->CreateSolidColorBrush(tint_color, &tint_brush_);
    }
    else
    {
        tint_brush_->SetColor(tint_color);
    }

    if (!shadow_brush_)
    {
        target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.70f), &shadow_brush_);
    }

    build_material_tokens(settings);
}

D2D1_COLOR_F Renderer::mix_color(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
{
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F Renderer::with_alpha(D2D1_COLOR_F c, float alpha)
{
    c.a = clamp(alpha, 0.0f, 1.0f);
    return c;
}

std::uint32_t Renderer::pack_color8(const D2D1_COLOR_F& c)
{
    auto q = [](float v)
    {
        return static_cast<std::uint32_t>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

std::uint64_t Renderer::hash_stops(const D2D1_GRADIENT_STOP* s, UINT32 n)
{
    std::uint64_t h = 1469598103934665603ull;
    for (UINT32 i = 0; i < n; ++i)
    {
        h = (h ^ pack_color8(s[i].color)) * 1099511628211ull;
        h = (h ^ static_cast<std::uint64_t>(s[i].position * 1000.0f + 0.5f)) * 1099511628211ull;
    }
    return h;
}

// Call once per frame, right after BeginDraw().
void Renderer::begin_frame_resources()
{
    brush_pool_used_ = 0;
    layer_pool_used_ = 0;
}

// A brush unique for the rest of this frame, with color and opacity reset.
ID2D1SolidColorBrush* Renderer::acquire_brush(const D2D1_COLOR_F& color)
{
    if (brush_pool_used_ >= brush_pool_.size())
    {
        ComPtr<ID2D1SolidColorBrush> b;
        if (FAILED(target_->CreateSolidColorBrush(color, &b)) || !b)
        {
            return nullptr;
        }
        brush_pool_.push_back(std::move(b));
    }
    ID2D1SolidColorBrush* brush = brush_pool_[brush_pool_used_++].Get();
    brush->SetColor(color);
    brush->SetOpacity(1.0f);
    return brush;
}

// Layers likewise: each acquire is distinct within a frame, so nested PushLayer calls are safe.
ID2D1Layer* Renderer::acquire_layer()
{
    if (layer_pool_used_ >= layer_pool_.size())
    {
        ComPtr<ID2D1Layer> l;
        if (FAILED(target_->CreateLayer(&l)) || !l)
        {
            return nullptr;
        }
        layer_pool_.push_back(std::move(l));
    }
    return layer_pool_[layer_pool_used_++].Get();
}

void Renderer::set_persistent_brush(ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& c)
{
    if (!brush)
    {
        target_->CreateSolidColorBrush(c, &brush);
    }
    else
    {
        brush->SetColor(c);
    }
}

// The collection + brush are rebuilt only when the stops change; start/end move per call.
ID2D1LinearGradientBrush* Renderer::use_linear_gradient(LinearGradientSlot& slot, const D2D1_GRADIENT_STOP* stops,
                                                        UINT32 count, D2D1_POINT_2F start, D2D1_POINT_2F end)
{
    const std::uint64_t key = hash_stops(stops, count);
    if (!slot.brush || slot.key != key)
    {
        slot.brush.Reset();
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(
                target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll)) ||
            !coll)
        {
            return nullptr;
        }
        if (FAILED(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(start, end), coll.Get(),
                                                      &slot.brush)) ||
            !slot.brush)
        {
            slot.brush.Reset();
            return nullptr;
        }
        slot.key = key;
    }
    slot.brush->SetStartPoint(start);
    slot.brush->SetEndPoint(end);
    return slot.brush.Get();
}

ID2D1RadialGradientBrush* Renderer::use_radial_gradient(RadialGradientSlot& slot, const D2D1_GRADIENT_STOP* stops,
                                                        UINT32 count, D2D1_POINT_2F center, float rx, float ry)
{
    const std::uint64_t key = hash_stops(stops, count);
    if (!slot.brush || slot.key != key)
    {
        slot.brush.Reset();
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(
                target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll)) ||
            !coll)
        {
            return nullptr;
        }
        if (FAILED(target_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), rx, ry), coll.Get(), &slot.brush)) ||
            !slot.brush)
        {
            slot.brush.Reset();
            return nullptr;
        }
        slot.key = key;
    }
    slot.brush->SetCenter(center);
    slot.brush->SetGradientOriginOffset(D2D1::Point2F(0, 0));
    slot.brush->SetRadiusX(rx);
    slot.brush->SetRadiusY(ry);
    return slot.brush.Get();
}

// Rounded-rect or notch geometry, cached by rect and radius (ref-counted, safe across eviction).
ComPtr<ID2D1Geometry> Renderer::get_shape_geometry(D2D1_RECT_F rect, float radius, bool notch)
{
    const ShapeKey key{rect.left, rect.top, rect.right, rect.bottom, radius, notch};
    ShapeEntry* victim = &shape_cache_[0];
    for (ShapeEntry& e : shape_cache_)
    {
        if (e.geom && e.key == key)
        {
            e.last_used = ++shape_clock_;
            return e.geom;
        }
        if (e.last_used < victim->last_used)
        {
            victim = &e; // empty entries have last_used == 0, so they go first
        }
    }

    ComPtr<ID2D1Geometry> geom;
    if (notch)
    {
        ComPtr<ID2D1PathGeometry> path = create_notch_geometry(rect, radius);
        if (path)
        {
            path.As(&geom);
        }
    }
    if (!geom)
    {
        ComPtr<ID2D1RoundedRectangleGeometry> rr;
        if (SUCCEEDED(d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &rr)) && rr)
        {
            rr.As(&geom);
        }
    }
    if (!geom)
    {
        return nullptr;
    }

    victim->key = key;
    victim->geom = geom;
    victim->last_used = ++shape_clock_;
    return geom;
}

void Renderer::release_cached_resources()
{
    brush_pool_.clear();
    layer_pool_.clear();
    brush_pool_used_ = 0;
    layer_pool_used_ = 0;
    raised_brush_.Reset();
    raised_strong_brush_.Reset();
    depth_grad_ = LinearGradientSlot{};
    track_grad_ = LinearGradientSlot{};
    bloom_grad_ = RadialGradientSlot{};
    swap_group_layer_.Reset();
    for (ShapeEntry& e : shape_cache_)
    {
        e = ShapeEntry{};
    }
}

void Renderer::build_material_tokens(const Settings& settings)
{
    MaterialTokens t;
    t.opacity = settings_opacity_;
    t.base = pill_bg_color_;

    // Light surfaces need dark scrims/edges, dark surfaces need light ones.
    const bool on_dark = relative_luminance(t.base) < 0.45;
    t.on_dark = on_dark;

    const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    const D2D1_COLOR_F black = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
    const D2D1_COLOR_F lift = on_dark ? white : black;

    t.raised = with_alpha(lift, on_dark ? 0.085f : 0.062f);
    t.raised_strong = with_alpha(lift, on_dark ? 0.150f : 0.105f);
    t.hairline = with_alpha(lift, on_dark ? 0.100f : 0.085f);
    t.shadow = with_alpha(black, 0.55f);

    t.stroke = settings.contour_border_color;
    t.accent = current_accent_;
    t.accent_soft = with_alpha(current_accent_, 0.18f);

    t.text_primary = with_alpha(settings.text_primary_color, 0.98f);
    t.text_secondary = with_alpha(settings.text_secondary_color, 0.88f);
    t.text_tertiary = with_alpha(mix_color(settings.text_secondary_color, t.base, 0.35f), 0.72f);

    material_ = t;
    set_persistent_brush(raised_brush_, t.raised);
    set_persistent_brush(raised_strong_brush_, t.raised_strong);
}

void Renderer::draw_pill(const SharedState& state, const Settings& settings, const Activity& activity, D2D1_RECT_F rect,
                         float scale, double now)
{
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float w = (rect.right - rect.left) * scale;
    const float h = (rect.bottom - rect.top) * scale;
    rect = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);

    float radius = settings.w11_style ? 8.0f * settings.size_scale : (rect.bottom - rect.top) * 0.5f;
    if (settings.notch_style)
    {
        radius = 16.0f * settings.size_scale;
    }
    else if (!settings.w11_style)
    {
        radius = std::min(radius, 44.0f * settings.size_scale);
    }
    if (pill_count_ < dwm_backdrop::max_pills)
    {
        pill_rects_[pill_count_] = rect;
        pill_radii_[pill_count_] = radius;
        ++pill_count_;
    }
    draw_soft_shadow(rect, radius);

    draw_pill_surface(rect, radius, activity.kind, settings);

    if (activity.kind == IslandKind::Progress)
    {
        draw_progress_ring(rect, state.progress.percent);
    }

    if (activity.kind == IslandKind::BatteryLow)
    {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 2.1));
        red_brush_->SetOpacity(0.45f + 0.45f * pulse);
        draw_island_shape(rect, radius, settings.w11_style, settings.notch_style, red_brush_.Get(), 2.0f);
        red_brush_->SetOpacity(1.0f);
    }

    D2D1_MATRIX_3X2_F old_transform;
    target_->GetTransform(&old_transform);
    D2D1_POINT_2F pill_center = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(settings.size_scale, settings.size_scale, pill_center) *
                          old_transform);

    float inv_scale = 1.0f / settings.size_scale;
    float un_w = (rect.right - rect.left) * inv_scale;
    float un_h = (rect.bottom - rect.top) * inv_scale;
    D2D1_RECT_F unscaled_rect = D2D1::RectF(pill_center.x - un_w * 0.5f, pill_center.y - un_h * 0.5f,
                                            pill_center.x + un_w * 0.5f, pill_center.y + un_h * 0.5f);

    switch (activity.kind)
    {
        case IslandKind::Media:
            draw_media(state, unscaled_rect, settings, now);
            break;
        case IslandKind::Clipboard:
            draw_clipboard(state, unscaled_rect);
            break;
        case IslandKind::Notification:
            draw_notification(state, unscaled_rect);
            break;
        case IslandKind::Volume:
            draw_volume(state, unscaled_rect);
            break;
        case IslandKind::Brightness:
            draw_brightness(state, unscaled_rect);
            break;
        case IslandKind::CapsLock:
            draw_caps_lock(state, unscaled_rect);
            break;
        case IslandKind::Device:
            draw_device(state, unscaled_rect);
            break;
        case IslandKind::Bluetooth:
            draw_bluetooth_device(state, unscaled_rect);
            break;
        case IslandKind::Timer:
            draw_timer(state, unscaled_rect);
            break;
        case IslandKind::BatteryLow:
            draw_battery(state, unscaled_rect);
            break;
        case IslandKind::Progress:
            draw_progress(state, unscaled_rect);
            break;
        case IslandKind::DoNotDisturb:
            draw_do_not_disturb(state, unscaled_rect);
            break;
        case IslandKind::Lookup:
            draw_lookup(state, unscaled_rect, settings, now);
            break;
        case IslandKind::Idle:
        default:
            draw_idle_dashboard(state, unscaled_rect, settings, now);
            break;
    }

    // Privacy dots: green = camera, orange = microphone.
    draw_privacy_dots(state, settings, unscaled_rect, now);

    target_->SetTransform(old_transform);
}

// Soft shadow from stacked rounded shapes with quadratic falloff (DC target: no GPU blur effect).
void Renderer::draw_soft_shadow(D2D1_RECT_F rect, float radius)
{
    // The island sits on top of the backdrop, so a shadow here would darken the blur.
    if (!g_settings.drop_shadow || g_settings.backdrop_material != BackdropMaterial::None)
    {
        return;
    }

    const float spread = clamp(14.0f * g_settings.size_scale, 6.0f, render_pad_y - 4.0f);
    const float y_offset = spread * 0.35f;
    constexpr int steps = 7;

    ID2D1SolidColorBrush* brush = acquire_brush(material_.shadow);
    if (!brush)
    {
        return;
    }

    for (int i = steps; i >= 1; --i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float grow = spread * t;
        const float alpha = material_.shadow.a * (1.0f - t) * (1.0f - t) * 0.55f * settings_opacity_;
        if (alpha <= 0.002f)
        {
            continue;
        }
        brush->SetOpacity(alpha);

        const D2D1_RECT_F shadow_rect = D2D1::RectF(rect.left - grow, rect.top - grow * 0.55f + y_offset,
                                                    rect.right + grow, rect.bottom + grow + y_offset);
        fill_island_shape(shadow_rect, radius + grow, g_settings.w11_style, g_settings.notch_style, brush);
    }
    brush->SetOpacity(1.0f);
}

// Depth shading: one downward shade, no lit edge.
void Renderer::fill_surface_depth(D2D1_RECT_F rect, float radius, bool strong)
{
    const float h = rect.bottom - rect.top;
    if (h <= 2.0f)
    {
        return;
    }

    const float bot_a = (strong ? 0.085f : 0.060f) * (material_.on_dark ? 1.0f : 0.55f) * settings_opacity_;

    static const D2D1_GRADIENT_STOP depth_stops[3] = {
        {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
        {0.45f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f)},
        {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)},
    };
    ID2D1LinearGradientBrush* brush = use_linear_gradient(
        depth_grad_, depth_stops, 3, D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom));
    if (!brush)
    {
        return;
    }
    brush->SetOpacity(clamp(bot_a, 0.0f, 1.0f));
    fill_island_shape(rect, radius, g_settings.w11_style, g_settings.notch_style, brush);
}

// Soft accent wash from the top, driven by the album-art accent.
void Renderer::fill_accent_bloom(D2D1_RECT_F rect, float radius, float strength)
{
    if (strength <= 0.01f)
    {
        return;
    }

    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;
    if (w <= 2.0f || h <= 2.0f)
    {
        return;
    }

    const D2D1_COLOR_F a = material_.accent;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(a.r, a.g, a.b, 1.0f)},
        {1.0f, D2D1::ColorF(a.r, a.g, a.b, 0.0f)},
    };
    const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f, rect.top);
    ID2D1RadialGradientBrush* brush = use_radial_gradient(bloom_grad_, stops, 2, center, w * 0.75f, h * 1.15f);
    if (!brush)
    {
        return;
    }
    brush->SetOpacity(clamp(strength * settings_opacity_, 0.0f, 1.0f));
    fill_island_shape(rect, radius, g_settings.w11_style, g_settings.notch_style, brush);
}

// Shared inner card for every dashboard. Fill only, no outline.
void Renderer::draw_card(D2D1_RECT_F rect, float radius, bool active)
{
    ID2D1SolidColorBrush* fill = active ? raised_strong_brush_.Get() : raised_brush_.Get();
    if (fill)
    {
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), fill);
    }
}

// Shared accent progress track (scrubber, volume, battery, timer).
void Renderer::draw_accent_track(D2D1_RECT_F track, float progress, float radius)
{
    if (raised_strong_brush_)
    {
        target_->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), raised_strong_brush_.Get());
    }

    const float span = (track.right - track.left) * clamp(progress, 0.0f, 1.0f);
    if (span <= 0.5f)
    {
        return;
    }

    const D2D1_RECT_F fill_rect = D2D1::RectF(track.left, track.top, track.left + span, track.bottom);

    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, with_alpha(mix_color(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.30f), 0.95f)},
        {1.0f, with_alpha(material_.accent, 1.0f)},
    };
    if (ID2D1LinearGradientBrush* grad =
            use_linear_gradient(track_grad_, stops, 2, D2D1::Point2F(fill_rect.left, fill_rect.top),
                                D2D1::Point2F(fill_rect.right, fill_rect.top)))
    {
        target_->FillRoundedRectangle(D2D1::RoundedRect(fill_rect, radius, radius), grad);
    }
}

void Renderer::draw_privacy_dots(const SharedState& state, const Settings& settings, D2D1_RECT_F rect, double now)
{
    UNREFERENCED_PARAMETER(now);
    const float height = rect.bottom - rect.top;
    if (height > 55.0f)
    {
        return;
    }

    const bool mic = state.system.mic_active && settings.privacy_dots && settings.privacy_dots_mic;
    const bool cam = state.system.camera_active && settings.privacy_dots && settings.privacy_dots_cam;
    const bool unread = notification_center_unread(settings);
    if (!mic && !cam && !unread)
    {
        return;
    }

    const float dot_r = 4.0f;
    const float margin = 16.0f;
    const float dot_y = rect.top + (rect.bottom - rect.top) * 0.5f;

    const float x = rect.right - margin - dot_r;

    if (unread)
    {
        D2D1_COLOR_F unread_color = material_.accent;
        unread_color.a = settings_opacity_;
        if (!unread_dot_brush_)
        {
            target_->CreateSolidColorBrush(unread_color, &unread_dot_brush_);
        }
        else
        {
            unread_dot_brush_->SetColor(unread_color);
        }

        if (unread_dot_brush_)
        {
            const float unread_x = (mic || cam) ? x - notification_center::unread_dot_gap : x;
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(unread_x, dot_y), notification_center::unread_dot_radius,
                                               notification_center::unread_dot_radius),
                                 unread_dot_brush_.Get());
        }
    }

    if (cam)
    {
        D2D1_COLOR_F cam_color = settings.privacy_dots_cam_hex;
        cam_color.a = settings_opacity_;
        if (!cam_dot_brush_)
        {
            target_->CreateSolidColorBrush(cam_color, &cam_dot_brush_);
        }
        else
        {
            cam_dot_brush_->SetColor(cam_color);
        }

        if (cam_dot_brush_)
        {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, dot_y), dot_r, dot_r), cam_dot_brush_.Get());
        }
    }
    else if (mic)
    {
        D2D1_COLOR_F mic_color = settings.privacy_dots_mic_hex;
        mic_color.a = settings_opacity_;
        if (!mic_dot_brush_)
        {
            target_->CreateSolidColorBrush(mic_color, &mic_dot_brush_);
        }
        else
        {
            mic_dot_brush_->SetColor(mic_color);
        }

        if (mic_dot_brush_)
        {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, dot_y), dot_r, dot_r), mic_dot_brush_.Get());
        }
    }
}

ComPtr<ID2D1PathGeometry> Renderer::create_notch_geometry(D2D1_RECT_F rect, float radius)
{
    ComPtr<ID2D1PathGeometry> geom;
    if (FAILED(d2d_factory_->CreatePathGeometry(&geom)))
    {
        return nullptr;
    }

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geom->Open(&sink)))
    {
        return nullptr;
    }

    float r = std::min({radius, (rect.right - rect.left) * 0.5f, (rect.bottom - rect.top) * 0.5f});
    if (r < 0.0f)
    {
        r = 0.0f;
    }

    sink->BeginFigure(D2D1::Point2F(rect.left, rect.top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(rect.right, rect.top));
    sink->AddLine(D2D1::Point2F(rect.right, rect.bottom - r));
    if (r > 0.0f)
    {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.right - r, rect.bottom), D2D1::SizeF(r, r), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddLine(D2D1::Point2F(rect.left + r, rect.bottom));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rect.left, rect.bottom - r), D2D1::SizeF(r, r), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    }
    else
    {
        sink->AddLine(D2D1::Point2F(rect.left, rect.bottom));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    return geom;
}

ComPtr<ID2D1Geometry> Renderer::create_island_mask_geometry(D2D1_RECT_F rect, float radius, bool notch_style)
{
    return get_shape_geometry(rect, radius, notch_style);
}

void Renderer::fill_island_shape(D2D1_RECT_F rect, float radius, bool w11_style, bool notch_style, ID2D1Brush* brush)
{
    if (!brush)
    {
        return;
    }
    if (notch_style)
    {
        ComPtr<ID2D1Geometry> geom = get_shape_geometry(rect, radius, true);
        if (geom)
        {
            target_->FillGeometry(geom.Get(), brush);
            return;
        }
    }
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
}

void Renderer::draw_island_shape(D2D1_RECT_F rect, float radius, bool w11_style, bool notch_style, ID2D1Brush* brush,
                                 float stroke_width)
{
    if (!brush)
    {
        return;
    }
    if (notch_style)
    {
        auto geom = create_notch_geometry(rect, radius);
        if (geom)
        {
            target_->DrawGeometry(geom.Get(), brush, stroke_width);
            return;
        }
    }
    target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, stroke_width);
}

// Island material, bottom-up: tint scrim -> base fill -> depth shading -> accent bloom -> contour.
void Renderer::draw_pill_surface(D2D1_RECT_F rect, float radius, IslandKind kind, const Settings& settings)
{
    if (tint_brush_ && settings.backdrop_material == BackdropMaterial::None)
    {
        fill_island_shape(rect, radius, settings.w11_style, settings.notch_style, tint_brush_.Get());
    }

    // Blur and tint come from the DWM backdrop window; only the fill is drawn here.

    D2D1_COLOR_F bg = pill_bg_color_;
    bg.a = clamp(bg.a * settings_opacity_, 0.0f, 1.0f);
    if (settings.backdrop_material != BackdropMaterial::None)
    {
        // An opaque fill would hide the backdrop, so cap it.
        bg.a = std::min(bg.a, settings.backdrop_fill_alpha);
    }
    if (ID2D1SolidColorBrush* fill_brush = acquire_brush(bg))
    {
        fill_island_shape(rect, radius, settings.w11_style, settings.notch_style, fill_brush);
    }

    if (settings.material_depth)
    {
        fill_surface_depth(rect, radius, kind == IslandKind::Media || kind == IslandKind::Idle);

        const float bloom = (kind == IslandKind::Media) ? 0.115f : (kind == IslandKind::Idle) ? 0.055f : 0.075f;
        fill_accent_bloom(rect, radius, bloom * settings.accent_bloom);
    }

    if (settings.contour_border_mode != ContourBorderMode::Borderless && settings.contour_border_enabled)
    {
        D2D1_COLOR_F border_color = settings.contour_border_color;
        float stroke_width = settings.w11_style ? 1.0f : 0.8f;

        if (settings.contour_border_mode == ContourBorderMode::Auto)
        {
            if (current_accent_.a > 0.0f)
            {
                border_color = current_accent_;
                border_color.a = std::min(1.0f, (kind == IslandKind::Idle ? 0.35f : 0.60f) * settings_opacity_);
                stroke_width = 1.0f;
            }
            else
            {
                border_color.a = std::min(1.0f, border_color.a * settings_opacity_);
            }
        }
        else
        {
            border_color.a = std::min(1.0f, border_color.a * settings_opacity_);
        }

        if (ID2D1SolidColorBrush* border = acquire_brush(border_color))
        {
            D2D1_RECT_F border_rect =
                D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
            draw_island_shape(border_rect, radius, settings.w11_style, settings.notch_style, border, stroke_width);
        }
    }
}

// Tab transition: tab_pos_ is a continuous position a damped spring chases; each visible tab is drawn
// from its distance to it (slide, shrink, fade), so motion is interruptible and never restarts.
void Renderer::reset_tab_transition()
{
    tab_init_ = false;
    tab_animating_ = false;
    g_tab_animating.store(false, std::memory_order_relaxed);
    g_tab_edge_bump.store(0, std::memory_order_relaxed);
}

// Vertical pagination dots; the active dot stretches into a pill and slides with the transition.
void Renderer::draw_tab_pagination(D2D1_RECT_F rect, int tab_count)
{
    if (tab_count <= 1)
    {
        return;
    }
    ID2D1SolidColorBrush* dot_brush = acquire_brush(with_alpha(material_.text_primary, 1.0f));
    if (!dot_brush)
    {
        return;
    }

    const float dot_x = rect.right - 10.0f;
    const float spacing = 9.0f;
    const float r = 2.5f;
    const float start_y = (rect.top + rect.bottom) * 0.5f - spacing * (tab_count - 1) * 0.5f;

    for (int i = 0; i < tab_count; ++i)
    {
        const float under = smooth_step01(clamp(1.0f - std::fabs(tab_pos_ - static_cast<float>(i)), 0.0f, 1.0f));
        dot_brush->SetOpacity(0.22f * (1.0f - under) * settings_opacity_);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_x, start_y + spacing * static_cast<float>(i)), r, r),
                             dot_brush);
    }

    const float pos = clamp(tab_pos_, -0.3f, static_cast<float>(tab_count) - 0.7f);
    const float stretch = clamp(std::fabs(tab_vel_) * 0.55f, 0.0f, 3.5f);
    const float half_h = r + 2.2f + stretch;
    const float cy = start_y + spacing * pos;
    dot_brush->SetOpacity(0.90f * settings_opacity_);
    target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(dot_x - r, cy - half_h, dot_x + r, cy + half_h), r, r),
                                  dot_brush);
}

// Seqlock write (odd, store, even) of the painted content rect for the window's hit tests.
void Renderer::publish_content_geometry(D2D1_RECT_F rect, float right_trim)
{
    const unsigned seq = g_media_hit_seq.load(std::memory_order_relaxed);
    g_media_hit_seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    g_media_hit_left.store(rect.left, std::memory_order_relaxed);
    g_media_hit_top.store(rect.top, std::memory_order_relaxed);
    g_media_hit_right.store(rect.right, std::memory_order_relaxed);
    g_media_hit_bottom.store(rect.bottom, std::memory_order_relaxed);
    g_media_hit_scale.store(g_settings.size_scale, std::memory_order_relaxed);
    g_media_hit_trim.store(right_trim, std::memory_order_relaxed);
    g_media_hit_stamp.store(GetTickCount64(), std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_release);
    g_media_hit_seq.store(seq + 2, std::memory_order_relaxed);
}

// Mask radius in content space: draw_pill applies size_scale later, so it must not be pre-multiplied.
float Renderer::content_island_radius(float content_height) const
{
    if (g_settings.notch_style)
    {
        return 16.0f;
    }
    if (g_settings.w11_style)
    {
        return 8.0f;
    }
    return std::min(content_height * 0.5f, 44.0f);
}

WeatherVisual weather_visual_from_code(int code)
{
    switch (code)
    {
        case 113:
            return WeatherVisual::Clear;
        case 116:
            return WeatherVisual::PartlyCloudy;
        case 119:
        case 122:
            return WeatherVisual::Cloudy;
        case 143:
        case 248:
        case 260:
            return WeatherVisual::Fog;
        case 200:
        case 386:
        case 389:
        case 392:
        case 395:
            return WeatherVisual::Storm;
        case 176:
        case 263:
        case 266:
        case 281:
        case 284:
        case 293:
        case 296:
        case 299:
        case 302:
        case 305:
        case 308:
        case 311:
        case 314:
        case 353:
        case 356:
        case 359:
            return WeatherVisual::Rain;
        case 179:
        case 182:
        case 185:
        case 227:
        case 230:
        case 317:
        case 320:
        case 323:
        case 326:
        case 329:
        case 332:
        case 335:
        case 338:
        case 350:
        case 362:
        case 365:
        case 368:
        case 371:
            return WeatherVisual::Snow;
        default:
            return WeatherVisual::Unknown;
    }
}

bool notification_center_unread(const Settings& settings)
{
    return settings.notification_center_module && g_notification_center_unread.load(std::memory_order_relaxed);
}

float idle_dot_reserve(const IdleStripMetrics& metrics)
{
    if (!metrics.has_privacy && !metrics.has_unread)
    {
        return 0.0f;
    }
    return idle_strip_layout::privacy_reserve +
           (metrics.has_privacy && metrics.has_unread ? notification_center::unread_dot_gap : 0.0f);
}

// Every digit becomes '0' before measuring, so a ticking Clock never changes the pill width.
std::wstring widest_digit_form(std::wstring text)
{
    for (wchar_t& c : text)
    {
        if (c >= L'0' && c <= L'9')
        {
            c = L'0';
        }
    }
    return text;
}

// Wide while lyrics are on and media is available, so the pill does not shrink on every track change.
bool collapsed_lyrics_active(const SharedState& state, const Settings& settings)
{
    return settings.collapsed_lyrics && settings.media && state.media.available;
}

}
