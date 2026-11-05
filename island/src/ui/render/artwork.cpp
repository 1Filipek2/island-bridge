#include "ui/render/renderer_impl.h"

namespace island
{

// Album art flip. Keyed on a pixel hash: the decoder issues new generations for identical images.
std::uint64_t Renderer::art_content_hash(const MediaSnapshot& media)
{
    if (!media.art)
    {
        return 0;
    }
    if (art_hash_value_ != 0 && art_hash_gen_ == media.art->generation)
    {
        return art_hash_value_;
    }
    std::uint64_t h = 1469598103934665603ull;
    h = (h ^ static_cast<std::uint64_t>(media.art->width)) * 1099511628211ull;
    h = (h ^ static_cast<std::uint64_t>(media.art->height)) * 1099511628211ull;
    const std::vector<std::uint8_t>& v = media.art->bgra;
    for (std::size_t i = 0; i < v.size(); i += 61)
    {
        h = (h ^ v[i]) * 1099511628211ull;
    }
    if (h == 0)
    {
        h = 1;
    }
    art_hash_gen_ = media.art->generation;
    art_hash_value_ = h;
    return h;
}

void Renderer::draw_art_face(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius)
{
    ComPtr<ID2D1RoundedRectangleGeometry> mask;
    HRESULT hr_mask = d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &mask);
    ComPtr<ID2D1Layer> layer;
    HRESULT hr_layer = target_->CreateLayer(nullptr, &layer);
    const bool rounded_clip = SUCCEEDED(hr_mask) && SUCCEEDED(hr_layer) && mask && layer;
    if (rounded_clip)
    {
        target_->PushLayer(D2D1::LayerParameters(rect, mask.Get()), layer.Get());
    }
    else
    {
        target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    if (bitmap)
    {
        target_->DrawBitmap(bitmap, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    else
    {
        accent_brush_->SetOpacity(0.24f);
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accent_brush_.Get());
        accent_brush_->SetOpacity(1.0f);
    }
    if (rounded_clip)
    {
        target_->PopLayer();
    }
    else
    {
        target_->PopAxisAlignedClip();
    }
}

void Renderer::draw_album_art_flip(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius,
                                   bool draw_badge)
{
    constexpr double flip_duration = 0.62;
    const bool recently_drawn = art_flip_last_drawn_ >= 0.0 && (now - art_flip_last_drawn_) < 0.25;
    art_flip_last_drawn_ = now;

    const std::uint64_t hash = art_content_hash(media);
    if (!art_flip_init_ || !recently_drawn)
    {
        art_flip_init_ = true;
        art_flip_hash_ = hash;
        art_flip_start_ = -1.0;
        art_flip_old_.Reset();
    }
    else if (hash != art_flip_hash_)
    {
        if (expanded_anim_ && art_flip_start_ < 0.0 && hash != 0)
        {
            art_flip_old_ = art_bitmap_; // still the previous cover: not rebuilt yet
            art_flip_start_ = now;
        }
        art_flip_hash_ = hash;
    }

    float p = -1.0f;
    if (art_flip_start_ >= 0.0)
    {
        p = static_cast<float>((now - art_flip_start_) / flip_duration);
        if (p >= 1.0f)
        {
            art_flip_start_ = -1.0;
            art_flip_old_.Reset();
            p = -1.0f;
        }
    }
    if (p < 0.0f)
    {
        draw_album_art(media, rect, now, radius, draw_badge);
        return;
    }

    g_layout_dirty = true;
    const float q = skip_ease_in_out(p);
    const float c = std::fabs(std::cos(3.14159265f * q)); // 1 = facing us, 0 = edge-on
    if (c < 0.02f)
    {
        return;
    }

    const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
    D2D1_MATRIX_3X2_F old_transform;
    target_->GetTransform(&old_transform);
    target_->SetTransform(D2D1::Matrix3x2F::Scale(c, 1.0f - 0.05f * (1.0f - c), center) * old_transform);

    if (q < 0.5f)
    {
        draw_art_face(art_flip_old_.Get(), rect, radius);
    }
    else
    {
        draw_album_art(media, rect, now, radius, draw_badge);
    }

    ComPtr<ID2D1SolidColorBrush> shade;
    if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f * (1.0f - c)), &shade)) && shade)
    {
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), shade.Get());
    }
    target_->SetTransform(old_transform);
}

// Collapsed pill album art: a track change plays as a record swap (old disc thrown away, new one drops
// in spinning); the same cover with a new track does a short press and rebound.
void Renderer::ensure_art_bitmap(const MediaSnapshot& media)
{
    if (!media.art)
    {
        return;
    }
    const BitmapPixels& art = *media.art;
    if (art_generation_ != art.generation || !art_bitmap_)
    {
        D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        target_->CreateBitmap(D2D1::SizeU(art.width, art.height), art.bgra.data(), art.width * 4, &props, &art_bitmap_);
        art_generation_ = art.generation;
    }
}

void Renderer::draw_art_disc(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius, float scale, float angle, float lag,
                             float alpha)
{
    if (alpha <= 0.004f || scale <= 0.02f)
    {
        return;
    }
    const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
    D2D1_MATRIX_3X2_F base;
    target_->GetTransform(&base);
    const D2D1_MATRIX_3X2_F scaled = D2D1::Matrix3x2F::Scale(scale, scale, c) * base;
    target_->SetTransform(scaled);

    ComPtr<ID2D1RoundedRectangleGeometry> mask;
    ComPtr<ID2D1Layer> layer;
    const bool clipped =
        SUCCEEDED(d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &mask)) &&
        SUCCEEDED(target_->CreateLayer(nullptr, &layer)) && mask && layer;
    if (clipped)
    {
        target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), clamp(alpha, 0.0f, 1.0f)),
                           layer.Get());
    }
    else
    {
        target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    const float speed = std::min(std::fabs(lag) / 16.0f, 1.0f);
    const int ghosts = (bitmap && speed > 0.05f) ? 3 : 0;
    for (int k = 0; k <= ghosts; ++k)
    {
        const float a = (k == 0) ? 1.0f : 0.30f * speed * (1.0f - 0.30f * static_cast<float>(k - 1));
        target_->SetTransform(D2D1::Matrix3x2F::Rotation(angle - lag * static_cast<float>(k), c) * scaled);
        if (bitmap)
        {
            target_->DrawBitmap(bitmap, rect, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        else
        {
            accent_brush_->SetOpacity(0.24f);
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accent_brush_.Get());
            accent_brush_->SetOpacity(1.0f);
        }
    }

    if (clipped)
    {
        target_->PopLayer();
    }
    else
    {
        target_->PopAxisAlignedClip();
    }
    target_->SetTransform(base);
}

void Renderer::draw_art_landing_fx(D2D1_RECT_F rect, float radius, float ring_p, float sheen_p, float dir)
{
    const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
    const float r0 = std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.5f;

    if (sheen_p > 0.0f && sheen_p < 1.0f)
    {
        const float e = skip_ease_in_out(sheen_p);
        const float pos = (e * 2.0f - 1.0f) * 1.3f * r0;
        const float half = 0.55f * r0;
        const float dx = 0.82f * dir, dy = -0.57f;
        const float peak = 0.34f * std::sin(3.14159265f * sheen_p);
        D2D1_GRADIENT_STOP stops[3] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, peak)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> glint;
        if (SUCCEEDED(target_->CreateGradientStopCollection(stops, 3, &coll)) && coll &&
            SUCCEEDED(target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(c.x + dx * (pos - half), c.y + dy * (pos - half)),
                                                    D2D1::Point2F(c.x + dx * (pos + half), c.y + dy * (pos + half))),
                coll.Get(), &glint)) &&
            glint)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), glint.Get());
        }
    }

    for (int i = 0; i < 2; ++i)
    {
        const float p = (ring_p - 0.16f * static_cast<float>(i)) / (1.0f - 0.16f * static_cast<float>(i));
        if (p <= 0.0f || p >= 1.0f)
        {
            continue;
        }
        const float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
        const float a = (i == 0 ? 0.60f : 0.28f) * (1.0f - p) * (1.0f - p);
        const float rr = r0 + 0.8f + 4.2f * e;
        accent_brush_->SetOpacity(a);
        target_->DrawEllipse(D2D1::Ellipse(c, rr, rr), accent_brush_.Get(), 0.6f + 1.4f * (1.0f - p));
    }
    accent_brush_->SetOpacity(1.0f);
}

void Renderer::draw_album_art_drop(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius)
{
    constexpr double full_dur = 0.95;
    constexpr double pulse_dur = 0.55;
    constexpr double title_wait = 0.9; // how long to wait for a new cover after a title change
    const bool recently_drawn = pill_art_last_drawn_ >= 0.0 && (now - pill_art_last_drawn_) < 0.25;
    pill_art_last_drawn_ = now;

    const std::uint64_t hash = art_content_hash(media);
    if (!pill_art_init_ || !recently_drawn || !pill_cover_anim_)
    {
        pill_art_init_ = true;
        pill_art_hash_ = hash;
        pill_title_ = media.title;
        pill_art_start_ = -1.0;
        pill_title_pending_at_ = -1.0;
        pill_art_old_.Reset();
        pill_art_last_.Reset();
    }
    else
    {
        float dir = 1.0f;
        if (last_skip_click_at_ >= 0.0 && (now - last_skip_click_at_) < 6.0)
        {
            dir = last_skip_dir_;
        }

        if (hash != pill_art_hash_)
        {
            if (pill_art_start_ < 0.0 && hash != 0)
            {
                pill_art_old_ = pill_art_last_;
                pill_art_start_ = now;
                pill_art_pulse_ = false;
                pill_art_dir_ = dir;
                pill_art_full_at_ = now;
                pill_title_pending_at_ = -1.0;
            }
            pill_art_hash_ = hash;
        }

        if (media.title != pill_title_)
        {
            const bool had_title = !pill_title_.empty();
            pill_title_ = media.title;
            if (had_title && !media.title.empty() && pill_art_start_ < 0.0 && (now - pill_art_full_at_) > 1.6)
            {
                pill_title_pending_at_ = now;
            }
        }
        if (pill_title_pending_at_ >= 0.0)
        {
            if (pill_art_start_ >= 0.0)
            {
                pill_title_pending_at_ = -1.0;
            }
            else if (now - pill_title_pending_at_ >= title_wait)
            {
                pill_title_pending_at_ = -1.0;
                pill_art_old_.Reset();
                pill_art_start_ = now;
                pill_art_pulse_ = true;
                pill_art_dir_ = dir;
            }
            else
            {
                g_layout_dirty = true; // keep frames coming while we wait
            }
        }
    }

    ensure_art_bitmap(media);
    ID2D1Bitmap* fresh = !media.art ? nullptr : art_bitmap_.Get();

    float p = -1.0f;
    if (pill_art_start_ >= 0.0)
    {
        p = static_cast<float>((now - pill_art_start_) / (pill_art_pulse_ ? pulse_dur : full_dur));
        if (p >= 1.0f)
        {
            pill_art_start_ = -1.0;
            pill_art_old_.Reset();
            p = -1.0f;
        }
    }

    if (p < 0.0f)
    {
        draw_album_art(media, rect, now, radius, false);
    }
    else
    {
        g_layout_dirty = true;
        const float dir = pill_art_dir_;

        if (!pill_art_pulse_)
        {
            if (pill_art_old_)
            {
                const float u = clamp(p / 0.40f, 0.0f, 1.0f);
                if (u < 1.0f)
                {
                    const float up = std::max(u - 0.058f, 0.0f);
                    const float k = u * u * u, kp = up * up * up;
                    draw_art_disc(pill_art_old_.Get(), rect, radius, 1.0f - 0.26f * k, dir * 150.0f * k,
                                  dir * 150.0f * (k - kp), 1.0f - skip_smooth(u / 0.9f));
                }
            }
            if (p > 0.14f)
            {
                const float u = clamp((p - 0.14f) / 0.86f, 0.0f, 1.0f);
                const float up = std::max(u - 0.027f, 0.0f);
                const float settle = 1.0f - skip_smooth((u - 0.8f) / 0.2f);
                const float scale = 1.0f + 0.30f * std::exp(-5.2f * u) * std::cos(7.5f * u) * settle;
                const float angle = -dir * 220.0f * std::pow(1.0f - u, 3.0f);
                const float angle_prev = -dir * 220.0f * std::pow(1.0f - up, 3.0f);
                draw_art_disc(fresh, rect, radius, scale, angle, angle - angle_prev, skip_smooth(u / 0.28f));
            }
            draw_art_landing_fx(rect, radius, (p - 0.32f) / 0.60f, (p - 0.34f) / 0.46f, dir);
        }
        else
        {
            auto wob = [](float x)
            {
                return std::exp(-5.5f * x) * std::sin(11.0f * x);
            };
            const float w = wob(p);
            const float wp = wob(std::max(p - 0.04f, 0.0f));
            draw_art_disc(fresh, rect, radius, 1.0f - 0.20f * w, dir * 18.0f * w, dir * 18.0f * (w - wp), 1.0f);
            draw_art_landing_fx(rect, radius, (p - 0.08f) / 0.62f, (p - 0.12f) / 0.55f, dir);
        }
    }

    if (fresh)
    {
        pill_art_last_ = art_bitmap_;
    }
    else
    {
        pill_art_last_.Reset();
    }
}

// Audio spectrum styles. draw_spectrum is the single entry point; it detects track changes and hands
// the animation Clock to the selected style through spec_t_ / spec_dir_.
float Renderer::spec_band_range(const SharedState& state, float from, float to) const
{
    // Average of the analyzer bands covering [from, to) in 0..spectrum_bands.
    int a = std::clamp(static_cast<int>(std::floor(from)), 0, spectrum_bands - 1);
    int b = std::clamp(static_cast<int>(std::ceil(to)), a + 1, spectrum_bands);
    float sum = 0.0f;
    for (int i = a; i < b; ++i)
    {
        sum += state.bands[static_cast<std::size_t>(i)];
    }
    return sum / static_cast<float>(b - a);
}

void Renderer::draw_spectrum(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                             bool playing)
{
    constexpr double spec_dur = 1.15;
    const bool recently_drawn = spec_last_drawn_ >= 0.0 && (now - spec_last_drawn_) < 0.25;
    float dt = recently_drawn ? static_cast<float>(now - spec_last_drawn_) : 0.016f;
    dt = clamp(dt, 0.001f, 0.05f);
    spec_last_drawn_ = now;

    const std::size_t key =
        std::hash<std::wstring>{}(state.media.title) * 31u + std::hash<std::wstring>{}(state.media.artist);
    if (!spec_init_ || !recently_drawn)
    {
        spec_init_ = true;
        spec_key_ = key;
        spec_start_ = -1.0;
        if (!recently_drawn)
        {
            std::fill(std::begin(spec_peak_), std::end(spec_peak_), 0.0f);
            std::fill(std::begin(spec_peak_vel_), std::end(spec_peak_vel_), 0.0f);
            spec_bass_slow_ = 0.0f;
            spec_kick_ = 0.0f;
        }
    }
    else if (key != spec_key_ && !state.media.title.empty())
    {
        const bool running = spec_start_ >= 0.0 && (now - spec_start_) < 0.35;
        spec_key_ = key;
        // Title and artist often land a few frames apart: absorb the second one.
        if (!running)
        {
            spec_start_ = now;
            spec_dir_ = 1.0f;
            if (last_skip_click_at_ >= 0.0 && (now - last_skip_click_at_) < 6.0)
            {
                spec_dir_ = last_skip_dir_;
            }
            for (float& v : spec_peak_vel_)
            {
                v = 0.0f;
            }
            for (int i = 0; i < spectrum_bands; ++i)
            {
                spec_peak_[i] *= 0.6f;
            }
        }
    }

    spec_t_ = -1.0f;
    if (spec_start_ >= 0.0)
    {
        const float t = static_cast<float>((now - spec_start_) / spec_dur);
        if (t >= 1.0f)
        {
            spec_start_ = -1.0;
        }
        else
        {
            spec_t_ = t;
            g_layout_dirty = true;
        }
    }

    const float bass = spec_band_range(state, 0.0f, 4.0f);
    spec_bass_slow_ += (bass - spec_bass_slow_) * (1.0f - std::exp(-dt / 0.45f));
    const float beat = std::max(0.0f, bass - spec_bass_slow_ - 0.04f);
    spec_kick_ = std::max(spec_kick_ * std::exp(-dt / 0.18f), clamp(beat * 3.2f, 0.0f, 1.0f));
    spec_bass_ += (bass - spec_bass_) * (1.0f - std::exp(-dt / 0.07f));
    spec_phase_ += dt * (1.6f + 5.0f * spec_bass_ + 8.0f * spec_kick_);

    switch (settings.spectrum_style)
    {
        case SpectrumStyle::Orb:
            draw_spectrum_orb(state, rect);
            break;
        case SpectrumStyle::Plasma:
            draw_spectrum_plasma(state, rect);
            break;
        case SpectrumStyle::Led:
            draw_spectrum_led(state, rect, dt);
            break;
        default:
            draw_playing_indicator(rect, playing, now);
            break;
    }
}

// Pulse Orb
void Renderer::draw_spectrum_orb(const SharedState& state, D2D1_RECT_F rect)
{
    const float w = rect.right - rect.left, h = rect.bottom - rect.top;
    const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
    const float r = std::min(w, h) * 0.5f + 1.0f; // outer reach
    const float r0 = r * 0.40f;                   // core radius at rest
    const int spoke_count = r < 14.0f ? 16 : 24;
    const int half = spoke_count / 2;
    constexpr float two_pi = 6.28318531f;

    const float t = spec_t_;
    // Spokes are fixed (bass at the bottom, mirrored); only the track-change burst spins the orb.
    float len_scale = 1.0f, core_scale = 1.0f, ring_shrink = 0.0f, spin = 0.0f;
    float burst_alpha = 0.0f, burst_r = 0.0f;
    if (t >= 0.0f)
    {
        if (t < 0.28f)
        {
            const float u = skip_smooth(t / 0.28f);
            len_scale = 1.0f - u;
            core_scale = 1.0f - 0.35f * u;
            ring_shrink = 0.30f * u;
        }
        else
        {
            const float u = clamp((t - 0.28f) / 0.72f, 0.0f, 1.0f);
            const float spring = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
            len_scale = spring;
            core_scale = 1.0f + 0.55f * std::exp(-7.0f * u) * std::cos(6.0f * u);
            ring_shrink = 0.30f * std::exp(-9.0f * u);
            spin += spec_dir_ * 4.2f * std::pow(1.0f - u, 3.0f);
            burst_r = r0 + (r + 3.0f - r0) * skip_ease_in_out(u / 0.8f);
            burst_alpha = 0.75f * (1.0f - clamp(u / 0.85f, 0.0f, 1.0f)) * (1.0f - clamp(u / 0.85f, 0.0f, 1.0f));
        }
    }

    const float base_r = r0 * (1.0f - ring_shrink);
    const float bass_pulse = 0.18f * spec_bass_ + 0.22f * spec_kick_;

    ensure_round_join_style();
    const float stroke_w = clamp(two_pi * (base_r + 1.0f) / static_cast<float>(spoke_count) * 0.62f, 1.0f, r * 0.16f);

    accent_brush_->SetOpacity(0.25f + 0.35f * spec_bass_ + 0.25f * spec_kick_);
    const float shell_r = base_r * (0.90f + 0.18f * bass_pulse);
    target_->DrawEllipse(D2D1::Ellipse(c, shell_r, shell_r), accent_brush_.Get(), 0.8f);

    for (int k = 0; k < spoke_count; ++k)
    {
        const int m = k < half ? k : spoke_count - 1 - k;
        const float lo = static_cast<float>(m) * spectrum_bands / half;
        float v = spec_band_range(state, lo, lo + static_cast<float>(spectrum_bands) / half);
        v = clamp(v * 1.1f, 0.04f, 1.0f);
        float ls = len_scale;
        if (t >= 0.28f)
        {
            const float u = clamp((t - 0.28f - 0.012f * m) / 0.72f, 0.0f, 1.0f);
            ls = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
        }
        const float len = (r - base_r - 1.5f) * (0.10f + 0.90f * v) * clamp(ls, 0.0f, 1.45f);
        const float a = spin + 1.5707963f + two_pi * (static_cast<float>(k) + 0.5f) / spoke_count;
        const float ca = std::cos(a), sa = std::sin(a);
        const float l = std::max(len, 0.6f);
        const D2D1_POINT_2F p0 = D2D1::Point2F(c.x + ca * (base_r + 1.0f), c.y + sa * (base_r + 1.0f));
        const D2D1_POINT_2F p1 = D2D1::Point2F(c.x + ca * (base_r + 1.0f + l), c.y + sa * (base_r + 1.0f + l));
        accent_brush_->SetOpacity(0.10f + 0.14f * v);
        target_->DrawLine(p0, p1, accent_brush_.Get(), stroke_w * 2.0f, round_join_style_.Get());
        accent_brush_->SetOpacity(0.45f + 0.55f * v);
        target_->DrawLine(p0, p1, accent_brush_.Get(), stroke_w, round_join_style_.Get());
    }

    accent_brush_->SetOpacity(0.95f);
    const float core_r = std::max(0.8f, base_r * 0.58f * core_scale * (1.0f + bass_pulse));
    target_->FillEllipse(D2D1::Ellipse(c, core_r, core_r), accent_brush_.Get());
    ComPtr<ID2D1SolidColorBrush> hot;
    if (SUCCEEDED(target_->CreateSolidColorBrush(mix_color(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.65f), &hot)) &&
        hot)
    {
        hot->SetOpacity(0.55f + 0.35f * spec_kick_);
        target_->FillEllipse(D2D1::Ellipse(c, core_r * 0.45f, core_r * 0.45f), hot.Get());
    }

    if (burst_alpha > 0.01f)
    {
        accent_brush_->SetOpacity(burst_alpha);
        target_->DrawEllipse(D2D1::Ellipse(c, burst_r, burst_r), accent_brush_.Get(), 0.8f + 1.4f * burst_alpha);
    }
    accent_brush_->SetOpacity(1.0f);
}

// Plasma Thread
void Renderer::draw_spectrum_plasma(const SharedState& state, D2D1_RECT_F rect)
{
    constexpr int arc_points = 34;
    const float w = rect.right - rect.left, h = rect.bottom - rect.top;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float max_a = h * 0.46f;
    const float t = spec_t_;
    const float dir = spec_dir_;

    ensure_round_join_style();
    ComPtr<ID2D1SolidColorBrush> hot;
    target_->CreateSolidColorBrush(mix_color(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.6f), &hot);

    const float front = t < 0.15f ? -0.4f : -0.4f + 1.9f * skip_ease_in_out((t - 0.15f) / 0.78f);

    for (int strand = 0; strand < 2; ++strand)
    {
        D2D1_POINT_2F pts[arc_points];
        for (int i = 0; i < arc_points; ++i)
        {
            const float xn = static_cast<float>(i) / (arc_points - 1);
            const float xd = dir >= 0.0f ? xn : 1.0f - xn; // distance along the sweep
            // max(): sin(pi) is slightly negative in float and pow(neg, 0.7) is NaN.
            float env = std::pow(std::max(0.0f, std::sin(3.14159265f * xn)), 0.7f);
            float band = spec_band_range(state, xn * (spectrum_bands - 1), xn * (spectrum_bands - 1) + 1.5f);
            band = clamp(0.10f + 0.95f * band + 0.25f * spec_kick_, 0.0f, 1.25f);

            float ignite = 1.0f, boost = 0.0f;
            if (t >= 0.0f)
            {
                if (t < 0.15f)
                {
                    ignite = 1.0f - skip_smooth(t / 0.15f);
                }
                else
                {
                    ignite = skip_smooth((front - xd) / 0.22f + 0.15f);
                    const float g = (xd - front) / 0.16f;
                    boost = 0.95f * std::exp(-g * g);
                }
            }
            const float ph = spec_phase_ + (strand ? 3.14159265f : 0.0f);
            const float wave =
                std::sin(xn * (7.0f + 4.0f * band) - ph) * 0.65f + std::sin(xn * 13.0f + ph * 0.6f) * 0.35f;
            const float a = max_a * env * (band * ignite + boost * env);
            pts[i] = D2D1::Point2F(rect.left + xn * w, cy + wave * a * (strand ? -1.0f : 1.0f));
        }
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(d2d_factory_->CreatePathGeometry(&path)) || !path)
        {
            continue;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink)) || !sink)
        {
            continue;
        }
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLines(pts + 1, arc_points - 1);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close()))
        {
            continue;
        }

        accent_brush_->SetOpacity(strand ? 0.16f : 0.22f);
        target_->DrawGeometry(path.Get(), accent_brush_.Get(), 4.2f, round_join_style_.Get());
        accent_brush_->SetOpacity(strand ? 0.62f : 0.95f);
        target_->DrawGeometry(path.Get(), accent_brush_.Get(), strand ? 1.1f : 1.6f, round_join_style_.Get());
        if (hot && !strand)
        {
            hot->SetOpacity(0.35f + 0.4f * spec_kick_);
            target_->DrawGeometry(path.Get(), hot.Get(), 0.6f, round_join_style_.Get());
        }
    }

    if (t >= 0.15f && hot)
    {
        const float xd = clamp(front, 0.0f, 1.0f);
        const float xn = dir >= 0.0f ? xd : 1.0f - xd;
        const float a = 1.0f - clamp((t - 0.80f) / 0.2f, 0.0f, 1.0f);
        if (front > -0.1f && front < 1.15f)
        {
            hot->SetOpacity(0.85f * a);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 2.2f, 2.2f), hot.Get());
            accent_brush_->SetOpacity(0.28f * a);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 5.5f, 5.5f), accent_brush_.Get());
        }
    }
    accent_brush_->SetOpacity(1.0f);
}

// Peak Matrix: LED cells per column with gravity-driven falling peak caps.
void Renderer::draw_spectrum_led(const SharedState& state, D2D1_RECT_F rect, float dt)
{
    const float w = rect.right - rect.left, h = rect.bottom - rect.top;
    const float col_gap = 1.8f, col_w = 2.6f;
    const int cols = std::clamp(static_cast<int>((w + col_gap) / (col_w + col_gap)), 3, 14);
    const float cw = (w - col_gap * (cols - 1)) / cols;
    const int rows = std::clamp(static_cast<int>(h / 3.6f), 4, 8);
    const float cell_gap = 1.0f;
    const float ch = (h - cell_gap * (rows - 1)) / rows;
    const float t = spec_t_;
    const float dir = spec_dir_;

    ComPtr<ID2D1SolidColorBrush> hot;
    target_->CreateSolidColorBrush(mix_color(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.7f), &hot);

    const float travel = static_cast<float>(cols) + 4.0f;
    const float scan = t < 0.0f ? -10.0f : -1.5f + travel * skip_ease_in_out(t / 0.82f);

    for (int c = 0; c < cols; ++c)
    {
        const int slot = dir >= 0.0f ? c : cols - 1 - c; // order the scan reaches this column
        const std::size_t wf_size = state.waveform.size();
        const std::size_t wf_offset = static_cast<std::size_t>(cols - c) * 3;
        const std::size_t wf_src = (state.waveform_write + wf_size * 2 - wf_offset) % wf_size;
        float v = clamp(state.waveform[wf_src], 0.03f, 1.0f);

        float& pk = spec_peak_[c];
        float& pv = spec_peak_vel_[c];
        if (v >= pk)
        {
            pk = v;
            pv = 0.0f;
        }
        else
        {
            pv += 2.4f * dt;
            pk = std::max(v, pk - pv * dt);
        }

        float col_alpha = 1.0f, flash = 0.0f;
        if (t >= 0.0f)
        {
            const float d = scan - static_cast<float>(slot); // >0: scanline already passed
            if (d < 0.0f)
            {
                col_alpha = 0.35f;
            }
            else
            {
                flash = clamp(1.0f - d / 3.2f, 0.0f, 1.0f);
                col_alpha = 0.35f + 0.65f * skip_smooth(d / 1.2f);
                if (d < 1.6f)
                {
                    pk = std::min(pk, 0.0f + v);
                }
            }
        }

        const float x = rect.left + c * (cw + col_gap);
        const int lit = static_cast<int>(std::round(v * rows));
        const int cap = std::clamp(static_cast<int>(std::round(pk * rows)) - 1, 0, rows - 1);
        for (int r = 0; r < rows; ++r)
        {
            const float yb = rect.bottom - r * (ch + cell_gap);
            D2D1_RECT_F cell = D2D1::RectF(x, yb - ch, x + cw, yb);
            const float rr = std::min(cw, ch) * 0.35f;
            const float topness = rows > 1 ? static_cast<float>(r) / (rows - 1) : 0.0f;
            if (flash > 0.0f)
            {
                ID2D1SolidColorBrush* b = (hot && flash > 0.7f && r >= rows / 2) ? hot.Get() : accent_brush_.Get();
                b->SetOpacity(clamp(0.25f + 0.75f * flash, 0.0f, 1.0f) * (0.6f + 0.4f * (1.0f - topness)));
                target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), b);
            }
            else if (r < lit)
            {
                accent_brush_->SetOpacity((0.50f + 0.45f * topness) * col_alpha);
                target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accent_brush_.Get());
            }
            else if (r == cap && cap >= lit && pk > 0.05f && hot)
            {
                hot->SetOpacity(0.9f * col_alpha);
                target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), hot.Get());
            }
            else
            {
                accent_brush_->SetOpacity(0.09f * col_alpha);
                target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accent_brush_.Get());
            }
        }
    }
    accent_brush_->SetOpacity(1.0f);
}

void Renderer::draw_album_art(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius, bool draw_badge)
{
    ComPtr<ID2D1Geometry> mask = get_shape_geometry(rect, radius, false);
    ID2D1Layer* layer = acquire_layer();
    const bool rounded_clip = mask && layer;
    if (rounded_clip)
    {
        target_->PushLayer(D2D1::LayerParameters(rect, mask.Get()), layer);
    }
    else
    {
        target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    if (media.art)
    {
        ensure_art_bitmap(media);

        D2D1_RECT_F dst = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
        target_->DrawBitmap(art_bitmap_.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    else
    {
        accent_brush_->SetOpacity(0.24f);
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accent_brush_.Get());
        accent_brush_->SetOpacity(1.0f);
        if (media.source_icon)
        {
            D2D1_RECT_F icon_rect = D2D1::RectF(rect.left + 11, rect.top + 11, rect.right - 11, rect.bottom - 11);
            draw_bitmap_pixels(*media.source_icon, icon_rect, media_source_icon_bitmap_, media_source_icon_generation_,
                               0.95f);
        }
        else
        {
            target_->DrawTextW(media.source_badge.empty() ? L"\u25b6" : media.source_badge.c_str(),
                               static_cast<UINT32>(media.source_badge.empty() ? 1 : media.source_badge.size()),
                               text_format_.Get(), rect, text_brush_.Get());
        }
    }

    if (draw_badge && media.source_icon && art_badge_alpha_ > 0.01f)
    {
        D2D1_RECT_F badge = D2D1::RectF(rect.right - 24, rect.bottom - 22, rect.right - 3, rect.bottom - 3);
        draw_circular_bitmap_pixels(
            *media.source_icon, D2D1::Point2F((badge.left + badge.right) * 0.5f, (badge.top + badge.bottom) * 0.5f),
            9.5f, media_source_icon_bitmap_, media_source_icon_generation_, 0.98f * art_badge_alpha_);
    }

    if (rounded_clip)
    {
        target_->PopLayer();
    }
    else
    {
        target_->PopAxisAlignedClip();
    }
}

void Renderer::draw_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F rect, ComPtr<ID2D1Bitmap>& cache,
                                  std::uint64_t& cached_generation, float opacity)
{
    if (pixels.bgra.empty())
    {
        return;
    }

    if (cached_generation != pixels.generation || !cache)
    {
        D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        target_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height), pixels.bgra.data(), pixels.width * 4, &props,
                              &cache);
        cached_generation = pixels.generation;
    }

    if (cache)
    {
        target_->DrawBitmap(cache.Get(), rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

void Renderer::draw_rounded_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F badge, float corner_radius,
                                          ComPtr<ID2D1Bitmap>& cache, std::uint64_t& cached_generation, float opacity)
{
    if (pixels.bgra.empty())
    {
        return;
    }

    const float pad = 2.0f;
    D2D1_RECT_F icon_rect = D2D1::RectF(badge.left + pad, badge.top + pad, badge.right - pad, badge.bottom - pad);
    const float inner_r = std::max(0.0f, corner_radius - pad);

    ComPtr<ID2D1RoundedRectangleGeometry> mask;
    d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(icon_rect, inner_r, inner_r), &mask);
    ComPtr<ID2D1Layer> layer;
    target_->CreateLayer(nullptr, &layer);

    if (mask && layer)
    {
        target_->PushLayer(D2D1::LayerParameters(icon_rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           layer.Get());
        draw_bitmap_pixels(pixels, icon_rect, cache, cached_generation, opacity);
        target_->PopLayer();
    }
    else
    {
        draw_bitmap_pixels(pixels, icon_rect, cache, cached_generation, opacity);
    }
}

// Like draw_rounded_bitmap_pixels, but cover-fit (center crop) instead of stretching.
void Renderer::draw_cover_fit_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F badge, float corner_radius,
                                            ComPtr<ID2D1Bitmap>& cache, std::uint64_t& cached_generation, float opacity)
{
    if (pixels.bgra.empty() || !pixels.width || !pixels.height)
    {
        return;
    }

    const float pad = 2.0f;
    D2D1_RECT_F icon_rect = D2D1::RectF(badge.left + pad, badge.top + pad, badge.right - pad, badge.bottom - pad);
    const float inner_r = std::max(0.0f, corner_radius - pad);
    const float box_w = icon_rect.right - icon_rect.left;
    const float box_h = icon_rect.bottom - icon_rect.top;
    if (box_w <= 0.0f || box_h <= 0.0f)
    {
        return;
    }

    const float src_aspect = static_cast<float>(pixels.width) / static_cast<float>(pixels.height);
    const float box_aspect = box_w / box_h;
    float draw_w = box_w;
    float draw_h = box_h;
    if (src_aspect > box_aspect)
    {
        draw_h = box_h;
        draw_w = box_h * src_aspect;
    }
    else
    {
        draw_w = box_w;
        draw_h = box_w / src_aspect;
    }
    const float cx = (icon_rect.left + icon_rect.right) * 0.5f;
    const float cy = (icon_rect.top + icon_rect.bottom) * 0.5f;
    D2D1_RECT_F draw_rect = D2D1::RectF(cx - draw_w * 0.5f, cy - draw_h * 0.5f, cx + draw_w * 0.5f, cy + draw_h * 0.5f);

    ComPtr<ID2D1RoundedRectangleGeometry> mask;
    d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(icon_rect, inner_r, inner_r), &mask);
    ComPtr<ID2D1Layer> layer;
    target_->CreateLayer(nullptr, &layer);

    if (mask && layer)
    {
        target_->PushLayer(D2D1::LayerParameters(icon_rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           layer.Get());
        draw_bitmap_pixels(pixels, draw_rect, cache, cached_generation, opacity);
        target_->PopLayer();
    }
    else
    {
        draw_bitmap_pixels(pixels, draw_rect, cache, cached_generation, opacity);
    }
}

void Renderer::draw_circular_bitmap_pixels(const BitmapPixels& pixels, D2D1_POINT_2F center, float radius,
                                           ComPtr<ID2D1Bitmap>& cache, std::uint64_t& cached_generation, float opacity)
{
    if (pixels.bgra.empty())
    {
        return;
    }

    D2D1_RECT_F rect = D2D1::RectF(center.x - radius, center.y - radius, center.x + radius, center.y + radius);
    ComPtr<ID2D1EllipseGeometry> ellipse;
    d2d_factory_->CreateEllipseGeometry(D2D1::Ellipse(center, radius, radius), &ellipse);
    ComPtr<ID2D1Layer> layer;
    target_->CreateLayer(nullptr, &layer);

    if (ellipse && layer)
    {
        target_->PushLayer(D2D1::LayerParameters(rect, ellipse.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), layer.Get());
        draw_bitmap_pixels(pixels, rect, cache, cached_generation, opacity);
        target_->PopLayer();
    }
    else
    {
        draw_bitmap_pixels(pixels, rect, cache, cached_generation, opacity);
    }

    ComPtr<ID2D1SolidColorBrush> border;
    target_->CreateSolidColorBrush(material_.hairline, &border);
    if (border)
    {
        target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), border.Get(), 1.0f);
    }
}

}
