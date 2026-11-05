#include "ui/render/renderer_impl.h"

namespace island
{

// Takes the frame's private settings copy: g_settings can be replaced mid-frame.
void Renderer::draw_media(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now)
{
    const float height = rect.bottom - rect.top;

    {
        const float cur_w = rect.right - rect.left;
        if (std::fabs(cur_w - island_size_last_w_) > 0.25f || std::fabs(height - island_size_last_h_) > 0.25f)
        {
            island_size_last_w_ = cur_w;
            island_size_last_h_ = height;
            island_size_moved_at_ = now;
        }
        island_settled_ = (now - island_size_moved_at_) > 0.15;
    }

    expanded_anim_ = settings.expanded_media_anim;
    pill_cover_anim_ = settings.pill_cover_anim;

    const bool side_lyrics_on = false;
    const float immersive_mix = update_immersive_mix(settings, height, now);
    g_immersive_mix_pub.store(immersive_mix, std::memory_order_relaxed);
    const float side_lyrics_extra =
        side_lyrics_on ? std::max(0.0f, (rect.right - rect.left) - media_layout::expanded_width) : 0.0f;

    publish_content_geometry(rect, side_lyrics_extra);

    const float radius = content_island_radius(height);
    ComPtr<ID2D1Geometry> mask = create_island_mask_geometry(rect, radius, settings.notch_style);
    ID2D1Layer* layer = acquire_layer();

    float expanded_alpha = std::clamp((height - media_layout::expanded_min_height) / 60.0f, 0.0f, 1.0f);
    float collapsed_alpha = std::clamp((80.0f - height) / 30.0f, 0.0f, 1.0f);

    // Shared album art: one element morphs from the collapsed circle to the expanded square as the island
    // grows (art_morph: 0 = collapsed, 1 = expanded at 120 px).
    constexpr float collapsed_pill_height = 44.0f;
    constexpr float art_morph_end_height = 120.0f;
    constexpr float collapsed_art_pad = 6.0f;
    constexpr float collapsed_art_size = collapsed_pill_height - collapsed_art_pad * 2.0f;
    const float art_morph_raw =
        clamp((height - collapsed_pill_height) / (art_morph_end_height - collapsed_pill_height), 0.0f, 1.0f);

    // Expanding may not finish the morph faster than art_expand_sec (the spring alone would take a few frames).
    constexpr float art_expand_sec = 0.26f;
    const bool art_on_media_tab = normalized_tab_index(settings) == 0;
    const bool fresh_morph = art_morph_last_ >= 0.0 && (now - art_morph_last_) < 0.25;
    const float dt_morph = fresh_morph ? clamp(static_cast<float>(now - art_morph_last_), 0.001f, 0.05f) : 0.0f;
    art_morph_last_ = now;

    if (!fresh_morph || art_morph_raw <= art_morph_lin_ || !art_on_media_tab)
    {
        art_morph_lin_ = art_morph_raw;
    }
    else
    {
        art_morph_lin_ = std::min(art_morph_raw, art_morph_lin_ + dt_morph / art_expand_sec);
        g_layout_dirty = true;
    }
    const float art_morph = smooth_step01(art_morph_lin_);

    if (art_morph >= 1.0f)
    {
        if (!fresh_morph)
        {
            art_badge_alpha_ = 1.0f;
        }
        else if (art_badge_alpha_ < 1.0f)
        {
            art_badge_alpha_ = std::min(1.0f, art_badge_alpha_ + dt_morph / 0.20f);
            g_layout_dirty = true;
        }
    }
    else
    {
        art_badge_alpha_ = 0.0f;
    }
    float pill_clock_section = 0.0f;
    if (settings.media_pill_clock && idle_text_format_)
    {
        pill_clock_section = measure_media_pill(state, settings).section_width;
    }
    const D2D1_RECT_F collapsed_art_rect =
        D2D1::RectF(rect.left + pill_clock_section + collapsed_art_pad, rect.top + collapsed_art_pad,
                    rect.left + pill_clock_section + collapsed_art_pad + collapsed_art_size,
                    rect.top + collapsed_art_pad + collapsed_art_size);
    auto lerp_f = [](float a, float b, float t)
    {
        return a + (b - a) * t;
    };
    const D2D1_RECT_F normal_art_rect =
        D2D1::RectF(rect.left + media_layout::art_inset_x, rect.top + media_layout::art_inset_y,
                    rect.left + media_layout::art_inset_x + media_layout::art_size,
                    rect.top + media_layout::art_inset_y + media_layout::art_size);
    const D2D1_RECT_F immersive_art_rect =
        D2D1::RectF(rect.left + immersive_layout::art_left, rect.top + immersive_layout::art_top,
                    rect.left + immersive_layout::art_left + immersive_layout::art_size,
                    rect.top + immersive_layout::art_top + immersive_layout::art_size);
    const D2D1_RECT_F expanded_art_rect =
        D2D1::RectF(lerp_f(normal_art_rect.left, immersive_art_rect.left, immersive_mix),
                    lerp_f(normal_art_rect.top, immersive_art_rect.top, immersive_mix),
                    lerp_f(normal_art_rect.right, immersive_art_rect.right, immersive_mix),
                    lerp_f(normal_art_rect.bottom, immersive_art_rect.bottom, immersive_mix));
    const float expanded_art_radius = lerp_f(16.0f, immersive_layout::art_size * 0.5f, immersive_mix);
    const D2D1_RECT_F morph_art_rect =
        D2D1::RectF(lerp_f(collapsed_art_rect.left, expanded_art_rect.left, art_morph),
                    lerp_f(collapsed_art_rect.top, expanded_art_rect.top, art_morph),
                    lerp_f(collapsed_art_rect.right, expanded_art_rect.right, art_morph),
                    lerp_f(collapsed_art_rect.bottom, expanded_art_rect.bottom, art_morph));
    const float morph_art_radius = lerp_f(collapsed_art_size * 0.5f, expanded_art_radius, art_morph);

    // Expanded UI
    if (expanded_alpha > 0.01f && mask && layer)
    {
        target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), expanded_alpha, nullptr,
                                                 D2D1_LAYER_OPTIONS_NONE),
                           layer);

        // Order must match active_tab_count / file_tray_tab_index.
        std::vector<int> active_tabs;
        active_tabs.push_back(0); // Media
        if (settings.notification_center_module)
        {
            active_tabs.push_back(7);
        }
        if (settings.weather)
        {
            active_tabs.push_back(2); // Weather
        }
        active_tabs.push_back(1); // Calendar
        if (settings.github_module)
        {
            active_tabs.push_back(6);
        }
        if (settings.hardware_monitor_module)
        {
            active_tabs.push_back(3);
        }
        if (settings.file_tray_module)
        {
            active_tabs.push_back(4);
        }

        int max_tabs = static_cast<int>(active_tabs.size());
        int tab_idx = normalized_tab_index(settings);
        if (tab_idx >= max_tabs)
        {
            tab_idx = max_tabs - 1;
        }

        auto draw_tab_content = [&](int active_tab_id)
        {
            if (active_tab_id == 0)
            {
                const D2D1_RECT_F full_island_rect = rect;
                const D2D1_RECT_F rect =
                    D2D1::RectF(full_island_rect.left, full_island_rect.top, full_island_rect.right - side_lyrics_extra,
                                full_island_rect.bottom);
                float swap_alpha = 1.0f, swap_dx = 0.0f;
                update_source_swap(state, now, &swap_alpha, &swap_dx);

                const bool content_grouped = begin_swap_group(swap_alpha, swap_dx);
                if (swap_alpha > 0.003f)
                {
                    const float art_size = media_layout::art_size;
                    D2D1_RECT_F art_rect =
                        D2D1::RectF(rect.left + media_layout::art_inset_x, rect.top + media_layout::art_inset_y,
                                    rect.left + media_layout::art_inset_x + art_size,
                                    rect.top + media_layout::art_inset_y + art_size);
                    // Until the morph finishes, the shared art pass owns the cover.
                    if (art_morph >= 1.0f)
                    {
                        draw_album_art_flip(state.media, expanded_art_rect, now, expanded_art_radius,
                                            immersive_mix < 0.01f);
                    }

                    const bool normal_faded = begin_immersive_fade(1.0f - immersive_mix);
                    if (immersive_mix < 0.999f)
                    {
                        const float wave_w = 32.0f;
                        const float wave_h = 20.0f;
                        D2D1_RECT_F wave_rect =
                            D2D1::RectF(rect.right - 24.0f - wave_w, rect.top + 20.0f + (art_size - wave_h) * 0.5f,
                                        rect.right - 24.0f, rect.top + 20.0f + (art_size + wave_h) * 0.5f);

                        const float text_left = art_rect.right + 18.0f;
                        const float text_right = wave_rect.left - 16.0f;

                        D2D1_RECT_F title_rect = D2D1::RectF(text_left, rect.top + 34.0f, text_right, rect.top + 54.0f);
                        const std::wstring title_text =
                            state.media.title.empty() ? std::wstring(loc(L"Unknown")) : state.media.title;
                        if (!draw_title_transition(title_text, title_rect, text_brush_.Get(), now,
                                                   !state.media.title.empty()))
                        {
                            draw_marquee_text(title_text, title_rect, text_format_.Get(), text_brush_.Get(), now, 42.0f,
                                              marquee_title_cache_);
                        }

                        D2D1_RECT_F artist_rect =
                            D2D1::RectF(text_left, rect.top + 54.0f, text_right, rect.top + 74.0f);
                        muted_brush_->SetOpacity(0.80f);
                        draw_swap_marquee(swap_artist_, state.media.artist, artist_rect, small_text_format_.Get(),
                                          muted_brush_.Get(), now, 30.0f, marquee_artist_cache_, 0.06f);
                        muted_brush_->SetOpacity(0.75f);

                        if (!state.media.album_title.empty())
                        {
                            D2D1_RECT_F album_rect =
                                D2D1::RectF(text_left, rect.top + 68.0f, text_right, rect.top + 84.0f);
                            muted_brush_->SetOpacity(0.70f);
                            draw_swap_marquee(swap_album_, state.media.album_title, album_rect,
                                              small_text_format_.Get(), muted_brush_.Get(), now, 28.0f,
                                              marquee_album_cache_, 0.12f);
                            muted_brush_->SetOpacity(0.75f);
                        }

                        if (settings.spectrum_style == SpectrumStyle::Bars)
                        {
                            draw_spectrum(state, wave_rect, settings, now, state.media.playing);
                        }
                        else if (state.media.playing)
                        {
                            draw_spectrum(state, wave_rect, settings, now);
                        }
                        else
                        {
                            const float gap = 2.5f;
                            const float available_w = wave_rect.right - wave_rect.left;
                            const int count = 7;
                            const float bar_width = (available_w - gap * (count - 1)) / count;
                            const float center_y = (wave_rect.top + wave_rect.bottom) * 0.5f;
                            muted_brush_->SetOpacity(0.5f);
                            for (int i = 0; i < count; ++i)
                            {
                                const float dot_x = wave_rect.left + i * (bar_width + gap) + bar_width * 0.5f;
                                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_x, center_y), 1.2f, 1.2f),
                                                     muted_brush_.Get());
                            }
                        }

                        const float src_line_top = rect.top + (state.media.album_title.empty() ? 70.0f : 85.0f);
                        draw_source_line(state.media,
                                         D2D1::RectF(text_left, src_line_top, text_right, src_line_top + 14.0f), now);
                    }
                    end_immersive_fade(normal_faded);
                    draw_immersive_lyrics_view(state, rect, settings, now, immersive_mix);
                }
                end_swap_group(content_grouped);

                D2D1_MATRIX_3X2_F player_zone_old_transform;
                target_->GetTransform(&player_zone_old_transform);
                if (immersive_mix > 0.001f)
                {
                    const float ps = 1.0f + (immersive_layout::player_scale - 1.0f) * immersive_mix;
                    target_->SetTransform(
                        D2D1::Matrix3x2F::Scale(ps, ps,
                                                D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                                              rect.top + immersive_layout::player_pivot_y)) *
                        player_zone_old_transform);
                }

                const bool timeline_grouped = begin_swap_group(swap_alpha, 0.0f);

                const float scrubber_y = rect.top + media_layout::scrubber_y;
                double current_position = state.media.position_ticks / 10000000.0;
                double duration = state.media.end_ticks / 10000000.0;
                if (state.media.playing && state.media.last_updated_ticks > 0)
                {
                    current_position += (GetTickCount64() - state.media.last_updated_ticks) / 1000.0;
                }
                current_position = std::max(0.0, std::min(current_position, duration));

                const bool is_dragging_this_bar = g_scrubbing.load(std::memory_order_relaxed);
                float progress;
                if (is_dragging_this_bar)
                {
                    progress = clamp(g_scrub_drag_fraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    current_position = duration * progress;
                }
                else
                {
                    progress = duration > 0.0 ? static_cast<float>(current_position / duration) : 0.0f;
                }

                auto format_time = [](double seconds) -> std::wstring
                {
                    if (seconds <= 0.0 || _isnan(seconds))
                    {
                        return L"0:00";
                    }
                    int m = static_cast<int>(seconds) / 60;
                    int s = static_cast<int>(seconds) % 60;
                    wchar_t buf[16];
                    swprintf_s(buf, L"%d:%02d", m, s);
                    return buf;
                };

                std::wstring elapsed_str = format_time(current_position);
                std::wstring remain_str = L"-" + format_time(duration - current_position);

                const float scrub_left = rect.left + media_layout::scrub_margin;
                const float scrub_right = rect.right - media_layout::scrub_margin;

                const float bar_left = scrub_left + media_layout::scrub_bar_left_inset;
                const float bar_right = scrub_right - media_layout::scrub_bar_right_inset;
                const float time_gap = 8.0f;

                D2D1_RECT_F el_rect =
                    D2D1::RectF(scrub_left, scrubber_y - 10.0f, bar_left - time_gap, scrubber_y + 10.0f);
                D2D1_RECT_F rem_rect =
                    D2D1::RectF(bar_right + time_gap, scrubber_y - 10.0f, scrub_right, scrubber_y + 10.0f);

                muted_brush_->SetOpacity(0.8f);
                if (small_text_format_)
                {
                    small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    target_->DrawTextW(elapsed_str.c_str(), static_cast<UINT32>(elapsed_str.size()),
                                       small_text_format_.Get(), el_rect, muted_brush_.Get());

                    small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    target_->DrawTextW(remain_str.c_str(), static_cast<UINT32>(remain_str.size()),
                                       small_text_format_.Get(), rem_rect, muted_brush_.Get());

                    small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }

                if (settings.progress_style != ProgressStyle::Slim)
                {
                    draw_styled_progress(settings.progress_style, bar_left, bar_right, scrubber_y, progress,
                                         is_dragging_this_bar, state.media.playing, now);
                }
                else
                {
                    const float bar_half = is_dragging_this_bar ? 3.5f : 2.5f;
                    draw_accent_track(D2D1::RectF(bar_left, scrubber_y - bar_half, bar_right, scrubber_y + bar_half),
                                      progress, bar_half);

                    const D2D1_COLOR_F scrub_color =
                        (current_accent_.a > 0.0f) ? current_accent_ : D2D1::ColorF(0x4cc9f0);
                    const float scrub_w = (bar_right - bar_left) * progress;
                    const float thumb_x = bar_left + scrub_w;
                    const float thumb_r = is_dragging_this_bar ? 6.5f : 4.5f;

                    if (is_dragging_this_bar)
                    {
                        ComPtr<ID2D1SolidColorBrush> thumb_halo;
                        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(scrub_color, 0.22f), &thumb_halo)) &&
                            thumb_halo)
                        {
                            target_->FillEllipse(
                                D2D1::Ellipse(D2D1::Point2F(thumb_x, scrubber_y), thumb_r * 2.2f, thumb_r * 2.2f),
                                thumb_halo.Get());
                        }
                    }

                    ComPtr<ID2D1SolidColorBrush> thumb_fill;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), &thumb_fill)) &&
                        thumb_fill)
                    {
                        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, scrubber_y), thumb_r, thumb_r),
                                             thumb_fill.Get());
                    }
                    ComPtr<ID2D1SolidColorBrush> thumb_ring;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(scrub_color, 0.85f), &thumb_ring)) &&
                        thumb_ring)
                    {
                        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, scrubber_y), thumb_r, thumb_r),
                                             thumb_ring.Get(), 1.6f);
                    }
                }

                end_swap_group(timeline_grouped);

                // Controls: positions come from media_layout, the same values the hit test uses.
                const float cy = rect.top + media_layout::controls_y;
                const float cx = (rect.left + rect.right) * 0.5f;
                draw_media_controls(state.media.playing, D2D1::Point2F(cx - media_layout::control_spacing, cy),
                                    D2D1::Point2F(cx, cy), D2D1::Point2F(cx + media_layout::control_spacing, cy), now);
                draw_source_dock(state, rect, now); // stays still during a switch
                if (settings.media_tab_lyrics)
                {
                    draw_immersive_lyrics_button(rect, immersive_mix);
                }
                target_->SetTransform(player_zone_old_transform);
                if (side_lyrics_extra > 1.0f)
                {
                    draw_media_side_lyrics(
                        state,
                        D2D1::RectF(rect.right, full_island_rect.top, full_island_rect.right, full_island_rect.bottom),
                        settings, now);
                }
            }
            else if (active_tab_id == 1)
            {
                SYSTEMTIME local = {};
                GetLocalTime(&local);
                draw_calendar_dashboard(state, rect, settings, now, 1.0f, local);
            }
            else if (active_tab_id == 2)
            {
                bool has_weather = state.weather.has_data && (now - state.weather.last_updated < 3600.0);
                std::wstring w_icon = L"🌡️";
                std::wstring w_text = loc(L"Loading...");
                if (has_weather)
                {
                    w_text = state.weather.weather_desc;
                    get_weather_icon_and_text(state.weather.weather_code, w_icon, w_text);
                }
                draw_weather_dashboard(state, rect, settings, now, 1.0f, has_weather, w_icon, w_text);
            }
            else if (active_tab_id == 6)
            {
                draw_git_hub_dashboard(state, rect, settings, 1.0f);
            }
            else if (active_tab_id == 3)
            {
                draw_hardware_monitor_dashboard(state, rect, settings, 1.0f);
            }
            else if (active_tab_id == 4)
            {
                draw_file_tray_dashboard(state, rect, settings, 1.0f);
            }
            else if (active_tab_id == 7)
            {
                draw_notification_center_dashboard(state, rect, 1.0f);
            }
        };

        draw_tab_transition(0, tab_idx, active_tabs, rect, now, draw_tab_content);
        draw_tab_pagination(rect, max_tabs);

        target_->PopLayer();
    }

    // Collapsed UI
    if (collapsed_alpha > 0.01f && mask && layer && !g_tray_drag_over.load(std::memory_order_relaxed))
    {
        g_idle_tab = 0;
        reset_tab_transition();
        target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), collapsed_alpha, nullptr,
                                                 D2D1_LAYER_OPTIONS_NONE),
                           layer);
        ;

        const float cy = (rect.top + rect.bottom) * 0.5f;

        MediaPillMetrics pill_metrics;
        if (settings.media_pill_clock && idle_text_format_)
        {
            pill_metrics = measure_media_pill(state, settings);
        }
        const float clock_section = pill_metrics.section_width;

        const D2D1_RECT_F art_rect = collapsed_art_rect;

        float shift_x = 0.0f;
        if (state.system.mic_active || state.system.camera_active)
        {
            shift_x = 22.0f;
        }

        D2D1_RECT_F wave_rect =
            D2D1::RectF(rect.right - 42.0f - shift_x, cy - 10.0f, rect.right - 14.0f - shift_x, cy + 10.0f);
        if (settings.spectrum_style == SpectrumStyle::Bars)
        {
            draw_spectrum(state, wave_rect, settings, now, state.media.playing);
        }
        else if (state.media.playing)
        {
            draw_spectrum(state, wave_rect, settings, now);
        }
        else
        {
            const float gap = 2.5f;
            const float available_w = wave_rect.right - wave_rect.left;
            const int count = std::max(1, static_cast<int>((available_w + gap) / (2.0f + gap)));
            const float bar_width = (available_w - gap * (count - 1)) / count;
            muted_brush_->SetOpacity(0.5f);
            for (int i = 0; i < count; ++i)
            {
                const float dot_x = wave_rect.left + i * (bar_width + gap) + bar_width * 0.5f;
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dot_x, cy), 1.2f, 1.2f), muted_brush_.Get());
            }
        }

        if (collapsed_lyrics_active(state, settings))
        {
            const float lyrics_alpha = smooth_step01(((rect.right - rect.left) - 175.0f) / 60.0f);
            if (lyrics_alpha > 0.01f)
            {
                const float text_left = art_rect.right + 10.0f;
                const float text_right = wave_rect.left - 10.0f;
                draw_collapsed_lyrics(state, D2D1::RectF(text_left, rect.top + 4.0f, text_right, rect.bottom - 4.0f),
                                      now, lyrics_alpha);
            }
        }

        if (clock_section > 0.0f)
        {
            SYSTEMTIME local = {};
            GetLocalTime(&local);
            const std::wstring clock_text = format_island_time(local, settings.clock_follow_system,
                                                               settings.use24_hour_clock, settings.show_seconds);
            const float clock_left = rect.left + media_pill_layout::clock_pad_left;
            text_brush_->SetOpacity(0.96f);
            target_->DrawTextW(clock_text.c_str(), static_cast<UINT32>(clock_text.size()), idle_text_format_.Get(),
                               D2D1::RectF(clock_left, rect.top, clock_left + pill_metrics.clock_width, rect.bottom),
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            text_brush_->SetOpacity(1.0f);

            ComPtr<ID2D1SolidColorBrush> divider;
            target_->CreateSolidColorBrush(with_alpha(material_.hairline, material_.hairline.a * settings_opacity_),
                                           &divider);
            if (divider)
            {
                const float div_x = clock_left + pill_metrics.clock_width + media_pill_layout::slot_gap;
                const float div_w = media_pill_layout::divider_width;
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(div_x, rect.top + media_pill_layout::divider_inset_y, div_x + div_w,
                                                  rect.bottom - media_pill_layout::divider_inset_y),
                                      div_w * 0.5f, div_w * 0.5f),
                    divider.Get());
            }
        }

        target_->PopLayer();
    }

    // Shared album art pass: drawn once outside both cross-fading layers, so the cover morphs smoothly
    // while the island resizes.
    {
        const bool tray_drag = g_tray_drag_over.load(std::memory_order_relaxed);
        const bool on_media_tab = normalized_tab_index(settings) == 0;
        const float target_alpha = tray_drag ? 0.0f : (on_media_tab ? 1.0f : collapsed_alpha);

        const bool recent = art_outer_last_ >= 0.0 && (now - art_outer_last_) < 0.25;
        const float dt_art = recent ? clamp(static_cast<float>(now - art_outer_last_), 0.001f, 0.05f) : 0.016f;
        art_outer_last_ = now;
        if (!recent || art_morph >= 1.0f || !on_media_tab)
        {
            art_outer_alpha_ = target_alpha;
        }
        else
        {
            art_outer_alpha_ += (target_alpha - art_outer_alpha_) * (1.0f - std::exp(-dt_art / 0.06f));
        }

        float hide_s = 1.0f;
        if (island_hiding_)
        {
            const float need_w = pill_clock_section + collapsed_art_pad * 2.0f + collapsed_art_size;
            const float cur_w = rect.right - rect.left;
            hide_s = smooth_step01(std::min(height / collapsed_pill_height, cur_w / need_w));
        }
        // On the media tab the cover morphs circle -> square; on other tabs it stays a small circle.
        const D2D1_RECT_F art_base_rect = on_media_tab ? morph_art_rect : collapsed_art_rect;
        const float art_base_radius = on_media_tab ? morph_art_radius : collapsed_art_size * 0.5f;

        D2D1_RECT_F art_draw_rect = art_base_rect;
        float art_draw_radius = art_base_radius;
        if (hide_s < 1.0f)
        {
            const float pcx = (rect.left + rect.right) * 0.5f;
            const float pcy = (rect.top + rect.bottom) * 0.5f;
            const float acx = (art_base_rect.left + art_base_rect.right) * 0.5f;
            const float acy = (art_base_rect.top + art_base_rect.bottom) * 0.5f;
            const float ncx = pcx + (acx - pcx) * hide_s;
            const float ncy = pcy + (acy - pcy) * hide_s;
            const float hw = (art_base_rect.right - art_base_rect.left) * 0.5f * hide_s;
            const float hh = (art_base_rect.bottom - art_base_rect.top) * 0.5f * hide_s;
            art_draw_rect = D2D1::RectF(ncx - hw, ncy - hh, ncx + hw, ncy + hh);
            art_draw_radius = art_base_radius * hide_s;
        }
        const float art_alpha_eff = art_outer_alpha_ * hide_s;

        if (art_morph < 1.0f && art_alpha_eff > 0.01f)
        {
            ID2D1Layer* art_layer = (art_alpha_eff < 0.99f) ? acquire_layer() : nullptr;
            if (art_layer)
            {
                target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                                         clamp(art_alpha_eff, 0.0f, 1.0f), nullptr,
                                                         D2D1_LAYER_OPTIONS_NONE),
                                   art_layer);
            }
            draw_album_art_drop(state.media, art_draw_rect, now, art_draw_radius);
            if (art_layer)
            {
                target_->PopLayer();
            }
        }
    }
}

// Out: old content slides away. Hold: nothing drawn while the media thread switches. In: new content
// slides in. The request is sent after Out, so old and new are never visible together.
void Renderer::update_source_swap(const SharedState& state, double now, float* alpha, float* dx)
{
    *alpha = 1.0f;
    *dx = 0.0f;

    if (swap_phase_ != KSwapNone && swap_last_update_ >= 0.0 && now - swap_last_update_ > 0.5)
    {
        if (swap_phase_ == KSwapOut)
        {
            request_media_source(swap_target_);
        }
        swap_phase_ = KSwapNone;
    }
    swap_last_update_ = now;

    if (!expanded_anim_ && swap_phase_ != KSwapNone)
    {
        if (swap_phase_ == KSwapOut)
        {
            request_media_source(swap_target_);
        }
        swap_phase_ = KSwapNone;
    }

    const unsigned seq = g_switch_seq.load();
    if (seq != swap_seen_seq_ && swap_phase_ != KSwapIn)
    {
        swap_seen_seq_ = seq;
        {
            std::lock_guard lock(g_media_source_mutex);
            swap_target_ = g_switch_target;
        }
        swap_dir_ = g_switch_dir.load() >= 0 ? 1.0f : -1.0f;
        if (!expanded_anim_)
        {
            request_media_source(swap_target_);
        }
        else if (swap_phase_ == KSwapNone)
        {
            swap_phase_ = KSwapOut;
            swap_start_ = now;
        }
        else if (swap_phase_ == KSwapHold)
        {
            request_media_source(swap_target_); // retarget while nothing is visible
            swap_start_ = now;
        }
    }

    switch (swap_phase_)
    {
        case KSwapOut:
        {
            const float t = clamp(static_cast<float>((now - swap_start_) / media_layout::switch_out_sec), 0.0f, 1.0f);
            const float e = t * t; // ease-in
            *alpha = 1.0f - e;
            *dx = -swap_dir_ * media_layout::switch_shift * e; // opposite to the switch direction
            if (t >= 1.0f)
            {
                request_media_source(swap_target_);
                swap_phase_ = KSwapHold;
                swap_start_ = now;
                *alpha = 0.0f;
            }
            break;
        }
        case KSwapHold:
            *alpha = 0.0f;
            *dx = -swap_dir_ * media_layout::switch_shift;
            if (state.media.source_app_user_model_id == swap_target_ ||
                now - swap_start_ > media_layout::switch_hold_timeout_sec)
            {
                cancel_media_change_anims();
                swap_phase_ = KSwapIn;
                swap_start_ = now;
            }
            break;
        case KSwapIn:
        {
            const float t = clamp(static_cast<float>((now - swap_start_) / media_layout::switch_in_sec), 0.0f, 1.0f);
            const float inv = 1.0f - t;
            const float e = 1.0f - inv * inv * inv; // ease-out cubic
            *alpha = e;
            *dx = swap_dir_ * media_layout::switch_shift * (1.0f - e);
            if (t >= 1.0f)
            {
                swap_phase_ = KSwapNone;
                *alpha = 1.0f;
                *dx = 0.0f;
            }
            break;
        }
        default:
            break;
    }
    if (swap_phase_ != KSwapNone)
    {
        g_layout_dirty = true;
    }
}

// Forget "drawn a moment ago" so the new source simply appears.
void Renderer::cancel_media_change_anims()
{
    title_anim_active_ = false;
    title_old_.clear();
    title_new_.clear();
    title_last_drawn_ = -1.0;
    art_flip_start_ = -1.0;
    art_flip_old_.Reset();
    art_flip_last_drawn_ = -1.0;
    swap_artist_.start = -1.0;
    swap_artist_.last_drawn = -1.0;
    swap_album_.start = -1.0;
    swap_album_.last_drawn = -1.0;
    spec_start_ = -1.0;
    spec_last_drawn_ = -1.0;
}

bool Renderer::begin_swap_group(float alpha, float dx)
{
    if (alpha >= 0.999f && std::fabs(dx) < 0.01f)
    {
        return false;
    }
    if (!swap_group_layer_ && (FAILED(target_->CreateLayer(&swap_group_layer_)) || !swap_group_layer_))
    {
        return false;
    }
    target_->GetTransform(&swap_group_old_transform_);
    target_->SetTransform(D2D1::Matrix3x2F::Translation(dx, 0.0f) * swap_group_old_transform_);
    target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), clamp(alpha, 0.0f, 1.0f), nullptr,
                                             D2D1_LAYER_OPTIONS_NONE),
                       swap_group_layer_.Get());
    return true;
}

void Renderer::end_swap_group(bool began)
{
    if (!began)
    {
        return;
    }
    target_->PopLayer();
    target_->SetTransform(swap_group_old_transform_);
    swap_group_layer_.Reset();
}

// "Playing from Brave" / "Paused in Spotify", 11px, tertiary text.
void Renderer::draw_source_line(const MediaSnapshot& media, D2D1_RECT_F area, double now)
{
    if (media.source_name.empty() || !small_text_format_)
    {
        return;
    }
    const std::wstring text =
        format_with_name(loc(media.playing ? L"Playing from %s" : L"Paused in %s"), media.source_name);
    if (!scratch_color_brush_)
    {
        target_->CreateSolidColorBrush(material_.text_tertiary, &scratch_color_brush_);
    }
    else
    {
        scratch_color_brush_->SetColor(material_.text_tertiary);
    }
    if (!scratch_color_brush_)
    {
        return;
    }
    draw_marquee_text(text, area, small_text_format_.Get(), scratch_color_brush_.Get(), now, 26.0f,
                      marquee_source_cache_);
}

// One rounded pill, one circular slot per source. Shown only with 2+ sources.
void Renderer::draw_source_dock(const SharedState& state, D2D1_RECT_F rect, double now)
{
    const int source_count = static_cast<int>(state.media_sources.size());
    if (source_count < 2)
    {
        dock_init_ = false;
        return;
    }

    const int live_idx = active_media_source_index(state);
    const DockSlots slots = compute_dock_slots(source_count, live_idx); // same inputs as the hit test
    if (slots.count < 2)
    {
        return;
    }

    int shown_idx = live_idx;
    if (swap_phase_ != KSwapNone && !swap_target_.empty())
    {
        for (int i = 0; i < source_count; ++i)
        {
            if (state.media_sources[i].aumid == swap_target_)
            {
                shown_idx = i;
                break;
            }
        }
    }
    auto slot_of = [&](int source_idx)
    {
        for (int s = 0; s < slots.count; ++s)
        {
            if (slots.source_index[s] == source_idx)
            {
                return s;
            }
        }
        return -1;
    };
    int target_slot = slot_of(shown_idx);
    if (target_slot < 0)
    {
        target_slot = slot_of(live_idx);
    }
    if (target_slot < 0)
    {
        target_slot = 0;
    }

    const bool fresh =
        dock_init_ && dock_last_drawn_ >= 0.0 && (now - dock_last_drawn_) < 0.25 && dock_slot_count_ == slots.count;
    const float dt = fresh ? clamp(static_cast<float>(now - dock_last_drawn_), 0.001f, 0.05f) : 0.016f;
    dock_last_drawn_ = now;

    if (!fresh)
    {
        dock_init_ = true;
        dock_slot_count_ = slots.count;
        dock_ind_pos_ = dock_ind_from_ = dock_ind_target_ = static_cast<float>(target_slot);
        dock_ind_start_ = -1.0;
        for (int i = 0; i < media_layout::dock_max_slots; ++i)
        {
            dock_hover_[i] = 0.0f;
            dock_press_[i] = 0.0f;
        }
    }
    else if (static_cast<float>(target_slot) != dock_ind_target_)
    {
        dock_ind_from_ = dock_ind_pos_;
        dock_ind_target_ = static_cast<float>(target_slot);
        dock_ind_start_ = now;
    }
    if (dock_ind_start_ >= 0.0)
    {
        const float p = clamp(static_cast<float>((now - dock_ind_start_) / media_layout::dock_slide_sec), 0.0f, 1.0f);
        dock_ind_pos_ = dock_ind_from_ + (dock_ind_target_ - dock_ind_from_) * skip_ease_in_out(p);
        if (p >= 1.0f)
        {
            dock_ind_pos_ = dock_ind_target_;
            dock_ind_start_ = -1.0;
        }
        g_layout_dirty = true;
    }

    const int hovered = g_hovered_source_slot.load(std::memory_order_relaxed);
    const int pressed = g_pressed_source_slot.load(std::memory_order_relaxed);
    bool moving = false;
    for (int i = 0; i < slots.count; ++i)
    {
        const float hover_target = (i == hovered) ? 1.0f : 0.0f;
        dock_hover_[i] += (hover_target - dock_hover_[i]) * (1.0f - std::exp(-dt / 0.09f));
        if (std::fabs(hover_target - dock_hover_[i]) < 0.004f)
        {
            dock_hover_[i] = hover_target;
        }
        else
        {
            moving = true;
        }

        const float press_target = (i == pressed) ? 1.0f : 0.0f;
        const float tau = press_target > dock_press_[i] ? 0.025f : 0.075f;
        dock_press_[i] += (press_target - dock_press_[i]) * (1.0f - std::exp(-dt / tau));
        if (std::fabs(press_target - dock_press_[i]) < 0.004f)
        {
            dock_press_[i] = press_target;
        }
        else
        {
            moving = true;
        }
    }
    if (moving)
    {
        g_layout_dirty = true;
    }

    const float cy = rect.top + media_layout::controls_y;
    const float x0 = rect.left + media_layout::dock_left;
    const D2D1_RECT_F pill =
        D2D1::RectF(x0, cy - media_layout::dock_height * 0.5f, x0 + media_layout::dock_width(slots.count),
                    cy + media_layout::dock_height * 0.5f);
    draw_card(pill, media_layout::dock_height * 0.5f);

    const D2D1_COLOR_F pill_bg =
        mix_color(with_alpha(material_.base, 1.0f), with_alpha(material_.raised, 1.0f), material_.raised.a);

    const float stride = media_layout::dock_slot + media_layout::dock_gap;
    const float ind_cx =
        rect.left + media_layout::dock_slot_left(0) + dock_ind_pos_ * stride + media_layout::dock_slot * 0.5f;
    const float ind_r = media_layout::dock_slot * 0.5f;
    ComPtr<ID2D1SolidColorBrush> ind_fill;
    if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raised_strong, &ind_fill)) && ind_fill)
    {
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ind_cx, cy), ind_r, ind_r), ind_fill.Get());
    }
    accent_brush_->SetOpacity(1.0f);
    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ind_cx, cy), ind_r - 0.75f, ind_r - 0.75f), accent_brush_.Get(),
                         1.5f);

    for (int i = 0; i < slots.count; ++i)
    {
        const D2D1_POINT_2F c =
            D2D1::Point2F(rect.left + media_layout::dock_slot_left(i) + media_layout::dock_slot * 0.5f, cy);
        const float active = clamp(1.0f - std::fabs(dock_ind_pos_ - static_cast<float>(i)), 0.0f, 1.0f);
        const float lit = std::max(active, dock_hover_[i]);
        const float opacity = 0.5f + 0.5f * lit; // 50% idle, 100% active / hovered
        const float s = 1.0f - 0.10f * clamp(dock_press_[i], 0.0f, 1.0f);
        const D2D1_RECT_F slot_rect = D2D1::RectF(c.x - 13.0f, c.y - 13.0f, c.x + 13.0f, c.y + 13.0f);

        D2D1_MATRIX_3X2_F old_transform;
        target_->GetTransform(&old_transform);
        target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * old_transform);

        const int src_idx = slots.source_index[i];
        if (src_idx < 0)
        {
            wchar_t more[16] = {};
            swprintf_s(more, L"+%d", slots.overflow);
            if (bold_text_format_)
            {
                muted_brush_->SetOpacity(0.55f + 0.45f * lit);
                target_->DrawTextW(more, static_cast<UINT32>(std::wcslen(more)), bold_text_format_.Get(), slot_rect,
                                   muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                muted_brush_->SetOpacity(0.75f);
            }
        }
        else
        {
            const MediaSourceInfo& src = state.media_sources[src_idx];
            if (src.icon)
            {
                draw_circular_bitmap_pixels(*src.icon, c, media_layout::dock_icon * 0.5f, dock_icon_bitmap_[i],
                                            dock_icon_gen_[i], opacity);
            }
            else if (bold_text_format_ && !src.badge.empty())
            {
                text_brush_->SetOpacity(opacity);
                target_->DrawTextW(src.badge.c_str(), static_cast<UINT32>(src.badge.size()), bold_text_format_.Get(),
                                   slot_rect, text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                text_brush_->SetOpacity(0.96f);
            }

            if (src.playing)
            {
                const D2D1_POINT_2F d = D2D1::Point2F(c.x + 9.5f, c.y + 9.5f);
                ComPtr<ID2D1SolidColorBrush> ring, dot;
                if (SUCCEEDED(target_->CreateSolidColorBrush(pill_bg, &ring)) && ring)
                {
                    target_->FillEllipse(D2D1::Ellipse(d, 5.0f, 5.0f), ring.Get());
                }
                if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f), &dot)) && dot)
                {
                    target_->FillEllipse(D2D1::Ellipse(d, 3.5f, 3.5f), dot.Get());
                }
            }
        }
        target_->SetTransform(old_transform);
    }
}

void Renderer::update_media_button_animations(double now, bool playing)
{
    float dt = 0.016f;
    if (last_media_btn_time_ > 0.0)
    {
        dt = static_cast<float>(std::max(0.001, std::min(now - last_media_btn_time_, 0.05)));
    }
    last_media_btn_time_ = now;

    const int pressed_cmd = g_pressed_media_button.load();
    bool is_still_animating = false;

    for (int i = 0; i < 3; ++i)
    {
        const float target = (pressed_cmd == i) ? 1.0f : 0.0f;
        const float tau = (target > media_btn_press_[i]) ? 0.025f : 0.075f;
        const float k = 1.0f - std::exp(-dt / tau);
        media_btn_press_[i] += (target - media_btn_press_[i]) * k;

        if (std::abs(media_btn_press_[i] - target) < 0.002f)
        {
            media_btn_press_[i] = target;
        }
        else
        {
            is_still_animating = true;
        }
    }

    // Play/pause morph: animates only if the button was on screen a moment ago.
    {
        constexpr double morph_duration = 0.34;
        const bool recently_drawn = play_morph_last_drawn_ >= 0.0 && (now - play_morph_last_drawn_) < 0.25;
        play_morph_last_drawn_ = now;
        const float target = playing ? 0.0f : 1.0f;
        if (!play_morph_init_ || !recently_drawn || !expanded_anim_)
        {
            play_morph_init_ = true;
            play_morph_ = play_morph_from_ = play_morph_target_ = target;
            play_morph_start_ = -1.0;
        }
        else if (target != play_morph_target_)
        {
            play_morph_from_ = play_morph_;
            play_morph_target_ = target;
            play_morph_start_ = now;
        }
        if (play_morph_start_ >= 0.0)
        {
            const float p = static_cast<float>((now - play_morph_start_) / morph_duration);
            if (p >= 1.0f)
            {
                play_morph_ = play_morph_target_;
                play_morph_start_ = -1.0;
            }
            else
            {
                play_morph_ = play_morph_from_ + (play_morph_target_ - play_morph_from_) * skip_ease_in_out(p);
                is_still_animating = true;
            }
        }
    }

    constexpr double skip_duration = 0.46;
    const unsigned trig[2] = {g_skip_trigger_prev.load(), g_skip_trigger_next.load()};
    for (int i = 0; i < 2; ++i)
    {
        if (trig[i] != skip_seen_trigger_[i])
        {
            skip_seen_trigger_[i] = trig[i];
            last_skip_click_at_ = now;
            last_skip_dir_ = (i == 0) ? -1.0f : 1.0f;
            if (expanded_anim_ && (skip_anim_start_[i] < 0.0 || (now - skip_anim_start_[i]) / skip_duration >= 0.5))
            {
                skip_anim_start_[i] = now;
            }
        }
        if (skip_anim_start_[i] >= 0.0)
        {
            const double p = (now - skip_anim_start_[i]) / skip_duration;
            if (p >= 1.0)
            {
                skip_anim_start_[i] = -1.0;
                skip_progress_[i] = -1.0f;
            }
            else
            {
                skip_progress_[i] = static_cast<float>(p);
                is_still_animating = true;
            }
        }
        else
        {
            skip_progress_[i] = -1.0f;
        }
    }

    if (is_still_animating)
    {
        g_layout_dirty = true;
    }
}

// front-left minus back-left, / height
HRESULT Renderer::build_rounded_triangle(float x0, float w, float h, ComPtr<ID2D1PathGeometry>& out)
{
    const D2D1_POINT_2F v[3] = {D2D1::Point2F(x0, -0.5f * h), D2D1::Point2F(x0, 0.5f * h), D2D1::Point2F(x0 + w, 0.0f)};
    const float d[3] = {0.11f * h, 0.11f * h, 0.14f * h};
    D2D1_POINT_2F a[3], b[3];
    for (int i = 0; i < 3; ++i)
    {
        const D2D1_POINT_2F p = v[(i + 2) % 3], n = v[(i + 1) % 3];
        float px = p.x - v[i].x, py = p.y - v[i].y;
        float nx = n.x - v[i].x, ny = n.y - v[i].y;
        const float pl = std::sqrt(px * px + py * py), nl = std::sqrt(nx * nx + ny * ny);
        a[i] = D2D1::Point2F(v[i].x + px / pl * d[i], v[i].y + py / pl * d[i]);
        b[i] = D2D1::Point2F(v[i].x + nx / nl * d[i], v[i].y + ny / nl * d[i]);
    }
    ComPtr<ID2D1PathGeometry> geom;
    HRESULT hr = d2d_factory_->CreatePathGeometry(&geom);
    if (FAILED(hr) || !geom)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }
    ComPtr<ID2D1GeometrySink> sink;
    hr = geom->Open(&sink);
    if (FAILED(hr) || !sink)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }
    sink->BeginFigure(b[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(a[1]);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[1], b[1]));
    sink->AddLine(a[2]);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[2], b[2]));
    sink->AddLine(a[0]);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[0], b[0]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    if (FAILED(hr))
    {
        return hr;
    }
    out = geom;
    return S_OK;
}

bool Renderer::ensure_skip_geometry()
{
    if (skip_tri_full_ && skip_tri_notch_)
    {
        return true;
    }
    if (!d2d_factory_)
    {
        return false;
    }
    const float h = media_layout::nav_button_radius * 0.78f;
    const float w = skip_tri_w * h, pitch = skip_pitch * h;

    ComPtr<ID2D1PathGeometry> full, back, front;
    if (FAILED(build_rounded_triangle(0.0f, w, h, full)))
    {
        return false;
    }
    if (FAILED(build_rounded_triangle(0.0f, w, h, back)))
    {
        return false;
    }
    if (FAILED(build_rounded_triangle(pitch, w, h, front)))
    {
        return false;
    }

    // The back triangle is cut by an enlarged copy of the front one (works on translucent backgrounds).
    ComPtr<ID2D1TransformedGeometry> grown;
    if (FAILED(d2d_factory_->CreateTransformedGeometry(
            front.Get(), D2D1::Matrix3x2F::Scale(1.16f, 1.16f, D2D1::Point2F(pitch + w / 3.0f, 0.0f)), &grown)) ||
        !grown)
    {
        return false;
    }

    ComPtr<ID2D1PathGeometry> notch;
    if (FAILED(d2d_factory_->CreatePathGeometry(&notch)) || !notch)
    {
        return false;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(notch->Open(&sink)) || !sink)
    {
        return false;
    }
    if (FAILED(back->CombineWithGeometry(grown.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.05f, sink.Get())))
    {
        return false;
    }
    if (FAILED(sink->Close()))
    {
        return false;
    }

    skip_tri_full_ = full;
    skip_tri_notch_ = notch;
    return true;
}

float Renderer::skip_smooth(float x)
{
    x = clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float Renderer::skip_ease_in_out(float x)
{
    x = clamp(x, 0.0f, 1.0f);
    return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
}

// forward = next (>>), else previous (<<). p < 0 = resting, otherwise progress 0..1.
void Renderer::draw_skip_icon(D2D1_POINT_2F center, bool forward, float p, float opacity, float press)
{
    if (!ensure_skip_geometry())
    {
        return;
    }

    const float h = media_layout::nav_button_radius * 0.78f;
    const float w = skip_tri_w * h, pitch = skip_pitch * h;
    const float pair_left = -0.5f * (pitch + w);

    D2D1_MATRIX_3X2_F old_transform;
    target_->GetTransform(&old_transform);
    const float sf = 1.0f - 0.13f * press;
    const D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Scale(forward ? 1.0f : -1.0f, 1.0f) *
                                   D2D1::Matrix3x2F::Translation(center.x, center.y) *
                                   D2D1::Matrix3x2F::Scale(sf, sf, center) * old_transform;

    auto fill = [&](ID2D1Geometry* g, float x, float scale, float alpha)
    {
        if (alpha <= 0.003f)
        {
            return;
        }
        const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(w / 3.0f, 0.0f)) *
                                    D2D1::Matrix3x2F::Translation(x, 0.0f) * base;
        target_->SetTransform(m);
        accent_brush_->SetOpacity(clamp(opacity * alpha, 0.0f, 1.0f));
        target_->FillGeometry(g, accent_brush_.Get());
    };

    if (p < 0.0f)
    {
        fill(skip_tri_notch_.Get(), pair_left, 1.0f, 1.0f);
        fill(skip_tri_full_.Get(), pair_left + pitch, 1.0f, 1.0f);
    }
    else
    {
        const float e = skip_ease_in_out(p);

        // B (front): pushed forward, recedes and fades out.
        const float b_fade = 1.0f - skip_smooth(p / 0.75f);
        fill(skip_tri_full_.Get(), pair_left + pitch + pitch * 1.15f * e, 1.0f - 0.2f * e, b_fade);

        // A (back): glides into B's slot; its notch dissolves as it becomes the front.
        const float s = skip_smooth((p - 0.1f) / 0.8f);
        fill(skip_tri_notch_.Get(), pair_left + pitch * e, 1.0f, 1.0f - s);
        fill(skip_tri_full_.Get(), pair_left + pitch * e, 1.0f, s);

        // A' (new back): grows in behind, where A used to be.
        const float c_in = skip_smooth((p - 0.3f) / 0.7f);
        fill(skip_tri_notch_.Get(), pair_left - pitch * 0.45f * (1.0f - c_in), 0.7f + 0.3f * c_in, c_in);
    }

    target_->SetTransform(old_transform);
    accent_brush_->SetOpacity(1.0f);
}

void Renderer::draw_media_controls(bool playing, D2D1_POINT_2F prev, D2D1_POINT_2F play, D2D1_POINT_2F next, double now)
{
    update_media_button_animations(now, playing);
    draw_media_button(prev, media_layout::nav_button_radius, 0, false);
    draw_media_button(play, media_layout::play_button_radius, playing ? 1 : 2, true);
    draw_media_button(next, media_layout::nav_button_radius, 3, false);
}

void Renderer::draw_media_button(D2D1_POINT_2F center, float radius, int kind, bool primary)
{
    int button_cmd = (kind == 0) ? 0 : ((kind == 1 || kind == 2) ? 1 : 2);
    bool is_hovered = (g_hovered_media_button.load() == button_cmd);
    const float press = (button_cmd >= 0 && button_cmd < 3) ? media_btn_press_[button_cmd] : 0.0f;

    const float r = radius * (1.0f - 0.13f * press);

    const D2D1_COLOR_F resting_bg =
        D2D1::ColorF(1.0f, 1.0f, 1.0f, primary ? (is_hovered ? 0.16f : 0.080f) : (is_hovered ? 0.09f : 0.040f));
    const D2D1_COLOR_F pressed_bg =
        D2D1::ColorF(current_accent_.r, current_accent_.g, current_accent_.b, primary ? 0.28f : 0.18f);

    const D2D1_COLOR_F current_bg = D2D1::ColorF(
        resting_bg.r + (pressed_bg.r - resting_bg.r) * press, resting_bg.g + (pressed_bg.g - resting_bg.g) * press,
        resting_bg.b + (pressed_bg.b - resting_bg.b) * press, resting_bg.a + (pressed_bg.a - resting_bg.a) * press);

    if (ID2D1SolidColorBrush* bg = acquire_brush(current_bg))
    {
        target_->FillEllipse(D2D1::Ellipse(center, r, r), bg);
    }

    const float base_opacity = primary ? (is_hovered ? 1.0f : 0.88f) : (is_hovered ? 0.92f : 0.62f);
    const float icon_opacity = clamp(base_opacity + (1.0f - base_opacity) * press, 0.0f, 1.0f);
    accent_brush_->SetOpacity(icon_opacity);

    if (kind == 0 || kind == 3)
    {
        draw_skip_icon(center, kind == 3, skip_progress_[kind == 3 ? 1 : 0], icon_opacity, press);
        accent_brush_->SetOpacity(1.0f);
        return;
    }

    if (kind == 1 || kind == 2)
    {
        draw_play_pause_icon(center, icon_opacity, press);
        accent_brush_->SetOpacity(1.0f);
        return;
    }

    const wchar_t* glyph = nullptr;
    IDWriteTextFormat* fmt = nullptr;
    if (using_fluent_icons_)
    {
        if (kind == 0)
        {
            glyph = L"\uF8AC";
            fmt = media_nav_icon_format_.Get();
        }
        else if (kind == 1)
        {
            glyph = L"\uF8AE";
            fmt = media_play_icon_format_.Get();
        }
        else if (kind == 2)
        {
            glyph = L"\uF5B0";
            fmt = media_play_icon_format_.Get();
        }
        else if (kind == 3)
        {
            glyph = L"\uF8AD";
            fmt = media_nav_icon_format_.Get();
        }
    }
    else
    {
        if (kind == 0)
        {
            glyph = L"\uE100";
            fmt = media_nav_icon_format_.Get();
        }
        else if (kind == 1)
        {
            glyph = L"\uE103";
            fmt = media_play_icon_format_.Get();
        }
        else if (kind == 2)
        {
            glyph = L"\uE102";
            fmt = media_play_icon_format_.Get();
        }
        else if (kind == 3)
        {
            glyph = L"\uE101";
            fmt = media_nav_icon_format_.Get();
        }
    }

    if (glyph && fmt)
    {
        const float offset_x = (kind == 2) ? 1.0f : 0.0f;
        D2D1_RECT_F glyph_rect = D2D1::RectF(center.x - radius + offset_x, center.y - radius,
                                             center.x + radius + offset_x, center.y + radius);

        if (press > 0.001f)
        {
            D2D1_MATRIX_3X2_F old_transform;
            target_->GetTransform(&old_transform);
            const float scale_factor = 1.0f - 0.13f * press;
            target_->SetTransform(D2D1::Matrix3x2F::Scale(scale_factor, scale_factor, center) * old_transform);
            target_->DrawTextW(glyph, 1, fmt, glyph_rect, accent_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            target_->SetTransform(old_transform);
        }
        else
        {
            target_->DrawTextW(glyph, 1, fmt, glyph_rect, accent_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    accent_brush_->SetOpacity(1.0f);
}

const Renderer::PlayPauseShapes& Renderer::get_play_pause_shapes()
{
    static const PlayPauseShapes shapes = []
    {
        PlayPauseShapes s = {};
        const float rr = play_pause_corner;
        const float bx0 = 2.1f, bx1 = 6.5f, by = 7.5f;
        s.pause[0] = D2D1::Point2F(-bx1 + rr, -by + rr);
        s.pause[1] = D2D1::Point2F(-bx0 - rr, -by + rr);
        s.pause[2] = D2D1::Point2F(-bx0 - rr, by - rr);
        s.pause[3] = D2D1::Point2F(-bx1 + rr, by - rr);
        s.pause[4] = D2D1::Point2F(bx0 + rr, -by + rr);
        s.pause[5] = D2D1::Point2F(bx1 - rr, -by + rr);
        s.pause[6] = D2D1::Point2F(bx1 - rr, by - rr);
        s.pause[7] = D2D1::Point2F(bx0 + rr, by - rr);

        const D2D1_POINT_2F a = D2D1::Point2F(-5.5f, -7.5f);
        const D2D1_POINT_2F b = D2D1::Point2F(-5.5f, 7.5f);
        const D2D1_POINT_2F c = D2D1::Point2F(7.5f, 0.0f);
        const D2D1_POINT_2F t = D2D1::Point2F(1.0f, -3.75f);
        const D2D1_POINT_2F u = D2D1::Point2F(1.0f, 3.75f);
        const float la = std::hypot(b.x - c.x, b.y - c.y);
        const float lb = std::hypot(c.x - a.x, c.y - a.y);
        const float lc = std::hypot(a.x - b.x, a.y - b.y);
        const float per = la + lb + lc;
        const float ix = (la * a.x + lb * b.x + lc * c.x) / per;
        const float iy = (la * a.y + lb * b.y + lc * c.y) / per;
        const float area = 0.5f * std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        const float inr = 2.0f * area / per;
        const float k = (inr - rr) / inr;
        auto shrink = [&](D2D1_POINT_2F p)
        {
            return D2D1::Point2F(ix + k * (p.x - ix), iy + k * (p.y - iy));
        };
        s.play[0] = shrink(a);
        s.play[1] = shrink(t);
        s.play[2] = shrink(u);
        s.play[3] = shrink(b);
        s.play[4] = shrink(t);
        s.play[5] = shrink(c);
        s.play[6] = shrink(c);
        s.play[7] = shrink(u);
        return s;
    }();
    return shapes;
}

bool Renderer::ensure_round_join_style()
{
    if (round_join_style_)
    {
        return true;
    }
    if (!d2d_factory_)
    {
        return false;
    }
    return SUCCEEDED(d2d_factory_->CreateStrokeStyle(
               D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                           D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f),
               nullptr, 0, &round_join_style_)) &&
           round_join_style_;
}

void Renderer::draw_play_pause_icon(D2D1_POINT_2F center, float opacity, float press)
{
    if (!d2d_factory_)
    {
        return;
    }
    ensure_round_join_style();
    const PlayPauseShapes& sh = get_play_pause_shapes();
    const float m = clamp(play_morph_, 0.0f, 1.0f);

    D2D1_POINT_2F p[8];
    for (int i = 0; i < 8; ++i)
    {
        p[i] = D2D1::Point2F(sh.pause[i].x + (sh.play[i].x - sh.pause[i].x) * m,
                             sh.pause[i].y + (sh.play[i].y - sh.pause[i].y) * m);
    }

    ComPtr<ID2D1PathGeometry> quad[2];
    for (int q = 0; q < 2; ++q)
    {
        if (FAILED(d2d_factory_->CreatePathGeometry(&quad[q])) || !quad[q])
        {
            return;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(quad[q]->Open(&sink)) || !sink)
        {
            return;
        }
        sink->BeginFigure(p[q * 4], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(p[q * 4 + 1]);
        sink->AddLine(p[q * 4 + 2]);
        sink->AddLine(p[q * 4 + 3]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close()))
        {
            return;
        }
    }

    D2D1_MATRIX_3X2_F old_transform;
    target_->GetTransform(&old_transform);

    // Drawn opaque inside a layer carrying the opacity, so overlaps never double-blend.
    ComPtr<ID2D1Layer> layer;
    const bool layered = SUCCEEDED(target_->CreateLayer(nullptr, &layer)) && layer;
    if (layered)
    {
        target_->PushLayer(
            D2D1::LayerParameters(D2D1::RectF(center.x - 18.0f, center.y - 18.0f, center.x + 18.0f, center.y + 18.0f),
                                  nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity, nullptr,
                                  D2D1_LAYER_OPTIONS_NONE),
            layer.Get());
    }
    accent_brush_->SetOpacity(layered ? 1.0f : opacity);

    const float sf = 1.0f - 0.13f * press;
    target_->SetTransform(D2D1::Matrix3x2F::Translation(center.x, center.y) * D2D1::Matrix3x2F::Scale(sf, sf, center) *
                          old_transform);
    for (int q = 0; q < 2; ++q)
    {
        target_->FillGeometry(quad[q].Get(), accent_brush_.Get());
        target_->DrawGeometry(quad[q].Get(), accent_brush_.Get(), play_pause_corner * 2.0f, round_join_style_.Get());
    }
    target_->SetTransform(old_transform);
    if (layered)
    {
        target_->PopLayer();
    }
    accent_brush_->SetOpacity(1.0f);
}

// Song title change animation. Scripts that shape glyphs together fall back to a plain redraw.
bool Renderer::title_needs_shaping(wchar_t c)
{
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0590 && c <= 0x0DFF) || (c >= 0x0E00 && c <= 0x0EFF) || c == 0x200D;
}

// Same formula as draw_marquee_text, so the hand-off at the end does not jump.
float Renderer::title_scroll_offset(const TitleScroll& sc, double now)
{
    return sc.scrolls ? std::fmod(static_cast<float>(now) * 42.0f, sc.cycle) : 0.0f;
}

bool Renderer::build_title_glyphs(const std::wstring& text, float wrap_height, float available, double now,
                                  bool all_layouts, std::vector<TitleGlyph>& out, TitleScroll& scroll)
{
    out.clear();
    scroll = TitleScroll{};
    if (!dwrite_factory_ || !text_format_ || text.empty() || text.size() > 120)
    {
        return false;
    }

    ComPtr<IDWriteTextLayout> full;
    if (FAILED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), text_format_.Get(),
                                                 2000.0f, wrap_height, &full)) ||
        !full)
    {
        return false;
    }
    DWRITE_TEXT_METRICS tm = {};
    full->GetMetrics(&tm);
    scroll.width = tm.widthIncludingTrailingWhitespace;
    scroll.scrolls = scroll.width > available;
    scroll.cycle = scroll.width + 38.0f;
    scroll.offset0 = title_scroll_offset(scroll, now);

    for (UINT32 i = 0; i < text.size();)
    {
        const wchar_t c = text[i];
        if (title_needs_shaping(c))
        {
            return false;
        }
        const UINT32 len = (IS_HIGH_SURROGATE(c) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) ? 2 : 1;
        if (c != L' ' && c != L'\t' && c != 0x00A0 && c != 0x3000)
        {
            TitleGlyph g;
            g.ch0 = c;
            g.ch1 = (len == 2) ? text[i + 1] : 0;
            float px = 0.0f, py = 0.0f;
            DWRITE_HIT_TEST_METRICS htm = {};
            if (FAILED(full->HitTestTextPosition(i, FALSE, &px, &py, &htm)))
            {
                return false;
            }
            g.x = px;
            g.sx = px - scroll.offset0;
            g.eligible = (g.sx + htm.width > 0.0f) && (g.sx < available);
            if (all_layouts || g.eligible)
            {
                if (FAILED(dwrite_factory_->CreateTextLayout(text.c_str() + i, len, text_format_.Get(), 2000.0f,
                                                             wrap_height, &g.layout)) ||
                    !g.layout)
                {
                    return false;
                }
                DWRITE_TEXT_METRICS gm = {};
                g.layout->GetMetrics(&gm);
                g.w = gm.widthIncludingTrailingWhitespace;
                g.h = gm.height;
                const float hv =
                    std::sin(static_cast<float>(i) * 12.9898f + static_cast<float>(c) * 78.233f) * 43758.5453f;
                g.seed = hv - std::floor(hv);
                out.push_back(std::move(g));
            }
        }
        i += len;
    }
    return !out.empty();
}

// Longest common subsequence of identical visible glyphs; ties prefer the shortest travel.
void Renderer::match_title_glyphs(std::vector<TitleGlyph>& a, std::vector<TitleGlyph>& b)
{
    const std::size_t n = a.size(), m = b.size();
    std::vector<int> dp((n + 1) * (m + 1), 0);
    auto at = [&](std::size_t i, std::size_t j) -> int&
    {
        return dp[i * (m + 1) + j];
    };
    auto same = [&](std::size_t i, std::size_t j)
    {
        return a[i].eligible && b[j].eligible && a[i].ch0 == b[j].ch0 && a[i].ch1 == b[j].ch1;
    };
    auto pair_score = [&](std::size_t i, std::size_t j)
    {
        return 1000 - static_cast<int>(std::min(std::fabs(a[i].sx - b[j].sx), 900.0f));
    };
    for (std::size_t i = 1; i <= n; ++i)
    {
        for (std::size_t j = 1; j <= m; ++j)
        {
            int best = std::max(at(i - 1, j), at(i, j - 1));
            if (same(i - 1, j - 1))
            {
                best = std::max(best, at(i - 1, j - 1) + pair_score(i - 1, j - 1));
            }
            at(i, j) = best;
        }
    }
    std::size_t i = n, j = m;
    while (i > 0 && j > 0)
    {
        if (same(i - 1, j - 1) && at(i, j) == at(i - 1, j - 1) + pair_score(i - 1, j - 1))
        {
            a[i - 1].match = static_cast<int>(j - 1);
            b[j - 1].match = static_cast<int>(i - 1);
            --i;
            --j;
        }
        else if (at(i - 1, j) >= at(i, j - 1))
        {
            --i;
        }
        else
        {
            --j;
        }
    }
}

void Renderer::start_title_transition(const std::wstring& old_text, const std::wstring& new_text, D2D1_RECT_F rect,
                                      double now)
{
    const float h = rect.bottom - rect.top;
    const float avail = rect.right - rect.left;
    if (!build_title_glyphs(old_text, h, avail, now, false, title_old_, title_old_scroll_) ||
        !build_title_glyphs(new_text, h, avail, now, true, title_new_, title_new_scroll_))
    {
        title_old_.clear();
        title_new_.clear();
        return;
    }
    match_title_glyphs(title_old_, title_new_);
    title_avail_ = avail;
    title_anim_start_ = now;
    title_anim_dur_ = 1.0f;
    title_anim_active_ = true;
}

// t: 0 = solid, 1 = gone. Reversed (t counting down) makes letters condense in.
void Renderer::draw_smoke_glyph(const TitleGlyph& g, D2D1_POINT_2F origin, float t, float amp, ID2D1Brush* brush,
                                float base_opacity)
{
    if (t >= 0.999f || !g.layout)
    {
        return;
    }
    if (t <= 0.0f)
    {
        brush->SetOpacity(base_opacity);
        target_->DrawTextLayout(origin, g.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        return;
    }
    D2D1_MATRIX_3X2_F old_t;
    target_->GetTransform(&old_t);

    const float seed = g.seed;
    const float rise = 16.0f * amp * std::pow(t, 1.15f);
    const float sway = std::sin(t * (4.5f + 3.0f * seed) + seed * 6.2832f) * 4.5f * amp * std::sqrt(t);
    const float angle = (seed - 0.5f) * 50.0f * t * amp;
    const float sx = 1.0f + 0.55f * t * amp;
    const float sy = 1.0f + 1.40f * t * amp;
    const float alpha = std::pow(1.0f - t, 1.6f) * base_opacity;
    const float soft = skip_smooth(t / 0.25f);
    const float spread = t * 3.2f * amp;
    const D2D1_POINT_2F pivot = D2D1::Point2F(origin.x + g.w * 0.5f, origin.y + g.h * 0.82f);

    for (int k = 0; k < 4; ++k)
    {
        const float dx = (k == 1) ? -spread * 0.9f : (k == 3 ? spread * 0.9f : 0.0f);
        const float dy = -static_cast<float>(k) * spread * 0.7f;
        const float a = (k == 0) ? alpha * (1.0f - 0.55f * soft) : alpha * 0.28f * soft;
        if (a <= 0.003f)
        {
            continue;
        }
        brush->SetOpacity(clamp(a, 0.0f, 1.0f));
        target_->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy, pivot) *
                              D2D1::Matrix3x2F::Rotation(angle * (1.0f + 0.25f * static_cast<float>(k)), pivot) *
                              D2D1::Matrix3x2F::Translation(sway + dx, -rise + dy) * old_t);
        target_->DrawTextLayout(origin, g.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    target_->SetTransform(old_t);
    brush->SetOpacity(base_opacity);
}

// True when it drew the title itself (animation running).
bool Renderer::draw_title_transition(const std::wstring& text, D2D1_RECT_F rect, ID2D1Brush* brush, double now,
                                     bool real_title)
{
    const bool recently_drawn = title_last_drawn_ >= 0.0 && (now - title_last_drawn_) < 0.25;
    title_last_drawn_ = now;

    if (text != title_shown_)
    {
        const std::wstring old_text = title_shown_;
        const bool old_was_real = !title_shown_placeholder_;
        title_shown_ = text;
        title_shown_placeholder_ = !real_title;
        title_anim_active_ = false;
        title_old_.clear();
        title_new_.clear();
        if (expanded_anim_ && recently_drawn && real_title && old_was_real && !old_text.empty())
        {
            start_title_transition(old_text, text, rect, now);
        }
    }
    else if (title_anim_active_ && (!recently_drawn || !expanded_anim_))
    {
        title_anim_active_ = false;
        title_old_.clear();
        title_new_.clear();
    }

    if (!title_anim_active_)
    {
        return false;
    }

    const float time = static_cast<float>(now - title_anim_start_);
    if (time >= title_anim_dur_)
    {
        title_anim_active_ = false;
        title_old_.clear();
        title_new_.clear();
        return false;
    }

    const float base_opacity = brush->GetOpacity();
    const float avail = std::max(title_avail_, 1.0f);
    const float new_offset = title_scroll_offset(title_new_scroll_, now);
    target_->PushAxisAlignedClip(D2D1::RectF(rect.left - 4.0f, rect.top - 20.0f, rect.right + 4.0f, rect.bottom + 6.0f),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const float old_span = std::max(std::min(title_old_scroll_.width, avail), 1.0f);
    for (const TitleGlyph& g : title_old_)
    {
        if (g.match >= 0 || !g.eligible)
        {
            continue;
        }
        const float f = clamp(g.sx / old_span, 0.0f, 1.0f);
        const float t = clamp((time - 0.22f * f) / 0.60f, 0.0f, 1.0f);
        draw_smoke_glyph(g, D2D1::Point2F(rect.left + g.sx, rect.top), t, 1.0f, brush, base_opacity);
    }

    brush->SetOpacity(base_opacity);
    for (const TitleGlyph& g : title_old_)
    {
        if (g.match < 0)
        {
            continue;
        }
        const TitleGlyph& n = title_new_[static_cast<std::size_t>(g.match)];
        const float e = skip_ease_in_out((time - 0.04f) / 0.55f);
        const float target_x = n.x - new_offset;
        const float x = g.sx + (target_x - g.sx) * e;
        const float lift = -1.5f * std::sin(3.14159265f * e);
        target_->DrawTextLayout(D2D1::Point2F(rect.left + x, rect.top + lift), g.layout.Get(), brush,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    const float new_span = std::max(std::min(title_new_scroll_.width, avail), 1.0f);
    for (const TitleGlyph& g : title_new_)
    {
        if (g.match >= 0)
        {
            continue;
        }
        const float xs = g.x - new_offset;
        const float f = clamp(xs / new_span, 0.0f, 1.0f);
        const float u = clamp((time - 0.28f - 0.20f * f) / 0.50f, 0.0f, 1.0f);
        if (xs + g.w > 0.0f && xs < avail)
        {
            draw_smoke_glyph(g, D2D1::Point2F(rect.left + xs, rect.top), 1.0f - u, 0.45f, brush, base_opacity);
        }
        if (title_new_scroll_.scrolls)
        {
            const float xs2 = xs + title_new_scroll_.cycle;
            if (xs2 + g.w > 0.0f && xs2 < avail)
            {
                draw_smoke_glyph(g, D2D1::Point2F(rect.left + xs2, rect.top), 1.0f - u, 0.45f, brush, base_opacity);
            }
        }
    }

    brush->SetOpacity(base_opacity);
    target_->PopAxisAlignedClip();
    g_layout_dirty = true;
    return true;
}

// Progress bar styles (Wavy, Squiggle, Bar). Slim keeps its own path in draw_media. Every shape stays
// inside the scrubber hit area.
void Renderer::draw_styled_progress(ProgressStyle style, float bar_left, float bar_right, float cy, float progress,
                                    bool dragging, bool playing, double now)
{
    const float width = bar_right - bar_left;
    if (width <= 8.0f)
    {
        return;
    }
    progress = clamp(progress, 0.0f, 1.0f);
    const float thumb_x = bar_left + width * progress;

    ComPtr<ID2D1SolidColorBrush> accent, track;
    if (FAILED(target_->CreateSolidColorBrush(with_alpha(material_.accent, 1.0f), &accent)) || !accent)
    {
        return;
    }
    if (FAILED(target_->CreateSolidColorBrush(material_.raised_strong, &track)) || !track)
    {
        return;
    }
    ensure_round_join_style();

    {
        const bool fresh = progress_last_time_ >= 0.0 && (now - progress_last_time_) < 0.25;
        const float target = playing ? 1.0f : 0.0f;
        if (!fresh)
        {
            progress_amp_ = target;
        }
        else
        {
            const float dt = static_cast<float>(now - progress_last_time_);
            progress_amp_ += (target - progress_amp_) * (1.0f - std::exp(-dt / 0.16f));
            if (std::fabs(target - progress_amp_) < 0.004f)
            {
                progress_amp_ = target;
            }
            else
            {
                g_layout_dirty = true;
            }
        }
        progress_last_time_ = now;
    }
    const float amp = progress_amp_;

    auto draw_end_dot = [&](float x)
    {
        ComPtr<ID2D1SolidColorBrush> dot;
        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.9f), &dot)) && dot)
        {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 1.7f, 1.7f), dot.Get());
        }
    };

    if (style == ProgressStyle::Bar)
    {
        const float half = dragging ? 4.5f : 4.0f;
        const float gap = dragging ? 5.0f : 4.0f;
        const float fill_right = thumb_x - gap;
        const float track_left = thumb_x + gap;
        if (fill_right - bar_left > 1.0f)
        {
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(bar_left, cy - half, fill_right, cy + half), half, half), accent.Get());
        }
        if (bar_right - track_left > 1.0f)
        {
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(track_left, cy - half, bar_right, cy + half), half, half), track.Get());
            draw_end_dot(bar_right - half);
        }
        const float tw = dragging ? 4.0f : 3.0f;
        const float th = dragging ? 24.0f : 20.0f;
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(thumb_x - tw * 0.5f, cy - th * 0.5f, thumb_x + tw * 0.5f, cy + th * 0.5f),
                              tw * 0.5f, tw * 0.5f),
            accent.Get());
        return;
    }

    const bool wavy = (style == ProgressStyle::Wavy);
    const float a = (wavy ? 3.2f : 1.8f) * amp;
    const float lambda = wavy ? 26.0f : 15.0f;
    const double freq = wavy ? 0.9 : 0.45;
    const float stroke_w = wavy ? 3.4f : 3.0f;
    const float k = 6.2831853f / lambda;
    const float phase = static_cast<float>(std::fmod(now * freq, 1.0) * 6.2831853);
    const float end_taper = wavy ? 16.0f : 10.0f;

    if (thumb_x - bar_left > 1.5f)
    {
        std::vector<D2D1_POINT_2F> pts;
        pts.reserve(static_cast<std::size_t>((thumb_x - bar_left) / 1.25f) + 3);
        for (float x = bar_left;; x += 1.25f)
        {
            const float xx = std::min(x, thumb_x);
            const float taper = std::min(skip_smooth((xx - bar_left) / 10.0f), skip_smooth((thumb_x - xx) / end_taper));
            pts.push_back(D2D1::Point2F(xx, cy + a * taper * std::sin(k * (xx - bar_left) - phase)));
            if (x >= thumb_x)
            {
                break;
            }
        }
        ComPtr<ID2D1PathGeometry> path;
        if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&path)) && path)
        {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(path->Open(&sink)) && sink)
            {
                sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                if (pts.size() > 1)
                {
                    sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close()))
                {
                    target_->DrawGeometry(path.Get(), accent.Get(), stroke_w, round_join_style_.Get());
                }
            }
        }
    }

    const float thumb_r = dragging ? 7.0f : 5.5f;
    const float track_start = thumb_x + (wavy ? thumb_r + 3.0f : 4.0f);
    if (bar_right - track_start > 2.0f)
    {
        const float tw = wavy ? 2.2f : 2.8f;
        target_->DrawLine(D2D1::Point2F(track_start, cy), D2D1::Point2F(bar_right - 2.0f, cy), track.Get(), tw,
                          round_join_style_.Get());
        if (wavy)
        {
            draw_end_dot(bar_right - 1.7f);
        }
    }

    if (wavy)
    {
        if (dragging)
        {
            ComPtr<ID2D1SolidColorBrush> halo;
            if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.22f), &halo)) && halo)
            {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, cy), thumb_r * 2.0f, thumb_r * 2.0f),
                                     halo.Get());
            }
        }
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, cy), thumb_r, thumb_r), accent.Get());
    }
    else
    {
        const float tw = dragging ? 4.0f : 3.0f;
        const float th = dragging ? 22.0f : 18.0f;
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(thumb_x - tw * 0.5f, cy - th * 0.5f, thumb_x + tw * 0.5f, cy + th * 0.5f),
                              tw * 0.5f, tw * 0.5f),
            accent.Get());
    }
}

// Artist and album lines: the old line drifts up and fades, the new one rises in.
void Renderer::draw_swap_marquee(SwapTextState& st, const std::wstring& text, D2D1_RECT_F rect,
                                 IDWriteTextFormat* format, ID2D1Brush* brush, double now, float speed,
                                 MarqueeLayoutCache& cache, float delay)
{
    constexpr float swap_duration = 0.38f;
    const bool recently_drawn = st.last_drawn >= 0.0 && (now - st.last_drawn) < 0.25;
    st.last_drawn = now;

    if (text != st.shown)
    {
        if (recently_drawn && expanded_anim_)
        {
            st.prev = st.shown;
            st.start = now;
        }
        else
        {
            st.prev.clear();
            st.start = -1.0;
        }
        st.shown = text;
    }
    else if (st.start >= 0.0 && !recently_drawn)
    {
        st.start = -1.0;
    }

    const float elapsed = st.start >= 0.0 ? static_cast<float>(now - st.start) : -1.0f;
    if (elapsed < 0.0f || elapsed >= delay + swap_duration)
    {
        st.start = -1.0;
        draw_marquee_text(text, rect, format, brush, now, speed, cache);
        return;
    }

    const float u = clamp((elapsed - delay) / swap_duration, 0.0f, 1.0f);
    const float e = skip_ease_in_out(u);
    const float base = brush->GetOpacity();
    const float dy = 5.0f;
    D2D1_RECT_F up = rect;
    up.top -= dy * e;
    up.bottom -= dy * e;
    D2D1_RECT_F down = rect;
    down.top += dy * (1.0f - e);
    down.bottom += dy * (1.0f - e);

    brush->SetOpacity(base * (1.0f - skip_smooth(u * 1.4f)));
    draw_marquee_text(st.prev, up, format, brush, now, speed, st.prev_cache);
    brush->SetOpacity(base * skip_smooth((u - 0.25f) / 0.75f));
    draw_marquee_text(text, down, format, brush, now, speed, cache);
    brush->SetOpacity(base);
    g_layout_dirty = true;
}

}
