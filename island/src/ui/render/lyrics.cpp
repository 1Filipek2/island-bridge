#include "ui/render/renderer_impl.h"

namespace island
{

// Karaoke: a soft wave rides the wipe across the active line (layout cached per line).
void Renderer::draw_karaoke_letter_pop(const wchar_t* text, UINT32 text_length, IDWriteTextFormat* format,
                                       D2D1_RECT_F line_rect, float wipe_x, float base_opacity, float unsung_opacity,
                                       bool glow_enabled, float glow_strength)
{
    if (!text || text_length == 0 || !format || base_opacity <= 0.01f)
    {
        return;
    }

    const float layout_width = line_rect.right - line_rect.left;
    const float layout_height = line_rect.bottom - line_rect.top;
    if (layout_width <= 0.0f || layout_height <= 0.0f)
    {
        return;
    }

    const std::wstring text_view(text, text_length);
    if (karaoke_cache_.text != text_view || karaoke_cache_.format != format ||
        std::fabs(karaoke_cache_.layout_width - layout_width) > 0.5f ||
        std::fabs(karaoke_cache_.layout_height - layout_height) > 0.5f || !karaoke_cache_.layout)
    {
        karaoke_cache_.layout.Reset();
        karaoke_cache_.clusters.clear();
        karaoke_cache_.valid = false;
        karaoke_cache_.text = text_view;
        karaoke_cache_.format = format;
        karaoke_cache_.layout_width = layout_width;
        karaoke_cache_.layout_height = layout_height;

        dwrite_factory_->CreateTextLayout(text, text_length, format, layout_width, layout_height,
                                          &karaoke_cache_.layout);
        if (karaoke_cache_.layout)
        {
            // Walk shaped clusters, not UTF-16 units, so multi-unit letters (Devanagari) stay intact.
            UINT32 cluster_count = 0;
            karaoke_cache_.layout->GetClusterMetrics(nullptr, 0, &cluster_count);
            if (cluster_count > 0)
            {
                std::vector<DWRITE_CLUSTER_METRICS> raw_clusters(cluster_count);
                if (SUCCEEDED(
                        karaoke_cache_.layout->GetClusterMetrics(raw_clusters.data(), cluster_count, &cluster_count)))
                {
                    karaoke_cache_.clusters.reserve(cluster_count);
                    UINT32 cluster_start = 0;
                    for (UINT32 c = 0; c < cluster_count; ++c)
                    {
                        const UINT32 cluster_length = raw_clusters[c].length;
                        const UINT32 i = cluster_start;
                        cluster_start += cluster_length;

                        KaraokeLayoutCache::ClusterInfo info;
                        info.text_start = i;
                        info.text_length = cluster_length;
                        info.is_whitespace = raw_clusters[c].isWhitespace || cluster_length == 0;

                        if (!info.is_whitespace)
                        {
                            DWRITE_HIT_TEST_METRICS hit{};
                            UINT32 hit_count = 0;
                            if (SUCCEEDED(karaoke_cache_.layout->HitTestTextRange(i, cluster_length, 0.0f, 0.0f, &hit,
                                                                                  1, &hit_count)) &&
                                hit_count > 0 && hit.width > 0.0f)
                            {
                                info.rect = D2D1::RectF(hit.left, hit.top, hit.left + hit.width, hit.top + hit.height);
                            }
                            else
                            {
                                info.is_whitespace = true;
                            }
                        }

                        karaoke_cache_.clusters.push_back(info);
                    }
                    karaoke_cache_.valid = true;
                }
            }
        }
    }

    if (!karaoke_cache_.valid)
    {
        muted_brush_->SetOpacity(base_opacity * unsung_opacity);
        target_->DrawTextW(text, text_length, format, line_rect, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (wipe_x > line_rect.left)
        {
            D2D1_RECT_F sung_clip =
                D2D1::RectF(line_rect.left, line_rect.top, std::min(wipe_x, line_rect.right), line_rect.bottom);
            target_->PushAxisAlignedClip(sung_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            text_brush_->SetOpacity(base_opacity);
            target_->DrawTextW(text, text_length, format, line_rect, text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            target_->PopAxisAlignedClip();
        }
        return;
    }

    constexpr float wave_trail_width = 150.0f;
    constexpr float wave_lead_width = 46.0f;
    constexpr int glow_stamp_count = 4;
    constexpr float glow_radius = 0.85f;
    constexpr float glow_alpha = 0.075f;

    for (const auto& cluster : karaoke_cache_.clusters)
    {
        if (cluster.is_whitespace)
        {
            continue;
        }

        const D2D1_RECT_F hit_rect =
            D2D1::RectF(cluster.rect.left + line_rect.left, cluster.rect.top + line_rect.top,
                        cluster.rect.right + line_rect.left, cluster.rect.bottom + line_rect.top);

        const float char_center_x = (hit_rect.left + hit_rect.right) * 0.5f;
        const float dist_from_wipe = char_center_x - wipe_x; // <= 0 means already sung

        float pop;
        if (dist_from_wipe <= 0.0f)
        {
            const float t = clamp(-dist_from_wipe / wave_trail_width, 0.0f, 1.0f);
            pop = 0.5f * (1.0f + std::cos(t * 3.14159265f));
        }
        else
        {
            const float t = clamp(dist_from_wipe / wave_lead_width, 0.0f, 1.0f);
            pop = 0.5f * (1.0f + std::cos(t * 3.14159265f));
        }

        const bool sung = dist_from_wipe <= 0.0f;
        D2D1_RECT_F char_rect = D2D1::RectF(hit_rect.left, line_rect.top, line_rect.right, line_rect.bottom);
        const wchar_t* cluster_text = text + cluster.text_start;

        if (glow_enabled && glow_strength > 0.01f && pop > 0.02f)
        {
            const float alpha = glow_alpha * pop * base_opacity * glow_strength;
            if (alpha > 0.003f)
            {
                text_brush_->SetOpacity(alpha);
                for (int s = 0; s < glow_stamp_count; ++s)
                {
                    const float a = (2.0f * 3.14159265f * s) / glow_stamp_count;
                    const float ox = std::cos(a) * glow_radius;
                    const float oy = std::sin(a) * glow_radius;
                    D2D1_RECT_F stamp_rect = D2D1::RectF(char_rect.left + ox, char_rect.top + oy, char_rect.right + ox,
                                                         char_rect.bottom + oy);
                    target_->DrawTextW(cluster_text, cluster.text_length, format, stamp_rect, text_brush_.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                text_brush_->SetOpacity(base_opacity);
            }
        }

        if (sung)
        {
            text_brush_->SetOpacity(base_opacity);
            target_->DrawTextW(cluster_text, cluster.text_length, format, char_rect, text_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        else
        {
            muted_brush_->SetOpacity(base_opacity * unsung_opacity);
            target_->DrawTextW(cluster_text, cluster.text_length, format, char_rect, muted_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

// Immersive lyrics (Modules.MediaTabLyrics): cross-fades the media card into a lyrics view. Scrubber,
// controls and source dock stay.
float Renderer::update_immersive_mix(const Settings& settings, float height, double now)
{
    if (!settings.media_tab_lyrics)
    {
        if (g_immersive_lyrics.load(std::memory_order_relaxed))
        {
            g_immersive_lyrics.store(false, std::memory_order_relaxed);
        }
        immersive_mix_lin_ = 0.0f;
        immersive_mix_last_ = -1.0;
        return 0.0f;
    }

    const float target = g_immersive_lyrics.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

    // Collapsed: keep the choice, so the next expand opens straight into the same view.
    if (height < 52.0f)
    {
        immersive_mix_lin_ = target;
        immersive_mix_last_ = -1.0;
        return smooth_step01(target);
    }

    const bool fresh = immersive_mix_last_ >= 0.0 && (now - immersive_mix_last_) < 0.25;
    if (!fresh)
    {
        immersive_mix_lin_ = target;
    }
    else
    {
        const float dt = clamp(static_cast<float>(now - immersive_mix_last_), 0.001f, 0.05f);
        const float step = dt / static_cast<float>(immersive_layout::fade_sec);
        if (target > immersive_mix_lin_)
        {
            immersive_mix_lin_ = std::min(target, immersive_mix_lin_ + step);
        }
        else
        {
            immersive_mix_lin_ = std::max(target, immersive_mix_lin_ - step);
        }
        if (immersive_mix_lin_ != target)
        {
            g_layout_dirty = true; // keep frames coming until it settles
        }
    }
    immersive_mix_last_ = now;
    return smooth_step01(immersive_mix_lin_);
}

bool Renderer::begin_immersive_fade(float alpha)
{
    if (alpha >= 0.999f)
    {
        return false;
    }
    ID2D1Layer* layer = acquire_layer();
    if (!layer)
    {
        return false;
    }
    target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), clamp(alpha, 0.0f, 1.0f), nullptr,
                                             D2D1_LAYER_OPTIONS_NONE),
                       layer);
    return true;
}

void Renderer::end_immersive_fade(bool began)
{
    if (began)
    {
        target_->PopLayer();
    }
}

void Renderer::draw_immersive_lyrics_button(D2D1_RECT_F rect, float mix)
{
    if (!ensure_round_join_style())
    {
        return;
    }
    const float r = immersive_layout::btn_radius;
    const D2D1_POINT_2F c =
        D2D1::Point2F(rect.right - immersive_layout::btn_inset_right - r, rect.top + media_layout::controls_y);
    const bool hovered = g_hovered_lyrics_btn.load(std::memory_order_relaxed);

    const D2D1_COLOR_F rest_bg = D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered ? 0.09f : 0.040f);
    const D2D1_COLOR_F on_bg =
        D2D1::ColorF(current_accent_.r, current_accent_.g, current_accent_.b, hovered ? 0.32f : 0.24f);
    if (ID2D1SolidColorBrush* bg = acquire_brush(mix_color(rest_bg, on_bg, mix)))
    {
        target_->FillEllipse(D2D1::Ellipse(c, r, r), bg);
    }

    const float rest_opacity = hovered ? 0.92f : 0.62f;
    const float opacity = clamp(rest_opacity + (1.0f - rest_opacity) * mix, 0.0f, 1.0f);
    const float sw = 1.4f;
    ID2D1StrokeStyle* st = round_join_style_.Get();
    const D2D1_RECT_F body = D2D1::RectF(c.x - 7.0f, c.y - 7.2f, c.x + 7.0f, c.y + 3.8f);
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(body, 3.0f, 3.0f);

    if (mix > 0.01f)
    {
        accent_brush_->SetOpacity(0.30f * mix);
        target_->FillRoundedRectangle(rr, accent_brush_.Get());
    }
    accent_brush_->SetOpacity(opacity);
    target_->DrawRoundedRectangle(rr, accent_brush_.Get(), sw, st);
    target_->DrawLine(D2D1::Point2F(c.x - 4.5f, body.bottom), D2D1::Point2F(c.x - 4.5f, c.y + 7.2f),
                      accent_brush_.Get(), sw, st);
    target_->DrawLine(D2D1::Point2F(c.x - 4.5f, c.y + 7.2f), D2D1::Point2F(c.x - 0.8f, body.bottom),
                      accent_brush_.Get(), sw, st);
    target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 3.4f), D2D1::Point2F(c.x + 3.8f, c.y - 3.4f), accent_brush_.Get(),
                      1.2f, st);
    target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 0.2f), D2D1::Point2F(c.x + 1.4f, c.y - 0.2f), accent_brush_.Get(),
                      1.2f, st);
    accent_brush_->SetOpacity(1.0f);
}

void Renderer::ensure_immersive_format(const Settings& settings)
{
    const float ts = clamp(settings.text_scale, 0.7f, 1.6f);
    if (imm_format_ && std::fabs(imm_fmt_scale_ - ts) < 0.001f && imm_fmt_family_ == settings.font_family)
    {
        return;
    }
    imm_format_.Reset();
    imm_cache_.clear();
    const wchar_t* family = settings.font_family.empty() ? L"Segoe UI Variable Display" : settings.font_family.c_str();
    HRESULT hr = dwrite_factory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL, 15.0f * ts, L"", &imm_format_);
    if (FAILED(hr) || !imm_format_)
    {
        imm_format_.Reset();
        dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, 15.0f * ts, L"", &imm_format_);
    }
    if (imm_format_)
    {
        imm_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        imm_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    imm_fmt_scale_ = ts;
    imm_fmt_family_ = settings.font_family;
}

void Renderer::draw_immersive_lyrics_view(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                                          double now, float mix)
{
    if (mix <= 0.003f || !text_format_ || !small_text_format_ || !dwrite_factory_)
    {
        return;
    }

    const float width = rect.right - rect.left;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float zone_top = rect.top + immersive_layout::lyrics_top;
    const float zone_bottom = rect.top + immersive_layout::lyrics_bottom;
    const float cy = (zone_top + zone_bottom) * 0.5f;
    const float avail_w = width - immersive_layout::pad_x * 2.0f;

    const bool fresh = imm_last_drawn_ >= 0.0 && (now - imm_last_drawn_) < 0.25;
    const float dt = fresh ? clamp(static_cast<float>(now - imm_last_drawn_), 0.001f, 0.05f) : 0.016f;
    imm_last_drawn_ = now;
    g_last_lyrics_view_time.store(now, std::memory_order_relaxed);

    ID2D1Layer* fade_layer = acquire_layer();
    if (fade_layer)
    {
        target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), clamp(mix, 0.0f, 1.0f), nullptr,
                                                 D2D1_LAYER_OPTIONS_NONE),
                           fade_layer);
    }

    // Accent glow behind the lyrics
    {
        float level = 0.0f;
        if (settings.spectrum_style == SpectrumStyle::Bars)
        {
            level = state.media.playing ? playing_indicator_glow(now) : 0.0f;
        }
        else if (state.media.playing)
        {
            const std::size_t n = state.waveform.size();
            level = clamp(state.waveform[(state.waveform_write + n - 1) % n], 0.0f, 1.0f);
        }
        imm_glow_ += (level - imm_glow_) * (1.0f - std::exp(-dt / 0.12f));

        const D2D1_COLOR_F a = material_.accent;
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(a.r, a.g, a.b, 1.0f)},
            {1.0f, D2D1::ColorF(a.r, a.g, a.b, 0.0f)},
        };
        const D2D1_POINT_2F gc = D2D1::Point2F(cx, cy + 6.0f);
        const float rx = width * 0.46f;
        const float ry = 66.0f;
        if (ID2D1RadialGradientBrush* glow = use_radial_gradient(bloom_grad_, stops, 2, gc, rx, ry))
        {
            const float strength = (0.10f + 0.10f * imm_glow_) * clamp(settings.accent_bloom, 0.0f, 2.0f);
            glow->SetOpacity(clamp(strength, 0.0f, 0.8f) * settings_opacity_);
            target_->FillEllipse(D2D1::Ellipse(gc, rx, ry), glow);
        }
    }

    // One-line header: "Title · Artist" next to the small cover circle
    {
        const float text_left = rect.left + immersive_layout::header_text_left;
        const float text_right = rect.right - immersive_layout::header_right_pad;
        const float mid_y = rect.top + immersive_layout::art_top + immersive_layout::art_size * 0.5f;
        std::wstring head = state.media.title.empty() ? std::wstring(loc(L"Unknown")) : state.media.title;
        if (!state.media.artist.empty())
        {
            head += L"  \u00b7  " + state.media.artist;
        }
        text_brush_->SetOpacity(0.70f);
        draw_marquee_text(head, D2D1::RectF(text_left, mid_y - 8.0f, text_right, mid_y + 8.0f),
                          small_text_format_.Get(), text_brush_.Get(), now, 30.0f, marquee_immersive_cache_);
    }

    // Lyrics
    target_->PushAxisAlignedClip(D2D1::RectF(rect.left + 18.0f, zone_top, rect.right - 18.0f, zone_bottom),
                                 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const auto& lines = state.lyrics.lines;
    const bool usable = state.lyrics.has_data && state.lyrics.synced && !lines.empty() &&
                        state.lyrics.matched_title == state.media.title &&
                        state.lyrics.matched_artist == state.media.artist;

    if (!usable)
    {
        imm_cache_.clear();
        imm_track_key_.clear();
        const wchar_t* msg = state.lyrics.fetching
                                 ? loc(L"Fetching lyrics...")
                                 : (state.lyrics.has_data ? loc(L"No synced lyrics") : loc(L"No lyrics found"));
        const float opacity =
            state.lyrics.fetching ? 0.50f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6)) : 0.60f;
        DWRITE_TEXT_METRICS tm{};
        if (auto layout = make_line_layout(msg, text_format_.Get(), avail_w, &tm, true))
        {
            muted_brush_->SetOpacity(opacity);
            target_->DrawTextLayout(D2D1::Point2F(cx - tm.width * 0.5f, cy - tm.height * 0.5f), layout.Get(),
                                    muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
    else
    {
        ensure_immersive_format(settings);

        double pos_sec = state.media.position_ticks / 10000000.0;
        if (state.media.playing && state.media.last_updated_ticks > 0)
        {
            pos_sec += (GetTickCount64() - state.media.last_updated_ticks) / 1000.0;
        }
        if (state.media.playing)
        {
            pos_sec += lyrics_lookahead_sec();
        }
        const std::int64_t pos_ms = std::max<std::int64_t>(0, static_cast<std::int64_t>(pos_sec * 1000.0));

        int lo = 0;
        int hi = static_cast<int>(lines.size());
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (lines[mid].time_ms <= pos_ms)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        const int idx = lo - 1; // -1 = intro, before the first line

        // Spring-eased scroll toward the active line.
        const std::wstring track_key = state.lyrics.matched_artist + L"|" + state.lyrics.matched_title;
        if (!fresh || track_key != imm_track_key_)
        {
            imm_track_key_ = track_key;
            imm_cache_.clear();
            imm_scroll_ = static_cast<float>(idx);
            imm_vel_ = 0.0f;
        }
        const float target = static_cast<float>(idx);
        if (std::fabs(imm_scroll_ - target) > 8.0f)
        { // big seek: don't scroll through every line
            imm_scroll_ = target;
            imm_vel_ = 0.0f;
        }
        float remaining = dt;
        while (remaining > 0.0f)
        {
            const float h = std::min(remaining, 0.004f);
            imm_vel_ += (-immersive_layout::spring_stiffness * (imm_scroll_ - target) -
                         immersive_layout::spring_damping * imm_vel_) *
                        h;
            imm_scroll_ += imm_vel_ * h;
            remaining -= h;
        }
        if (std::fabs(imm_scroll_ - target) < 0.0015f && std::fabs(imm_vel_) < 0.01f)
        {
            imm_scroll_ = target;
            imm_vel_ = 0.0f;
        }

        auto line_for = [&](int i) -> CollapsedLyricLine&
        {
            auto it = imm_cache_.find(i);
            if (it == imm_cache_.end())
            {
                if (imm_cache_.size() > 16)
                {
                    imm_cache_.clear();
                }
                CollapsedLyricLine built;
                const std::wstring text = (i >= 0 && i < static_cast<int>(lines.size()) && !lines[i].text.empty())
                                              ? lines[i].text
                                              : std::wstring(L"\u266A");
                build_collapsed_lyric_line(built, text, imm_format_.Get());
                it = imm_cache_.emplace(i, std::move(built)).first;
            }
            return it->second;
        };

        const float neighbour = settings.lyrics_neighbor_lines_visible ? 0.62f : 0.40f;
        auto alpha_for = [&](float ad)
        {
            if (ad <= 1.0f)
            {
                return 1.0f + (neighbour - 1.0f) * smooth_step01(ad);
            }
            return neighbour * (1.0f - smooth_step01((ad - 1.0f) / 0.45f));
        };
        auto offset_for = [&](float d)
        {
            const float ad = std::fabs(d);
            const float y = ad <= 1.0f ? immersive_layout::line_pitch * ad
                                       : immersive_layout::line_pitch + immersive_layout::outer_pitch * (ad - 1.0f);
            return d < 0.0f ? -y : y;
        };

        const D2D1_COLOR_F tc = text_brush_->GetColor();
        const D2D1_GRADIENT_STOP wipe_stops[2] = {
            {0.0f, D2D1::ColorF(tc.r, tc.g, tc.b, 1.0f)},
            {1.0f, D2D1::ColorF(tc.r, tc.g, tc.b, 0.0f)},
        };

        D2D1_MATRIX_3X2_F old_transform;
        target_->GetTransform(&old_transform);

        const int first = static_cast<int>(std::floor(imm_scroll_)) - 3;
        for (int i = first; i <= first + 7; ++i)
        {
            if (i < -1 || i >= static_cast<int>(lines.size()))
            {
                continue;
            }
            const float d = static_cast<float>(i) - imm_scroll_;
            const float ad = std::fabs(d);
            if (ad > 1.6f)
            {
                continue;
            }
            const float alpha = alpha_for(ad);
            if (alpha <= 0.01f)
            {
                continue;
            }

            CollapsedLyricLine& l = line_for(i);
            if (!l.layout || l.width <= 0.0f)
            {
                continue;
            }

            const float focus = 1.0f - smooth_step01(clamp(ad, 0.0f, 1.0f));
            float s = 0.80f + 0.28f * focus;
            if (l.width * s > avail_w)
            {
                s = std::max(avail_w / l.width, s * 0.62f);
            }
            const float yc = cy + offset_for(d);
            const D2D1_POINT_2F origin = D2D1::Point2F(cx - l.width * 0.5f, yc - l.height * 0.5f);

            target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(cx, yc)) * old_transform);

            if (settings.karaoke_lyrics && i == idx && idx >= 0)
            {
                muted_brush_->SetOpacity(alpha * (0.62f + 0.38f * (1.0f - focus)));
                target_->DrawTextLayout(origin, l.layout.Get(), muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

                std::int64_t start_ms = lines[i].time_ms;
                std::int64_t end_ms = start_ms + 4000;
                if (i + 1 < static_cast<int>(lines.size()))
                {
                    end_ms = lines[i + 1].time_ms;
                }
                else if (state.media.end_ticks > 0)
                {
                    end_ms = state.media.end_ticks / 10000;
                }
                const std::int64_t span = end_ms - start_ms;
                const float p =
                    span > 0 ? clamp(static_cast<float>(pos_ms - start_ms) / static_cast<float>(span), 0.0f, 1.0f)
                             : 1.0f;
                const float wipe_x = origin.x - 8.0f + (l.width + 16.0f) * p;
                if (focus > 0.01f)
                {
                    if (ID2D1LinearGradientBrush* wipe =
                            use_linear_gradient(imm_karaoke_grad_, wipe_stops, 2, D2D1::Point2F(wipe_x - 8.0f, yc),
                                                D2D1::Point2F(wipe_x + 8.0f, yc)))
                    {
                        wipe->SetOpacity(alpha * focus);
                        target_->DrawTextLayout(origin, l.layout.Get(), wipe, D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                }
            }
            else
            {
                if (focus < 0.999f)
                {
                    muted_brush_->SetOpacity(alpha * (1.0f - focus));
                    target_->DrawTextLayout(origin, l.layout.Get(), muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
                if (focus > 0.001f)
                {
                    text_brush_->SetOpacity(alpha * focus);
                    target_->DrawTextLayout(origin, l.layout.Get(), text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
            }
        }
        target_->SetTransform(old_transform);
    }

    target_->PopAxisAlignedClip();
    if (fade_layer)
    {
        target_->PopLayer();
    }
    text_brush_->SetOpacity(0.96f);
    muted_brush_->SetOpacity(0.75f);
}

void Renderer::build_side_lyric_line(CollapsedLyricLine& out, const std::wstring& text, float width, float max_height,
                                     const Settings& settings)
{
    out.text = text;
    out.layout.Reset();
    out.width = 0.0f;
    out.height = 0.0f;
    out.is_title = false;
    if (!dwrite_factory_ || text.empty())
    {
        return;
    }

    const wchar_t* family = settings.font_family.empty() ? L"Segoe UI Variable Display" : settings.font_family.c_str();
    const float ts = clamp(settings.text_scale, 0.7f, 1.6f);
    const float min_size = 10.0f * ts;
    float size = 16.0f * ts;

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        ComPtr<IDWriteTextFormat> fmt;
        HRESULT hr =
            dwrite_factory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
        if (FAILED(hr) || !fmt)
        {
            fmt.Reset();
            hr = dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                                   &fmt);
            if (FAILED(hr) || !fmt)
            {
                return;
            }
        }
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(), width,
                                                     4096.0f, &layout)) ||
            !layout)
        {
            return;
        }
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        out.layout = layout;
        out.width = tm.width;
        out.height = tm.height;

        if (tm.height <= max_height || size <= min_size)
        {
            break;
        }
        size = std::max(min_size, size - 1.5f * ts);
    }
}

// Lyrics side panel for the expanded media tab (the strip right of the player).
void Renderer::draw_media_side_lyrics(const SharedState& state, D2D1_RECT_F area, const Settings& settings, double now)
{
    const float area_w = area.right - area.left;
    if (area_w < 8.0f || !text_format_ || !small_text_format_ || !dwrite_factory_)
    {
        return;
    }

    const float reveal = smooth_step01((area_w - 6.0f) / (media_layout::side_lyrics_width * 0.55f));
    if (reveal <= 0.01f)
    {
        return;
    }

    constexpr float lyrics_pad_x = 18.0f;
    constexpr float slide_px = 12.0f;
    constexpr double anim_sec = 0.30;
    const float text_left = area.left + lyrics_pad_x;
    const float text_w = media_layout::side_lyrics_width - lyrics_pad_x * 2.0f;

    const bool fresh = side_lyric_last_drawn_ >= 0.0 && (now - side_lyric_last_drawn_) < 0.25;
    side_lyric_last_drawn_ = now;

    ID2D1Layer* fade_layer = acquire_layer();
    if (fade_layer)
    {
        target_->PushLayer(D2D1::LayerParameters(area, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), reveal, nullptr, D2D1_LAYER_OPTIONS_NONE),
                           fade_layer);
    }
    else
    {
        target_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    if (ID2D1SolidColorBrush* divider =
            acquire_brush(with_alpha(material_.hairline, material_.hairline.a * settings_opacity_)))
    {
        target_->FillRectangle(D2D1::RectF(area.left, area.top + 24.0f, area.left + 1.0f, area.bottom - 24.0f),
                               divider);
    }

    const auto& lines = state.lyrics.lines;
    const bool usable = state.lyrics.has_data && state.lyrics.synced && !lines.empty() &&
                        state.lyrics.matched_title == state.media.title &&
                        state.lyrics.matched_artist == state.media.artist;

    if (!usable)
    {
        side_lyric_idx_ = -2;
        side_lyric_cur_ = CollapsedLyricLine{};
        side_lyric_prev_ = CollapsedLyricLine{};

        const wchar_t* msg = state.lyrics.fetching ? loc(L"Fetching lyrics...") : loc(L"No lyrics found");
        float opacity = 0.60f;
        if (state.lyrics.fetching)
        {
            opacity = 0.50f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6));
        }
        DWRITE_TEXT_METRICS tm{};
        if (auto layout = make_line_layout(msg, text_format_.Get(), text_w, &tm, true))
        {
            muted_brush_->SetOpacity(opacity);
            target_->DrawTextLayout(D2D1::Point2F(text_left, (area.top + area.bottom) * 0.5f - tm.height * 0.5f),
                                    layout.Get(), muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
    else
    {
        double pos_sec = state.media.position_ticks / 10000000.0;
        if (state.media.playing && state.media.last_updated_ticks > 0)
        {
            pos_sec += (GetTickCount64() - state.media.last_updated_ticks) / 1000.0;
        }
        if (state.media.playing)
        {
            pos_sec += lyrics_lookahead_sec();
        }
        const std::int64_t pos_ms = std::max<std::int64_t>(0, static_cast<std::int64_t>(pos_sec * 1000.0));

        int lo = 0;
        int hi = static_cast<int>(lines.size());
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (lines[mid].time_ms <= pos_ms)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        const int idx = lo - 1; // -1 = before the first line

        const float mid_top = area.top + 46.0f;
        const float mid_h = std::max(20.0f, (area.bottom - 46.0f) - mid_top);

        const bool style_changed =
            std::fabs(side_lyric_scale_ - settings.text_scale) > 0.001f || side_lyric_family_ != settings.font_family;
        const bool track_changed = state.media.title != side_lyric_title_ || state.media.artist != side_lyric_artist_;

        if (idx != side_lyric_idx_ || style_changed || track_changed || !side_lyric_cur_.layout)
        {
            if (style_changed || track_changed || !fresh || side_lyric_idx_ == -2)
            {
                side_lyric_prev_ = CollapsedLyricLine{};
                side_lyric_anim_start_ = now - 10.0; // snap, no animation
            }
            else
            {
                side_lyric_prev_ = std::move(side_lyric_cur_);
                side_lyric_anim_start_ = now;
            }
            side_lyric_idx_ = idx;
            side_lyric_title_ = state.media.title;
            side_lyric_artist_ = state.media.artist;
            side_lyric_scale_ = settings.text_scale;
            side_lyric_family_ = settings.font_family;

            const std::wstring text =
                (idx >= 0 && !lines[idx].text.empty()) ? lines[idx].text : std::wstring(L"\u266A");
            build_side_lyric_line(side_lyric_cur_, text, text_w, mid_h, settings);
        }

        const float t = clamp(static_cast<float>((now - side_lyric_anim_start_) / anim_sec), 0.0f, 1.0f);
        if (t < 1.0f)
        {
            g_layout_dirty = true;
        }
        const float inv = 1.0f - t;
        const float e = 1.0f - inv * inv * inv;

        auto draw_dim = [&](int line_idx, float center_y)
        {
            if (line_idx < 0 || line_idx >= static_cast<int>(lines.size()))
            {
                return;
            }
            const std::wstring& s = lines[line_idx].text;
            if (s.empty())
            {
                return;
            }
            DWRITE_TEXT_METRICS tm{};
            if (auto layout = make_line_layout(s, small_text_format_.Get(), text_w, &tm, true))
            {
                muted_brush_->SetOpacity(0.50f * e);
                target_->DrawTextLayout(D2D1::Point2F(text_left, center_y - tm.height * 0.5f), layout.Get(),
                                        muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        };
        draw_dim(idx - 1, area.top + 26.0f);
        draw_dim(idx + 1, area.bottom - 26.0f);

        target_->PushAxisAlignedClip(D2D1::RectF(area.left, area.top + 40.0f, area.right, area.bottom - 40.0f),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (t < 1.0f && side_lyric_prev_.layout)
        {
            const CollapsedLyricLine& p = side_lyric_prev_;
            const float y = mid_top + std::max(0.0f, (mid_h - p.height) * 0.5f) - e * slide_px;
            text_brush_->SetOpacity(0.90f * (1.0f - clamp(t * 1.6f, 0.0f, 1.0f)));
            target_->DrawTextLayout(D2D1::Point2F(text_left, y), p.layout.Get(), text_brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        if (side_lyric_cur_.layout)
        {
            const CollapsedLyricLine& c = side_lyric_cur_;
            const float y = mid_top + std::max(0.0f, (mid_h - c.height) * 0.5f) + (1.0f - e) * slide_px;
            text_brush_->SetOpacity(0.97f * e);
            target_->DrawTextLayout(D2D1::Point2F(text_left, y), c.layout.Get(), text_brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        target_->PopAxisAlignedClip();
    }

    if (fade_layer)
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

void Renderer::build_collapsed_lyric_line(CollapsedLyricLine& out, const std::wstring& text, IDWriteTextFormat* fmt)
{
    out.text = text;
    out.layout.Reset();
    out.width = 0.0f;
    out.height = 0.0f;
    if (SUCCEEDED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 4096.0f, 40.0f,
                                                    &out.layout)) &&
        out.layout)
    {
        out.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        out.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS tm{};
        out.layout->GetMetrics(&tm);
        out.width = tm.widthIncludingTrailingWhitespace;
        out.height = tm.height;
    }
}

// Current synced lyric line; on a change the old one slides up and out, the new one rises in.
void Renderer::draw_collapsed_lyrics(const SharedState& state, D2D1_RECT_F area, double now, float alpha)
{
    g_last_lyrics_view_time.store(now, std::memory_order_relaxed);
    IDWriteTextFormat* fmt = text_format_.Get();
    if (!fmt || !dwrite_factory_ || alpha <= 0.01f)
    {
        return;
    }
    const float avail_w = area.right - area.left;
    if (avail_w < 40.0f)
    {
        return;
    }

    const auto& lines = state.lyrics.lines;

    // Lyrics count only when they belong to the track playing right now.
    const bool lyrics_usable = state.lyrics.has_data && state.lyrics.synced && !lines.empty() &&
                               state.lyrics.matched_title == state.media.title &&
                               state.lyrics.matched_artist == state.media.artist;

    int idx = -1; // -1 = before the first line, or no usable lyrics
    std::int64_t pos_ms = 0;
    if (lyrics_usable)
    {
        double pos_sec = state.media.position_ticks / 10000000.0;
        if (state.media.playing && state.media.last_updated_ticks > 0)
        {
            pos_sec += (GetTickCount64() - state.media.last_updated_ticks) / 1000.0;
        }
        if (state.media.playing)
        {
            pos_sec += lyrics_lookahead_sec();
        }
        pos_ms = std::max<std::int64_t>(0, static_cast<std::int64_t>(pos_sec * 1000.0));

        // Binary search for the last line whose timestamp has passed.
        int lo = 0;
        int hi = static_cast<int>(lines.size());
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (lines[mid].time_ms <= pos_ms)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        idx = lo - 1;
    }

    constexpr int title_key = -3; // distinct from idx == -1 (before the first line)
    const bool show_title = !lyrics_usable;
    const int show_idx = show_title ? title_key : idx;

    std::wstring fallback = state.media.title;
    if (fallback.empty())
    {
        fallback = state.media.source_name;
    }
    if (fallback.empty())
    {
        fallback = loc(L"Media");
    }

    const bool track_changed =
        state.media.title != collapsed_lyric_title_ || state.media.artist != collapsed_lyric_artist_;
    const bool fallback_text_changed = show_title && collapsed_lyric_cur_.text != fallback;
    if (track_changed || show_idx != collapsed_lyric_idx_ || fallback_text_changed)
    {
        if (track_changed || collapsed_lyric_idx_ == -2)
        {
            collapsed_lyric_prev_ = CollapsedLyricLine{};
            collapsed_lyric_anim_start_ = now - 10.0;
        }
        else
        {
            collapsed_lyric_prev_ = std::move(collapsed_lyric_cur_);
            collapsed_lyric_anim_start_ = now;
        }
        collapsed_lyric_title_ = state.media.title;
        collapsed_lyric_artist_ = state.media.artist;
        collapsed_lyric_idx_ = show_idx;

        const std::wstring text =
            show_title ? fallback
                       : ((idx >= 0 && !lines[idx].text.empty()) ? lines[idx].text : std::wstring(L"\u266A"));
        build_collapsed_lyric_line(collapsed_lyric_cur_, text, fmt);
        collapsed_lyric_cur_.is_title = show_title;
    }
    if (!collapsed_lyric_cur_.layout)
    {
        return;
    }

    constexpr double anim_sec = 0.30;
    constexpr float slide_px = 13.0f;
    const float t = clamp(static_cast<float>((now - collapsed_lyric_anim_start_) / anim_sec), 0.0f, 1.0f);
    const float inv = 1.0f - t;
    const float e = 1.0f - inv * inv * inv;

    const float cy = (area.top + area.bottom) * 0.5f;
    target_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (t < 1.0f && collapsed_lyric_prev_.layout)
    {
        const CollapsedLyricLine& p = collapsed_lyric_prev_;
        float x = area.left;
        if (p.width <= avail_w)
        {
            x = area.left + (avail_w - p.width) * 0.5f;
        }
        else if (!p.is_title)
        {
            x = area.left - (p.width - avail_w); // lyric rests at its panned end
        }
        const float y = cy - p.height * 0.5f - e * slide_px;
        text_brush_->SetOpacity(alpha * (1.0f - clamp(t * 1.6f, 0.0f, 1.0f)) * 0.9f);
        target_->DrawTextLayout(D2D1::Point2F(x, y), p.layout.Get(), text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    {
        const CollapsedLyricLine& c = collapsed_lyric_cur_;
        float x = area.left + (avail_w - c.width) * 0.5f; // centred when it fits
        const float overflow = c.width - avail_w;
        float title_cycle = 0.0f; // > 0 only when the title is scrolling as a marquee
        if (overflow > 0.0f)
        {
            if (c.is_title)
            {
                title_cycle = c.width + 38.0f;
                x = area.left - std::fmod(static_cast<float>(now) * 30.0f, title_cycle);
            }
            else if (show_idx >= 0)
            {
                const std::int64_t start_ms = lines[show_idx].time_ms;
                std::int64_t end_ms = start_ms + 4000;
                if (show_idx + 1 < static_cast<int>(lines.size()))
                {
                    end_ms = lines[show_idx + 1].time_ms;
                }
                else if (state.media.end_ticks > 0)
                {
                    end_ms = state.media.end_ticks / 10000;
                }
                const std::int64_t span = end_ms - start_ms;
                const float prog =
                    span > 0 ? clamp(static_cast<float>(pos_ms - start_ms) / static_cast<float>(span), 0.0f, 1.0f)
                             : 1.0f;
                x = area.left - overflow * smooth_step01((prog - 0.12f) / 0.70f);
            }
        }
        const float y = cy - c.height * 0.5f + (1.0f - e) * slide_px;
        text_brush_->SetOpacity(alpha * e * (c.is_title ? 0.90f : 0.97f));
        target_->DrawTextLayout(D2D1::Point2F(x, y), c.layout.Get(), text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (title_cycle > 0.0f)
        {
            target_->DrawTextLayout(D2D1::Point2F(x + title_cycle, y), c.layout.Get(), text_brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    target_->PopAxisAlignedClip();
    text_brush_->SetOpacity(0.96f);
}

}
