#pragma once

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

WeatherVisual weather_visual_from_code(int code);

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

bool notification_center_unread(const Settings& settings);

float idle_dot_reserve(const IdleStripMetrics& metrics);

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

std::wstring widest_digit_form(std::wstring text);

// Collapsed pill lyrics
constexpr float collapsed_lyrics_width = 320.0f;

bool collapsed_lyrics_active(const SharedState& state, const Settings& settings);

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
    bool initialize(HWND hwnd);
    bool render(const SharedState& state, const Settings& settings, const Activity& primary,
                const std::optional<Activity>& secondary, float width, float height, float nudge, bool hover,
                bool pinned, double now);
    void shutdown();
    IdleStripMetrics measure_idle_strip(const SharedState& state, const Settings& settings, double now);
    MediaPillMetrics measure_media_pill(const SharedState& state, const Settings& settings);

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
    float measure_text_width_cached(const std::wstring& text, IDWriteTextFormat* fmt, TextWidthCache& cache);
    bool create_backing_bitmap(int width, int height);

    float last_font_scale_ = 0.0f;
    float last_text_scale_ = 0.0f;
    std::wstring last_font_family_;
    void ensure_text_formats(float scale, const std::wstring& font_family, float text_scale);
    void ensure_brushes(const Settings& settings, const SharedState& state, double now);

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

    static D2D1_COLOR_F mix_color(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
    static D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float alpha);

    // Cached Direct2D resources: nothing below is created per frame once warm. render thread only.
    struct LinearGradientSlot
    {
        ComPtr<ID2D1LinearGradientBrush> brush;
        std::uint64_t key = 0;
    };

    struct RadialGradientSlot
    {
        ComPtr<ID2D1RadialGradientBrush> brush;
        std::uint64_t key = 0;
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
        std::uint64_t last_used = 0;
    };

    static constexpr std::size_t shape_cache_size = 32;
    static std::uint32_t pack_color8(const D2D1_COLOR_F& c);
    static std::uint64_t hash_stops(const D2D1_GRADIENT_STOP* s, UINT32 n);
    void begin_frame_resources();
    ID2D1SolidColorBrush* acquire_brush(const D2D1_COLOR_F& color);
    ID2D1Layer* acquire_layer();
    void set_persistent_brush(ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& c);
    ID2D1LinearGradientBrush* use_linear_gradient(LinearGradientSlot& slot, const D2D1_GRADIENT_STOP* stops,
                                                  UINT32 count, D2D1_POINT_2F start, D2D1_POINT_2F end);
    ID2D1RadialGradientBrush* use_radial_gradient(RadialGradientSlot& slot, const D2D1_GRADIENT_STOP* stops,
                                                  UINT32 count, D2D1_POINT_2F center, float rx, float ry);
    ComPtr<ID2D1Geometry> get_shape_geometry(D2D1_RECT_F rect, float radius, bool notch);
    void release_cached_resources();
    void build_material_tokens(const Settings& settings);
    void draw_pill(const SharedState& state, const Settings& settings, const Activity& activity, D2D1_RECT_F rect,
                   float scale, double now);
    void draw_soft_shadow(D2D1_RECT_F rect, float radius);
    void fill_surface_depth(D2D1_RECT_F rect, float radius, bool strong);
    void fill_accent_bloom(D2D1_RECT_F rect, float radius, float strength);
    void draw_card(D2D1_RECT_F rect, float radius, bool active = false);
    void draw_accent_track(D2D1_RECT_F track, float progress, float radius);
    void draw_privacy_dots(const SharedState& state, const Settings& settings, D2D1_RECT_F rect, double now);
    ComPtr<ID2D1PathGeometry> create_notch_geometry(D2D1_RECT_F rect, float radius);
    ComPtr<ID2D1Geometry> create_island_mask_geometry(D2D1_RECT_F rect, float radius, bool notch_style);
    void fill_island_shape(D2D1_RECT_F rect, float radius, bool w11_style, bool notch_style, ID2D1Brush* brush);
    void draw_island_shape(D2D1_RECT_F rect, float radius, bool w11_style, bool notch_style, ID2D1Brush* brush,
                           float stroke_width);
    void draw_pill_surface(D2D1_RECT_F rect, float radius, IslandKind kind, const Settings& settings);
    static void get_weather_icon_and_text(int code, std::wstring& icon, std::wstring& text);
    static int get_days_in_month(int year, int month);
    static int get_day_of_week(int year, int month, int day);
    void draw_calendar_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                                 float scale, SYSTEMTIME& local);
    std::wstring wind_dir_to_arrow(const std::wstring& dir);
    void draw_cloud_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush);
    void draw_sun_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush, float core_radius = 0.26f);
    void draw_lightning_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush);
    void draw_weather_icon(D2D1_POINT_2F center, float size, WeatherVisual visual, ID2D1Brush* strong,
                           ID2D1Brush* soft);
    void draw_metric_glyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush);
    void draw_weather_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                                float scale, bool has_weather, const std::wstring& w_icon, const std::wstring& w_text);
    void draw_tray_bin_button(D2D1_RECT_F box, bool enabled, bool hovered);
    void draw_tray_paste_button(D2D1_RECT_F box, bool hovered);
    void draw_tray_remove_button(D2D1_POINT_2F c, float size, bool hovered, bool row_hovered);
    void draw_file_tray_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale);
    std::size_t notification_center_slot(std::uint64_t generation);
    void draw_notification_center_icon(const notification_center::Group& group, D2D1_RECT_F badge, float radius);
    void draw_notification_center_back(D2D1_RECT_F box, float scale, bool hovered);
    void draw_notification_center_detail(const notification_center::Group& group, std::size_t offset, D2D1_RECT_F rect,
                                         float scale);
    void draw_notification_center_dashboard(const SharedState& state, D2D1_RECT_F rect, float scale);
    D2D1_COLOR_F load_state_color(float fraction) const;
    void draw_git_hub_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale);
    void draw_hardware_monitor_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                                         float scale);
    void draw_karaoke_letter_pop(const wchar_t* text, UINT32 text_length, IDWriteTextFormat* format,
                                 D2D1_RECT_F line_rect, float wipe_x, float base_opacity, float unsung_opacity,
                                 bool glow_enabled, float glow_strength = 1.0f);
    void draw_time_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                             float scale, SYSTEMTIME& local);
    void draw_idle_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now);
    void draw_game_overlay(const SharedState& state, D2D1_RECT_F rect, float unused_scale);
    void draw_game_metric_card(D2D1_RECT_F rect, const wchar_t* label, int percent, int glyph, float radius);
    void reset_tab_transition();

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

        constexpr float tab_stiffness = 250.0f; // higher = faster (settles in ~0.29s)
        constexpr float tab_damping = 31.0f;    // ~0.92 damping ratio: no visible overshoot
        constexpr float travel_px = 34.0f;      // vertical gap between neighbouring tabs
        constexpr float min_scale = 0.965f;     // size of a tab that is fully out of view
        constexpr float fade_power = 1.4f;      // >1 thins the mid-point slightly so text never ghosts

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
                tab_vel_ += (-tab_stiffness * (tab_pos_ - tab_target_) - tab_damping * tab_vel_) * h;
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

    void draw_tab_pagination(D2D1_RECT_F rect, int tab_count);
    void publish_content_geometry(D2D1_RECT_F rect, float right_trim = 0.0f);
    float content_island_radius(float content_height) const;
    void draw_media(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now);
    float update_immersive_mix(const Settings& settings, float height, double now);
    bool begin_immersive_fade(float alpha);
    void end_immersive_fade(bool began);
    void draw_immersive_lyrics_button(D2D1_RECT_F rect, float mix);
    void ensure_immersive_format(const Settings& settings);
    void draw_immersive_lyrics_view(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                                    float mix);
    void build_side_lyric_line(CollapsedLyricLine& out, const std::wstring& text, float width, float max_height,
                               const Settings& settings);
    void draw_media_side_lyrics(const SharedState& state, D2D1_RECT_F area, const Settings& settings, double now);
    void build_collapsed_lyric_line(CollapsedLyricLine& out, const std::wstring& text, IDWriteTextFormat* fmt);
    void draw_collapsed_lyrics(const SharedState& state, D2D1_RECT_F area, double now, float alpha);

    // Source switch animation and source dock
    enum
    {
        KSwapNone = 0,
        KSwapOut,
        KSwapHold,
        KSwapIn
    };

    void update_source_swap(const SharedState& state, double now, float* alpha, float* dx);
    void cancel_media_change_anims();
    bool begin_swap_group(float alpha, float dx);
    void end_swap_group(bool began);
    void draw_source_line(const MediaSnapshot& media, D2D1_RECT_F area, double now);
    void draw_source_dock(const SharedState& state, D2D1_RECT_F rect, double now);
    void update_media_button_animations(double now, bool playing);

    // Skip icon: two rounded triangles with a depth swap animation
    static constexpr float skip_tri_w = 0.626f; // triangle width / height
    static constexpr float skip_pitch = 0.451f;
    HRESULT build_rounded_triangle(float x0, float w, float h, ComPtr<ID2D1PathGeometry>& out);
    bool ensure_skip_geometry();
    static float skip_smooth(float x);
    static float skip_ease_in_out(float x);
    void draw_skip_icon(D2D1_POINT_2F center, bool forward, float p, float opacity, float press);
    void draw_media_controls(bool playing, D2D1_POINT_2F prev, D2D1_POINT_2F play, D2D1_POINT_2F next, double now);
    void draw_media_button(D2D1_POINT_2F center, float radius, int kind, bool primary);

    // Play/pause morph: both icons are two quads with matching vertices.
    static constexpr float play_pause_corner = 1.5f;

    struct PlayPauseShapes
    {
        D2D1_POINT_2F pause[8];
        D2D1_POINT_2F play[8];
    };

    static const PlayPauseShapes& get_play_pause_shapes();
    bool ensure_round_join_style();
    void draw_play_pause_icon(D2D1_POINT_2F center, float opacity, float press);
    static bool title_needs_shaping(wchar_t c);
    static float title_scroll_offset(const TitleScroll& sc, double now);
    bool build_title_glyphs(const std::wstring& text, float wrap_height, float available, double now, bool all_layouts,
                            std::vector<TitleGlyph>& out, TitleScroll& scroll);
    static void match_title_glyphs(std::vector<TitleGlyph>& a, std::vector<TitleGlyph>& b);
    void start_title_transition(const std::wstring& old_text, const std::wstring& new_text, D2D1_RECT_F rect,
                                double now);
    void draw_smoke_glyph(const TitleGlyph& g, D2D1_POINT_2F origin, float t, float amp, ID2D1Brush* brush,
                          float base_opacity);
    bool draw_title_transition(const std::wstring& text, D2D1_RECT_F rect, ID2D1Brush* brush, double now,
                               bool real_title);
    void draw_styled_progress(ProgressStyle style, float bar_left, float bar_right, float cy, float progress,
                              bool dragging, bool playing, double now);
    void draw_swap_marquee(SwapTextState& st, const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                           ID2D1Brush* brush, double now, float speed, MarqueeLayoutCache& cache, float delay);
    std::uint64_t art_content_hash(const MediaSnapshot& media);
    void draw_art_face(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius);
    void draw_album_art_flip(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius, bool draw_badge);
    void ensure_art_bitmap(const MediaSnapshot& media);
    void draw_art_disc(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius, float scale, float angle, float lag,
                       float alpha);
    void draw_art_landing_fx(D2D1_RECT_F rect, float radius, float ring_p, float sheen_p, float dir);
    void draw_album_art_drop(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius);
    float spec_band_range(const SharedState& state, float from, float to) const;
    void draw_spectrum(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                       bool playing = true);
    void draw_spectrum_orb(const SharedState& state, D2D1_RECT_F rect);
    void draw_spectrum_plasma(const SharedState& state, D2D1_RECT_F rect);
    void draw_spectrum_led(const SharedState& state, D2D1_RECT_F rect, float dt);
    void draw_level_banner(D2D1_RECT_F rect, const wchar_t* glyph, const std::wstring& label, const wchar_t* value,
                           float pct, bool drained);
    void draw_brightness(const SharedState& state, D2D1_RECT_F rect);
    void draw_album_art(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius = 9.0f,
                        bool draw_badge = true);
    void draw_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F rect, ComPtr<ID2D1Bitmap>& cache,
                            std::uint64_t& cached_generation, float opacity = 1.0f);
    void draw_rounded_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F badge, float corner_radius,
                                    ComPtr<ID2D1Bitmap>& cache, std::uint64_t& cached_generation, float opacity = 1.0f);
    void draw_cover_fit_bitmap_pixels(const BitmapPixels& pixels, D2D1_RECT_F badge, float corner_radius,
                                      ComPtr<ID2D1Bitmap>& cache, std::uint64_t& cached_generation,
                                      float opacity = 1.0f);
    void draw_circular_bitmap_pixels(const BitmapPixels& pixels, D2D1_POINT_2F center, float radius,
                                     ComPtr<ID2D1Bitmap>& cache, std::uint64_t& cached_generation,
                                     float opacity = 1.0f);
    void draw_marquee_text(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format, ID2D1Brush* brush,
                           double now, float speed, MarqueeLayoutCache& cache);
    float playing_indicator_level(int bar, double now) const;
    float playing_indicator_glow(double now) const;
    void draw_playing_indicator(D2D1_RECT_F rect, bool playing, double now);
    void draw_countdown_progress(float left, float right, float bottom, float progress);
    void draw_clipboard(const SharedState& state, D2D1_RECT_F rect);
    void draw_notification(const SharedState& state, D2D1_RECT_F rect);
    void draw_volume(const SharedState& state, D2D1_RECT_F rect);
    void draw_timer(const SharedState& state, D2D1_RECT_F rect);
    void draw_caps_lock(const SharedState& state, D2D1_RECT_F rect);
    void draw_device(const SharedState& state, D2D1_RECT_F rect);
    void draw_do_not_disturb(const SharedState& state, D2D1_RECT_F rect);
    void draw_bluetooth_category_icon(D2D1_RECT_F badge, BluetoothDeviceCategory category);
    void draw_bluetooth_device(const SharedState& state, D2D1_RECT_F rect);

    // Quick lookup card
public:
    float measure_lookup_card(const SharedState& state, const Settings& settings);

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

    float lookup_grow(const Settings& settings) const;
    float lookup_card_height(const LookupSnapshot& lk, const LookupFit& fit, float grow) const;
    void ensure_lookup_formats(const Settings& settings);
    std::wstring fit_text_to_lines(const std::wstring& text, IDWriteTextFormat* fmt, float width, int max_lines,
                                   float* out_height);
    const LookupFit& ensure_lookup_fit(const LookupSnapshot& lk, const Settings& settings);
    ComPtr<IDWriteTextLayout> make_line_layout(const std::wstring& text, IDWriteTextFormat* fmt, float max_width,
                                               DWRITE_TEXT_METRICS* metrics, bool ellipsis = false);
    ID2D1StrokeStyle* round_stroke();
    void draw_lookup_glyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush);
    void draw_lookup_field(D2D1_RECT_F rect, double now);
    void draw_lookup_rows(D2D1_RECT_F rect, const std::vector<LookupRow>& rows, bool header);
    void draw_lookup_status(D2D1_RECT_F rect, const LookupSnapshot& lk);
    void draw_lookup_result(const SharedState& state, D2D1_RECT_F rect, const Settings& settings);
    void draw_lookup(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now);

    ComPtr<ID2D1StrokeStyle> round_stroke_;
    ComPtr<IDWriteTextFormat> lookup_body_format_;
    ComPtr<IDWriteTextFormat> lookup_example_format_;
    float lookup_format_scale_ = -1.0f;
    std::wstring lookup_format_family_;
    LookupFit lookup_fit_;
    void draw_battery(const SharedState& state, D2D1_RECT_F rect);
    void draw_progress(const SharedState& state, D2D1_RECT_F rect);
    void draw_progress_ring(D2D1_RECT_F rect, int percent);

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
    std::uint64_t art_generation_ = 0;
    SYSTEMTIME calendar_cached_date_{};
    std::wstring calendar_cached_month_name_;
    std::wstring calendar_cached_weekday_name_;
    std::array<std::wstring, 7> calendar_cached_initials_; // indexed 0 = Sunday
    int calendar_cached_first_day_ = 0;                    // resolved, 0 = Sunday
    int calendar_cached_first_day_setting_ = -2;           // -2 forces the first fill
    ComPtr<IDWriteTextFormat> weather_desc_format_;
    float weather_desc_format_size_ = -1.0f;
    std::uint64_t notification_icon_generation_ = 0;
    std::uint64_t media_source_icon_generation_ = 0;
    std::uint64_t clipboard_icon_generation_ = 0;
    std::uint64_t clipboard_image_generation_ = 0;
    ComPtr<ID2D1Bitmap> file_tray_icon_bitmap_;
    std::uint64_t file_tray_icon_generation_ = 0;
    std::array<ComPtr<ID2D1Bitmap>, notification_center::max_apps> notification_center_bitmaps_;
    std::array<std::uint64_t, notification_center::max_apps> notification_center_generations_{};
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
    std::uint64_t art_flip_hash_ = 0;
    std::uint64_t art_hash_gen_ = 0;
    std::uint64_t art_hash_value_ = 0;
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
    std::uint64_t pill_art_hash_ = 0;
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
    std::size_t spec_key_ = 0;
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
    std::uint64_t dock_icon_gen_[media_layout::dock_max_slots] = {};
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
    std::size_t brush_pool_used_ = 0;
    std::vector<ComPtr<ID2D1Layer>> layer_pool_;
    std::size_t layer_pool_used_ = 0;
    ComPtr<ID2D1SolidColorBrush> raised_brush_;
    ComPtr<ID2D1SolidColorBrush> raised_strong_brush_;
    LinearGradientSlot depth_grad_;
    LinearGradientSlot track_grad_;
    RadialGradientSlot bloom_grad_;
    ShapeEntry shape_cache_[shape_cache_size];
    std::uint64_t shape_clock_ = 0;
};

}
