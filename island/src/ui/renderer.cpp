#include "ui/renderer.h"
#include "ui/actions.h"
#include "ui/backdrop.h"
#include "core/color.h"
#include "core/constants.h"
#include "ui/file_tray.h"
#include "ui/hit_test.h"
#include "ui/icons.h"
#include "system/input_hooks.h"
#include "core/layout.h"
#include "core/localization.h"
#include "media/media_source.h"
#include "system/monitor.h"
#include "notifications/notification_center.h"
#include "core/pixels.h"
#include "ui/placement.h"
#include "system/privacy.h"
#include "services/quick_lookup.h"
#include "core/state.h"
#include "system/system_monitor.h"
#include "core/tabs.h"
#include "core/util.h"
#include "ui/window.h"

namespace island
{
namespace
{
// Weather icons: vectors, not emoji, so they take the accent color and scale cleanly.
enum class WeatherVisual
{
    Clear,
    PartlyCloudy,
    Cloudy,
    Fog,
    Storm,
    Rain,
    Snow,
    Unknown,
};

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

// draw_karaoke_letter_pop cache: the layout depends only on the line text and box, so it is rebuilt
// only when the line changes.
struct KaraokeLayoutCache
{
    struct ClusterInfo
    {
        UINT32 text_start = 0;
        UINT32 text_length = 0;
        bool is_whitespace = true;
        D2D1_RECT_F rect{};
    };

    std::wstring text;
    IDWriteTextFormat* format = nullptr;
    float layout_width = 0.0f;
    float layout_height = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    std::vector<ClusterInfo> clusters;
    bool valid = false;
};

struct MarqueeLayoutCache
{
    std::wstring text;
    IDWriteTextFormat* format = nullptr;
    float wrap_width = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
};

// Collapsed idle strip geometry from measure_idle_strip: total_width sizes the island, the slot widths
// place the Clock, divider and weather.
struct IdleStripMetrics
{
    float clock_width = 0.0f;
    float weather_width = 0.0f;
    bool has_weather = false;
    bool has_privacy = false;
    bool has_unread = false;
    float total_width = idle_strip_layout::min_width;
};

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
    return idle_strip_layout::k_privacy_reserve +
           (metrics.has_privacy && metrics.has_unread ? notification_center::unread_dot_gap : 0.0f);
}

// One visible character of the song title, used by the title-change animation.
struct TitleGlyph
{
    wchar_t ch0 = 0;      // first UTF-16 unit (used to match letters between titles)
    wchar_t ch1 = 0;      // second unit of a surrogate pair, otherwise 0
    float x = 0.0f;       // left edge inside the full title layout
    float sx = 0.0f;      // on-screen left edge (relative to the title rect) when the animation started
    bool eligible = true; // visible when the animation started (only these get paired)
    float w = 0.0f;
    float h = 0.0f;
    float seed = 0.0f; // stable 0..1 value so each letter's smoke is unique
    int match = -1;    // index of the paired glyph in the other title, or -1
    ComPtr<IDWriteTextLayout> layout;
};

struct TitleScroll
{
    bool scrolls = false;
    float width = 0.0f;
    float cycle = 0.0f;
    float offset0 = 0.0f;
};

// Swap animation of the artist and album lines.
struct SwapTextState
{
    std::wstring shown;  // text currently on screen
    std::wstring prev;   // text that is fading out
    double start = -1.0; // animation start, -1 = idle
    double last_drawn = -1.0;
    MarqueeLayoutCache prev_cache;
};

struct MediaPillMetrics
{
    float clock_width = 0.0f;
    float section_width = 0.0f; // Clock + divider block on the left
    float total_width = media_pill_layout::base_width;
};

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

// Collapsed pill lyrics
constexpr float collapsed_lyrics_width = 320.0f;

// Wide while lyrics are on and media is available, so the pill does not shrink on every track change.
bool collapsed_lyrics_active(const SharedState& state, const Settings& settings)
{
    return settings.collapsed_lyrics && settings.media && state.media.available;
}

// One lyric line, built when the line changes and reused every frame.
struct CollapsedLyricLine
{
    std::wstring text;
    ComPtr<IDWriteTextLayout> layout;
    float width = 0.0f;
    float height = 0.0f;
    bool is_title = false; // true when this is the media-title fallback, not a lyric line
};

class Renderer
{
public:
    bool initialize(HWND hwnd)
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

        D2D1_RENDER_TARGET_PROPERTIES props =
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
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

    bool render(const SharedState& state, const Settings& settings, const Activity& primary,
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
        RECT bounds;
        bounds.left = std::max(0, static_cast<int>(std::floor(left - render_pad_x - 4.0f)));
        bounds.top = std::max(0, static_cast<int>(std::floor(top - render_pad_y - 12.0f)));
        bounds.right = std::min(pixel_width, static_cast<int>(std::ceil(left + width + render_pad_x + 4.0f)));
        bounds.bottom = std::min(pixel_height, static_cast<int>(std::ceil(top + height + render_pad_y + 12.0f)));

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
                const float gap = 12.0f * settings.size_scale;
                const float max_h = std::max(primary.height, secondary->height);
                // Notch and border-merged islands hang from the top edge.
                const bool hang_from_top = settings.notch_style || settings.border_merged_mode;
                const float p_top = hang_from_top ? top : top + (max_h - primary.height) * 0.5f;
                const float s_top = hang_from_top ? top : top + (max_h - secondary->height) * 0.5f;

                draw_pill(state, settings, primary,
                          D2D1::RectF(left, p_top, left + primary.width, p_top + primary.height), scale, now);
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

    void shutdown()
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
    IdleStripMetrics measure_idle_strip(const SharedState& state, const Settings& settings, double now)
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

        float total = idle_strip_layout::k_pad_x * 2.0f + metrics.clock_width;
        if (metrics.has_weather)
        {
            total += idle_strip_layout::slot_gap * 2.0f + idle_strip_layout::divider_width + metrics.weather_width;
        }
        metrics.has_unread = notification_center_unread(settings);
        total += idle_dot_reserve(metrics);

        total = std::ceil(total / idle_strip_layout::width_quantum) * idle_strip_layout::width_quantum;
        metrics.total_width = clamp(total, idle_strip_layout::min_width, idle_strip_layout::k_max_width);
        return metrics;
    }

    // Collapsed media pill with the optional Clock (content space, measured in the widest-digit form).
    MediaPillMetrics measure_media_pill(const SharedState& state, const Settings& settings)
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

private:
    // Keyed on the string: ensure_text_formats invalidates the caches whenever it rebuilds the formats.
    struct TextWidthCache
    {
        std::wstring key;
        float width = 0.0f;
        bool valid = false;
    };

    TextWidthCache idle_clock_width_cache_;
    TextWidthCache idle_weather_width_cache_;

    float measure_text_width_cached(const std::wstring& text, IDWriteTextFormat* fmt, TextWidthCache& cache)
    {
        if (cache.valid && cache.key == text)
        {
            return cache.width;
        }

        float width = 0.0f;
        if (fmt && dwrite_factory_ && !text.empty())
        {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                                            4096.0f, idle_strip_layout::k_height, &layout)) &&
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

    bool create_backing_bitmap(int width, int height)
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

    float last_font_scale_ = 0.0f;
    float last_text_scale_ = 0.0f;
    std::wstring last_font_family_;

    // text_scale changes only the type; size_scale magnifies the whole island through a transform.
    void ensure_text_formats(float scale, const std::wstring& font_family, float text_scale)
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
                if (fallback && wcscmp(family, fallback) != 0)
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
                                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f,
                                                           L"", &media_nav_icon_format_);
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

    void ensure_brushes(const Settings& settings, const SharedState& state, double now)
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

    // Material tokens: recomputed once per frame from the theme, adapted to the background luminance.
    struct MaterialTokens
    {
        D2D1_COLOR_F base{};          // the pill's own fill
        D2D1_COLOR_F raised{};        // inner cards, chips, wells
        D2D1_COLOR_F raised_strong{}; // pressed / active chips
        D2D1_COLOR_F hairline{};      // 1px separators between content
        D2D1_COLOR_F stroke{};        // outer contour
        D2D1_COLOR_F shadow{};        // drop shadow tint
        D2D1_COLOR_F text_primary{};
        D2D1_COLOR_F text_secondary{};
        D2D1_COLOR_F text_tertiary{};
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F accent_soft{};
        bool on_dark = true;
        float opacity = 1.0f;
    };

    static D2D1_COLOR_F mix_color(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
    {
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }

    static D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float alpha)
    {
        c.a = clamp(alpha, 0.0f, 1.0f);
        return c;
    }

    // Cached Direct2D resources: nothing below is created per frame once warm. render thread only.
    struct LinearGradientSlot
    {
        ComPtr<ID2D1LinearGradientBrush> brush;
        uint64_t key = 0;
    };

    struct RadialGradientSlot
    {
        ComPtr<ID2D1RadialGradientBrush> brush;
        uint64_t key = 0;
    };

    struct ShapeKey
    {
        float l = 0, t = 0, r = 0, b = 0, radius = 0;
        bool notch = false;

        bool operator==(const ShapeKey& o) const
        {
            return l == o.l && t == o.t && r == o.r && b == o.b && radius == o.radius && notch == o.notch;
        }
    };

    struct ShapeEntry
    {
        ShapeKey key;
        ComPtr<ID2D1Geometry> geom;
        uint64_t last_used = 0;
    };

    static constexpr size_t shape_cache_size = 32;

    static uint32_t pack_color8(const D2D1_COLOR_F& c)
    {
        auto q = [](float v)
        {
            return static_cast<uint32_t>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
    }

    static uint64_t hash_stops(const D2D1_GRADIENT_STOP* s, UINT32 n)
    {
        uint64_t h = 1469598103934665603ull;
        for (UINT32 i = 0; i < n; ++i)
        {
            h = (h ^ pack_color8(s[i].color)) * 1099511628211ull;
            h = (h ^ static_cast<uint64_t>(s[i].position * 1000.0f + 0.5f)) * 1099511628211ull;
        }
        return h;
    }

    // Call once per frame, right after BeginDraw().
    void begin_frame_resources()
    {
        brush_pool_used_ = 0;
        layer_pool_used_ = 0;
    }

    // A brush unique for the rest of this frame, with color and opacity reset.
    ID2D1SolidColorBrush* acquire_brush(const D2D1_COLOR_F& color)
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
    ID2D1Layer* acquire_layer()
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

    void set_persistent_brush(ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& c)
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
    ID2D1LinearGradientBrush* use_linear_gradient(LinearGradientSlot& slot, const D2D1_GRADIENT_STOP* stops,
                                                  UINT32 count, D2D1_POINT_2F start, D2D1_POINT_2F end)
    {
        const uint64_t key = hash_stops(stops, count);
        if (!slot.brush || slot.key != key)
        {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                             &coll)) ||
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

    ID2D1RadialGradientBrush* use_radial_gradient(RadialGradientSlot& slot, const D2D1_GRADIENT_STOP* stops,
                                                  UINT32 count, D2D1_POINT_2F center, float rx, float ry)
    {
        const uint64_t key = hash_stops(stops, count);
        if (!slot.brush || slot.key != key)
        {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                             &coll)) ||
                !coll)
            {
                return nullptr;
            }
            if (FAILED(target_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), rx, ry), coll.Get(),
                    &slot.brush)) ||
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
    ComPtr<ID2D1Geometry> get_shape_geometry(D2D1_RECT_F rect, float radius, bool notch)
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
            if (SUCCEEDED(d2d_factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &rr)) &&
                rr)
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

    void release_cached_resources()
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

    void build_material_tokens(const Settings& settings)
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

    void draw_pill(const SharedState& state, const Settings& settings, const Activity& activity, D2D1_RECT_F rect,
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
    void draw_soft_shadow(D2D1_RECT_F rect, float radius)
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
    void fill_surface_depth(D2D1_RECT_F rect, float radius, bool strong)
    {
        const float h = rect.bottom - rect.top;
        if (h <= 2.0f)
        {
            return;
        }

        const float bot_a = (strong ? 0.085f : 0.060f) * (material_.on_dark ? 1.0f : 0.55f) * settings_opacity_;

        static const D2D1_GRADIENT_STOP k_stops[3] = {
            {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
            {0.45f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f)},
            {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)},
        };
        ID2D1LinearGradientBrush* brush = use_linear_gradient(
            depth_grad_, k_stops, 3, D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom));
        if (!brush)
        {
            return;
        }
        brush->SetOpacity(clamp(bot_a, 0.0f, 1.0f));
        fill_island_shape(rect, radius, g_settings.w11_style, g_settings.notch_style, brush);
    }

    // Soft accent wash from the top, driven by the album-art accent.
    void fill_accent_bloom(D2D1_RECT_F rect, float radius, float strength)
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
    void draw_card(D2D1_RECT_F rect, float radius, bool active = false)
    {
        ID2D1SolidColorBrush* fill = active ? raised_strong_brush_.Get() : raised_brush_.Get();
        if (fill)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), fill);
        }
    }

    // Shared accent progress track (scrubber, volume, battery, timer).
    void draw_accent_track(D2D1_RECT_F track, float progress, float radius)
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

    void draw_privacy_dots(const SharedState& state, const Settings& settings, D2D1_RECT_F rect, double now)
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
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(unread_x, dot_y),
                                                   notification_center::unread_dot_radius,
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

    ComPtr<ID2D1PathGeometry> create_notch_geometry(D2D1_RECT_F rect, float radius)
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

    ComPtr<ID2D1Geometry> create_island_mask_geometry(D2D1_RECT_F rect, float radius, bool notch_style)
    {
        return get_shape_geometry(rect, radius, notch_style);
    }

    void fill_island_shape(D2D1_RECT_F rect, float radius, bool w11_style, bool notch_style, ID2D1Brush* brush)
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

    void draw_island_shape(D2D1_RECT_F rect, float radius, bool w11_style, bool notch_style, ID2D1Brush* brush,
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
    void draw_pill_surface(D2D1_RECT_F rect, float radius, IslandKind kind, const Settings& settings)
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

    static void get_weather_icon_and_text(int code, std::wstring& icon, std::wstring& text)
    {
        switch (code)
        {
            case 113:
                icon = L"☀️";
                break;
            case 116:
                icon = L"⛅";
                break;
            case 119:
            case 122:
                icon = L"☁️";
                break;
            case 143:
            case 248:
            case 260:
                icon = L"🌫️";
                break;
            case 200:
            case 386:
            case 389:
            case 392:
            case 395:
                icon = L"⛈️";
                break;
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
                icon = L"🌧️";
                break;
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
                icon = L"❄️";
                break;
            default:
                icon = L"🌡️";
                break;
        }
    }

    static int get_days_in_month(int year, int month)
    {
        if (month == 2)
        {
            bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            return leap ? 29 : 28;
        }
        if (month == 4 || month == 6 || month == 9 || month == 11)
        {
            return 30;
        }
        return 31;
    }

    static int get_day_of_week(int year, int month, int day)
    {
        if (month < 3)
        {
            month += 12;
            year -= 1;
        }
        int k = year % 100;
        int j = year / 100;
        int h = (day + 13 * (month + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
        return (h + 6) % 7;
    }

    void draw_calendar_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                                 float scale, SYSTEMTIME& local)
    {
        (void)state;
        (void)now;

        // Hero column: month over the day number over the weekday.
        const D2D1_RECT_F hero = D2D1::RectF(rect.left + 22.0f * scale, rect.top + 16.0f * scale,
                                             rect.left + 115.0f * scale, rect.bottom - 20.0f * scale);
        draw_card(hero, 12.0f * scale);

        D2D1_COLOR_F accent_color =
            (settings.calendar_accent == CalendarAccentMode::System) ? get_system_accent_color() : material_.accent;
        accent_color.a = 0.95f * settings_opacity_;

        ComPtr<ID2D1SolidColorBrush> accent;
        target_->CreateSolidColorBrush(accent_color, &accent);

        if (calendar_cached_date_.wYear != local.wYear || calendar_cached_date_.wMonth != local.wMonth ||
            calendar_cached_date_.wDay != local.wDay ||
            calendar_cached_first_day_setting_ != settings.first_day_of_week)
        {
            wchar_t month_name_buf[32] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, L"MMMM", month_name_buf, ARRAYSIZE(month_name_buf),
                            nullptr);
            calendar_cached_month_name_ = month_name_buf;

            wchar_t weekday_name_buf[32] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, L"dddd", weekday_name_buf, ARRAYSIZE(weekday_name_buf),
                            nullptr);
            calendar_cached_weekday_name_ = weekday_name_buf;

            calendar_cached_first_day_ = settings.first_day_of_week;
            if (calendar_cached_first_day_ < 0)
            {
                DWORD locale_first_day = 0; // locale counts 0 = Monday
                calendar_cached_first_day_ =
                    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                                    reinterpret_cast<LPWSTR>(&locale_first_day),
                                    sizeof(locale_first_day) / sizeof(wchar_t)) > 0
                        ? static_cast<int>((locale_first_day + 1) % 7)
                        : 0;
            }

            static const wchar_t* const fallback_initials[] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
            for (int weekday = 0; weekday < 7; ++weekday)
            {
                wchar_t initial_buf[16] = {};
                // CAL_SSHORTESTDAYNAME1 is Monday; weekday 0 here is Sunday.
                const CALTYPE cal_type = CAL_SSHORTESTDAYNAME1 + static_cast<CALTYPE>((weekday + 6) % 7);
                calendar_cached_initials_[weekday] =
                    GetCalendarInfoEx(LOCALE_NAME_USER_DEFAULT, CAL_GREGORIAN, nullptr, cal_type, initial_buf,
                                      ARRAYSIZE(initial_buf), nullptr) > 0
                        ? initial_buf
                        : fallback_initials[weekday];
            }
            calendar_cached_first_day_setting_ = settings.first_day_of_week;

            calendar_cached_date_ = local;
        }

        if (bold_text_format_)
        {
            target_->DrawTextW(calendar_cached_month_name_.c_str(),
                               static_cast<UINT32>(calendar_cached_month_name_.size()), bold_text_format_.Get(),
                               D2D1::RectF(hero.left + 3.0f * scale, hero.top + 6.0f * scale, hero.right - 3.0f * scale,
                                           hero.top + 22.0f * scale),
                               accent.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        wchar_t year_str[16] = {};
        swprintf_s(year_str, L"%d", local.wYear);
        muted_brush_->SetOpacity(0.62f);
        if (small_text_format_)
        {
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(year_str, static_cast<UINT32>(wcslen(year_str)), small_text_format_.Get(),
                               D2D1::RectF(hero.left, hero.top + 21.0f * scale, hero.right, hero.top + 36.0f * scale),
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        wchar_t day_str[16] = {};
        swprintf_s(day_str, L"%d", local.wDay);
        text_brush_->SetOpacity(0.97f);
        target_->DrawTextW(day_str, static_cast<UINT32>(wcslen(day_str)),
                           cal_day_large_format_ ? cal_day_large_format_.Get() : huge_text_format_.Get(),
                           D2D1::RectF(hero.left, hero.top + 34.0f * scale, hero.right, hero.bottom - 26.0f * scale),
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        muted_brush_->SetOpacity(0.70f);
        if (bold_text_format_)
        {
            target_->DrawTextW(calendar_cached_weekday_name_.c_str(),
                               static_cast<UINT32>(calendar_cached_weekday_name_.size()), bold_text_format_.Get(),
                               D2D1::RectF(hero.left + 4.0f * scale, hero.bottom - 24.0f * scale,
                                           hero.right - 4.0f * scale, hero.bottom - 6.0f * scale),
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Month grid
        const float grid_start = rect.left + 134.0f * scale;
        const float grid_top = rect.top + 16.0f * scale;
        const float col_w = 28.0f * scale;
        const float header_h = 18.0f * scale;
        const int first_day = calendar_cached_first_day_;

        const int start_day_idx = (get_day_of_week(local.wYear, local.wMonth, 1) - first_day + 7) % 7;
        const int month_days = get_days_in_month(local.wYear, local.wMonth);
        const int row_count = (start_day_idx + month_days + 6) / 7; // 5 or 6

        // Row height comes from the available space so a six-row month is never clipped.
        const float dates_top = grid_top + header_h + 7.0f * scale;
        const float grid_bottom = rect.bottom - 14.0f * scale;
        const float row_h = (grid_bottom - dates_top) / static_cast<float>(row_count > 0 ? row_count : 1);

        IDWriteTextFormat* grid_fmt = cal_grid_format_ ? cal_grid_format_.Get() : bold_text_format_.Get();

        muted_brush_->SetOpacity(0.55f);
        for (int i = 0; i < 7; ++i)
        {
            const D2D1_RECT_F cell =
                D2D1::RectF(grid_start + i * col_w, grid_top, grid_start + (i + 1) * col_w, grid_top + header_h);
            const std::wstring& initial = calendar_cached_initials_[(first_day + i) % 7];
            target_->DrawTextW(initial.c_str(), static_cast<UINT32>(initial.size()), grid_fmt, cell, muted_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        const float hr_y = grid_top + header_h + 3.0f * scale;
        ComPtr<ID2D1SolidColorBrush> hr_brush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(
                with_alpha(material_.hairline, material_.hairline.a * settings_opacity_), &hr_brush)) &&
            hr_brush)
        {
            target_->DrawLine(D2D1::Point2F(grid_start + 2.0f * scale, hr_y),
                              D2D1::Point2F(grid_start + 7.0f * col_w - 2.0f * scale, hr_y), hr_brush.Get(),
                              1.0f * scale);
        }

        const float marker_r = std::min(col_w, row_h) * 0.5f - 1.5f * scale;

        int row = 0;
        int col = start_day_idx;
        for (int d = 1; d <= month_days; ++d)
        {
            const D2D1_RECT_F cell = D2D1::RectF(grid_start + col * col_w, dates_top + row * row_h,
                                                 grid_start + (col + 1) * col_w, dates_top + (row + 1) * row_h);
            const std::wstring day_text = std::to_wstring(d);
            const int weekday = (first_day + col) % 7;

            if (d == local.wDay)
            {
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cell.left + col_w * 0.5f, cell.top + row_h * 0.5f), marker_r, marker_r),
                    accent.Get());
                text_brush_->SetOpacity(1.0f);
                target_->DrawTextW(day_text.c_str(), static_cast<UINT32>(day_text.size()), grid_fmt, cell,
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            else if (weekday == 0 || weekday == 6)
            {
                muted_brush_->SetOpacity(0.62f);
                target_->DrawTextW(day_text.c_str(), static_cast<UINT32>(day_text.size()), grid_fmt, cell,
                                   muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            else
            {
                text_brush_->SetOpacity(0.88f);
                target_->DrawTextW(day_text.c_str(), static_cast<UINT32>(day_text.size()), grid_fmt, cell,
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            if (++col > 6)
            {
                col = 0;
                ++row;
            }
        }

        text_brush_->SetOpacity(0.96f);
        muted_brush_->SetOpacity(0.75f);
    }

    // Flow arrows, not bearing arrows: wind from the north-east points south-west, like weather apps.
    std::wstring wind_dir_to_arrow(const std::wstring& dir)
    {
        if (dir == L"N")
        {
            return L"\x2193";
        }
        if (dir == L"NNE" || dir == L"NE" || dir == L"ENE")
        {
            return L"\x2199";
        }
        if (dir == L"E")
        {
            return L"\x2190";
        }
        if (dir == L"ESE" || dir == L"SE" || dir == L"SSE")
        {
            return L"\x2196";
        }
        if (dir == L"S")
        {
            return L"\x2191";
        }
        if (dir == L"SSW" || dir == L"SW" || dir == L"WSW")
        {
            return L"\x2197";
        }
        if (dir == L"W")
        {
            return L"\x2192";
        }
        if (dir == L"WNW" || dir == L"NW" || dir == L"NNW")
        {
            return L"\x2198";
        }
        return dir;
    }

    // Vector weather icons, sized from `s` and centered on `c`.

    void draw_cloud_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush)
    {
        const D2D1_RECT_F base = D2D1::RectF(c.x - 0.44f * s, c.y + 0.02f * s, c.x + 0.44f * s, c.y + 0.21f * s);
        target_->FillRoundedRectangle(D2D1::RoundedRect(base, 0.10f * s, 0.10f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.02f * s), 0.18f * s, 0.18f * s),
                             brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.01f * s, c.y - 0.10f * s), 0.24f * s, 0.24f * s),
                             brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + 0.25f * s, c.y + 0.03f * s), 0.17f * s, 0.17f * s),
                             brush);
    }

    void draw_sun_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush, float core_radius = 0.26f)
    {
        target_->FillEllipse(D2D1::Ellipse(c, core_radius * s, core_radius * s), brush);

        const float inner = (core_radius + 0.09f) * s;
        const float outer = (core_radius + 0.21f) * s;
        for (int i = 0; i < 8; ++i)
        {
            const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            target_->DrawLine(D2D1::Point2F(c.x + ca * inner, c.y + sa * inner),
                              D2D1::Point2F(c.x + ca * outer, c.y + sa * outer), brush, std::max(1.2f, 0.055f * s));
        }
    }

    void draw_lightning_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush)
    {
        ComPtr<ID2D1PathGeometry> bolt;
        if (FAILED(d2d_factory_->CreatePathGeometry(&bolt)) || !bolt)
        {
            return;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(bolt->Open(&sink)) || !sink)
        {
            return;
        }
        sink->BeginFigure(D2D1::Point2F(c.x + 0.07f * s, c.y + 0.16f * s), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(c.x - 0.11f * s, c.y + 0.44f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.00f * s, c.y + 0.44f * s));
        sink->AddLine(D2D1::Point2F(c.x - 0.06f * s, c.y + 0.66f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.15f * s, c.y + 0.36f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.03f * s, c.y + 0.36f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.12f * s, c.y + 0.16f * s));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target_->FillGeometry(bolt.Get(), brush);
    }

    void draw_weather_icon(D2D1_POINT_2F center, float size, WeatherVisual visual, ID2D1Brush* strong, ID2D1Brush* soft)
    {
        const float s = size;
        const float stroke = std::max(1.3f, 0.06f * s);

        switch (visual)
        {
            case WeatherVisual::Clear:
                draw_sun_shape(center, s, strong, 0.28f);
                break;

            case WeatherVisual::PartlyCloudy:
            {
                draw_sun_shape(D2D1::Point2F(center.x - 0.20f * s, center.y - 0.20f * s), s * 0.62f, soft, 0.30f);
                draw_cloud_shape(D2D1::Point2F(center.x + 0.05f * s, center.y + 0.06f * s), s * 0.92f, strong);
                break;
            }

            case WeatherVisual::Cloudy:
                draw_cloud_shape(D2D1::Point2F(center.x, center.y - 0.06f * s), s, soft);
                draw_cloud_shape(D2D1::Point2F(center.x + 0.04f * s, center.y + 0.06f * s), s * 0.86f, strong);
                break;

            case WeatherVisual::Fog:
            {
                draw_cloud_shape(D2D1::Point2F(center.x, center.y - 0.16f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i)
                {
                    const float y = center.y + (0.24f + 0.15f * static_cast<float>(i)) * s;
                    const float half = (0.34f - 0.05f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(center.x - half, y), D2D1::Point2F(center.x + half, y), soft,
                                      stroke);
                }
                break;
            }

            case WeatherVisual::Storm:
                draw_cloud_shape(D2D1::Point2F(center.x, center.y - 0.20f * s), s * 0.92f, soft);
                draw_lightning_shape(center, s, strong);
                break;

            case WeatherVisual::Rain:
            {
                draw_cloud_shape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i)
                {
                    const float x = center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    const float y = center.y + 0.24f * s;
                    target_->DrawLine(D2D1::Point2F(x + 0.05f * s, y), D2D1::Point2F(x - 0.03f * s, y + 0.26f * s),
                                      soft, stroke);
                }
                break;
            }

            case WeatherVisual::Snow:
            {
                draw_cloud_shape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i)
                {
                    const D2D1_POINT_2F f = D2D1::Point2F(center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s,
                                                          center.y + (0.34f + (i == 1 ? 0.06f : 0.0f)) * s);
                    const float r = 0.075f * s;
                    for (int k = 0; k < 3; ++k)
                    {
                        const float a = static_cast<float>(k) * 3.14159265f / 3.0f;
                        target_->DrawLine(D2D1::Point2F(f.x - std::cos(a) * r, f.y - std::sin(a) * r),
                                          D2D1::Point2F(f.x + std::cos(a) * r, f.y + std::sin(a) * r), soft,
                                          std::max(1.0f, 0.035f * s));
                    }
                }
                break;
            }

            case WeatherVisual::Unknown:
            default:
            {
                const float stem_w = 0.11f * s;
                const D2D1_RECT_F stem =
                    D2D1::RectF(center.x - stem_w, center.y - 0.40f * s, center.x + stem_w, center.y + 0.18f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, stem_w, stem_w), soft);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + 0.26f * s), 0.19f * s, 0.19f * s),
                                     strong);
                const D2D1_RECT_F mercury = D2D1::RectF(center.x - stem_w * 0.55f, center.y - 0.12f * s,
                                                        center.x + stem_w * 0.55f, center.y + 0.20f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(mercury, stem_w * 0.55f, stem_w * 0.55f), strong);
                break;
            }
        }
    }

    // Small vector glyphs shared by the weather, hardware and game overlay cards:
    // 0 wind, 1 thermometer, 2 droplet, 3 cpu, 4 memory, 5 gpu, 6 upload, 7 download, 8 disk, 9 gauge.
    void draw_metric_glyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush)
    {
        const float stroke = std::max(1.1f, 0.11f * s);

        if (kind == 9)
        { // gauge: dial arc, ticks and a needle
            const float r = 0.40f * s;
            target_->DrawEllipse(D2D1::Ellipse(c, r, r), brush, stroke);
            for (int i = 0; i < 5; ++i)
            {
                const float a = -3.14159265f * 0.8f + static_cast<float>(i) * 3.14159265f * 0.4f;
                const float ca = std::cos(a);
                const float sa = std::sin(a);
                target_->DrawLine(D2D1::Point2F(c.x + ca * r, c.y + sa * r),
                                  D2D1::Point2F(c.x + ca * (r - 0.10f * s), c.y + sa * (r - 0.10f * s)), brush,
                                  stroke * 0.7f);
            }
            target_->FillEllipse(D2D1::Ellipse(c, 0.075f * s, 0.075f * s), brush);
            const float na = -3.14159265f * 0.25f;
            target_->DrawLine(c, D2D1::Point2F(c.x + std::cos(na) * r * 0.82f, c.y + std::sin(na) * r * 0.82f), brush,
                              stroke);
            return;
        }

        // Arrow used by the upload/download glyphs; `dir` is -1 up, +1 down.
        auto draw_arrow = [&](float dir)
        {
            target_->DrawLine(D2D1::Point2F(c.x, c.y - 0.38f * s * dir), D2D1::Point2F(c.x, c.y + 0.34f * s * dir),
                              brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x + 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
        };

        switch (kind)
        {
            case 3:
            { // cpu: chip body with pins on all four sides
                const D2D1_RECT_F body =
                    D2D1::RectF(c.x - 0.28f * s, c.y - 0.28f * s, c.x + 0.28f * s, c.y + 0.28f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.07f * s, 0.07f * s), brush, stroke);
                const D2D1_RECT_F core =
                    D2D1::RectF(c.x - 0.11f * s, c.y - 0.11f * s, c.x + 0.11f * s, c.y + 0.11f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(core, 0.03f * s, 0.03f * s), brush);
                for (int i = -1; i <= 1; ++i)
                {
                    const float o = static_cast<float>(i) * 0.15f * s;
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y - 0.28f * s), D2D1::Point2F(c.x + o, c.y - 0.42f * s),
                                      brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y + 0.28f * s), D2D1::Point2F(c.x + o, c.y + 0.42f * s),
                                      brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x - 0.28f * s, c.y + o), D2D1::Point2F(c.x - 0.42f * s, c.y + o),
                                      brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + 0.28f * s, c.y + o), D2D1::Point2F(c.x + 0.42f * s, c.y + o),
                                      brush, stroke * 0.8f);
                }
                break;
            }
            case 4:
            { // memory: module with contact notches along the bottom
                const D2D1_RECT_F body =
                    D2D1::RectF(c.x - 0.42f * s, c.y - 0.26f * s, c.x + 0.42f * s, c.y + 0.22f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                for (int i = -2; i <= 2; ++i)
                {
                    const float x = c.x + static_cast<float>(i) * 0.16f * s;
                    target_->DrawLine(D2D1::Point2F(x, c.y - 0.10f * s), D2D1::Point2F(x, c.y + 0.06f * s), brush,
                                      stroke * 0.85f);
                }
                target_->DrawLine(D2D1::Point2F(c.x - 0.20f * s, c.y + 0.34f * s),
                                  D2D1::Point2F(c.x + 0.20f * s, c.y + 0.34f * s), brush, stroke);
                break;
            }
            case 5:
            { // gpu: board with a fan
                const D2D1_RECT_F body =
                    D2D1::RectF(c.x - 0.44f * s, c.y - 0.24f * s, c.x + 0.44f * s, c.y + 0.26f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                target_->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.15f * s, 0.15f * s), brush,
                    stroke * 0.9f);
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.045f * s, 0.045f * s), brush);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y - 0.10f * s),
                                  D2D1::Point2F(c.x + 0.32f * s, c.y - 0.10f * s), brush, stroke * 0.8f);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y + 0.04f * s),
                                  D2D1::Point2F(c.x + 0.32f * s, c.y + 0.04f * s), brush, stroke * 0.8f);
                break;
            }
            case 6: // upload
                draw_arrow(-1.0f);
                break;
            case 7: // download
                draw_arrow(1.0f);
                break;
            case 8:
            { // disk: stacked platters
                for (int i = 0; i < 3; ++i)
                {
                    const float y = c.y - 0.22f * s + static_cast<float>(i) * 0.22f * s;
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, y), 0.36f * s, 0.12f * s), brush,
                                         stroke * 0.9f);
                }
                break;
            }
            default:
                break;
        }
        if (kind >= 3)
        {
            return;
        }

        switch (kind)
        {
            case 0:
            { // wind: three streaming lines
                const float lens[3] = {0.46f, 0.30f, 0.38f};
                for (int i = 0; i < 3; ++i)
                {
                    const float y = c.y + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(c.x - 0.44f * s, y),
                                      D2D1::Point2F(c.x - 0.44f * s + lens[i] * s * 1.9f, y), brush, stroke);
                }
                break;
            }
            case 1:
            { // thermometer
                const float w = 0.13f * s;
                const D2D1_RECT_F stem = D2D1::RectF(c.x - w, c.y - 0.42f * s, c.x + w, c.y + 0.12f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, w, w), brush);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y + 0.24f * s), 0.22f * s, 0.22f * s), brush);
                break;
            }
            case 2:
            default:
            { // droplet: circle body with a tapered tip
                ComPtr<ID2D1PathGeometry> drop;
                if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&drop)) && drop)
                {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(drop->Open(&sink)) && sink)
                    {
                        sink->BeginFigure(D2D1::Point2F(c.x, c.y - 0.44f * s), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c.x + 0.34f * s, c.y - 0.02f * s),
                                                            D2D1::Point2F(c.x + 0.30f * s, c.y + 0.36f * s),
                                                            D2D1::Point2F(c.x, c.y + 0.38f * s)));
                        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c.x - 0.30f * s, c.y + 0.36f * s),
                                                            D2D1::Point2F(c.x - 0.34f * s, c.y - 0.02f * s),
                                                            D2D1::Point2F(c.x, c.y - 0.44f * s)));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        target_->FillGeometry(drop.Get(), brush);
                    }
                }
                break;
            }
        }
    }

    void draw_weather_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                                float scale, bool has_weather, const std::wstring& w_icon, const std::wstring& w_text)
    {
        wchar_t w_temp[32] = {};
        if (has_weather)
        {
            swprintf_s(w_temp, L"%.0f\x00B0", state.weather.temperature);
        }
        else
        {
            wcscpy_s(w_temp, L"--\x00B0");
        }

        std::wstring city = has_weather ? state.weather.city : std::wstring(loc(L"Locating..."));
        std::wstring desc = w_text;

        // Layout: left a hero block (place, reading, condition), right a stack of metric cards.
        const float hero_left = rect.left + 26.0f * scale;
        const float hero_right = rect.left + 188.0f * scale;
        const float divider_x = rect.left + 200.0f * scale;
        const float metric_left = rect.left + 214.0f * scale;
        const float metric_right = rect.right - 22.0f * scale;

        ComPtr<ID2D1SolidColorBrush> icon_strong;
        ComPtr<ID2D1SolidColorBrush> icon_soft;
        target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.95f * settings_opacity_), &icon_strong);
        target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.42f * settings_opacity_), &icon_soft);

        text_brush_->SetOpacity(0.55f);
        if (small_text_format_)
        {
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(city.c_str(), static_cast<UINT32>(city.length()), small_text_format_.Get(),
                               D2D1::RectF(hero_left, rect.top + 26.0f * scale, hero_right, rect.top + 44.0f * scale),
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const float reading_center_y = rect.top + 82.0f * scale;
        if (has_weather && icon_strong && icon_soft)
        {
            draw_weather_icon(D2D1::Point2F(hero_left + 22.0f * scale, reading_center_y), 44.0f * scale,
                              weather_visual_from_code(state.weather.weather_code), icon_strong.Get(), icon_soft.Get());
        }
        else if (icon_strong && icon_soft)
        {
            draw_weather_icon(D2D1::Point2F(hero_left + 22.0f * scale, reading_center_y), 44.0f * scale,
                              WeatherVisual::Unknown, icon_strong.Get(), icon_soft.Get());
        }

        text_brush_->SetOpacity(0.98f);
        if (huge_text_format_)
        {
            huge_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(w_temp, static_cast<UINT32>(wcslen(w_temp)), huge_text_format_.Get(),
                               D2D1::RectF(hero_left + 52.0f * scale, reading_center_y - 32.0f * scale, hero_right,
                                           reading_center_y + 32.0f * scale),
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            huge_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const float desc_top = rect.top + 120.0f * scale;
        const float desc_bottom = rect.top + 162.0f * scale;

        const size_t desc_length = desc.length();
        float desc_font_size = 13.5f;
        if (desc_length > 58)
        {
            desc_font_size = 9.8f;
        }
        else if (desc_length > 44)
        {
            desc_font_size = 10.5f;
        }
        else if (desc_length > 32)
        {
            desc_font_size = 11.5f;
        }
        else if (desc_length > 22)
        {
            desc_font_size = 12.5f;
        }
        desc_font_size *= scale;

        if (std::fabs(weather_desc_format_size_ - desc_font_size) > 0.01f || !weather_desc_format_)
        {
            weather_desc_format_.Reset();
            if (dwrite_factory_)
            {
                bool created = false;
                if (!settings.font_family.empty())
                {
                    HRESULT hr = dwrite_factory_->CreateTextFormat(
                        settings.font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, desc_font_size, L"", &weather_desc_format_);
                    if (SUCCEEDED(hr))
                    {
                        created = true;
                    }
                }
                if (!created)
                {
                    HRESULT hr = dwrite_factory_->CreateTextFormat(
                        L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, desc_font_size, L"", &weather_desc_format_);
                    if (FAILED(hr))
                    {
                        dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                          desc_font_size, L"", &weather_desc_format_);
                    }
                }
            }
            if (weather_desc_format_)
            {
                weather_desc_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                weather_desc_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                weather_desc_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
            weather_desc_format_size_ = desc_font_size;
        }

        text_brush_->SetOpacity(0.82f);
        target_->DrawTextW(desc.c_str(), static_cast<UINT32>(desc.length()),
                           weather_desc_format_ ? weather_desc_format_.Get() : text_format_.Get(),
                           D2D1::RectF(hero_left, desc_top, hero_right, desc_bottom), text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_NONE);
        text_brush_->SetOpacity(0.96f);

        ComPtr<ID2D1SolidColorBrush> divider;
        target_->CreateSolidColorBrush(with_alpha(material_.hairline, material_.hairline.a * settings_opacity_),
                                       &divider);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(divider_x, rect.top + 34.0f * scale, divider_x + 1.0f * scale, rect.bottom - 34.0f * scale),
                0.5f * scale, 0.5f * scale),
            divider.Get());

        // Metric cards: icon and label on one line, the value beneath (long localized labels fit).
        struct Metric
        {
            int glyph;
            const wchar_t* label;
            std::wstring value;
        };

        const std::wstring wind_unit = settings.weather_fahrenheit ? L" mph" : L" km/h";
        const Metric metrics[3] = {
            {0, loc(L"Wind"),
             has_weather ? state.weather.wind_speed + wind_unit + L" " + wind_dir_to_arrow(state.weather.wind_dir)
                         : std::wstring(L"--")},
            {1, loc(L"Feels Like"), has_weather ? state.weather.feels_like + L"\x00B0" : std::wstring(L"--")},
            {2, loc(L"Humidity"), has_weather ? state.weather.humidity + L"%" : std::wstring(L"--")},
        };

        const float card_h = 40.0f * scale;
        const float card_gap = 7.0f * scale;
        const float stack_h = card_h * 3.0f + card_gap * 2.0f;
        float card_top = (rect.top + rect.bottom) * 0.5f - stack_h * 0.5f;

        for (const Metric& m : metrics)
        {
            const D2D1_RECT_F card = D2D1::RectF(metric_left, card_top, metric_right, card_top + card_h);
            draw_card(card, 10.0f * scale);

            if (icon_soft)
            {
                draw_metric_glyph(D2D1::Point2F(card.left + 17.0f * scale, card.top + 14.0f * scale), 14.0f * scale,
                                  m.glyph, icon_soft.Get());
            }

            const float text_left = card.left + 30.0f * scale;
            if (small_text_format_)
            {
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                muted_brush_->SetOpacity(0.62f);
                target_->DrawTextW(m.label, static_cast<UINT32>(wcslen(m.label)), small_text_format_.Get(),
                                   D2D1::RectF(text_left, card.top + 4.0f * scale, card.right - 8.0f * scale,
                                               card.top + 20.0f * scale),
                                   muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (text_format_)
            {
                text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text_brush_->SetOpacity(0.95f);
                target_->DrawTextW(m.value.c_str(), static_cast<UINT32>(m.value.size()), text_format_.Get(),
                                   D2D1::RectF(text_left, card.top + 19.0f * scale, card.right - 8.0f * scale,
                                               card.bottom - 3.0f * scale),
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            card_top += card_h + card_gap;
        }

        text_brush_->SetOpacity(0.96f);
        muted_brush_->SetOpacity(0.75f);
    }

    void draw_tray_bin_button(D2D1_RECT_F box, bool enabled, bool hovered)
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

        target_->DrawLine(D2D1::Point2F(c.x - 0.31f * s, c.y - 0.17f * s),
                          D2D1::Point2F(c.x + 0.31f * s, c.y - 0.17f * s), brush.Get(), stroke, round.Get());
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.30f * s, c.x + 0.11f * s, c.y - 0.17f * s),
                              0.04f * s, 0.04f * s),
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

    void draw_tray_paste_button(D2D1_RECT_F box, bool hovered)
    {
        const float s = box.right - box.left;
        const D2D1_POINT_2F c = D2D1::Point2F((box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f);

        if (hovered)
        {
            ComPtr<ID2D1SolidColorBrush> bg;
            if (SUCCEEDED(
                    target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.20f * settings_opacity_), &bg)) &&
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
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.25f * s, c.y - 0.24f * s, c.x + 0.25f * s, c.y + 0.31f * s),
                              0.07f * s, 0.07f * s),
            brush.Get(), stroke, round.Get());

        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.32f * s, c.x + 0.11f * s, c.y - 0.15f * s),
                              0.05f * s, 0.05f * s),
            brush.Get());

        target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.00f * s),
                          D2D1::Point2F(c.x + 0.12f * s, c.y + 0.00f * s), brush.Get(), stroke * 0.85f, round.Get());
        target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.14f * s),
                          D2D1::Point2F(c.x + 0.05f * s, c.y + 0.14f * s), brush.Get(), stroke * 0.85f, round.Get());
    }

    void draw_tray_remove_button(D2D1_POINT_2F c, float size, bool hovered, bool row_hovered)
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
    void draw_file_tray_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale)
    {
        const float pad_x = file_tray_layout::k_pad_x * scale;

        const D2D1_RECT_F header_rect =
            D2D1::RectF(rect.left + pad_x, rect.top + 18.0f * scale, rect.right - pad_x, rect.top + 38.0f * scale);
        text_brush_->SetOpacity(0.96f);
        if (bold_text_format_)
        {
            bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = loc(L"File Tray");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), bold_text_format_.Get(), header_rect,
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const size_t total = state.file_tray_items.size();

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
            target_->DrawTextW(count_buf, static_cast<UINT32>(wcslen(count_buf)), small_text_format_.Get(), count_rect,
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        draw_tray_paste_button(paste_box, g_hovered_file_tray_action.load(std::memory_order_relaxed) == -3);
        draw_tray_bin_button(clear_box, total > 0, g_hovered_file_tray_action.load(std::memory_order_relaxed) == -2);

        const float list_top = rect.top + file_tray_layout::k_list_top * scale;
        const float list_bottom = rect.bottom - file_tray_layout::k_list_bottom_inset * scale;

        if (state.file_tray_items.empty())
        {
            const D2D1_RECT_F drop_rect = D2D1::RectF(rect.left + pad_x, list_top, rect.right - pad_x, list_bottom);
            ComPtr<ID2D1SolidColorBrush> dash;
            if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.hairline, material_.hairline.a * 1.6f),
                                                         &dash)) &&
                dash)
            {
                ComPtr<ID2D1StrokeStyle> dash_style;
                D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
                props.dashStyle = D2D1_DASH_STYLE_DASH;
                props.dashCap = D2D1_CAP_STYLE_ROUND;
                d2d_factory_->CreateStrokeStyle(props, nullptr, 0, &dash_style);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(drop_rect, 12.0f * scale, 12.0f * scale), dash.Get(),
                                              1.4f, dash_style.Get());
            }

            if (text_format_)
            {
                text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                const wchar_t* hint = loc(L"Drag files here");
                muted_brush_->SetOpacity(0.80f);
                target_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), text_format_.Get(), drop_rect,
                                   muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            muted_brush_->SetOpacity(0.75f);
            return;
        }

        const float row_h = file_tray_layout::k_row_height * scale;
        const float row_gap = file_tray_layout::k_row_gap * scale;
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
                wcscpy_s(size_buf, L"—");
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
                D2D1::Point2F(row.right - remove_margin - remove_size * 0.5f, (row.top + row.bottom) * 0.5f),
                remove_size, remove_hovered, hovered);

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
                target_->DrawTextW(size_buf, static_cast<UINT32>(wcslen(size_buf)), small_text_format_.Get(), size_rect,
                                   muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
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
            target_->DrawTextW(more_buf, static_cast<UINT32>(wcslen(more_buf)), small_text_format_.Get(), more_rect,
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        text_brush_->SetOpacity(0.96f);
        muted_brush_->SetOpacity(0.75f);
    }

    std::size_t notification_center_slot(uint64_t generation)
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

    void draw_notification_center_icon(const notification_center::Group& group, D2D1_RECT_F badge, float radius)
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

    void draw_notification_center_back(D2D1_RECT_F box, float scale, bool hovered)
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
        target_->DrawLine(D2D1::Point2F(cx + arm * 0.5f, cy - arm), D2D1::Point2F(cx - arm * 0.5f, cy),
                          text_brush_.Get(), stroke);
        target_->DrawLine(D2D1::Point2F(cx - arm * 0.5f, cy), D2D1::Point2F(cx + arm * 0.5f, cy + arm),
                          text_brush_.Get(), stroke);
    }

    void draw_notification_center_detail(const notification_center::Group& group, std::size_t offset, D2D1_RECT_F rect,
                                         float scale)
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
            const D2D1_RECT_F range_rect =
                D2D1::RectF(heading_right - 72.0f * scale, text_top, heading_right, text_bottom);
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            muted_brush_->SetOpacity(0.75f);
            target_->DrawTextW(range_buf, static_cast<UINT32>(wcslen(range_buf)), small_text_format_.Get(), range_rect,
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
            target_->DrawTextW(
                heading.c_str(), static_cast<UINT32>(heading.size()), bold_text_format_.Get(),
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

    void draw_notification_center_dashboard(const SharedState& state, D2D1_RECT_F rect, float scale)
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
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), bold_text_format_.Get(), header_rect,
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
            target_->DrawTextW(count_buf, static_cast<UINT32>(wcslen(count_buf)), small_text_format_.Get(), count_rect,
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
                const D2D1_RECT_F empty_rect =
                    D2D1::RectF(rect.left + pad_x, list_top, rect.right - pad_x, list_bottom);
                text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const wchar_t* hint = loc(L"No notifications");
                muted_brush_->SetOpacity(0.80f);
                target_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), text_format_.Get(), empty_rect,
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
            const D2D1_RECT_F badge = D2D1::RectF(row.left + 7.0f * scale, icon_top,
                                                  row.left + 7.0f * scale + icon_size, icon_top + icon_size);
            draw_notification_center_icon(group, badge, 7.0f * scale);

            draw_tray_remove_button(
                D2D1::Point2F(row.right - remove_margin - remove_size * 0.5f, (row.top + row.bottom) * 0.5f),
                remove_size, hovered_action == drawn, hovered);

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
                                   D2D1::RectF(text_left, mid_y, text_right, row.bottom - 2.0f * scale),
                                   muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            ++drawn;
        }

        text_brush_->SetOpacity(0.96f);
        muted_brush_->SetOpacity(0.75f);
    }

    // Load color is semantic: accent when comfortable, amber under pressure, red when saturated.
    D2D1_COLOR_F load_state_color(float fraction) const
    {
        if (fraction >= 0.90f)
        {
            return D2D1::ColorF(1.0f, 0.35f, 0.32f, 1.0f);
        }
        if (fraction >= 0.75f)
        {
            return D2D1::ColorF(1.0f, 0.69f, 0.13f, 1.0f);
        }
        return material_.accent;
    }

    void draw_git_hub_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale)
    {
        (void)settings;

        const GitHubSnapshot& github = state.github;
        const float pad_x = 24.0f * scale;

        text_brush_->SetOpacity(0.96f);
        if (bold_text_format_)
        {
            bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(
                L"GitHub", 6, bold_text_format_.Get(),
                D2D1::RectF(rect.left + pad_x, rect.top + 16.0f * scale, rect.right - pad_x, rect.top + 32.0f * scale),
                text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (!github.has_data)
        {
            const wchar_t* loading = loc(L"Loading...");
            text_brush_->SetOpacity(0.55f);
            if (small_text_format_)
            {
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                target_->DrawTextW(loading, static_cast<UINT32>(wcslen(loading)), small_text_format_.Get(),
                                   D2D1::RectF(rect.left + pad_x, rect.top + 80.0f * scale, rect.right - pad_x,
                                               rect.top + 104.0f * scale),
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            text_brush_->SetOpacity(1.0f);
            return;
        }

        const std::wstring handle = L"@" + github.user;
        text_brush_->SetOpacity(0.55f);
        if (small_text_format_)
        {
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            target_->DrawTextW(
                handle.c_str(), static_cast<UINT32>(handle.length()), small_text_format_.Get(),
                D2D1::RectF(rect.left + pad_x, rect.top + 17.0f * scale, rect.right - pad_x, rect.top + 33.0f * scale),
                text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        static constexpr float level_alpha[] = {0.30f, 0.52f, 0.76f, 1.0f};
        ComPtr<ID2D1SolidColorBrush> level_brushes[4];
        for (int i = 0; i < 4; ++i)
        {
            target_->CreateSolidColorBrush(with_alpha(material_.accent, level_alpha[i] * settings_opacity_),
                                           &level_brushes[i]);
        }

        const float cell = 10.0f * scale;
        const float pitch = 12.0f * scale;
        const float grid_width = pitch * GitHubSnapshot::weeks - (pitch - cell);
        const float grid_left = (rect.left + rect.right - grid_width) * 0.5f;
        const float grid_top = rect.top + 40.0f * scale;
        const float radius = 2.5f * scale;

        for (int week = 0; week < GitHubSnapshot::weeks; ++week)
        {
            for (int day = 0; day < 7; ++day)
            {
                const int level = github.cells[week * 7 + day];
                if (level < 0)
                {
                    continue;
                }
                const float x = grid_left + week * pitch;
                const float y = grid_top + day * pitch;
                const D2D1_ROUNDED_RECT square =
                    D2D1::RoundedRect(D2D1::RectF(x, y, x + cell, y + cell), radius, radius);
                ID2D1SolidColorBrush* brush =
                    level == 0 ? raised_brush_.Get() : level_brushes[std::min(level, 4) - 1].Get();
                if (brush)
                {
                    target_->FillRoundedRectangle(square, brush);
                }
            }
        }

        wchar_t today_text[32], year_text[32], streak_text[32];
        swprintf_s(today_text, L"%d today", github.today_count);
        swprintf_s(year_text, L"%d this year", github.year_total);
        swprintf_s(streak_text, L"%d day streak", github.streak);

        const float stats_top = grid_top + 7.0f * pitch + 6.0f * scale;
        const float stats_bottom = stats_top + 18.0f * scale;
        text_brush_->SetOpacity(0.72f);
        if (small_text_format_)
        {
            const struct
            {
                const wchar_t* text;
                DWRITE_TEXT_ALIGNMENT alignment;
            } stats[] = {{today_text, DWRITE_TEXT_ALIGNMENT_LEADING},
                         {year_text, DWRITE_TEXT_ALIGNMENT_CENTER},
                         {streak_text, DWRITE_TEXT_ALIGNMENT_TRAILING}};

            for (const auto& stat : stats)
            {
                small_text_format_->SetTextAlignment(stat.alignment);
                target_->DrawTextW(stat.text, static_cast<UINT32>(wcslen(stat.text)), small_text_format_.Get(),
                                   D2D1::RectF(grid_left, stats_top, grid_left + grid_width, stats_bottom),
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        text_brush_->SetOpacity(1.0f);
    }

    void draw_hardware_monitor_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                                         float scale)
    {
        (void)settings;

        const float pad_x = 24.0f * scale;

        text_brush_->SetOpacity(0.96f);
        const wchar_t* hw_title = loc(L"Hardware Monitor");
        if (bold_text_format_)
        {
            bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(
                hw_title, static_cast<UINT32>(wcslen(hw_title)), bold_text_format_.Get(),
                D2D1::RectF(rect.left + pad_x, rect.top + 16.0f * scale, rect.right - pad_x, rect.top + 32.0f * scale),
                text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        wchar_t cpu_buf[32], ram_buf[40], gpu_buf[32], up_buf[32], down_buf[32], disk_buf[32];
        swprintf_s(cpu_buf, L"%d%%", state.system.cpu_percent);
        swprintf_s(ram_buf, L"%.1f / %.1f GB", state.system.memory_used_gb, state.system.memory_total_gb);
        if (state.system.gpu_percent >= 0)
        {
            swprintf_s(gpu_buf, L"%d%%", state.system.gpu_percent);
        }
        else
        {
            wcscpy_s(gpu_buf, L"--");
        }
        swprintf_s(up_buf, L"%.1f Mbps", state.system.net_up_mbps);
        swprintf_s(down_buf, L"%.1f Mbps", state.system.net_down_mbps);
        const int disk_used = 100 - state.system.disk_free_percent;
        swprintf_s(disk_buf, L"%d%%", disk_used);

        const float ram_fraction = (state.system.memory_total_gb > 0.01f)
                                       ? clamp(state.system.memory_used_gb / state.system.memory_total_gb, 0.0f, 1.0f)
                                       : -1.0f;

        struct HwMetric
        {
            int glyph;
            const wchar_t* label;
            const wchar_t* value;
            float fraction; // negative means "no meaningful 0-100 scale"
        };

        // Network throughput has no natural ceiling, so those cards get no load bar.
        const HwMetric metrics[6] = {
            {3, L"CPU", cpu_buf, clamp(state.system.cpu_percent / 100.0f, 0.0f, 1.0f)},
            {6, L"NET UP", up_buf, -1.0f},
            {4, L"RAM", ram_buf, ram_fraction},
            {7, L"NET DOWN", down_buf, -1.0f},
            {5, L"GPU", gpu_buf,
             state.system.gpu_percent >= 0 ? clamp(state.system.gpu_percent / 100.0f, 0.0f, 1.0f) : -1.0f},
            {8, L"DISK", disk_buf, clamp(disk_used / 100.0f, 0.0f, 1.0f)},
        };

        const float col_gap = 9.0f * scale;
        const float col_w = (rect.right - rect.left - pad_x * 2.0f - col_gap) * 0.5f;
        const float row_h = 38.0f * scale;
        const float row_gap = 6.0f * scale;
        const float grid_top = rect.top + 38.0f * scale;

        for (int i = 0; i < 6; ++i)
        {
            const HwMetric& m = metrics[i];
            const int col = i % 2;
            const int row = i / 2;

            const float card_left = rect.left + pad_x + static_cast<float>(col) * (col_w + col_gap);
            const float card_top = grid_top + static_cast<float>(row) * (row_h + row_gap);
            const D2D1_RECT_F card = D2D1::RectF(card_left, card_top, card_left + col_w, card_top + row_h);

            draw_card(card, 9.0f * scale);

            const bool has_bar = m.fraction >= 0.0f;
            const D2D1_COLOR_F tint = has_bar ? load_state_color(m.fraction) : material_.accent;

            if (ID2D1SolidColorBrush* glyph_brush = acquire_brush(with_alpha(tint, 0.85f * settings_opacity_)))
            {
                draw_metric_glyph(D2D1::Point2F(card.left + 16.0f * scale, card.top + 15.0f * scale), 15.0f * scale,
                                  m.glyph, glyph_brush);
            }

            const float text_left = card.left + 30.0f * scale;
            const float text_right = card.right - 9.0f * scale;

            if (small_text_format_)
            {
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                muted_brush_->SetOpacity(0.60f);
                target_->DrawTextW(
                    m.label, static_cast<UINT32>(wcslen(m.label)), small_text_format_.Get(),
                    D2D1::RectF(text_left, card.top + 2.0f * scale, text_right, card.top + 15.0f * scale),
                    muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (text_format_)
            {
                text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text_brush_->SetOpacity(0.95f);
                const float value_bottom = has_bar ? card.top + 30.0f * scale : card.bottom - 4.0f * scale;
                target_->DrawTextW(m.value, static_cast<UINT32>(wcslen(m.value)), text_format_.Get(),
                                   D2D1::RectF(text_left, card.top + 15.0f * scale, text_right, value_bottom),
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (has_bar)
            {
                const D2D1_RECT_F track =
                    D2D1::RectF(text_left, card.bottom - 6.0f * scale, text_right, card.bottom - 3.5f * scale);
                ComPtr<ID2D1SolidColorBrush> track_brush;
                if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raised_strong, &track_brush)) && track_brush)
                {
                    target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale),
                                                  track_brush.Get());
                }

                const float span = (track.right - track.left) * m.fraction;
                if (span > 0.5f)
                {
                    if (ID2D1SolidColorBrush* fill_brush = acquire_brush(with_alpha(tint, 0.95f)))
                    {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                              1.25f * scale, 1.25f * scale),
                            fill_brush);
                    }
                }
            }
        }

        text_brush_->SetOpacity(0.96f);
        muted_brush_->SetOpacity(0.75f);
    }

    // Karaoke: a soft wave rides the wipe across the active line (layout cached per line).
    void draw_karaoke_letter_pop(const wchar_t* text, UINT32 text_length, IDWriteTextFormat* format,
                                 D2D1_RECT_F line_rect, float wipe_x, float base_opacity, float unsung_opacity,
                                 bool glow_enabled, float glow_strength = 1.0f)
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
                    if (SUCCEEDED(karaoke_cache_.layout->GetClusterMetrics(raw_clusters.data(), cluster_count,
                                                                           &cluster_count)))
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
                                if (SUCCEEDED(karaoke_cache_.layout->HitTestTextRange(i, cluster_length, 0.0f, 0.0f,
                                                                                      &hit, 1, &hit_count)) &&
                                    hit_count > 0 && hit.width > 0.0f)
                                {
                                    info.rect =
                                        D2D1::RectF(hit.left, hit.top, hit.left + hit.width, hit.top + hit.height);
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
                target_->DrawTextW(text, text_length, format, line_rect, text_brush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
                        D2D1_RECT_F stamp_rect = D2D1::RectF(char_rect.left + ox, char_rect.top + oy,
                                                             char_rect.right + ox, char_rect.bottom + oy);
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

    void draw_time_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                             float scale, SYSTEMTIME& local)
    {
        (void)now;

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;

        if (settings.clock_accent_glow)
        {
            D2D1_COLOR_F accent_color = accent_brush_ ? accent_brush_->GetColor() : D2D1::ColorF(0x4cc9f0);
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(accent_color.r, accent_color.g, accent_color.b, 0.24f)},
                {1.0f, D2D1::ColorF(accent_color.r, accent_color.g, accent_color.b, 0.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> glow_stops;
            target_->CreateGradientStopCollection(stops, 2, &glow_stops);

            if (glow_stops)
            {
                const D2D1_POINT_2F glow_center = D2D1::Point2F(cx, cy - 8.0f * scale);
                ComPtr<ID2D1RadialGradientBrush> glow_brush;
                target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(glow_center, D2D1::Point2F(0, 0),
                                                                                       150.0f * scale, 70.0f * scale),
                                                   glow_stops.Get(), &glow_brush);
                if (glow_brush)
                {
                    target_->FillEllipse(D2D1::Ellipse(glow_center, 150.0f * scale, 70.0f * scale), glow_brush.Get());
                }
            }
        }

        const std::wstring time_text =
            format_island_time(local, settings.clock_follow_system, settings.use24_hour_clock, settings.show_seconds);
        const std::wstring date_text = format_island_date(local, settings.date_format, L"dddd, MMMM d");

        IDWriteTextFormat* big_fmt = time_dashboard_format_ ? time_dashboard_format_.Get() : huge_text_format_.Get();
        IDWriteTextFormat* small_fmt = date_dashboard_format_ ? date_dashboard_format_.Get() : bold_text_format_.Get();

        const std::wstring& headline = settings.date_first ? date_text : time_text;
        const std::wstring& subline = settings.date_first ? time_text : date_text;
        IDWriteTextFormat* headline_fmt = settings.date_first ? small_fmt : big_fmt;
        IDWriteTextFormat* subline_fmt = settings.date_first ? big_fmt : small_fmt;

        if (settings.date_first)
        {
            const D2D1_RECT_F head_rect = D2D1::RectF(rect.left, cy - 46.0f * scale, rect.right, cy - 22.0f * scale);
            text_brush_->SetOpacity(0.92f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headline_fmt, head_rect,
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F sub_rect = D2D1::RectF(rect.left, cy - 20.0f * scale, rect.right, cy + 44.0f * scale);
            text_brush_->SetOpacity(0.98f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), subline_fmt, sub_rect,
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        else
        {
            const D2D1_RECT_F head_rect = D2D1::RectF(rect.left, cy - 44.0f * scale, rect.right, cy + 20.0f * scale);
            text_brush_->SetOpacity(0.98f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headline_fmt, head_rect,
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F sub_rect = D2D1::RectF(rect.left, cy + 22.0f * scale, rect.right, cy + 46.0f * scale);
            muted_brush_->SetOpacity(0.85f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), subline_fmt, sub_rect,
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        text_brush_->SetOpacity(0.98f);

        const bool mic_active = state.system.mic_active && settings.privacy_dots && settings.privacy_dots_mic;
        const bool cam_active = state.system.camera_active && settings.privacy_dots && settings.privacy_dots_cam;

        if (cam_active || mic_active)
        {
            std::wstring label;
            D2D1_COLOR_F dot_color;

            if (cam_active && mic_active)
            {
                dot_color = settings.privacy_dots_cam_hex;
                if (!state.system.camera_app.empty() && !state.system.mic_app.empty() &&
                    state.system.camera_app == state.system.mic_app)
                {
                    label = state.system.camera_app + L" is using camera & microphone";
                }
                else if (!state.system.camera_app.empty() && !state.system.mic_app.empty())
                {
                    label = state.system.camera_app + L" & " + state.system.mic_app + L" using camera & mic";
                }
                else if (!state.system.camera_app.empty())
                {
                    label = state.system.camera_app + L" is using camera & microphone";
                }
                else if (!state.system.mic_app.empty())
                {
                    label = state.system.mic_app + L" is using camera & microphone";
                }
                else
                {
                    label = L"Camera & microphone in use";
                }
            }
            else if (cam_active)
            {
                dot_color = settings.privacy_dots_cam_hex;
                if (!state.system.camera_app.empty())
                {
                    label = state.system.camera_app + L" is using your camera";
                }
                else
                {
                    label = L"Camera in use";
                }
            }
            else
            {
                dot_color = settings.privacy_dots_mic_hex;
                if (!state.system.mic_app.empty())
                {
                    label = state.system.mic_app + L" is using your microphone";
                }
                else
                {
                    label = L"Microphone in use";
                }
            }

            IDWriteTextFormat* fmt = small_text_format_ ? small_text_format_.Get() : text_format_.Get();
            if (fmt && dwrite_factory_)
            {
                ComPtr<IDWriteTextLayout> text_layout;
                HRESULT hr = dwrite_factory_->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), fmt,
                                                               500.0f, 30.0f, &text_layout);

                if (SUCCEEDED(hr) && text_layout)
                {
                    DWRITE_TEXT_METRICS tm = {};
                    text_layout->GetMetrics(&tm);

                    const float pill_h = 22.0f * scale;
                    const float pill_w = tm.width + 28.0f * scale;
                    const float pill_x = cx - pill_w * 0.5f;
                    const float pill_y = rect.bottom - 18.0f * scale - pill_h;

                    ComPtr<ID2D1SolidColorBrush> badge_bg;
                    target_->CreateSolidColorBrush(with_alpha(material_.raised, material_.raised.a * settings_opacity_),
                                                   &badge_bg);
                    if (badge_bg)
                    {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(pill_x, pill_y, pill_x + pill_w, pill_y + pill_h),
                                              pill_h * 0.5f, pill_h * 0.5f),
                            badge_bg.Get());
                    }

                    ComPtr<ID2D1SolidColorBrush> dot_brush;
                    dot_color.a = settings_opacity_;
                    target_->CreateSolidColorBrush(dot_color, &dot_brush);
                    if (dot_brush)
                    {
                        const float badge_dot_r = 3.5f * scale;
                        target_->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(pill_x + 10.0f * scale, pill_y + pill_h * 0.5f), badge_dot_r,
                                          badge_dot_r),
                            dot_brush.Get());
                    }

                    muted_brush_->SetOpacity(0.90f);
                    target_->DrawTextLayout(D2D1::Point2F(pill_x + 18.0f * scale, pill_y + (pill_h - tm.height) * 0.5f),
                                            text_layout.Get(), muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                    muted_brush_->SetOpacity(0.75f);
                }
            }
        }

        text_brush_->SetOpacity(0.96f);
        muted_brush_->SetOpacity(0.75f);
    }

    void draw_idle_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now)
    {
        if (settings.game_overlay || g_game_overlay_pinned.load())
        {
            draw_game_overlay(state, rect, 1.0f);
            return;
        }
        if (!clock_format_)
        {
            return;
        }

        // Publish the content geometry so the File Tray hit test also works while idle.
        publish_content_geometry(rect);

        const float dash_height = rect.bottom - rect.top;
        const float dash_radius = content_island_radius(dash_height);
        ComPtr<ID2D1Geometry> dash_mask = create_island_mask_geometry(rect, dash_radius, g_settings.notch_style);
        ID2D1Layer* dash_layer = acquire_layer();
        const bool have_dash_mask = dash_mask && dash_layer;
        if (have_dash_mask)
        {
            target_->PushLayer(D2D1::LayerParameters(rect, dash_mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                               dash_layer);
        }
        else
        {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring collapsed_time =
            format_island_time(local, settings.clock_follow_system, settings.use24_hour_clock, settings.show_seconds);
        const wchar_t* time_buf = collapsed_time.c_str();

        const float scale = 1.0f;
        const float width = rect.right - rect.left;

        bool has_weather = state.weather.has_data && (now - state.weather.last_updated < 3600.0);
        std::wstring w_icon = L"🌡️";
        std::wstring w_text = loc(L"Loading...");
        if (has_weather)
        {
            w_text = state.weather.weather_desc;
            get_weather_icon_and_text(state.weather.weather_code, w_icon, w_text);
        }

        // Collapsed and expanded views cross-fade by height instead of a hard cut.
        (void)width;
        auto smooth_fade = [](float t)
        {
            t = clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const float collapsed_alpha = smooth_fade((70.0f - dash_height) / 30.0f);
        const float expanded_alpha = smooth_fade((dash_height - 56.0f) / 64.0f);

        ID2D1Layer* dash_fade_layer = acquire_layer();

        if (collapsed_alpha > 0.01f)
        {
            if (dash_fade_layer)
            {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                         D2D1::IdentityMatrix(), collapsed_alpha, nullptr,
                                                         D2D1_LAYER_OPTIONS_NONE),
                                   dash_fade_layer);
            }

            // Collapsed mode: the slots use the same measurements that sized the pill.
            const IdleStripMetrics idle_metrics = measure_idle_strip(state, settings, now);
            IDWriteTextFormat* idle_fmt = idle_text_format_ ? idle_text_format_.Get() : small_text_format_.Get();

            const float privacy_reserve = idle_dot_reserve(idle_metrics) * scale;
            const float inner_left = rect.left + idle_strip_layout::k_pad_x * scale;
            const float inner_right = rect.right - idle_strip_layout::k_pad_x * scale - privacy_reserve;

            float block_width = idle_metrics.clock_width;
            if (idle_metrics.has_weather)
            {
                block_width += (idle_strip_layout::slot_gap * 2.0f + idle_strip_layout::divider_width) * scale +
                               idle_metrics.weather_width;
            }

            const float slack = std::max(0.0f, (inner_right - inner_left) - block_width);
            float slot_x = inner_left + slack * 0.5f;

            text_brush_->SetOpacity(0.96f);
            const D2D1_RECT_F time_rect = D2D1::RectF(slot_x, rect.top, slot_x + idle_metrics.clock_width, rect.bottom);
            target_->DrawTextW(time_buf, static_cast<UINT32>(wcslen(time_buf)), idle_fmt, time_rect, text_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
            slot_x += idle_metrics.clock_width;

            if (idle_metrics.has_weather)
            {
                slot_x += idle_strip_layout::slot_gap * scale;

                ComPtr<ID2D1SolidColorBrush> divider;
                target_->CreateSolidColorBrush(with_alpha(material_.hairline, material_.hairline.a * settings_opacity_),
                                               &divider);
                if (divider)
                {
                    const float div_w = idle_strip_layout::divider_width * scale;
                    const float div_top = rect.top + idle_strip_layout::divider_inset_y * scale;
                    const float div_bottom = rect.bottom - idle_strip_layout::divider_inset_y * scale;
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(slot_x, div_top, slot_x + div_w, div_bottom), div_w * 0.5f,
                                          div_w * 0.5f),
                        divider.Get());
                }
                slot_x += (idle_strip_layout::divider_width + idle_strip_layout::slot_gap) * scale;

                wchar_t weather_label[32] = {};
                if (has_weather)
                {
                    swprintf_s(weather_label, L"%s %.0f\x00B0", w_icon.c_str(), state.weather.temperature);
                }
                else
                {
                    wcscpy_s(weather_label, ARRAYSIZE(weather_label), L"\U0001F321\uFE0F --\x00B0");
                }

                const D2D1_RECT_F w_rect =
                    D2D1::RectF(slot_x, rect.top, slot_x + idle_metrics.weather_width, rect.bottom);
                target_->DrawTextW(weather_label, static_cast<UINT32>(wcslen(weather_label)), idle_fmt, w_rect,
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
            text_brush_->SetOpacity(1.0f);

            if (dash_fade_layer)
            {
                target_->PopLayer();
            }
        }

        if (expanded_alpha > 0.01f)
        {
            if (dash_fade_layer)
            {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                         D2D1::IdentityMatrix(), expanded_alpha, nullptr,
                                                         D2D1_LAYER_OPTIONS_NONE),
                                   dash_fade_layer);
            }

            // Expanded mode. Order must match active_tab_count / file_tray_tab_index.
            std::vector<int> active_tabs;
            active_tabs.push_back(3); // Time (shown first when the island expands/on hover)
            if (settings.notification_center_module)
            {
                active_tabs.push_back(7);
            }
            if (settings.weather)
            {
                active_tabs.push_back(1); // Weather
            }
            active_tabs.push_back(0); // Calendar
            if (settings.github_module)
            {
                active_tabs.push_back(5);
            }
            if (settings.hardware_monitor_module)
            {
                active_tabs.push_back(2);
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

            // Laid out at the final size and pinned to the top, so growing the pill only reveals it.
            const float full_w = std::max(media_layout::expanded_width, rect.right - rect.left);
            const float full_h = std::max(media_layout::expanded_height, rect.bottom - rect.top);
            const float full_cx = (rect.left + rect.right) * 0.5f;
            const D2D1_RECT_F full_rect =
                D2D1::RectF(full_cx - full_w * 0.5f, rect.top, full_cx + full_w * 0.5f, rect.top + full_h);

            draw_tab_transition(1, tab_idx, active_tabs, full_rect, now,
                                [&](int active_tab_id)
                                {
                                    if (active_tab_id == 3)
                                    {
                                        draw_time_dashboard(state, full_rect, settings, now, scale, local);
                                    }
                                    else if (active_tab_id == 0)
                                    {
                                        draw_calendar_dashboard(state, full_rect, settings, now, scale, local);
                                    }
                                    else if (active_tab_id == 1)
                                    {
                                        draw_weather_dashboard(state, full_rect, settings, now, scale, has_weather,
                                                               w_icon, w_text);
                                    }
                                    else if (active_tab_id == 5)
                                    {
                                        draw_git_hub_dashboard(state, full_rect, settings, scale);
                                    }
                                    else if (active_tab_id == 2)
                                    {
                                        draw_hardware_monitor_dashboard(state, full_rect, settings, scale);
                                    }
                                    else if (active_tab_id == 4)
                                    {
                                        draw_file_tray_dashboard(state, full_rect, settings, scale);
                                    }
                                    else if (active_tab_id == 7)
                                    {
                                        draw_notification_center_dashboard(state, full_rect, scale);
                                    }
                                });

            draw_tab_pagination(full_rect, max_tabs);

            if (dash_fade_layer)
            {
                target_->PopLayer();
            }
        }

        if (expanded_alpha <= 0.01f && !g_tray_drag_over.load(std::memory_order_relaxed))
        {
            g_idle_tab = 0;
            reset_tab_transition();
        }

        if (have_dash_mask)
        {
            target_->PopLayer();
        }
        else
        {
            target_->PopAxisAlignedClip();
        }
    }

    void draw_game_overlay(const SharedState& state, D2D1_RECT_F rect, float unused_scale)
    {
        (void)unused_scale;
        const float scale = 1.0f;
        const bool compact = g_settings.game_overlay_compact;
        const game_overlay_layout::Metrics m = game_overlay_layout::For(compact);

        const float card_top = rect.top + m.pad_y;
        const float card_bottom = rect.bottom - m.pad_y;
        float cursor_x = rect.left + m.pad_x;

        // FPS gets a wider card and a larger figure; it has no 0-100 ceiling, so no load bar.
        if (g_settings.game_overlay_show_fps)
        {
            const D2D1_RECT_F fps_card = D2D1::RectF(cursor_x, card_top, cursor_x + m.fps_w, card_bottom);
            draw_card(fps_card, m.radius);

            ComPtr<ID2D1SolidColorBrush> fps_icon;
            if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.90f * settings_opacity_),
                                                         &fps_icon)) &&
                fps_icon)
            {
                draw_metric_glyph(D2D1::Point2F(fps_card.left + 15.0f * scale, fps_card.top + 15.0f * scale),
                                  15.0f * scale, 9, fps_icon.Get());
            }

            const float text_left = fps_card.left + 28.0f * scale;
            const float text_right = fps_card.right - 9.0f * scale;

            if (g_settings.show_metric_text && small_text_format_)
            {
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                muted_brush_->SetOpacity(0.58f);
                target_->DrawTextW(
                    L"FPS", 3, small_text_format_.Get(),
                    D2D1::RectF(text_left, fps_card.top + 2.0f * scale, text_right, fps_card.top + 16.0f * scale),
                    muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            wchar_t fps_value[16] = {};
            swprintf_s(fps_value, L"%d", state.system.render_fps);
            IDWriteTextFormat* fps_fmt = clock_format_ ? clock_format_.Get() : text_format_.Get();
            if (fps_fmt)
            {
                fps_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                text_brush_->SetOpacity(0.97f);
                const float top = g_settings.show_metric_text ? fps_card.top + 16.0f * scale : card_top;
                target_->DrawTextW(fps_value, static_cast<UINT32>(wcslen(fps_value)), fps_fmt,
                                   D2D1::RectF(text_left, top, text_right, card_bottom - 3.0f * scale),
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                fps_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            }

            cursor_x = fps_card.right + m.fps_gap;
        }

        // Load cards: laid out from the enabled metrics, so disabled ones leave no hole.
        struct GameCard
        {
            const wchar_t* label;
            int percent;
            int glyph;
            bool enabled;
        };

        const GameCard cards[] = {
            {L"CPU", state.system.cpu_percent, 3, g_settings.game_overlay_show_cpu},
            {L"RAM", state.system.memory_percent, 4, g_settings.game_overlay_show_ram},
            {L"GPU", state.system.gpu_percent, 5, g_settings.game_overlay_show_gpu},
            {L"DISK", 100 - state.system.disk_free_percent, 8, g_settings.game_overlay_show_disk},
        };

        for (const GameCard& card : cards)
        {
            if (!card.enabled)
            {
                continue;
            }
            if (cursor_x + m.card_w > rect.right - m.pad_x + 0.5f)
            {
                break; // ran out of room
            }
            draw_game_metric_card(D2D1::RectF(cursor_x, card_top, cursor_x + m.card_w, card_bottom), card.label,
                                  card.percent, card.glyph, m.radius);
            cursor_x += m.card_w + m.gap;
        }

        text_brush_->SetOpacity(0.90f);
        muted_brush_->SetOpacity(0.58f);
    }

    void draw_game_metric_card(D2D1_RECT_F rect, const wchar_t* label, int percent, int glyph, float radius)
    {
        const float scale = 1.0f;

        const bool known = percent >= 0;
        const float pct = known ? clamp(percent / 100.0f, 0.0f, 1.0f) : 0.0f;
        const D2D1_COLOR_F tint = known ? load_state_color(pct) : material_.accent;

        draw_card(rect, radius);

        ComPtr<ID2D1SolidColorBrush> glyph_brush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(tint, 0.85f * settings_opacity_), &glyph_brush)) &&
            glyph_brush)
        {
            draw_metric_glyph(D2D1::Point2F(rect.left + 15.0f * scale, rect.top + 15.0f * scale), 14.0f * scale, glyph,
                              glyph_brush.Get());
        }

        const float text_left = rect.left + 28.0f * scale;
        const float text_right = rect.right - 8.0f * scale;

        if (g_settings.show_metric_text && small_text_format_)
        {
            small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            muted_brush_->SetOpacity(0.58f);
            target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), small_text_format_.Get(),
                               D2D1::RectF(text_left, rect.top + 2.0f * scale, text_right, rect.top + 16.0f * scale),
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        wchar_t value[16] = {};
        if (known)
        {
            swprintf_s(value, L"%d%%", percent);
        }
        else
        {
            wcscpy_s(value, ARRAYSIZE(value), L"--");
        }

        if (text_format_)
        {
            text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text_brush_->SetOpacity(0.95f);
            const float value_top = g_settings.show_metric_text ? rect.top + 17.0f * scale : rect.top + 4.0f * scale;
            target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), text_format_.Get(),
                               D2D1::RectF(text_left, value_top, text_right, rect.bottom - 8.0f * scale),
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        const D2D1_RECT_F track = D2D1::RectF(rect.left + 13.0f * scale, rect.bottom - 7.0f * scale,
                                              rect.right - 13.0f * scale, rect.bottom - 4.5f * scale);
        ComPtr<ID2D1SolidColorBrush> track_brush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raised_strong, &track_brush)) && track_brush)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale), track_brush.Get());
        }
        const float span = (track.right - track.left) * pct;
        if (span > 0.5f)
        {
            ComPtr<ID2D1SolidColorBrush> fill_brush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(with_alpha(tint, 0.95f), &fill_brush)) && fill_brush)
            {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                      1.25f * scale, 1.25f * scale),
                    fill_brush.Get());
            }
        }

        text_brush_->SetOpacity(0.90f);
        muted_brush_->SetOpacity(0.58f);
    }

    // Tab transition: tab_pos_ is a continuous position a damped spring chases; each visible tab is drawn
    // from its distance to it (slide, shrink, fade), so motion is interruptible and never restarts.
    void reset_tab_transition()
    {
        tab_init_ = false;
        tab_animating_ = false;
        g_tab_animating.store(false, std::memory_order_relaxed);
        g_tab_edge_bump.store(0, std::memory_order_relaxed);
    }

    template <typename DrawFn>
    void draw_tab_transition(int context, int tab_idx, const std::vector<int>& tab_ids, D2D1_RECT_F rect, double now,
                             DrawFn&& draw_tab)
    {
        const int count = static_cast<int>(tab_ids.size());
        if (count <= 0)
        {
            reset_tab_transition();
            return;
        }
        tab_idx = clamp_int(tab_idx, 0, count - 1);

        constexpr float k_stiffness = 250.0f; // higher = faster (settles in ~0.29s)
        constexpr float k_damping = 31.0f;    // ~0.92 damping ratio: no visible overshoot
        constexpr float travel_px = 34.0f;    // vertical gap between neighbouring tabs
        constexpr float min_scale = 0.965f;   // size of a tab that is fully out of view
        constexpr float fade_power = 1.4f;    // >1 thins the mid-point slightly so text never ghosts

        float dt = static_cast<float>(now - tab_last_drawn_);
        tab_last_drawn_ = now;
        dt = tab_animating_ ? clamp(dt, 0.001f, 0.05f) : (1.0f / 120.0f);

        if (!tab_init_ || context != tab_context_ || count != tab_count_)
        {
            tab_init_ = true;
            tab_context_ = context;
            tab_count_ = count;
            tab_pos_ = static_cast<float>(tab_idx);
            tab_target_ = tab_pos_;
            tab_vel_ = 0.0f;
            tab_dir_ = 1.0f;
            tab_animating_ = false;
        }
        const float new_target = static_cast<float>(tab_idx);
        if (new_target != tab_target_)
        {
            tab_dir_ = new_target > tab_target_ ? 1.0f : -1.0f; // remembered for the whole move
            tab_target_ = new_target;
        }

        g_tab_edge_bump.store(0, std::memory_order_relaxed);

        if (tab_pos_ != tab_target_ || tab_vel_ != 0.0f)
        {
            float remaining = dt;
            while (remaining > 0.0f)
            {
                const float h = std::min(remaining, 0.004f);
                tab_vel_ += (-k_stiffness * (tab_pos_ - tab_target_) - k_damping * tab_vel_) * h;
                tab_pos_ += tab_vel_ * h;

                const float max_tab_pos = static_cast<float>(count - 1);
                if (tab_pos_ < 0.0f)
                {
                    tab_pos_ = 0.0f;
                    if (tab_vel_ < 0.0f)
                    {
                        tab_vel_ = 0.0f;
                    }
                }
                else if (tab_pos_ > max_tab_pos)
                {
                    tab_pos_ = max_tab_pos;
                    if (tab_vel_ > 0.0f)
                    {
                        tab_vel_ = 0.0f;
                    }
                }
                remaining -= h;
            }
            if (std::fabs(tab_pos_ - tab_target_) < 0.0008f && std::fabs(tab_vel_) < 0.02f)
            {
                tab_pos_ = tab_target_;
                tab_vel_ = 0.0f;
            }
        }

        const bool moving = (tab_pos_ != tab_target_ || tab_vel_ != 0.0f);
        tab_animating_ = moving;
        g_tab_animating.store(moving, std::memory_order_relaxed);

        if (!moving)
        {
            draw_tab(tab_ids[tab_idx]);
            return;
        }
        g_tab_last_paint_at.store(now, std::memory_order_relaxed); // render loop keeps frames coming until it settles

        const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        D2D1_MATRIX_3X2_F old_transform;
        target_->GetTransform(&old_transform);

        if (!tab_layer_)
        {
            target_->CreateLayer(&tab_layer_); // cached: re-creating it every frame is costly
        }

        auto draw_layer = [&](int index)
        {
            if (index < 0 || index >= count)
            {
                return;
            }
            const float d = static_cast<float>(index) - tab_pos_; // <0 above, >0 below
            const float ad = clamp(std::fabs(d), 0.0f, 1.0f);
            const float eased = smooth_step01(ad);

            const float alpha = std::pow(1.0f - eased, fade_power);
            if (alpha <= 0.004f)
            {
                return;
            }

            const float scale = 1.0f - (1.0f - min_scale) * eased;
            const float dy = d * travel_px;

            target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) *
                                  D2D1::Matrix3x2F::Translation(0.0f, dy) * old_transform);
            if (alpha < 0.995f && tab_layer_)
            {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                         D2D1::IdentityMatrix(), alpha, nullptr,
                                                         D2D1_LAYER_OPTIONS_NONE),
                                   tab_layer_.Get());
                draw_tab(tab_ids[index]);
                target_->PopLayer();
            }
            else
            {
                draw_tab(tab_ids[index]); // fully opaque: skip the layer, much cheaper
            }
            target_->SetTransform(old_transform);
        };

        const int lo = static_cast<int>(std::floor(tab_pos_));
        if ((tab_pos_ - static_cast<float>(lo)) < 0.5f)
        {
            draw_layer(lo + 1);
            draw_layer(lo);
        }
        else
        {
            draw_layer(lo);
            draw_layer(lo + 1);
        }
    }

    // Vertical pagination dots; the active dot stretches into a pill and slides with the transition.
    void draw_tab_pagination(D2D1_RECT_F rect, int tab_count)
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
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(dot_x - r, cy - half_h, dot_x + r, cy + half_h), r, r), dot_brush);
    }

    // Seqlock write (odd, store, even) of the painted content rect for the window's hit tests.
    void publish_content_geometry(D2D1_RECT_F rect, float right_trim = 0.0f)
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
    float content_island_radius(float content_height) const
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

    // Takes the frame's private settings copy: g_settings can be replaced mid-frame.
    void draw_media(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now)
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
                        rect.left + media_layout::art_inset_x + media_layout::k_art_size,
                        rect.top + media_layout::art_inset_y + media_layout::k_art_size);
        const D2D1_RECT_F immersive_art_rect =
            D2D1::RectF(rect.left + immersive_layout::art_left, rect.top + immersive_layout::art_top,
                        rect.left + immersive_layout::art_left + immersive_layout::k_art_size,
                        rect.top + immersive_layout::art_top + immersive_layout::k_art_size);
        const D2D1_RECT_F expanded_art_rect =
            D2D1::RectF(lerp_f(normal_art_rect.left, immersive_art_rect.left, immersive_mix),
                        lerp_f(normal_art_rect.top, immersive_art_rect.top, immersive_mix),
                        lerp_f(normal_art_rect.right, immersive_art_rect.right, immersive_mix),
                        lerp_f(normal_art_rect.bottom, immersive_art_rect.bottom, immersive_mix));
        const float expanded_art_radius = lerp_f(16.0f, immersive_layout::k_art_size * 0.5f, immersive_mix);
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
                        D2D1::RectF(full_island_rect.left, full_island_rect.top,
                                    full_island_rect.right - side_lyrics_extra, full_island_rect.bottom);
                    float swap_alpha = 1.0f, swap_dx = 0.0f;
                    update_source_swap(state, now, &swap_alpha, &swap_dx);

                    const bool content_grouped = begin_swap_group(swap_alpha, swap_dx);
                    if (swap_alpha > 0.003f)
                    {
                        const float art_size = media_layout::k_art_size;
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

                            D2D1_RECT_F title_rect =
                                D2D1::RectF(text_left, rect.top + 34.0f, text_right, rect.top + 54.0f);
                            const std::wstring title_text =
                                state.media.title.empty() ? std::wstring(loc(L"Unknown")) : state.media.title;
                            if (!draw_title_transition(title_text, title_rect, text_brush_.Get(), now,
                                                       !state.media.title.empty()))
                            {
                                draw_marquee_text(title_text, title_rect, text_format_.Get(), text_brush_.Get(), now,
                                                  42.0f, marquee_title_cache_);
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
                                             D2D1::RectF(text_left, src_line_top, text_right, src_line_top + 14.0f),
                                             now);
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

                    const float scrubber_y = rect.top + media_layout::k_scrubber_y;
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
                        draw_accent_track(
                            D2D1::RectF(bar_left, scrubber_y - bar_half, bar_right, scrubber_y + bar_half), progress,
                            bar_half);

                        const D2D1_COLOR_F scrub_color =
                            (current_accent_.a > 0.0f) ? current_accent_ : D2D1::ColorF(0x4cc9f0);
                        const float scrub_w = (bar_right - bar_left) * progress;
                        const float thumb_x = bar_left + scrub_w;
                        const float thumb_r = is_dragging_this_bar ? 6.5f : 4.5f;

                        if (is_dragging_this_bar)
                        {
                            ComPtr<ID2D1SolidColorBrush> thumb_halo;
                            if (SUCCEEDED(
                                    target_->CreateSolidColorBrush(with_alpha(scrub_color, 0.22f), &thumb_halo)) &&
                                thumb_halo)
                            {
                                target_->FillEllipse(
                                    D2D1::Ellipse(D2D1::Point2F(thumb_x, scrubber_y), thumb_r * 2.2f, thumb_r * 2.2f),
                                    thumb_halo.Get());
                            }
                        }

                        ComPtr<ID2D1SolidColorBrush> thumb_fill;
                        if (SUCCEEDED(
                                target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), &thumb_fill)) &&
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
                                        D2D1::Point2F(cx, cy), D2D1::Point2F(cx + media_layout::control_spacing, cy),
                                        now);
                    draw_source_dock(state, rect, now); // stays still during a switch
                    if (settings.media_tab_lyrics)
                    {
                        draw_immersive_lyrics_button(rect, immersive_mix);
                    }
                    target_->SetTransform(player_zone_old_transform);
                    if (side_lyrics_extra > 1.0f)
                    {
                        draw_media_side_lyrics(state,
                                               D2D1::RectF(rect.right, full_island_rect.top, full_island_rect.right,
                                                           full_island_rect.bottom),
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
                    draw_collapsed_lyrics(state,
                                          D2D1::RectF(text_left, rect.top + 4.0f, text_right, rect.bottom - 4.0f), now,
                                          lyrics_alpha);
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
                target_->DrawTextW(
                    clock_text.c_str(), static_cast<UINT32>(clock_text.size()), idle_text_format_.Get(),
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
                        D2D1::RoundedRect(D2D1::RectF(div_x, rect.top + media_pill_layout::divider_inset_y,
                                                      div_x + div_w, rect.bottom - media_pill_layout::divider_inset_y),
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

    // Immersive lyrics (Modules.MediaTabLyrics): cross-fades the media card into a lyrics view. Scrubber,
    // controls and source dock stay.

    float update_immersive_mix(const Settings& settings, float height, double now)
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

    bool begin_immersive_fade(float alpha)
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

    void end_immersive_fade(bool began)
    {
        if (began)
        {
            target_->PopLayer();
        }
    }

    void draw_immersive_lyrics_button(D2D1_RECT_F rect, float mix)
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
        target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 3.4f), D2D1::Point2F(c.x + 3.8f, c.y - 3.4f),
                          accent_brush_.Get(), 1.2f, st);
        target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 0.2f), D2D1::Point2F(c.x + 1.4f, c.y - 0.2f),
                          accent_brush_.Get(), 1.2f, st);
        accent_brush_->SetOpacity(1.0f);
    }

    void ensure_immersive_format(const Settings& settings)
    {
        const float ts = clamp(settings.text_scale, 0.7f, 1.6f);
        if (imm_format_ && std::fabs(imm_fmt_scale_ - ts) < 0.001f && imm_fmt_family_ == settings.font_family)
        {
            return;
        }
        imm_format_.Reset();
        imm_cache_.clear();
        const wchar_t* family =
            settings.font_family.empty() ? L"Segoe UI Variable Display" : settings.font_family.c_str();
        HRESULT hr =
            dwrite_factory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
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

    void draw_immersive_lyrics_view(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                                    float mix)
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
        const float avail_w = width - immersive_layout::k_pad_x * 2.0f;

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
                const size_t n = state.waveform.size();
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
            const float mid_y = rect.top + immersive_layout::art_top + immersive_layout::k_art_size * 0.5f;
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
            const float opacity = state.lyrics.fetching
                                      ? 0.50f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6))
                                      : 0.60f;
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
            const int64_t pos_ms = std::max<int64_t>(0, static_cast<int64_t>(pos_sec * 1000.0));

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

                    int64_t start_ms = lines[i].time_ms;
                    int64_t end_ms = start_ms + 4000;
                    if (i + 1 < static_cast<int>(lines.size()))
                    {
                        end_ms = lines[i + 1].time_ms;
                    }
                    else if (state.media.end_ticks > 0)
                    {
                        end_ms = state.media.end_ticks / 10000;
                    }
                    const int64_t span = end_ms - start_ms;
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
                        target_->DrawTextLayout(origin, l.layout.Get(), muted_brush_.Get(),
                                                D2D1_DRAW_TEXT_OPTIONS_NONE);
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

    void build_side_lyric_line(CollapsedLyricLine& out, const std::wstring& text, float width, float max_height,
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

        const wchar_t* family =
            settings.font_family.empty() ? L"Segoe UI Variable Display" : settings.font_family.c_str();
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
            if (FAILED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(),
                                                         width, 4096.0f, &layout)) ||
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
    void draw_media_side_lyrics(const SharedState& state, D2D1_RECT_F area, const Settings& settings, double now)
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

        constexpr float k_pad_x = 18.0f;
        constexpr float slide_px = 12.0f;
        constexpr double anim_sec = 0.30;
        const float text_left = area.left + k_pad_x;
        const float text_w = media_layout::side_lyrics_width - k_pad_x * 2.0f;

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
            const int64_t pos_ms = std::max<int64_t>(0, static_cast<int64_t>(pos_sec * 1000.0));

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

            const bool style_changed = std::fabs(side_lyric_scale_ - settings.text_scale) > 0.001f ||
                                       side_lyric_family_ != settings.font_family;
            const bool track_changed =
                state.media.title != side_lyric_title_ || state.media.artist != side_lyric_artist_;

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

    void build_collapsed_lyric_line(CollapsedLyricLine& out, const std::wstring& text, IDWriteTextFormat* fmt)
    {
        out.text = text;
        out.layout.Reset();
        out.width = 0.0f;
        out.height = 0.0f;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 4096.0f,
                                                        40.0f, &out.layout)) &&
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
    void draw_collapsed_lyrics(const SharedState& state, D2D1_RECT_F area, double now, float alpha)
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
        int64_t pos_ms = 0;
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
            pos_ms = std::max<int64_t>(0, static_cast<int64_t>(pos_sec * 1000.0));

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
            target_->DrawTextLayout(D2D1::Point2F(x, y), p.layout.Get(), text_brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
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
                    const int64_t start_ms = lines[show_idx].time_ms;
                    int64_t end_ms = start_ms + 4000;
                    if (show_idx + 1 < static_cast<int>(lines.size()))
                    {
                        end_ms = lines[show_idx + 1].time_ms;
                    }
                    else if (state.media.end_ticks > 0)
                    {
                        end_ms = state.media.end_ticks / 10000;
                    }
                    const int64_t span = end_ms - start_ms;
                    const float prog =
                        span > 0 ? clamp(static_cast<float>(pos_ms - start_ms) / static_cast<float>(span), 0.0f, 1.0f)
                                 : 1.0f;
                    x = area.left - overflow * smooth_step01((prog - 0.12f) / 0.70f);
                }
            }
            const float y = cy - c.height * 0.5f + (1.0f - e) * slide_px;
            text_brush_->SetOpacity(alpha * e * (c.is_title ? 0.90f : 0.97f));
            target_->DrawTextLayout(D2D1::Point2F(x, y), c.layout.Get(), text_brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (title_cycle > 0.0f)
            {
                target_->DrawTextLayout(D2D1::Point2F(x + title_cycle, y), c.layout.Get(), text_brush_.Get(),
                                        D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        target_->PopAxisAlignedClip();
        text_brush_->SetOpacity(0.96f);
    }

    // Source switch animation and source dock
    enum
    {
        KSwapNone = 0,
        KSwapOut,
        KSwapHold,
        KSwapIn
    };

    // Out: old content slides away. Hold: nothing drawn while the media thread switches. In: new content
    // slides in. The request is sent after Out, so old and new are never visible together.
    void update_source_swap(const SharedState& state, double now, float* alpha, float* dx)
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
                const float t =
                    clamp(static_cast<float>((now - swap_start_) / media_layout::switch_out_sec), 0.0f, 1.0f);
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
                const float t =
                    clamp(static_cast<float>((now - swap_start_) / media_layout::switch_in_sec), 0.0f, 1.0f);
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
    void cancel_media_change_anims()
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

    bool begin_swap_group(float alpha, float dx)
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

    void end_swap_group(bool began)
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
    void draw_source_line(const MediaSnapshot& media, D2D1_RECT_F area, double now)
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
    void draw_source_dock(const SharedState& state, D2D1_RECT_F rect, double now)
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
            const float p =
                clamp(static_cast<float>((now - dock_ind_start_) / media_layout::dock_slide_sec), 0.0f, 1.0f);
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
            D2D1::RectF(x0, cy - media_layout::k_dock_height * 0.5f, x0 + media_layout::dock_width(slots.count),
                        cy + media_layout::k_dock_height * 0.5f);
        draw_card(pill, media_layout::k_dock_height * 0.5f);

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
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ind_cx, cy), ind_r - 0.75f, ind_r - 0.75f),
                             accent_brush_.Get(), 1.5f);

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
                    target_->DrawTextW(more, static_cast<UINT32>(wcslen(more)), bold_text_format_.Get(), slot_rect,
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
                    target_->DrawTextW(src.badge.c_str(), static_cast<UINT32>(src.badge.size()),
                                       bold_text_format_.Get(), slot_rect, text_brush_.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

    void update_media_button_animations(double now, bool playing)
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

    // Skip icon: two rounded triangles with a depth swap animation
    static constexpr float skip_tri_w = 0.626f; // triangle width / height
    static constexpr float skip_pitch = 0.451f; // front-left minus back-left, / height

    HRESULT build_rounded_triangle(float x0, float w, float h, ComPtr<ID2D1PathGeometry>& out)
    {
        const D2D1_POINT_2F v[3] = {D2D1::Point2F(x0, -0.5f * h), D2D1::Point2F(x0, 0.5f * h),
                                    D2D1::Point2F(x0 + w, 0.0f)};
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

    bool ensure_skip_geometry()
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

    static float skip_smooth(float x)
    {
        x = clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }

    static float skip_ease_in_out(float x)
    {
        x = clamp(x, 0.0f, 1.0f);
        return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
    }

    // forward = next (>>), else previous (<<). p < 0 = resting, otherwise progress 0..1.
    void draw_skip_icon(D2D1_POINT_2F center, bool forward, float p, float opacity, float press)
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

    void draw_media_controls(bool playing, D2D1_POINT_2F prev, D2D1_POINT_2F play, D2D1_POINT_2F next, double now)
    {
        update_media_button_animations(now, playing);
        draw_media_button(prev, media_layout::nav_button_radius, 0, false);
        draw_media_button(play, media_layout::play_button_radius, playing ? 1 : 2, true);
        draw_media_button(next, media_layout::nav_button_radius, 3, false);
    }

    void draw_media_button(D2D1_POINT_2F center, float radius, int kind, bool primary)
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

    // Play/pause morph: both icons are two quads with matching vertices.
    static constexpr float play_pause_corner = 1.5f;

    struct PlayPauseShapes
    {
        D2D1_POINT_2F pause[8];
        D2D1_POINT_2F play[8];
    };

    static const PlayPauseShapes& get_play_pause_shapes()
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

    bool ensure_round_join_style()
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

    void draw_play_pause_icon(D2D1_POINT_2F center, float opacity, float press)
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
            target_->PushLayer(D2D1::LayerParameters(
                                   D2D1::RectF(center.x - 18.0f, center.y - 18.0f, center.x + 18.0f, center.y + 18.0f),
                                   nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity, nullptr,
                                   D2D1_LAYER_OPTIONS_NONE),
                               layer.Get());
        }
        accent_brush_->SetOpacity(layered ? 1.0f : opacity);

        const float sf = 1.0f - 0.13f * press;
        target_->SetTransform(D2D1::Matrix3x2F::Translation(center.x, center.y) *
                              D2D1::Matrix3x2F::Scale(sf, sf, center) * old_transform);
        for (int q = 0; q < 2; ++q)
        {
            target_->FillGeometry(quad[q].Get(), accent_brush_.Get());
            target_->DrawGeometry(quad[q].Get(), accent_brush_.Get(), play_pause_corner * 2.0f,
                                  round_join_style_.Get());
        }
        target_->SetTransform(old_transform);
        if (layered)
        {
            target_->PopLayer();
        }
        accent_brush_->SetOpacity(1.0f);
    }

    // Song title change animation. Scripts that shape glyphs together fall back to a plain redraw.
    static bool title_needs_shaping(wchar_t c)
    {
        return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0590 && c <= 0x0DFF) || (c >= 0x0E00 && c <= 0x0EFF) ||
               c == 0x200D;
    }

    // Same formula as draw_marquee_text, so the hand-off at the end does not jump.
    static float title_scroll_offset(const TitleScroll& sc, double now)
    {
        return sc.scrolls ? std::fmod(static_cast<float>(now) * 42.0f, sc.cycle) : 0.0f;
    }

    bool build_title_glyphs(const std::wstring& text, float wrap_height, float available, double now, bool all_layouts,
                            std::vector<TitleGlyph>& out, TitleScroll& scroll)
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
    static void match_title_glyphs(std::vector<TitleGlyph>& a, std::vector<TitleGlyph>& b)
    {
        const size_t n = a.size(), m = b.size();
        std::vector<int> dp((n + 1) * (m + 1), 0);
        auto at = [&](size_t i, size_t j) -> int&
        {
            return dp[i * (m + 1) + j];
        };
        auto same = [&](size_t i, size_t j)
        {
            return a[i].eligible && b[j].eligible && a[i].ch0 == b[j].ch0 && a[i].ch1 == b[j].ch1;
        };
        auto pair_score = [&](size_t i, size_t j)
        {
            return 1000 - static_cast<int>(std::min(std::fabs(a[i].sx - b[j].sx), 900.0f));
        };
        for (size_t i = 1; i <= n; ++i)
        {
            for (size_t j = 1; j <= m; ++j)
            {
                int best = std::max(at(i - 1, j), at(i, j - 1));
                if (same(i - 1, j - 1))
                {
                    best = std::max(best, at(i - 1, j - 1) + pair_score(i - 1, j - 1));
                }
                at(i, j) = best;
            }
        }
        size_t i = n, j = m;
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

    void start_title_transition(const std::wstring& old_text, const std::wstring& new_text, D2D1_RECT_F rect,
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
    void draw_smoke_glyph(const TitleGlyph& g, D2D1_POINT_2F origin, float t, float amp, ID2D1Brush* brush,
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
    bool draw_title_transition(const std::wstring& text, D2D1_RECT_F rect, ID2D1Brush* brush, double now,
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
        target_->PushAxisAlignedClip(
            D2D1::RectF(rect.left - 4.0f, rect.top - 20.0f, rect.right + 4.0f, rect.bottom + 6.0f),
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
            const TitleGlyph& n = title_new_[static_cast<size_t>(g.match)];
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
    void draw_styled_progress(ProgressStyle style, float bar_left, float bar_right, float cy, float progress,
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
                    D2D1::RoundedRect(D2D1::RectF(bar_left, cy - half, fill_right, cy + half), half, half),
                    accent.Get());
            }
            if (bar_right - track_left > 1.0f)
            {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track_left, cy - half, bar_right, cy + half), half, half),
                    track.Get());
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
            pts.reserve(static_cast<size_t>((thumb_x - bar_left) / 1.25f) + 3);
            for (float x = bar_left;; x += 1.25f)
            {
                const float xx = std::min(x, thumb_x);
                const float taper =
                    std::min(skip_smooth((xx - bar_left) / 10.0f), skip_smooth((thumb_x - xx) / end_taper));
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
    void draw_swap_marquee(SwapTextState& st, const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                           ID2D1Brush* brush, double now, float speed, MarqueeLayoutCache& cache, float delay)
    {
        constexpr float k_dur = 0.38f;
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
        if (elapsed < 0.0f || elapsed >= delay + k_dur)
        {
            st.start = -1.0;
            draw_marquee_text(text, rect, format, brush, now, speed, cache);
            return;
        }

        const float u = clamp((elapsed - delay) / k_dur, 0.0f, 1.0f);
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

    // Album art flip. Keyed on a pixel hash: the decoder issues new generations for identical images.
    uint64_t art_content_hash(const MediaSnapshot& media)
    {
        if (!media.art)
        {
            return 0;
        }
        if (art_hash_value_ != 0 && art_hash_gen_ == media.art->generation)
        {
            return art_hash_value_;
        }
        uint64_t h = 1469598103934665603ull;
        h = (h ^ static_cast<uint64_t>(media.art->width)) * 1099511628211ull;
        h = (h ^ static_cast<uint64_t>(media.art->height)) * 1099511628211ull;
        const std::vector<uint8_t>& v = media.art->bgra;
        for (size_t i = 0; i < v.size(); i += 61)
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

    void draw_art_face(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius)
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

    void draw_album_art_flip(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius, bool draw_badge)
    {
        constexpr double flip_duration = 0.62;
        const bool recently_drawn = art_flip_last_drawn_ >= 0.0 && (now - art_flip_last_drawn_) < 0.25;
        art_flip_last_drawn_ = now;

        const uint64_t hash = art_content_hash(media);
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
        if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f * (1.0f - c)), &shade)) &&
            shade)
        {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), shade.Get());
        }
        target_->SetTransform(old_transform);
    }

    // Collapsed pill album art: a track change plays as a record swap (old disc thrown away, new one drops
    // in spinning); the same cover with a new track does a short press and rebound.
    void ensure_art_bitmap(const MediaSnapshot& media)
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
            target_->CreateBitmap(D2D1::SizeU(art.width, art.height), art.bgra.data(), art.width * 4, &props,
                                  &art_bitmap_);
            art_generation_ = art.generation;
        }
    }

    void draw_art_disc(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius, float scale, float angle, float lag,
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

    void draw_art_landing_fx(D2D1_RECT_F rect, float radius, float ring_p, float sheen_p, float dir)
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
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(c.x + dx * (pos - half), c.y + dy * (pos - half)),
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

    void draw_album_art_drop(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius)
    {
        constexpr double full_dur = 0.95;
        constexpr double pulse_dur = 0.55;
        constexpr double title_wait = 0.9; // how long to wait for a new cover after a title change
        const bool recently_drawn = pill_art_last_drawn_ >= 0.0 && (now - pill_art_last_drawn_) < 0.25;
        pill_art_last_drawn_ = now;

        const uint64_t hash = art_content_hash(media);
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
    float spec_band_range(const SharedState& state, float from, float to) const
    {
        // Average of the analyzer bands covering [from, to) in 0..k_spectrum_bands.
        int a = std::clamp(static_cast<int>(std::floor(from)), 0, spectrum_bands - 1);
        int b = std::clamp(static_cast<int>(std::ceil(to)), a + 1, spectrum_bands);
        float sum = 0.0f;
        for (int i = a; i < b; ++i)
        {
            sum += state.bands[static_cast<size_t>(i)];
        }
        return sum / static_cast<float>(b - a);
    }

    void draw_spectrum(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                       bool playing = true)
    {
        constexpr double spec_dur = 1.15;
        const bool recently_drawn = spec_last_drawn_ >= 0.0 && (now - spec_last_drawn_) < 0.25;
        float dt = recently_drawn ? static_cast<float>(now - spec_last_drawn_) : 0.016f;
        dt = clamp(dt, 0.001f, 0.05f);
        spec_last_drawn_ = now;

        const size_t key =
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
    void draw_spectrum_orb(const SharedState& state, D2D1_RECT_F rect)
    {
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        const float r = std::min(w, h) * 0.5f + 1.0f; // outer reach
        const float r0 = r * 0.40f;                   // core radius at rest
        const int k_spokes = r < 14.0f ? 16 : 24;
        const int half = k_spokes / 2;
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
        const float stroke_w = clamp(two_pi * (base_r + 1.0f) / static_cast<float>(k_spokes) * 0.62f, 1.0f, r * 0.16f);

        accent_brush_->SetOpacity(0.25f + 0.35f * spec_bass_ + 0.25f * spec_kick_);
        const float shell_r = base_r * (0.90f + 0.18f * bass_pulse);
        target_->DrawEllipse(D2D1::Ellipse(c, shell_r, shell_r), accent_brush_.Get(), 0.8f);

        for (int k = 0; k < k_spokes; ++k)
        {
            const int m = k < half ? k : k_spokes - 1 - k;
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
            const float a = spin + 1.5707963f + two_pi * (static_cast<float>(k) + 0.5f) / k_spokes;
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
        if (SUCCEEDED(
                target_->CreateSolidColorBrush(mix_color(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.65f), &hot)) &&
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
    void draw_spectrum_plasma(const SharedState& state, D2D1_RECT_F rect)
    {
        constexpr int k_pts = 34;
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
            D2D1_POINT_2F pts[k_pts];
            for (int i = 0; i < k_pts; ++i)
            {
                const float xn = static_cast<float>(i) / (k_pts - 1);
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
            sink->AddLines(pts + 1, k_pts - 1);
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
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 5.5f, 5.5f),
                                     accent_brush_.Get());
            }
        }
        accent_brush_->SetOpacity(1.0f);
    }

    // Peak Matrix: LED cells per column with gravity-driven falling peak caps.
    void draw_spectrum_led(const SharedState& state, D2D1_RECT_F rect, float dt)
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
            const size_t wf_size = state.waveform.size();
            const size_t wf_offset = static_cast<size_t>(cols - c) * 3;
            const size_t wf_src = (state.waveform_write + wf_size * 2 - wf_offset) % wf_size;
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

    // Level banner (volume and brightness)
    void draw_level_banner(D2D1_RECT_F rect, const wchar_t* glyph, const std::wstring& label, const wchar_t* value,
                           float pct, bool drained)
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

            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
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
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), small_text_format_.Get(), value_rect,
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

    void draw_brightness(const SharedState& state, D2D1_RECT_F rect)
    {
        wchar_t value[32] = {};
        swprintf_s(value, L"%d%%", state.brightness.percent);
        draw_level_banner(rect, L"\uE706", loc(L"Brightness"), value,
                          clamp(state.brightness.percent / 100.0f, 0.0f, 1.0f), false);
    }

    void draw_album_art(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius = 9.0f,
                        bool draw_badge = true)
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
                draw_bitmap_pixels(*media.source_icon, icon_rect, media_source_icon_bitmap_,
                                   media_source_icon_generation_, 0.95f);
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

    void draw_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F rect, ComPtr<ID2D1Bitmap>& cache,
                            uint64_t& cached_generation, float opacity = 1.0f)
    {
        if (pixels.bgra.empty())
        {
            return;
        }

        if (cached_generation != pixels.generation || !cache)
        {
            D2D1_BITMAP_PROPERTIES props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height), pixels.bgra.data(), pixels.width * 4,
                                  &props, &cache);
            cached_generation = pixels.generation;
        }

        if (cache)
        {
            target_->DrawBitmap(cache.Get(), rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    void draw_rounded_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F badge, float corner_radius,
                                    ComPtr<ID2D1Bitmap>& cache, uint64_t& cached_generation, float opacity = 1.0f)
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
    void draw_cover_fit_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F badge, float corner_radius,
                                      ComPtr<ID2D1Bitmap>& cache, uint64_t& cached_generation, float opacity = 1.0f)
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
        D2D1_RECT_F draw_rect =
            D2D1::RectF(cx - draw_w * 0.5f, cy - draw_h * 0.5f, cx + draw_w * 0.5f, cy + draw_h * 0.5f);

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

    void draw_circular_bitmap_pixels(const BitmapPixels& pixels, D2D1_POINT_2F center, float radius,
                                     ComPtr<ID2D1Bitmap>& cache, uint64_t& cached_generation, float opacity = 1.0f)
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
            target_->PushLayer(D2D1::LayerParameters(rect, ellipse.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                               layer.Get());
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

    void draw_marquee_text(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format, ID2D1Brush* brush,
                           double now, float speed, MarqueeLayoutCache& cache)
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
            dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 2000.0f,
                                              wrap_height, &cache.layout);
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

    float playing_indicator_level(int bar, double now) const
    {
        const double t = std::fmod(now, 3600.0);
        const double slow = std::sin(6.283185307179586 * (0.62 + 0.09 * bar) * t + 1.9 * bar);
        const double fast = std::sin(6.283185307179586 * (1.03 + 0.05 * bar) * t + 0.7 * bar * bar);
        const float wave = static_cast<float>(0.6 * slow + 0.4 * fast);
        return smooth_step01(0.5f + 0.5f * wave);
    }

    float playing_indicator_glow(double now) const
    {
        const float breath =
            0.5f + 0.5f * static_cast<float>(std::sin(6.283185307179586 * 0.45 * std::fmod(now, 3600.0)));
        return 0.22f + 0.14f * breath;
    }

    void draw_playing_indicator(D2D1_RECT_F rect, bool playing, double now)
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

    void draw_countdown_progress(float left, float right, float bottom, float progress)
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
        D2D1_RECT_F fill = D2D1::RectF(
            track.left, track.top, track.left + (track.right - track.left) * clamp(progress, 0.0f, 1.0f), track.bottom);
        accent_brush_->SetOpacity(0.70f);
        target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 1.25f, 1.25f), accent_brush_.Get());
        accent_brush_->SetOpacity(1.0f);
    }

    void draw_clipboard(const SharedState& state, D2D1_RECT_F rect)
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
                target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), icon_format_.Get(), badge,
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
        target_->DrawTextW(clip_title.c_str(), static_cast<UINT32>(clip_title.size()), small_text_format_.Get(),
                           title_rect, muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F text_rect = show_bar ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
                                         : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        draw_marquee_text(state.clipboard.text.empty() ? std::wstring(loc(L"Copied")) : state.clipboard.text, text_rect,
                          text_format_.Get(), text_brush_.Get(), now, 34.0f, marquee_clipboard_cache_);

        draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        muted_brush_->SetOpacity(0.58f);
    }

    void draw_notification(const SharedState& state, D2D1_RECT_F rect)
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
        D2D1_RECT_F badge =
            D2D1::RectF(rect.left + 14, cy - icon_sz * 0.5f, rect.left + 14 + icon_sz, cy + icon_sz * 0.5f);
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
        else if (const wchar_t initial = notification_initial(state.notification.app);
                 initial != 0 && bold_text_format_)
        {
            const DWRITE_TEXT_ALIGNMENT text_alignment = bold_text_format_->GetTextAlignment();
            const DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment = bold_text_format_->GetParagraphAlignment();
            bold_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            bold_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text_brush_->SetOpacity(0.95f);
            target_->DrawTextW(&initial, 1, bold_text_format_.Get(), badge, text_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
                target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), icon_format_.Get(), badge,
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

    void draw_volume(const SharedState& state, D2D1_RECT_F rect)
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

            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
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
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), small_text_format_.Get(), value_rect,
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

    void draw_timer(const SharedState& state, D2D1_RECT_F rect)
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
            target_->FillRectangle(D2D1::RectF(ring_center.x - 2.6f, ring_center.y - h * 0.5f, ring_center.x - 0.8f,
                                               ring_center.y + h * 0.5f),
                                   pause_brush.Get());
            target_->FillRectangle(D2D1::RectF(ring_center.x + 0.8f, ring_center.y - h * 0.5f, ring_center.x + 2.6f,
                                               ring_center.y + h * 0.5f),
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
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), text_format_.Get(), value_rect, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        text_brush_->SetOpacity(0.90f);
        muted_brush_->SetOpacity(0.58f);
    }

    void draw_caps_lock(const SharedState& state, D2D1_RECT_F rect)
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
        target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), clock_format_.Get(), badge, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        ComPtr<ID2D1SolidColorBrush> led_brush;
        D2D1_COLOR_F led_color =
            is_on ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f);
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

    void draw_device(const SharedState& state, D2D1_RECT_F rect)
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
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
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
        target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()), text_format_.Get(), name_rect,
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        text_brush_->SetOpacity(0.90f);

        draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        muted_brush_->SetOpacity(0.58f);
    }

    void draw_do_not_disturb(const SharedState& state, D2D1_RECT_F rect)
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
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        ComPtr<ID2D1SolidColorBrush> led_brush;
        D2D1_COLOR_F led_color =
            is_on ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f);
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

    void draw_bluetooth_category_icon(D2D1_RECT_F badge, BluetoothDeviceCategory category)
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
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), icon_format_.Get(), badge, text_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        text_brush_->SetOpacity(0.90f);
    }

    void draw_bluetooth_device(const SharedState& state, D2D1_RECT_F rect)
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
        target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()), text_format_.Get(), name_rect,
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

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
            D2D1_COLOR_F fill_color =
                battery <= 20 ? D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f) : D2D1::ColorF(1, 1, 1, 0.92f);
            target_->CreateSolidColorBrush(fill_color, &fill_brush);
            target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 0.8f, 0.8f), fill_brush.Get());

            wchar_t pct_buf[16] = {};
            swprintf_s(pct_buf, L"%d%%", battery);
            D2D1_RECT_F pct_rect = show_bar
                                       ? D2D1::RectF(col_center - 25.0f, cy - 2.0f, col_center + 25.0f, cy + 14.0f)
                                       : D2D1::RectF(col_center - 25.0f, cy - 1.0f, col_center + 25.0f, cy + 15.0f);
            text_brush_->SetOpacity(0.85f);
            if (small_text_format_)
            {
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                target_->DrawTextW(pct_buf, static_cast<UINT32>(wcslen(pct_buf)), small_text_format_.Get(), pct_rect,
                                   text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }

        draw_countdown_progress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        muted_brush_->SetOpacity(0.58f);
        text_brush_->SetOpacity(0.90f);
    }

    // Quick lookup card
public:
    // Content height the lookup card needs, so the island animates to exactly what draw_lookup paints.
    float measure_lookup_card(const SharedState& state, const Settings& settings)
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
                         lookup_layout::k_max_height);
        }
        return lookup_layout::status_height();
    }

private:
    struct LookupFit
    {
        std::wstring key;
        std::wstring body;
        std::wstring example;
        float body_h = 0.0f;
        float example_h = 0.0f;
        bool valid = false;
    };

    float lookup_grow(const Settings& settings) const
    {
        return std::max(1.0f, clamp(settings.text_scale, 0.7f, 1.6f));
    }

    float lookup_card_height(const LookupSnapshot& lk, const LookupFit& fit, float grow) const
    {
        float inner = fit.body_h;
        if (!fit.example.empty())
        {
            inner += lookup_layout::example_gap + fit.example_h;
        }
        const float h = lookup_layout::pad_top + lookup_layout::k_title_h * grow +
                        (lk.subtitle.empty() ? 2.0f : lookup_layout::subtitle_h * grow) + lookup_layout::k_card_gap +
                        inner + lookup_layout::card_pad * 2.0f + lookup_layout::pad_bottom;
        return clamp(h, lookup_layout::simple_height, lookup_layout::k_max_height);
    }

    void ensure_lookup_formats(const Settings& settings)
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

        const wchar_t* family =
            settings.font_family.empty() ? L"Segoe UI Variable Small" : settings.font_family.c_str();
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

    std::wstring fit_text_to_lines(const std::wstring& text, IDWriteTextFormat* fmt, float width, int max_lines,
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

        size_t keep = text.size();
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
                    size_t total = 0;
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
            while (!cut.empty() && (iswspace(cut.back()) || cut.back() == L',' || cut.back() == L';'))
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
    const LookupFit& ensure_lookup_fit(const LookupSnapshot& lk, const Settings& settings)
    {
        ensure_lookup_formats(settings);
        const std::wstring key = lk.body + L"\x1F" + lk.example;
        if (lookup_fit_.valid && lookup_fit_.key == key)
        {
            return lookup_fit_;
        }
        const float text_w = lookup_layout::k_width - lookup_layout::k_pad_x * 2.0f - lookup_layout::card_pad * 2.0f;
        LookupFit fit;
        fit.key = key;
        fit.body =
            fit_text_to_lines(lk.body, lookup_body_format_.Get(), text_w, lookup_layout::max_body_lines, &fit.body_h);
        fit.example = fit_text_to_lines(lk.example, lookup_example_format_.Get(), text_w,
                                        lookup_layout::max_example_lines, &fit.example_h);
        fit.valid = true;
        lookup_fit_ = std::move(fit);
        return lookup_fit_;
    }

    ComPtr<IDWriteTextLayout> make_line_layout(const std::wstring& text, IDWriteTextFormat* fmt, float max_width,
                                               DWRITE_TEXT_METRICS* metrics, bool ellipsis = false)
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

    ID2D1StrokeStyle* round_stroke()
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
    void draw_lookup_glyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush)
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
                target_->DrawLine(p(c.x + 0.34f * s, c.y - 0.28f * s), p(c.x + 0.34f * s, c.y + 0.08f * s), brush,
                                  stroke, round);
                target_->DrawLine(p(c.x + 0.34f * s, c.y + 0.08f * s), tip, brush, stroke, round);
                target_->DrawLine(tip, p(c.x - 0.14f * s, c.y - 0.12f * s), brush, stroke, round);
                target_->DrawLine(tip, p(c.x - 0.14f * s, c.y + 0.28f * s), brush, stroke, round);
                break;
            }
        }
    }

    void draw_lookup_field(D2D1_RECT_F rect, double now)
    {
        const LookupUiState& ui = g_lookup_ui;
        const D2D1_RECT_F field = D2D1::RectF(rect.left + lookup_layout::k_pad_x, rect.top + lookup_layout::field_top,
                                              rect.right - lookup_layout::k_pad_x,
                                              rect.top + lookup_layout::field_top + lookup_layout::field_h);
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
            target_->DrawLine(D2D1::Point2F(cx, mid_y - 9.0f), D2D1::Point2F(cx, mid_y + 9.0f), accent_brush_.Get(),
                              1.6f, round_stroke());
            accent_brush_->SetOpacity(1.0f);
        }
        target_->PopAxisAlignedClip();
        muted_brush_->SetOpacity(0.75f);
    }

    void draw_lookup_rows(D2D1_RECT_F rect, const std::vector<LookupRow>& rows, bool header)
    {
        const LookupUiState& ui = g_lookup_ui;
        const float left = rect.left + lookup_layout::k_pad_x;
        const float right = rect.right - lookup_layout::k_pad_x;
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
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), small_text_format_.Get(), band,
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (lookup_has_recent_rows(rows))
            {
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                const wchar_t* clear = loc(L"Clear");
                target_->DrawTextW(clear, static_cast<UINT32>(wcslen(clear)), small_text_format_.Get(), band,
                                   muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            y += lookup_layout::section_h;
        }

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const LookupRow& row = rows[i];
            const D2D1_RECT_F rr = D2D1::RectF(left, y, right, y + lookup_layout::k_row_h);
            const bool selected = static_cast<int>(i) == ui.selected;
            const float mid_y = y + lookup_layout::k_row_h * 0.5f;
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
                target_->DrawTextLayout(D2D1::Point2F(x0, mid_y - lm.height * 0.5f), label_layout.Get(),
                                        text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
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
            y += lookup_layout::k_row_h + lookup_layout::k_row_gap;
        }
        text_brush_->SetOpacity(0.96f);
    }

    void draw_lookup_status(D2D1_RECT_F rect, const LookupSnapshot& lk)
    {
        const float left = rect.left + lookup_layout::k_pad_x + 4.0f;
        const float right = rect.right - lookup_layout::k_pad_x - 4.0f;
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

    void draw_lookup_result(const SharedState& state, D2D1_RECT_F rect, const Settings& settings)
    {
        const LookupSnapshot& lk = state.lookup;
        const LookupFit& fit = ensure_lookup_fit(lk, settings);
        const float grow = lookup_grow(settings);
        const float title_h = lookup_layout::k_title_h * grow;
        const float left = rect.left + lookup_layout::k_pad_x;
        const float right = rect.right - lookup_layout::k_pad_x;
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
        y += lookup_layout::k_card_gap;

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
                           D2D1::RectF(card.left + pad, ty, card.right - pad, ty + fit.body_h + 2.0f),
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        ty += fit.body_h;

        if (!fit.example.empty())
        {
            ty += lookup_layout::example_gap;
            muted_brush_->SetOpacity(0.70f);
            target_->DrawTextW(fit.example.c_str(), static_cast<UINT32>(fit.example.size()),
                               lookup_example_format_.Get(),
                               D2D1::RectF(card.left + pad, ty, card.right - pad, ty + fit.example_h + 2.0f),
                               muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void draw_lookup(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now)
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

    ComPtr<ID2D1StrokeStyle> round_stroke_;
    ComPtr<IDWriteTextFormat> lookup_body_format_;
    ComPtr<IDWriteTextFormat> lookup_example_format_;
    float lookup_format_scale_ = -1.0f;
    std::wstring lookup_format_family_;
    LookupFit lookup_fit_;

    void draw_battery(const SharedState& state, D2D1_RECT_F rect)
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
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), text_format_.Get(), value_rect, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        text_brush_->SetOpacity(0.90f);
    }

    void draw_progress(const SharedState& state, D2D1_RECT_F rect)
    {
        wchar_t buffer[64] = {};
        swprintf_s(buffer, L"Progress %d%%", state.progress.percent);
        IDWriteTextFormat* fmt = idle_text_format_ ? idle_text_format_.Get() : text_format_.Get();
        target_->DrawTextW(buffer, static_cast<UINT32>(wcslen(buffer)), fmt, rect, text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void draw_progress_ring(D2D1_RECT_F rect, int percent)
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

    HWND hwnd_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP old_bitmap_ = nullptr;
    int bitmap_width_ = 0; // logical size of the frame being drawn
    int bitmap_height_ = 0;
    int bitmap_cap_w_ = 0; // allocated size of the DIB (>= logical size)
    int bitmap_cap_h_ = 0;

    POINT last_origin_ = {0, 0};
    RECT last_bounds_ = {0, 0, 0, 0};
    BYTE last_alpha_ = 0;
    bool stage_valid_ = false;
    double last_full_paint_ = -1e9;

    D2D1_RECT_F pill_rects_[dwm_backdrop::max_pills] = {};
    float pill_radii_[dwm_backdrop::max_pills] = {};
    int pill_count_ = 0;

    ComPtr<ID2D1Factory> d2d_factory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<IDWriteFactory> dwrite_factory_;
    ComPtr<IDWriteTextFormat> text_format_;
    ComPtr<IDWriteTextFormat> small_text_format_;
    ComPtr<IDWriteTextFormat> bold_text_format_;
    ComPtr<IDWriteTextFormat> huge_text_format_;
    ComPtr<IDWriteTextFormat> clock_format_;
    ComPtr<IDWriteTextFormat> icon_format_;
    ComPtr<IDWriteTextFormat> media_play_icon_format_;
    ComPtr<IDWriteTextFormat> media_nav_icon_format_;
    bool using_fluent_icons_ = true;
    ComPtr<IDWriteTextFormat> idle_text_format_;
    ComPtr<IDWriteTextFormat> cal_day_large_format_;
    ComPtr<IDWriteTextFormat> cal_grid_format_;
    ComPtr<IDWriteTextFormat> time_dashboard_format_;
    ComPtr<IDWriteTextFormat> date_dashboard_format_;
    ComPtr<ID2D1SolidColorBrush> accent_brush_;
    ComPtr<ID2D1SolidColorBrush> red_brush_;
    ComPtr<ID2D1SolidColorBrush> text_brush_;
    ComPtr<ID2D1SolidColorBrush> muted_brush_;
    ComPtr<ID2D1SolidColorBrush> tint_brush_;
    ComPtr<ID2D1SolidColorBrush> shadow_brush_;
    ComPtr<ID2D1SolidColorBrush> mic_dot_brush_;
    ComPtr<ID2D1SolidColorBrush> unread_dot_brush_;
    ComPtr<ID2D1SolidColorBrush> mic_glow_brush_;
    ComPtr<ID2D1SolidColorBrush> cam_dot_brush_;
    ComPtr<ID2D1SolidColorBrush> cam_glow_brush_;
    ComPtr<ID2D1SolidColorBrush> scratch_color_brush_;
    MarqueeLayoutCache marquee_title_cache_;
    MarqueeLayoutCache marquee_artist_cache_;
    MarqueeLayoutCache marquee_album_cache_;
    MarqueeLayoutCache marquee_clipboard_cache_;
    MarqueeLayoutCache marquee_notification_cache_;
    ComPtr<ID2D1Bitmap> art_bitmap_;
    ComPtr<ID2D1Bitmap> notification_icon_bitmap_;
    ComPtr<ID2D1Bitmap> media_source_icon_bitmap_;
    ComPtr<ID2D1Bitmap> clipboard_icon_bitmap_;
    ComPtr<ID2D1Bitmap> clipboard_image_bitmap_;
    uint64_t art_generation_ = 0;
    SYSTEMTIME calendar_cached_date_{};
    std::wstring calendar_cached_month_name_;
    std::wstring calendar_cached_weekday_name_;
    std::array<std::wstring, 7> calendar_cached_initials_; // indexed 0 = Sunday
    int calendar_cached_first_day_ = 0;                    // resolved, 0 = Sunday
    int calendar_cached_first_day_setting_ = -2;           // -2 forces the first fill
    ComPtr<IDWriteTextFormat> weather_desc_format_;
    float weather_desc_format_size_ = -1.0f;
    uint64_t notification_icon_generation_ = 0;
    uint64_t media_source_icon_generation_ = 0;
    uint64_t clipboard_icon_generation_ = 0;
    uint64_t clipboard_image_generation_ = 0;
    ComPtr<ID2D1Bitmap> file_tray_icon_bitmap_;
    uint64_t file_tray_icon_generation_ = 0;
    std::array<ComPtr<ID2D1Bitmap>, notification_center::max_apps> notification_center_bitmaps_;
    std::array<uint64_t, notification_center::max_apps> notification_center_generations_{};
    std::size_t notification_center_next_slot_ = 0;

    // Lyrics state
    KaraokeLayoutCache karaoke_cache_;

    // Collapsed lyrics state
    CollapsedLyricLine collapsed_lyric_cur_;
    CollapsedLyricLine collapsed_lyric_prev_;
    int collapsed_lyric_idx_ = -2; // -2 = nothing built yet (forces a snap)
    std::wstring collapsed_lyric_title_;
    std::wstring collapsed_lyric_artist_;
    double collapsed_lyric_anim_start_ = -10.0;

    // Media tab lyrics panel state
    CollapsedLyricLine side_lyric_cur_;
    CollapsedLyricLine side_lyric_prev_;
    int side_lyric_idx_ = -2; // -2 = nothing built yet (forces a snap)
    std::wstring side_lyric_title_;
    std::wstring side_lyric_artist_;
    std::wstring side_lyric_family_;
    float side_lyric_scale_ = -1.0f;
    double side_lyric_anim_start_ = -10.0;
    double side_lyric_last_drawn_ = -1.0;

    // Immersive lyrics state
    float immersive_mix_lin_ = 0.0f;
    double immersive_mix_last_ = -1.0;
    double imm_last_drawn_ = -1.0;
    float imm_scroll_ = 0.0f;
    float imm_vel_ = 0.0f;
    float imm_glow_ = 0.0f;
    std::wstring imm_track_key_;
    std::unordered_map<int, CollapsedLyricLine> imm_cache_;
    ComPtr<IDWriteTextFormat> imm_format_;
    float imm_fmt_scale_ = -1.0f;
    std::wstring imm_fmt_family_;
    MarqueeLayoutCache marquee_immersive_cache_;
    LinearGradientSlot imm_karaoke_grad_;
    float hover_scale_ = 1.0f; // pill scale set by render(); gives the resting expanded size
    float island_size_last_w_ = 0.0f;
    float island_size_last_h_ = 0.0f;
    double island_size_moved_at_ = -1e9;
    bool island_settled_ = true; // false while the island is still springing to a new size
    float settings_opacity_ = 0.96f;
    D2D1_COLOR_F pill_bg_color_ = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f);
    MaterialTokens material_{};
    D2D1_COLOR_F current_accent_ = D2D1::ColorF(0x4cc9f0);
    double last_accent_time_ = -1.0; // -1 = not yet set (will snap on first frame)
    float media_btn_press_[3] = {0.0f, 0.0f, 0.0f};
    double last_media_btn_time_ = -1.0;
    ComPtr<ID2D1PathGeometry> skip_tri_full_;  // skip icons: rounded triangle, left edge at x=0
    ComPtr<ID2D1PathGeometry> skip_tri_notch_; // same, with the front triangle's gap cut out
    unsigned skip_seen_trigger_[2] = {0, 0};
    double skip_anim_start_[2] = {-1.0, -1.0};
    float skip_progress_[2] = {-1.0f, -1.0f}; // index 0 = previous, 1 = next; <0 = idle, else 0..1
    double last_skip_click_at_ = -1.0;
    float last_skip_dir_ = 1.0f;
    float play_morph_ = 0.0f; // play/pause morph: 0 = pause bars, 1 = play triangle
    float play_morph_from_ = 0.0f;
    float play_morph_target_ = 0.0f;
    double play_morph_start_ = -1.0;
    double play_morph_last_drawn_ = -1.0;
    bool play_morph_init_ = false;
    ComPtr<ID2D1StrokeStyle> round_join_style_;
    std::vector<TitleGlyph> title_old_;
    std::vector<TitleGlyph> title_new_;
    std::wstring title_shown_;
    bool title_shown_placeholder_ = false;
    bool title_anim_active_ = false;
    double title_anim_start_ = 0.0;
    double title_last_drawn_ = -1.0;
    float title_anim_dur_ = 1.0f;
    float title_avail_ = 0.0f;
    TitleScroll title_old_scroll_;
    TitleScroll title_new_scroll_;
    SwapTextState swap_artist_;
    SwapTextState swap_album_;
    ComPtr<ID2D1Bitmap> art_flip_old_;
    double art_flip_start_ = -1.0;
    double art_flip_last_drawn_ = -1.0;
    uint64_t art_flip_hash_ = 0;
    uint64_t art_hash_gen_ = 0;
    uint64_t art_hash_value_ = 0;
    bool art_flip_init_ = false;
    ComPtr<ID2D1Bitmap> pill_art_old_;
    ComPtr<ID2D1Bitmap> pill_art_last_;
    double pill_art_start_ = -1.0; // -1 = idle
    double pill_art_last_drawn_ = -1.0;
    double pill_art_full_at_ = -1.0;
    double art_outer_last_ = -1.0;
    float art_outer_alpha_ = 1.0f;
    bool upkeep_pending_ = false;
    double last_upkeep_at_ = -1e9;
    bool island_hiding_ = false; // target size is 0 (auto-hide / fullscreen): art shrinks with the pill
    float art_morph_lin_ = 0.0f;
    double art_morph_last_ = -1.0;
    float art_badge_alpha_ = 1.0f;
    double pill_title_pending_at_ = -1.0;
    uint64_t pill_art_hash_ = 0;
    std::wstring pill_title_;
    float pill_art_dir_ = 1.0f;   // +1 clockwise (next), -1 counter-clockwise (previous)
    bool pill_art_pulse_ = false; // same cover, new track
    bool pill_art_init_ = false;
    bool expanded_anim_ = false;
    bool pill_cover_anim_ = false;
    float indicator_mix_ = 0.0f;
    double indicator_last_drawn_ = -1.0;
    bool spec_init_ = false;
    double spec_last_drawn_ = -1.0;
    double spec_start_ = -1.0;
    size_t spec_key_ = 0;
    float spec_t_ = -1.0f;
    float spec_dir_ = 1.0f;
    float spec_phase_ = 0.0f;
    float spec_bass_ = 0.0f;
    float spec_bass_slow_ = 0.0f;
    float spec_kick_ = 0.0f;
    float spec_peak_[spectrum_bands] = {};
    float spec_peak_vel_[spectrum_bands] = {};
    float progress_amp_ = 1.0f;
    double progress_last_time_ = -1.0;

    // Source switch and dock
    int swap_phase_ = 0;
    double swap_start_ = 0.0;
    double swap_last_update_ = -1.0;
    float swap_dir_ = 1.0f;
    unsigned swap_seen_seq_ = 0;
    std::wstring swap_target_;
    ComPtr<ID2D1Layer> swap_group_layer_;
    D2D1_MATRIX_3X2_F swap_group_old_transform_ = D2D1::Matrix3x2F::Identity();
    bool dock_init_ = false;
    int dock_slot_count_ = 0;
    double dock_last_drawn_ = -1.0;
    float dock_ind_pos_ = 0.0f;
    float dock_ind_from_ = 0.0f;
    float dock_ind_target_ = 0.0f;
    double dock_ind_start_ = -1.0;
    float dock_hover_[media_layout::dock_max_slots] = {};
    float dock_press_[media_layout::dock_max_slots] = {};
    ComPtr<ID2D1Bitmap> dock_icon_bitmap_[media_layout::dock_max_slots];
    uint64_t dock_icon_gen_[media_layout::dock_max_slots] = {};
    MarqueeLayoutCache marquee_source_cache_;

    // Tab transition state
    int tab_context_ = -1;
    int tab_count_ = 0;
    bool tab_init_ = false;
    bool tab_animating_ = false;
    float tab_pos_ = 0.0f;
    float tab_target_ = 0.0f;
    float tab_vel_ = 0.0f;
    float tab_dir_ = 1.0f;
    double tab_last_drawn_ = -1.0;
    ComPtr<ID2D1Layer> tab_layer_;

    // Cached resources (see "Cached Direct2D resources")
    std::vector<ComPtr<ID2D1SolidColorBrush>> brush_pool_;
    size_t brush_pool_used_ = 0;
    std::vector<ComPtr<ID2D1Layer>> layer_pool_;
    size_t layer_pool_used_ = 0;
    ComPtr<ID2D1SolidColorBrush> raised_brush_;
    ComPtr<ID2D1SolidColorBrush> raised_strong_brush_;
    LinearGradientSlot depth_grad_;
    LinearGradientSlot track_grad_;
    RadialGradientSlot bloom_grad_;
    ShapeEntry shape_cache_[shape_cache_size];
    uint64_t shape_clock_ = 0;
};

Activity activity_for_kind(IslandKind kind, const Settings& settings, const SharedState& state)
{
    Activity activity;
    activity.kind = kind;

    switch (kind)
    {
        case IslandKind::Media:
            activity.width =
                media_pill_layout::base_width +
                (state.system.mic_active || state.system.camera_active ? media_pill_layout::privacy_extra_width : 0.0f);
            activity.height = 44.0f;
            break;
        case IslandKind::Progress:
            activity.width = 230.0f;
            activity.height = 48.0f;
            break;
        case IslandKind::Clipboard:
            activity.width = 340.0f;
            activity.height = 56.0f;
            break;
        case IslandKind::Notification:
            activity.width = 360.0f;
            activity.height = 58.0f;
            break;
        case IslandKind::Volume:
        case IslandKind::Brightness:
            activity.width = 300.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::BatteryLow:
            activity.width = 290.0f;
            activity.height = 52.0f;
            break;
        case IslandKind::CapsLock:
            activity.width = 180.0f;
            activity.height = 42.0f;
            break;
        case IslandKind::Device:
            activity.width = 240.0f;
            activity.height = 50.0f;
            break;
        case IslandKind::Bluetooth:
            activity.width = 280.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::Timer:
            activity.width = 260.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::DoNotDisturb:
            activity.width = 220.0f;
            activity.height = 42.0f;
            break;
        case IslandKind::Lookup:
            // Seed only: the render loop measures the real height (measure_lookup_card).
            activity.width = lookup_layout::k_width;
            activity.height = 42.0f;
            break;
        case IslandKind::Idle:
        default:
            if (settings.auto_hide_idle_seconds == -1 && !state.system.mic_active && !state.system.camera_active)
            {
                activity.width = 0.0f;
                activity.height = 0.0f;
            }
            else
            {
                // Seed only: the render loop measures the real collapsed width (measure_idle_strip).
                activity.width = settings.weather ? 170.0f : 96.0f;
                activity.height = idle_strip_layout::k_height;
            }
            break;
    }

    activity.width *= settings.size_scale;
    activity.height *= settings.size_scale;
    return activity;
}

}

std::vector<IslandKind> choose_activities(const SharedState& state, const Settings& settings, double now)
{
    std::vector<IslandKind> activities;

    if (settings.quick_lookup && state.lookup.active && now < state.lookup.expires_at)
    {
        activities.push_back(IslandKind::Lookup);
    }

    if (state.clipboard.active && now < state.clipboard.expires_at)
    {
        activities.push_back(IslandKind::Clipboard);
    }
    if (settings.caps_lock && state.caps_lock.active && now < state.caps_lock.expires_at)
    {
        activities.push_back(IslandKind::CapsLock);
    }
    if (state.device.active && now < state.device.expires_at)
    {
        activities.push_back(IslandKind::Device);
    }
    if (settings.bluetooth_indicator && state.bluetooth_device.active && now < state.bluetooth_device.expires_at)
    {
        activities.push_back(IslandKind::Bluetooth);
    }
    if (settings.do_not_disturb_indicator && state.do_not_disturb.active && now < state.do_not_disturb.expires_at)
    {
        activities.push_back(IslandKind::DoNotDisturb);
    }
    if (settings.volume && state.volume.active && now < state.volume.expires_at)
    {
        activities.push_back(IslandKind::Volume);
    }
    if (settings.brightness && state.brightness.active && now < state.brightness.expires_at)
    {
        activities.push_back(IslandKind::Brightness);
    }
    if (state.notification.active && now < state.notification.expires_at)
    {
        activities.push_back(IslandKind::Notification);
    }
    if (settings.battery && state.battery.active && now < state.battery.expires_at)
    {
        activities.push_back(IslandKind::BatteryLow);
    }
    if (settings.progress && state.progress.active)
    {
        activities.push_back(IslandKind::Progress);
    }
    if (settings.timer_enabled &&
        ((state.timer.just_finished && now < state.timer.finished_expires_at) || state.timer.active))
    {
        activities.push_back(IslandKind::Timer);
    }
    if (settings.media && state.media.available)
    {
        activities.push_back(IslandKind::Media);
    }

    if (activities.empty())
    {
        activities.push_back(IslandKind::Idle);
    }

    return activities;
}

DWORD WINAPI render_thread_proc(void*)
{
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = window_class;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                window_class, L"Dynamic Island for Windows", WS_POPUP, 0, 0, 520, 140, nullptr, nullptr,
                                wc.hInstance, nullptr);

    if (!hwnd)
    {
        host::log(L"Failed to create Dynamic Island overlay window.");
        if (SUCCEEDED(hr_co))
        {
            CoUninitialize();
        }
        return 0;
    }

    g_hwnd = hwnd;
    if (g_shell_hook_message == 0)
    {
        g_shell_hook_message = RegisterWindowMessageW(L"SHELLHOOK");
    }
    if (g_taskbar_created_message == 0)
    {
        g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    }
    using ChangeWindowMessageFilterExT = BOOL(WINAPI*)(HWND, UINT, DWORD, PVOID);
    static auto p_change_window_message_filter_ex = reinterpret_cast<ChangeWindowMessageFilterExT>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"));
    if (p_change_window_message_filter_ex)
    {
        if (g_shell_hook_message)
        {
            p_change_window_message_filter_ex(hwnd, g_shell_hook_message, 1 /*MSGFLT_ALLOW*/, nullptr);
        }
        if (g_taskbar_created_message)
        {
            p_change_window_message_filter_ex(hwnd, g_taskbar_created_message, 1 /*MSGFLT_ALLOW*/, nullptr);
        }
    }
    else
    {
        using ChangeWindowMessageFilterT = BOOL(WINAPI*)(UINT, DWORD);
        static auto p_change_window_message_filter = reinterpret_cast<ChangeWindowMessageFilterT>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilter"));
        if (p_change_window_message_filter)
        {
            if (g_shell_hook_message)
            {
                p_change_window_message_filter(g_shell_hook_message, 1 /*MSGFLT_ADD*/);
            }
            if (g_taskbar_created_message)
            {
                p_change_window_message_filter(g_taskbar_created_message, 1 /*MSGFLT_ADD*/);
            }
        }
    }
    enable_blur_behind(hwnd);
    apply_backdrop_material(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    apply_hide_show_hotkey();

    if (g_settings.auto_hide_idle_seconds == 0)
    {
        g_manually_hidden = false;
        host::set_int_value(L"ManuallyHidden", 0);
    }
    else
    {
        g_manually_hidden = host::int_value(L"ManuallyHidden", 0) != 0;
    }
    if (g_manually_hidden.load())
    {
        ShowWindow(hwnd, SW_HIDE);
    }

    Renderer renderer;
    if (!renderer.initialize(hwnd))
    {
        DestroyWindow(hwnd);
        g_hwnd = nullptr;
        if (SUCCEEDED(hr_co))
        {
            CoUninitialize();
        }
        return 0;
    }

    // Restore the File Tray here: the shell icons need this thread's COM apartment.
    load_file_tray();

    // 1 ms timer resolution costs power, so it is held only while the island animates. The flag is set
    // only when timeBeginPeriod succeeded, so begin/end stay balanced.
    bool high_res_timer = false;

    constexpr double high_res_timer_hold_sec = 0.5;
    double last_animating_at = -1.0;

    auto set_high_res_timer = [&](bool want)
    {
        if (want == high_res_timer)
        {
            return;
        }
        if (want)
        {
            if (timeBeginPeriod(1) == TIMERR_NOERROR)
            {
                high_res_timer = true;
            }
        }
        else
        {
            timeEndPeriod(1);
            high_res_timer = false;
        }
    };

    SpringValue width_spring;
    SpringValue height_spring;
    SpringValue nudge_spring;
    SpringValue volume_bar_spring;
    width_spring.reset((g_settings.auto_hide_idle_seconds == -1 ? 0.0f : 120.0f) * g_settings.size_scale);
    height_spring.reset((g_settings.auto_hide_idle_seconds == -1 ? 0.0f : 36.0f) * g_settings.size_scale);
    nudge_spring.reset(0.0f);

    IslandKind previous_primary = IslandKind::Idle;
    auto previous_frame = std::chrono::steady_clock::now();
    auto next_frame_target = previous_frame;
    double next_battery_poll = 0.0;
    double next_progress_poll = 0.0;
    double next_system_poll = 0.0;
    double next_brightness_poll = 0.0;
    double next_privacy_poll = 0.0;
    bool was_manually_hidden = false;
    bool was_auto_hidden_parked = false;
    bool parked_for_fullscreen = false;

    static double last_interaction_time = now_seconds();
    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == wm_app_new_event)
            {
                nudge_spring.value = -6.0f;
                nudge_spring.velocity = 0.0f;
                nudge_spring.target = 0.0f;
                last_interaction_time = now_seconds();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        bind_volume_endpoint();

        bool just_unhidden = false;

        if (g_manually_hidden.load())
        {
            // Manual hide owns the window state, so auto-park bookkeeping is cleared.
            g_auto_hidden_parked = false;
            was_auto_hidden_parked = false;

            if (!was_manually_hidden)
            {
                ShowWindow(hwnd, SW_HIDE);
                dwm_backdrop::hide_all();
                g_audio_capture_needed.store(false, std::memory_order_relaxed);
                was_manually_hidden = true;
            }
            previous_frame = std::chrono::steady_clock::now();
            // Fully parked: only the stop event or a message wakes this thread; no timers.
            set_high_res_timer(false);
            MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, INFINITE, QS_ALLINPUT);
            next_frame_target = std::chrono::steady_clock::now();
            continue;
        }

        // Before the parked branch on purpose: that branch can block for up to 1.5 s.
        {
            static bool prev_parked_for_hook = false;
            const bool parked_now = g_auto_hidden_parked.load(std::memory_order_relaxed);
            if (parked_now != prev_parked_for_hook)
            {
                prev_parked_for_hook = parked_now;
                notify_mouse_thread_parked_changed();
            }
        }

        if (g_auto_hidden_parked.load())
        {
            if (g_settings.auto_hide_idle_seconds == 0 && !parked_for_fullscreen)
            {
                g_auto_hidden_parked = false;
            }
            else
            {
                previous_frame = std::chrono::steady_clock::now();
                set_high_res_timer(false);
                if (parked_for_fullscreen)
                {
                    MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, 1500, QS_ALLINPUT);
                    const bool still_fullscreen = g_settings.auto_hide_fullscreen && is_foreground_fullscreen(hwnd);
                    g_is_fullscreen.store(still_fullscreen, std::memory_order_relaxed);
                    if (!still_fullscreen)
                    {
                        g_fullscreen_override_visible = false;
                        g_auto_hidden_parked = false;
                    }
                    else if (g_fullscreen_override_visible.load())
                    {
                        g_auto_hidden_parked = false;
                    }
                }
                else
                {
                    MsgWaitForMultipleObjects(1, &g_stop_event, FALSE, INFINITE, QS_ALLINPUT);
                }
                next_frame_target = std::chrono::steady_clock::now();
                continue;
            }
        }

        if (was_manually_hidden)
        {
            was_manually_hidden = false;
            just_unhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            last_interaction_time = now_seconds();
            g_layout_dirty = true;
            previous_frame = std::chrono::steady_clock::now();
            next_frame_target = previous_frame;
        }

        if (was_auto_hidden_parked && !g_auto_hidden_parked.load())
        {
            was_auto_hidden_parked = false;
            just_unhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            last_interaction_time = now_seconds();
            g_layout_dirty = true;
            previous_frame = std::chrono::steady_clock::now();
            next_frame_target = previous_frame;
        }

        const double now = now_seconds();
        // The registry and device polls below can take several ms, so they wait for tab animations to settle.
        const bool tab_busy = g_tab_animating.load(std::memory_order_relaxed) &&
                              (now - g_tab_last_paint_at.load(std::memory_order_relaxed)) < 0.25;
        const bool spring_busy = std::fabs(width_spring.value - width_spring.target) > 0.5f ||
                                 std::fabs(height_spring.value - height_spring.target) > 0.5f ||
                                 std::fabs(width_spring.velocity) > 0.5f || std::fabs(height_spring.velocity) > 0.5f;
        const bool poll_busy = tab_busy || spring_busy;
        if (now >= next_battery_poll)
        {
            update_battery_snapshot();
            next_battery_poll = now + 15.0;
        }
        if (now >= next_progress_poll)
        {
            update_progress_snapshot();
            next_progress_poll = now + 0.25;
        }
        if (!poll_busy && now >= next_privacy_poll)
        {
            update_privacy_indicators();
            next_privacy_poll = now + 2.0; // poll every 2 s
        }

        bool timer_just_completed = false;
        SharedState snapshot;
        {
            std::lock_guard lock(g_state_mutex);
            snapshot = g_state;
            if (g_state.timer.active && g_state.timer.running && now >= g_state.timer.ends_at)
            {
                g_state.timer.active = false;
                g_state.timer.running = false;
                g_state.timer.just_finished = true;
                g_state.timer.finished_expires_at = now + 6.0;
                timer_just_completed = true;
            }
            if (g_state.timer.just_finished && now >= g_state.timer.finished_expires_at)
            {
                g_state.timer.just_finished = false;
            }
            snapshot.timer = g_state.timer;
            if (g_state.clipboard.active && now >= g_state.clipboard.expires_at)
            {
                g_state.clipboard.active = false;
                snapshot.clipboard.active = false;
            }
            if (g_state.notification.active && now >= g_state.notification.expires_at)
            {
                g_state.notification.active = false;
                snapshot.notification.active = false;
            }
            if (g_state.volume.active && now >= g_state.volume.expires_at)
            {
                g_state.volume.active = false;
                snapshot.volume.active = false;
            }
            if (g_state.brightness.active && now >= g_state.brightness.expires_at)
            {
                g_state.brightness.active = false;
                snapshot.brightness.active = false;
            }
            if (g_state.caps_lock.active && now >= g_state.caps_lock.expires_at)
            {
                g_state.caps_lock.active = false;
                snapshot.caps_lock.active = false;
            }
            if (g_state.battery.active && now >= g_state.battery.expires_at)
            {
                g_state.battery.active = false;
                snapshot.battery.active = false;
            }
            if (g_state.device.active && now >= g_state.device.expires_at)
            {
                g_state.device.active = false;
                snapshot.device.active = false;
            }
        }
        if (timer_just_completed)
        {
            trigger_nudge();
        }

        if (g_lookup_ui.open &&
            (!g_settings.quick_lookup || !snapshot.lookup.active || now >= snapshot.lookup.expires_at))
        {
            close_lookup_panel(hwnd);
        }

        const std::vector<IslandKind> kinds = choose_activities(snapshot, g_settings, now);
        Activity primary = activity_for_kind(kinds[0], g_settings, snapshot);
        std::optional<Activity> secondary;
        if (kinds.size() >= 2)
        {
            secondary = activity_for_kind(kinds[1], g_settings, snapshot);
        }

        const bool pinned = g_pinned_expanded.load();

        if (primary.kind != previous_primary)
        {
            if (primary.kind != IslandKind::Idle)
            {
                nudge_spring.value = -6.0f;
                nudge_spring.velocity = 0.0f;
                nudge_spring.target = 0.0f;
            }
            last_interaction_time = now;
        }
        previous_primary = primary.kind;

        POINT cursor = {};
        GetCursorPos(&cursor);

        // Pill rectangle in screen space, from the same layout render uses (the window is a fixed stage).
        const bool flush_top_now = g_settings.notch_style || g_settings.border_merged_mode;
        const StageLayout hover_stage = compute_stage_layout(g_settings, width_spring.value, height_spring.value);
        const float hover_top =
            hover_stage.pill_top + (flush_top_now ? std::max(0.0f, nudge_spring.value) : nudge_spring.value);
        RECT pill_rect = {hover_stage.origin.x + static_cast<int>(std::lround(hover_stage.pill_left)),
                          hover_stage.origin.y + static_cast<int>(std::lround(hover_top)),
                          hover_stage.origin.x +
                              static_cast<int>(std::lround(hover_stage.pill_left + width_spring.value)),
                          hover_stage.origin.y + static_cast<int>(std::lround(hover_top + height_spring.value))};

        bool hover = false;
        if (width_spring.value > 1.0f && height_spring.value > 1.0f)
        {
            hover = PtInRect(&pill_rect, cursor) != FALSE;
        }
        else if (g_settings.unhide_on_hover)
        {
            RECT dock_rect = get_island_dock_rect();
            hover = PtInRect(&dock_rect, cursor) != FALSE;
        }

        // A file drag over the island counts as hover, so it expands onto the File Tray.
        if (g_tray_drag_over.load(std::memory_order_relaxed))
        {
            hover = true;
        }

        bool needs_render = false;

        if (!hover && g_click_expanded.load())
        {
            g_click_expanded = false;
            needs_render = true;
        }
        if (!hover && g_notification_center_detail_open.load(std::memory_order_relaxed))
        {
            close_notification_center_detail();
            needs_render = true;
        }
        if (!hover && g_hovered_media_button.load() != -1)
        {
            g_hovered_media_button = -1;
            needs_render = true;
        }
        // Only the 5 s after a title change is transient, so the pill surfaces and then auto-hides again
        // while playback continues.
        const bool recent_track_change = g_settings.media_auto_expand && !media_expand_blocked(snapshot.media) &&
                                         primary.kind == IslandKind::Media && snapshot.media.playing &&
                                         !snapshot.media.title.empty() && (now - snapshot.media.title_changed_at < 5.0);

        bool is_transient_alert =
            (primary.kind == IslandKind::Clipboard || primary.kind == IslandKind::Notification ||
             primary.kind == IslandKind::Volume || primary.kind == IslandKind::Brightness ||
             primary.kind == IslandKind::BatteryLow || primary.kind == IslandKind::CapsLock ||
             primary.kind == IslandKind::Device || primary.kind == IslandKind::Bluetooth ||
             primary.kind == IslandKind::DoNotDisturb || primary.kind == IslandKind::Lookup || recent_track_change);

        const bool unhide_grace_active = (now < g_hotkey_unhide_until.load());
        const bool hover_unhides = g_settings.unhide_on_hover && hover;

        bool currently_hidden = false;
        if (!unhide_grace_active)
        {
            if (g_settings.auto_hide_idle_seconds == -1 && !is_transient_alert && !pinned)
            {
                currently_hidden = true;
            }
            else if (g_settings.auto_hide_idle_seconds > 0)
            {
                currently_hidden = (now - last_interaction_time > g_settings.auto_hide_idle_seconds);
            }
        }

        bool is_hover_expanded = g_settings.expand_on_hover ? hover : (hover && g_click_expanded.load());
        const bool game_metrics_present =
            primary.kind == IslandKind::Idle && (g_settings.game_overlay || g_game_overlay_pinned.load());
        if (game_metrics_present)
        {
            is_hover_expanded = false;
        }

        if (currently_hidden && !g_settings.unhide_on_hover)
        {
            is_hover_expanded = false;
        }
        else if (is_hover_expanded || hover_unhides || pinned || is_transient_alert || unhide_grace_active)
        {
            last_interaction_time = now;
        }

        bool is_hidden = false;
        if (!unhide_grace_active)
        {
            if (g_settings.auto_hide_idle_seconds == -1 && !is_transient_alert && !is_hover_expanded &&
                !hover_unhides && !pinned)
            {
                is_hidden = true;
            }
            else if (g_settings.auto_hide_idle_seconds > 0)
            {
                if (hover_unhides)
                {
                    is_hidden = false;
                }
                else
                {
                    is_hidden = (now - last_interaction_time > g_settings.auto_hide_idle_seconds);
                }
            }
        }

        static bool is_fullscreen = false;
        static double last_fullscreen_check = -1.0; // -1.0 guarantees the very first iteration checks
        if (now - last_fullscreen_check > 0.5)
        {
            const bool new_fullscreen = g_settings.auto_hide_fullscreen && is_foreground_fullscreen(hwnd);
            if (is_fullscreen && !new_fullscreen)
            {
                g_fullscreen_override_visible = false;
            }
            is_fullscreen = new_fullscreen;
            g_is_fullscreen.store(is_fullscreen, std::memory_order_relaxed);
            last_fullscreen_check = now;
        }

        // Reclaim the top of the z-order (rate-limited, skipped while hidden).
        static double last_topmost_check = 0.0;
        if (!is_fullscreen && !g_manually_hidden.load() && !g_auto_hidden_parked.load() &&
            now - last_topmost_check > 1.0)
        {
            ensure_topmost(hwnd, &pill_rect);
            last_topmost_check = now;
        }

        const bool mic_indicator_active =
            snapshot.system.mic_active && g_settings.privacy_dots && g_settings.privacy_dots_mic;
        const bool cam_indicator_active =
            snapshot.system.camera_active && g_settings.privacy_dots && g_settings.privacy_dots_cam;
        const bool privacy_active = mic_indicator_active || cam_indicator_active;

        // Any surface showing CPU, RAM, disk, GPU or network figures: decides both sampling the expensive
        // counters and repainting on their changes.
        const int metric_tab_idx = normalized_tab_index(g_settings);
        const bool on_hardware_monitor_tab = (metric_tab_idx == hardware_monitor_tab_index(g_settings));
        const bool hw_monitor_visible = (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) &&
                                        !is_fullscreen && !game_metrics_present && (pinned || is_hover_expanded) &&
                                        on_hardware_monitor_tab;
        const bool game_overlay_visible = game_metrics_present && !is_fullscreen;
        const bool system_metrics_visible = hw_monitor_visible || game_overlay_visible;

        if (g_settings.brightness && !poll_busy && now >= next_brightness_poll)
        {
            update_brightness_snapshot();
            next_brightness_poll = now + 0.25;
        }

        if (!poll_busy && now >= next_system_poll)
        {
            const bool need_gpu_stats = game_overlay_visible || hw_monitor_visible;
            const bool need_net_stats = hw_monitor_visible; // net is only ever drawn in the HW dashboard

            update_system_snapshot(need_gpu_stats, need_net_stats);
            if (!g_endpoint_volume)
            {
                g_volume_endpoint_stale = true;
            }
            next_system_poll = now + 1.0;
        }

        if (primary.kind == IslandKind::Idle)
        {
            if (!is_fullscreen && (pinned || is_hover_expanded))
            {
                primary.width = media_layout::expanded_width * g_settings.size_scale;
                primary.height = media_layout::expanded_height * g_settings.size_scale;
            }
            else if (primary.width > 0.0f)
            {
                primary.width =
                    renderer.measure_idle_strip(snapshot, g_settings, now).total_width * g_settings.size_scale;
            }
        }
        if (!is_fullscreen && primary.kind == IslandKind::Idle &&
            (g_settings.game_overlay || g_game_overlay_pinned.load()))
        {
            const int metric_count =
                (g_settings.game_overlay_show_cpu ? 1 : 0) + (g_settings.game_overlay_show_ram ? 1 : 0) +
                (g_settings.game_overlay_show_gpu ? 1 : 0) + (g_settings.game_overlay_show_disk ? 1 : 0);

            const bool compact = g_settings.game_overlay_compact;
            primary.width = game_overlay_layout::width(compact, g_settings.game_overlay_show_fps, metric_count) *
                            g_settings.size_scale;
            primary.height = game_overlay_layout::For(compact).height * g_settings.size_scale;
        }
        if (primary.kind == IslandKind::Media)
        {
            if (!is_fullscreen && (is_hover_expanded || pinned || recent_track_change))
            {
                primary.width = media_layout::expanded_width * g_settings.size_scale;
                primary.height = media_layout::expanded_height * g_settings.size_scale;
            }
            else
            {
                const float clock_section = g_settings.media_pill_clock
                                                ? renderer.measure_media_pill(snapshot, g_settings).section_width
                                                : 0.0f;
                if (collapsed_lyrics_active(snapshot, g_settings))
                {
                    primary.width = (collapsed_lyrics_width + clock_section) * g_settings.size_scale;
                }
                else if (g_settings.media_pill_clock)
                {
                    const float privacy_extra = snapshot.system.mic_active || snapshot.system.camera_active
                                                    ? media_pill_layout::privacy_extra_width
                                                    : 0.0f;
                    primary.width =
                        (media_pill_layout::base_width + privacy_extra + clock_section) * g_settings.size_scale;
                }
            }
        }

        if (primary.kind == IslandKind::Lookup)
        {
            secondary.reset();
            if (primary.width > 1.0f)
            {
                const Settings lookup_settings = get_settings_copy();
                primary.height = renderer.measure_lookup_card(snapshot, lookup_settings) * lookup_settings.size_scale;
            }
        }

        const bool fullscreen_suppressed =
            is_fullscreen && !g_fullscreen_override_visible.load(std::memory_order_relaxed);
        if ((is_hidden || fullscreen_suppressed) && !privacy_active && !pinned && !is_hover_expanded &&
            !is_transient_alert)
        {
            primary.width = 0.0f;
            primary.height = 0.0f;
            secondary.reset();
        }

        const bool media_waveform_visible =
            g_settings.media && snapshot.media.playing && g_settings.spectrum_style != SpectrumStyle::Bars &&
            ((primary.kind == IslandKind::Media && primary.width > 1.0f && primary.height > 1.0f) ||
             (secondary && secondary->kind == IslandKind::Media && secondary->width > 1.0f &&
              secondary->height > 1.0f));
        g_audio_capture_needed.store(media_waveform_visible, std::memory_order_relaxed);

        float target_width = primary.width;
        float target_height = primary.height;
        if (secondary)
        {
            target_width = primary.width + secondary->width + 12.0f * g_settings.size_scale;
            target_height = std::max(primary.height, secondary->height);
        }

        width_spring.target = target_width;
        height_spring.target = target_height;

        if (just_unhidden || (unhide_grace_active && width_spring.value < 0.5f && target_width > 1.0f))
        {
            width_spring.reset(target_width);
            height_spring.reset(target_height);
            nudge_spring.reset(0.0f);
        }

        const auto current_frame = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(current_frame - previous_frame).count();
        previous_frame = current_frame;
        dt = clamp(dt, 0.001f, 0.050f);

        float style_stiffness_mult = 1.0f;
        float style_damping_mult = 1.0f;
        if (g_settings.animation_style == AnimationStyle::Smooth)
        {
            style_stiffness_mult = 1.0f;
            style_damping_mult = 1.35f;
        }
        else if (g_settings.animation_style == AnimationStyle::Bouncy)
        {
            style_stiffness_mult = 1.1f;
            style_damping_mult = 0.70f;
        }
        else if (g_settings.animation_style == AnimationStyle::Snappy)
        {
            style_stiffness_mult = 1.5f;
            style_damping_mult = 1.25f;
        }

        const float speed = g_settings.animation_speed;
        float width_stiffness = 280.0f * style_stiffness_mult;
        float width_damping = 24.0f * style_damping_mult;
        if (target_width > width_spring.value)
        {
            width_stiffness = 380.0f * style_stiffness_mult;
            width_damping = 26.0f * style_damping_mult;
        }
        else if (target_width < width_spring.value)
        {
            width_stiffness = 200.0f * style_stiffness_mult;
            width_damping = 28.0f * style_damping_mult;
        }

        float height_stiffness = 280.0f * style_stiffness_mult;
        float height_damping = 24.0f * style_damping_mult;
        if (target_height > height_spring.value)
        {
            height_stiffness = 380.0f * style_stiffness_mult;
            height_damping = 26.0f * style_damping_mult;
        }
        else if (target_height < height_spring.value)
        {
            height_stiffness = 200.0f * style_stiffness_mult;
            height_damping = 28.0f * style_damping_mult;
        }

        width_spring.step(dt * speed, width_stiffness, width_damping);
        if (width_spring.value < 0.0f)
        {
            width_spring.value = 0.0f;
            width_spring.velocity = 0.0f;
        }

        height_spring.step(dt * speed, height_stiffness, height_damping);
        if (height_spring.value < 0.0f)
        {
            height_spring.value = 0.0f;
            height_spring.velocity = 0.0f;
        }

        nudge_spring.step(dt * speed, 280.0f * style_stiffness_mult, 24.0f * style_damping_mult);

        if (snapshot.volume.active)
        {
            volume_bar_spring.target = static_cast<float>(snapshot.volume.percent);
        }
        else
        {
            volume_bar_spring.reset(static_cast<float>(snapshot.system.volume_percent));
        }
        volume_bar_spring.step(dt * speed, 520.0f, 46.0f);
        snapshot.volume.display_percent = volume_bar_spring.value;

        {
            std::lock_guard lock(g_state_mutex);
            g_state.system.render_fps = clamp_int(static_cast<int>(1.0f / std::max(dt, 0.001f) + 0.5f), 0, 1000);
        }

        const bool dragging_or_hover =
            hover || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && PtInRect(&pill_rect, cursor));

        // Ctrl+hover: the island turns see-through and click-through until Ctrl is released.
        const bool ctrl_held = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool ctrl_hover_ct = hover && ctrl_held && !g_lookup_ui.open;

        set_click_through(hwnd, (primary.kind == IslandKind::Idle && !dragging_or_hover && !pinned) || ctrl_hover_ct);

        if (std::abs(width_spring.velocity) > 0.01f || std::abs(width_spring.target - width_spring.value) > 0.01f ||
            std::abs(height_spring.velocity) > 0.01f || std::abs(height_spring.target - height_spring.value) > 0.01f ||
            std::abs(nudge_spring.velocity) > 0.01f || std::abs(nudge_spring.target - nudge_spring.value) > 0.01f ||
            std::abs(volume_bar_spring.velocity) > 0.01f ||
            std::abs(volume_bar_spring.target - volume_bar_spring.value) > 0.01f)
        {
            needs_render = true;
        }

        {
            HMONITOR current_anchor = nullptr;
            get_anchor_work_rect(&current_anchor);
            const float current_dpi = get_monitor_dpi_scale(current_anchor);
            static HMONITOR s_last_anchor_mon = nullptr;
            static float s_last_dpi = 0.0f;
            if (current_anchor != s_last_anchor_mon || current_dpi != s_last_dpi)
            {
                s_last_anchor_mon = current_anchor;
                s_last_dpi = current_dpi;
                std::lock_guard lock(g_settings_mutex);
                if (g_settings.auto_dpi_scale)
                {
                    g_settings.size_scale = g_settings.base_size_scale * current_dpi;
                }
                else
                {
                    g_settings.size_scale = g_settings.base_size_scale;
                }
                g_layout_dirty = true;
            }
        }

        if (g_lookup_ui.open && primary.kind == IslandKind::Lookup)
        {
            static double s_last_lookup_paint = 0.0;
            if (now - s_last_lookup_paint >= 0.2)
            {
                s_last_lookup_paint = now;
                needs_render = true;
            }
        }

        // A tab transition keeps painting until its spring settles (without layout upkeep every frame).
        if (g_tab_animating.load(std::memory_order_relaxed))
        {
            if (tab_busy)
            {
                needs_render = true;
            }
            else
            {
                g_tab_animating.store(false, std::memory_order_relaxed);
            }
        }

        if (g_layout_dirty.load())
        {
            needs_render = true;
        }

        static bool prev_hover = false;
        static bool prev_pinned = false;
        if (hover != prev_hover || pinned != prev_pinned)
        {
            needs_render = true;
            prev_hover = hover;
            prev_pinned = pinned;
        }

        // Continuous animation (playing media bars, marquees, battery pulse) wants ~60 Hz; the pacer at the
        // bottom enforces the rate. Paused media is not continuous. The secondary pill counts too.
        const bool media_shown =
            primary.kind == IslandKind::Media || (secondary && secondary->kind == IslandKind::Media);
        const bool media_animating = media_shown && (snapshot.media.playing || is_hover_expanded || pinned);

        // Transient alert pills expire within seconds. Device, Bluetooth and DND count only with the
        // countdown bar on.
        const auto is_transient_pill = [](IslandKind kind)
        {
            return kind == IslandKind::BatteryLow || kind == IslandKind::Clipboard || kind == IslandKind::Notification;
        };
        const auto has_countdown_bar = [](IslandKind kind)
        {
            return kind == IslandKind::Device || kind == IslandKind::Bluetooth || kind == IslandKind::DoNotDisturb;
        };
        const bool countdown_bars_on = g_settings.status_countdown_progress;

        const bool continuous_animation =
            media_animating || is_transient_pill(primary.kind) || (secondary && is_transient_pill(secondary->kind)) ||
            (countdown_bars_on &&
             (has_countdown_bar(primary.kind) || (secondary && has_countdown_bar(secondary->kind))));
        if (continuous_animation)
        {
            needs_render = true;
        }

        if (snapshot.system.mic_active || snapshot.system.camera_active)
        {
            needs_render = true;
        }

        static SYSTEMTIME prev_time = {};
        const bool clock_on_screen =
            primary.kind == IslandKind::Idle || (primary.kind == IslandKind::Media && g_settings.media_pill_clock);
        if (clock_on_screen && !is_hidden)
        {
            SYSTEMTIME local = {};
            GetLocalTime(&local);
            if (local.wMinute != prev_time.wMinute)
            {
                needs_render = true;
                prev_time = local;
            }
        }

        // Text that ticks once a second (focus timer, Clock with seconds): repaint exactly when the shown
        // value changes, without asking for continuous pacing.
        int shown_second = -1;
        const bool timer_shown =
            primary.kind == IslandKind::Timer || (secondary && secondary->kind == IslandKind::Timer);
        if (timer_shown && snapshot.timer.running)
        {
            shown_second = static_cast<int>(std::ceil(snapshot.timer.ends_at - now));
        }
        else if (clock_on_screen && g_settings.show_seconds && !is_hidden)
        {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            shown_second = st.wSecond;
        }
        static int s_prev_shown_second = -1;
        if (shown_second != s_prev_shown_second)
        {
            s_prev_shown_second = shown_second;
            needs_render = true;
        }

        static uint64_t prev_art_gen = 0;
        static uint64_t prev_src_icon_gen = 0;
        static uint64_t prev_notif_icon_gen = 0;
        static uint64_t prev_clip_icon_gen = 0;
        static int prev_cpu = -1;
        static int prev_ram = -1;
        static int prev_disk = -1;
        static int prev_vol = -1;
        static int prev_bright = -1;
        static bool prev_muted = false;
        static int prev_bat = -1;
        static bool prev_charging = false;
        static int prev_prog = -1;
        static std::wstring prev_media_title;
        static bool prev_playing = false;

        // CPU, RAM and disk are compared only while something shows them (CPU changes on every sample).
        const bool system_metrics_changed = system_metrics_visible && (snapshot.system.cpu_percent != prev_cpu ||
                                                                       snapshot.system.memory_percent != prev_ram ||
                                                                       snapshot.system.disk_free_percent != prev_disk);

        if (snapshot.media.art_generation != prev_art_gen ||
            snapshot.media.source_icon_generation != prev_src_icon_gen || snapshot.media.title != prev_media_title ||
            snapshot.media.playing != prev_playing || snapshot.notification.icon->generation != prev_notif_icon_gen ||
            snapshot.clipboard.app_icon->generation != prev_clip_icon_gen || system_metrics_changed ||
            snapshot.system.volume_percent != prev_vol || snapshot.system.volume_muted != prev_muted ||
            snapshot.brightness.percent != prev_bright || snapshot.battery.percent != prev_bat ||
            snapshot.battery.charging != prev_charging || snapshot.progress.percent != prev_prog)
        {
            needs_render = true;
            prev_art_gen = snapshot.media.art_generation;
            prev_src_icon_gen = snapshot.media.source_icon_generation;
            prev_media_title = snapshot.media.title;
            prev_playing = snapshot.media.playing;
            prev_notif_icon_gen = snapshot.notification.icon->generation;
            prev_clip_icon_gen = snapshot.clipboard.app_icon->generation;
            prev_cpu = snapshot.system.cpu_percent;
            prev_ram = snapshot.system.memory_percent;
            prev_disk = snapshot.system.disk_free_percent;
            prev_vol = snapshot.system.volume_percent;
            prev_muted = snapshot.system.volume_muted;
            prev_bright = snapshot.brightness.percent;
            prev_bat = snapshot.battery.percent;
            prev_charging = snapshot.battery.charging;
            prev_prog = snapshot.progress.percent;
        }

        static bool prev_ctrl_hover_ct = false;
        if (ctrl_hover_ct != prev_ctrl_hover_ct)
        {
            needs_render = true;
            prev_ctrl_hover_ct = ctrl_hover_ct;
        }

        if (needs_render)
        {
            Settings render_settings = get_settings_copy();
            if (ctrl_hover_ct)
            {
                render_settings.pill_opacity = clamp(render_settings.pill_opacity * 0.35f, 0.15f, 0.45f);
            }
            else if (render_settings.theme_preset == ThemePreset::Graphite && g_pill_opacity_override.load() < 0)
            {
                const bool is_expanded =
                    is_hover_expanded || pinned || is_transient_alert || (width_spring.value > 260.0f);
                render_settings.pill_opacity = is_expanded ? 0.98f : 0.88f;
            }
            if (render_settings.backdrop_material != BackdropMaterial::None && !dwm_backdrop::available())
            {
                render_settings.backdrop_material = BackdropMaterial::None;
            }
            renderer.render(snapshot, render_settings, primary, secondary, width_spring.value, height_spring.value,
                            nudge_spring.value, hover, pinned, now);
        }

        // Zero-CPU parking for auto-hide: only after the collapse animation settled at 0.
        const bool wants_auto_hidden_park =
            !g_manually_hidden.load() && !pinned && !is_hover_expanded && !is_transient_alert && !privacy_active &&
            (is_hidden || fullscreen_suppressed) && width_spring.value < 0.5f && height_spring.value < 0.5f &&
            std::fabs(width_spring.velocity) < 0.5f && std::fabs(height_spring.velocity) < 0.5f;

        if (wants_auto_hidden_park)
        {
            parked_for_fullscreen = fullscreen_suppressed;
            was_auto_hidden_parked = true;
            g_auto_hidden_parked = true;
            ShowWindow(hwnd, SW_HIDE);
            dwm_backdrop::hide_all();
            g_audio_capture_needed.store(false, std::memory_order_relaxed);
            set_high_res_timer(false);
            continue;
        }

        int target_fps = g_settings.target_fps;
        if (target_fps <= 0)
        {
            static int s_cached_rate = 60;
            static double s_rate_checked_at = -1e9;
            if (now - s_rate_checked_at > 2.0)
            {
                s_cached_rate = get_monitor_refresh_rate(hwnd);
                s_rate_checked_at = now;
            }
            target_fps = s_cached_rate;
        }
        target_fps = clamp_int(target_fps, 30, 1000);
        double target_frame_ms = 1000.0 / static_cast<double>(target_fps);

        // High Target FPS only for spring motion; continuous content relaxes to 60 Hz (never above the user's
        // lower setting).
        const bool springs_animating =
            std::fabs(width_spring.value - width_spring.target) > 0.5f ||
            std::fabs(height_spring.value - height_spring.target) > 0.5f || std::fabs(width_spring.velocity) > 0.5f ||
            std::fabs(height_spring.velocity) > 0.5f || std::fabs(nudge_spring.value) > 0.5f ||
            std::fabs(nudge_spring.velocity) > 0.5f ||
            std::fabs(volume_bar_spring.value - volume_bar_spring.target) > 0.5f ||
            std::fabs(volume_bar_spring.velocity) > 0.5f || g_tab_animating.load(std::memory_order_relaxed);

        if (continuous_animation && !springs_animating)
        {
            constexpr double continuous_frame_ms = 1000.0 / 60.0;
            target_frame_ms = std::max(target_frame_ms, continuous_frame_ms);
        }

        // 1 ms timer resolution follows actual animation (continuous content or springs), not single repaints.
        if (continuous_animation || springs_animating)
        {
            last_animating_at = now;
        }
        set_high_res_timer(last_animating_at >= 0.0 && now - last_animating_at < high_res_timer_hold_sec);

        if (!needs_render)
        {
            // Nothing changed: poll at ~60 Hz.
            WaitForSingleObject(g_stop_event, 16);
            next_frame_target = std::chrono::steady_clock::now();
        }
        else if (springs_animating && g_settings.target_fps <= 0 && SUCCEEDED(DwmFlush()))
        {
            // Auto FPS while resizing: wait for the next DWM composition so frames land on vsync.
            next_frame_target = std::chrono::steady_clock::now();
        }
        else
        {
            next_frame_target += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(target_frame_ms));

            auto now_time = std::chrono::steady_clock::now();
            if (now_time < next_frame_target)
            {
                double remaining_ms = std::chrono::duration<double, std::milli>(next_frame_target - now_time).count();
                if (remaining_ms >= 1.5)
                {
                    WaitForSingleObject(g_stop_event, static_cast<DWORD>(remaining_ms - 0.5));
                }
                // Spinning pays off only for fast springs on high refresh rates.
                if (springs_animating && target_frame_ms < 10.0)
                {
                    while (std::chrono::steady_clock::now() < next_frame_target &&
                           WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
                    {
                        std::this_thread::yield();
                    }
                }
            }
            else
            {
                next_frame_target = now_time;
            }
        }
    }

    set_high_res_timer(false);

    unbind_volume_endpoint();
    dwm_backdrop::destroy_all();
    renderer.shutdown();
    DestroyWindow(hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(window_class, wc.hInstance);

    if (SUCCEEDED(hr_co))
    {
        CoUninitialize();
    }

    return 0;
}

}
