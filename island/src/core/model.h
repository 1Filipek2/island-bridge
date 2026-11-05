#pragma once

#include "core/common.h"
#include "notifications/notification_center.h"
#include "core/pixels.h"

namespace island
{

// The conversion and hit-test helpers live further down, just after Clamp().

enum class IslandKind
{
    Idle,
    Media,
    Progress,
    Clipboard,
    Notification,
    Volume,
    Brightness,
    BatteryLow,
    CapsLock,
    Device,
    Bluetooth,
    Timer,
    DoNotDisturb,
    Split,
    Lookup,
};

enum class BluetoothDeviceCategory
{
    Headphones,
    Speaker,
    Mouse,
    Keyboard,
    Phone,
    Generic,
};

enum class Position
{
    TopCenter,
    TopLeft,
    TopRight,
    BottomCenter,
    BottomLeft,
    BottomRight,
};

// Anything anchored to the bottom edge shares the same vertical placement and
// cannot use the top-edge macOS notch shape.
constexpr bool is_bottom_position(Position position)
{
    return position == Position::BottomCenter || position == Position::BottomLeft || position == Position::BottomRight;
}

enum class AccentMode
{
    Auto,
    System,
    Custom,
};

enum class AnimationStyle
{
    Smooth,
    Default,
    Bouncy,
    Snappy,
};

enum class ProgressStyle
{
    Slim,
    Wavy,
    Squiggle,
    Bar,
};

enum class SpectrumStyle
{
    Bars,
    Orb,
    Plasma,
    Led,
};

constexpr int spectrum_bands = 24; // log-spaced bands from SpectrumAnalyzer, stored in SharedState::bands

enum class CalendarAccentMode
{
    Red,
    Accent, // persisted as "red", from when it was a hardcoded red
    System,
};

// Curated palettes. These replace the original four (OLED Black, Fluent,
// Midnight Blue, Deep Purple), which were built for a material that painted a
// glass highlight along every edge. With the edge lighting gone, a palette has
// to carry the whole look on flat fills, so each of these is tuned for
// separation by value alone -- and one is light, which the old set had none of.
//
// Order is load-bearing: it is persisted as an integer index, so append new
// entries at the end rather than inserting. Custom must stay last.
enum class ThemePreset
{
    Obsidian,
    Graphite,
    Slate,
    Nord,
    Evergreen,
    Espresso,
    Plum,
    Porcelain,
    Custom,
};

// One table drives the settings dropdown, the right-click menu labels and the
// resolved colors. It used to be three separate lists, which is how the menu
// ended up carrying special cases for palette indexes that no longer matched.
//
// Secondary text is held at roughly 4.5:1 against its own background in every
// row, because no edge highlight is left to help muted labels separate from the
// surface behind them.
struct ThemePalette
{
    const wchar_t* id;    // value stored in Themes.ThemePreset
    const wchar_t* label; // right-click menu label
    const wchar_t* bg;
    const wchar_t* fg;
    const wchar_t* sec;
    const wchar_t* border;
};

static constexpr ThemePalette theme_palettes[] = {
    {L"obsidian", L"Obsidian (Default)", L"#08080A", L"#FFFFFF", L"#9B9BA5", L"#1E1E22"},
    {L"graphite", L"Graphite", L"#1C1C1E", L"#FFFFFF", L"#A8A8AE", L"#323236"},
    {L"slate", L"Slate", L"#111721", L"#E9EEF6", L"#92A2B8", L"#232C3A"},
    {L"nord", L"Nord", L"#2E3440", L"#ECEFF4", L"#A7B0C0", L"#3B4252"},
    {L"evergreen", L"Evergreen", L"#0C1512", L"#E4F1EA", L"#8CAE9D", L"#1A2A23"},
    {L"espresso", L"Espresso", L"#1A1512", L"#F6EEE7", L"#B7A395", L"#2E2420"},
    {L"plum", L"Plum", L"#16111C", L"#F1EAF6", L"#AC9BB9", L"#281F33"},
    {L"porcelain", L"Porcelain (Light)", L"#F5F6F8", L"#14161A", L"#5B6270", L"#D8DBE1"},
};

// Custom sits one past the last real palette. Derived, so adding a palette
// cannot leave a stale literal behind.
static constexpr int custom_theme_index = static_cast<int>(ARRAYSIZE(theme_palettes));

// Persisted under a new key. Retiring the original palettes renumbered these
// integers, and reusing "ColorTheme" would have silently reinterpreted a saved
// 4 (previously Custom) as the palette that now sits at index 4.
static constexpr const wchar_t* theme_value_name = L"ColorThemeV2";
static constexpr const wchar_t* legacy_theme_value_name = L"ColorTheme";

// Base menu command id for the theme submenu; entries occupy
// [kThemeMenuIdBase, kThemeMenuIdBase + kCustomThemeIndex].
static constexpr UINT theme_menu_id_base = 20;

// Maps a preset id to its palette index, accepting the retired ids too: a
// user's stored setting keeps its old string after the option disappears from
// the dropdown, so each legacy id is pointed at the new palette closest to it.
inline int theme_index_from_id(std::wstring_view id)
{
    auto iequals = [](std::wstring_view a, std::wstring_view b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (towlower(a[i]) != towlower(b[i]))
            {
                return false;
            }
        }
        return true;
    };

    for (int i = 0; i < custom_theme_index; ++i)
    {
        if (iequals(id, theme_palettes[i].id))
        {
            return i;
        }
    }
    if (iequals(id, L"custom"))
    {
        return custom_theme_index;
    }

    if (iequals(id, L"oled-black"))
    {
        return 0; // -> Obsidian
    }
    if (iequals(id, L"fluent") || iequals(id, L"mica") || iequals(id, L"dark-gray"))
    {
        return 1; // -> Graphite
    }
    if (iequals(id, L"midnight-blue"))
    {
        return 2; // -> Slate
    }
    if (iequals(id, L"deep-purple"))
    {
        return 6; // -> Plum
    }
    return -1;
}

// Real Windows blur / acrylic painted behind the island (#59).
enum class BackdropMaterial
{
    None,
    Blur,
    Acrylic,
};

enum class ContourBorderMode
{
    Default,
    Auto,
    Borderless,
};

struct Settings
{
    Position position = Position::TopCenter;
    int target_monitor = 0;
    int offset_x = 0;
    int offset_y = 0;
    bool separate_expanded_offset_y = false; // #83
    int offset_y_expanded = 0;               // #83
    float size_scale = 1.0f;
    float base_size_scale = 1.0f;
    std::wstring font_family;
    AccentMode accent_mode = AccentMode::Auto;
    D2D1_COLOR_F custom_accent = D2D1::ColorF(0x4cc9f0);
    int target_fps = 0; // 0 = Auto
    AnimationStyle animation_style = AnimationStyle::Default;
    ProgressStyle progress_style = ProgressStyle::Slim;
    SpectrumStyle spectrum_style = SpectrumStyle::Bars;
    bool expanded_media_anim = false; // hover player: text/cover flip/controls transitions
    bool pill_cover_anim = false;     // collapsed pill: cover spin/drop
    float animation_speed = 1.0f;
    bool media = true;
    bool media_auto_expand = false;
    bool clipboard = true;
    bool status_countdown_progress = false;
    bool battery = true;
    bool progress = true;
    bool volume = true;
    bool brightness = true;
    CalendarAccentMode calendar_accent = CalendarAccentMode::Accent;
    bool privacy_dots = true;
    bool privacy_dots_mic = true;
    bool privacy_dots_cam = true;
    bool privacy_dots_pulse = true;
    // Modules.CapsLock -- fixes windhawk-mods#4352, which asked for a way to turn
    // the Caps Lock / Num Lock indicator off.
    //
    // Enforced in three places, because any one of them alone leaks:
    //   ChooseActivities        - the pill is never selected for display
    //   WM_APP_CAPSLOCK handler - no state is recorded, no nudge is triggered
    //   CapsLockHookWanted      - WH_KEYBOARD_LL is not installed at all
    // The activity gate is the one that actually hides the pill; the others stop
    // the work leading up to it.
    bool caps_lock = true;
    bool timer_enabled = true;
    bool hide_show_hotkey_enabled = true;
    UINT hide_show_modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    UINT hide_show_vk = 'D';

    // Quick Lookup: hotkey-triggered dictionary / Wikipedia card.
    bool quick_lookup = false;
    UINT lookup_modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    UINT lookup_vk = VK_SPACE;
    bool bluetooth_indicator = true;
    bool bluetooth_show_battery = true;
    D2D1_COLOR_F privacy_dots_mic_hex = D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f);     // #FF9500
    D2D1_COLOR_F privacy_dots_cam_hex = D2D1::ColorF(0.133f, 0.776f, 0.239f, 1.0f); // #10B981
    float tint_opacity = 0.72f;
    float pill_opacity = 0.96f;
    bool game_overlay = false;
    bool show_metric_text = true;
    bool weather = true;
    std::wstring weather_city;
    bool weather_fahrenheit = false;
    bool lyrics_visualizer = true;
    bool karaoke_lyrics = true;
    bool karaoke_letter_glow = true;
    bool lyrics_neighbor_lines_visible = false;
    int lyrics_offset_ms = 0;      // Modules.LyricsOffsetMs, speakers / wired, ms
    int lyrics_offset_bt_ms = 0;   // Modules.LyricsOffsetBluetoothMs, Bluetooth output, ms
    bool collapsed_lyrics = false; // lyrics inside the collapsed media pill (right-click toggle)
    bool lyrics_cache = true;      // Modules.LyricsCache
    int lyrics_cache_max_mb = 25;  // Modules.LyricsCacheMaxMB; 0 = disk cache off
    bool media_tab_lyrics = false; // lyrics side panel in the expanded media tab
    int auto_hide_idle_seconds = 0;
    bool auto_hide_fullscreen = true;
    bool border_merged_mode = false;
    bool unhide_on_hover = true;
    bool always_on_top = true;
    bool expand_on_hover = true;
    bool auto_dpi_scale = true;
    bool w11_style = false;
    bool notch_style = false;
    // Color customization
    ThemePreset theme_preset = ThemePreset::Obsidian;
    D2D1_COLOR_F pill_bg_color = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f);        // #08080A
    D2D1_COLOR_F text_primary_color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);         // #FFFFFF
    D2D1_COLOR_F text_secondary_color = D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f); // #9B9BA5
    ContourBorderMode contour_border_mode = ContourBorderMode::Default;
    bool contour_border_enabled = true;
    D2D1_COLOR_F contour_border_color = D2D1::ColorF(0.200f, 0.200f, 0.220f, 1.0f); // #333338
    bool clock_accent_glow = true;
    bool privacy_dots_enabled = true;
    bool privacy_dot_pulsing = true;
    D2D1_COLOR_F mic_dot_color = D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f);     // #FF9500
    D2D1_COLOR_F cam_dot_color = D2D1::ColorF(0.204f, 0.780f, 0.349f, 1.0f); // #34C759
    bool hardware_monitor_module = true;
    bool github_module = true;
    bool do_not_disturb_indicator = true;
    bool notification_respect_dn_d = true;

    // ── Premium material / redesign ──────────────────────────────────────────
    bool material_depth = true;                                  // downward depth shading + accent bloom
    bool drop_shadow = true;                                     // soft shadow under the island
    float accent_bloom = 1.0f;                                   // 0..2 multiplier on the accent wash
    float text_scale = 1.0f;                                     // independent typography scale (#41)
    BackdropMaterial backdrop_material = BackdropMaterial::None; // #59
    float backdrop_tint = 0.55f;                                 // acrylic tint strength, 0..1
    float backdrop_fill_alpha = 0.45f;                           // how opaque the pill's own fill stays

    // ── Clock / date presentation (#61) ──────────────────────────────────────
    bool show_seconds = false;
    bool use24_hour_clock = false; // false = follow system locale
    bool clock_follow_system = true;
    std::wstring date_format;      // empty = locale default
    bool media_pill_clock = false; // show the clock inside the collapsed media pill
    bool date_first = false;
    int first_day_of_week = -1; // 0 = Sunday .. 6 = Saturday, -1 = follow Windows

    // ── Localization (#35) ───────────────────────────────────────────────────
    std::wstring language = L"auto";

    // ── File tray (#33) ──────────────────────────────────────────────────────
    bool file_tray_module = false;
    int file_tray_max_items = 10;
    bool file_tray_persist = true; // Modules.FileTrayPersist
    bool notification_center_module = true;

    // ── Media auto-expand exclusions (#62) ───────────────────────────────────
    std::vector<std::wstring> media_expand_blocklist;

    // ── Game overlay options (#25) ───────────────────────────────────────────
    bool game_overlay_show_fps = true;
    bool game_overlay_show_cpu = true;
    bool game_overlay_show_gpu = true;
    bool game_overlay_show_ram = true;
    bool game_overlay_show_disk = true;
    bool game_overlay_compact = false;
};

struct MediaSnapshot
{
    bool available = false;
    bool playing = false;
    std::wstring title;
    std::wstring artist;
    std::wstring album_title;
    std::wstring source_app_user_model_id;
    std::wstring source_name;
    std::wstring source_badge;
    PixelsRef art;
    PixelsRef source_icon;
    uint64_t art_generation = 0;
    uint64_t source_icon_generation = 0;
    double art_changed_at = 0.0;
    double title_changed_at = 0.0;
    int64_t position_ticks = 0;
    int64_t end_ticks = 0;
    int64_t last_updated_ticks = 0;
};

// One SMTC media session the dock can switch to.
struct MediaSourceInfo
{
    std::wstring aumid;
    std::wstring name;
    std::wstring badge;
    bool playing = false;
    PixelsRef icon;
};

struct ClipboardSnapshot
{
    bool active = false;
    bool image = false;
    std::wstring text;
    std::wstring app_name;
    PixelsRef app_icon;
    PixelsRef image_preview; // decoded thumbnail when the clipboard holds an image
    double expires_at = 0.0;
};

struct BatterySnapshot
{
    bool active = false;
    bool low = false;
    bool charging = false;
    int percent = 100;
    DWORD seconds_remaining = BATTERY_LIFE_UNKNOWN;
    double expires_at = 0.0;
};

struct ProgressSnapshot
{
    bool active = false;
    int percent = 0;
};

struct NotificationSnapshot
{
    bool active = false;
    std::wstring app;
    std::wstring title;
    std::wstring body;
    PixelsRef icon;
    double expires_at = 0.0;
};

struct VolumeSnapshot
{
    bool active = false;
    int percent = 0;
    bool muted = false;
    std::wstring device_name;
    double expires_at = 0.0;
    float display_percent = 0.0f; // animated bar level, set only on the render loop's snapshot copy
};

struct BrightnessSnapshot
{
    bool active = false;
    int percent = 0;
    double expires_at = 0.0;
};

struct CapsLockSnapshot
{
    bool active = false;
    bool caps_on = false;
    bool num_on = false;
    bool is_num_event = false;
    double expires_at = 0.0;
};

struct TimerSnapshot
{
    bool active = false;   // a session exists (running or paused)
    bool running = false;  // currently counting down
    bool is_break = false; // work vs break session
    int total_seconds = 0;
    double ends_at = 0.0; // NowSeconds() at completion, while running
    double remaining_at_pause = 0.0;
    bool just_finished = false;
    double finished_expires_at = 0.0;
};

enum class DeviceEventType
{
    Connected,
    Disconnected,
};

struct DeviceSnapshot
{
    bool active = false;
    DeviceEventType event_type = DeviceEventType::Connected;
    std::wstring device_name; // e.g. "USB Drive" or "Bluetooth Device"
    bool is_bluetooth_like = false;
    double expires_at = 0.0;
};

struct BluetoothDeviceSnapshot
{
    bool active = false;
    bool connected = true; // true = just connected, false = just disconnected
    std::wstring device_name;
    int battery_percent = -1; // -1 = unknown/unavailable
    BluetoothDeviceCategory category = BluetoothDeviceCategory::Generic;
    double expires_at = 0.0;
};

struct SystemSnapshot
{
    int volume_percent = 0;
    bool volume_muted = false;
    int cpu_percent = 0;
    int memory_percent = 0;
    float memory_used_gb = 0.0f;
    float memory_total_gb = 0.0f;
    int disk_free_percent = 0;
    int render_fps = 0;
    int gpu_percent = -1;
    float net_up_mbps = 0.0f;
    float net_down_mbps = 0.0f;
    bool charging = false;
    bool mic_active = false;    // orange dot: microphone in use
    bool camera_active = false; // green dot: camera in use
    std::wstring foreground_title;
    std::wstring mic_app;
    std::wstring camera_app;
};

struct Activity
{
    IslandKind kind = IslandKind::Idle;
    float width = 120.0f;
    float height = 36.0f;
};

struct WeatherSnapshot
{
    bool has_data = false;
    float temperature = 0.0f;
    int weather_code = 0;
    std::wstring city;
    std::wstring weather_desc;
    std::wstring wind_speed;
    std::wstring wind_dir;
    std::wstring humidity;
    std::wstring feels_like;
    double last_updated = 0.0;
};

struct GitHubSnapshot
{
    static constexpr int weeks = 26;
    bool has_data = false;
    std::wstring user;
    int today_count = 0;
    int year_total = 0;
    int streak = 0;
    std::array<signed char, weeks * 7> cells{}; // -1 = no such day yet
    double last_updated = 0.0;
};

struct DoNotDisturbSnapshot
{
    bool active = false;
    bool enabled = false;
    double expires_at = 0.0;
};

// One file parked on the island's shelf (#33). Kept deliberately small: the
// tray holds references, never copies of the files themselves.
struct FileTrayItem
{
    std::wstring path;
    std::wstring name;
    uint64_t size_bytes = 0;
    bool is_directory = false;
    bool is_text = false; // a pasted text snippet rather than a file reference
    std::wstring text;    // full snippet when isText is set
    PixelsRef icon;
};

struct LyricsLine
{
    int64_t time_ms = -1; // -1 for unsynced (plain) lines
    std::wstring text;
};

// Immutable, shared lyric lines. Same read API as the vector it replaces, so
// the drawing code (empty / size / operator[]) is unchanged.
class LyricsLines
{
public:
    LyricsLines() = default;

    explicit LyricsLines(std::vector<LyricsLine>&& v)
        : p_(std::make_shared<const std::vector<LyricsLine>>(std::move(v)))
    {
    }

    bool empty() const
    {
        return !p_ || p_->empty();
    }

    size_t size() const
    {
        return p_ ? p_->size() : 0;
    }

    const LyricsLine& operator[](size_t i) const
    {
        return (*p_)[i];
    }

    const std::vector<LyricsLine>& vec() const
    {
        static const std::vector<LyricsLine> none;
        return p_ ? *p_ : none;
    }

private:
    std::shared_ptr<const std::vector<LyricsLine>> p_;
};

struct LyricsSnapshot
{
    bool has_data = false;
    bool synced = false;
    bool not_found = false;
    bool fetching = false;
    std::wstring matched_title;
    std::wstring matched_artist;
    LyricsLines lines;
};

// ── Quick Lookup state ───────────────────────────────────────────────────────
enum class LookupStatus
{
    Search,   // panel open: search box + recent list
    Loading,  // request running
    Found,    // title / subtitle / body (/ example) are filled in
    NoResult, // nothing found, or the network failed
    NoText,   // legacy, no longer produced
};

struct LookupSnapshot
{
    bool active = false;
    LookupStatus status = LookupStatus::Loading;
    std::wstring query;    // the normalised text being looked up
    std::wstring title;    // headword / article title
    std::wstring subtitle; // "phonetic · part of speech", or the Wikipedia short description
    std::wstring body;     // definition / first sentences of the extract
    std::wstring example;  // dictionary example sentence, if any
    std::wstring source;   // "Dictionary" / "Wikipedia"
    double expires_at = 0.0;
};

// Shared by Renderer::MeasureLookupCard / DrawLookup and ActivityForKind so the
// size the island animates to and the layout painted inside it cannot drift.
// One finished lookup, kept in memory only (never written to disk).
struct LookupRecent
{
    std::wstring query;  // what was searched (normalised)
    std::wstring title;  // headword / article title that came back
    std::wstring detail; // one-line definition preview
    std::wstring source;
};

}

namespace island
{
namespace lookup_layout
{

constexpr float k_width = 380.0f;
constexpr float k_pad_x = 16.0f;
constexpr float pad_top = 12.0f;
constexpr float pad_bottom = 12.0f;
constexpr float k_title_h = 20.0f;
constexpr float subtitle_h = 15.0f;
constexpr float k_card_gap = 6.0f;
constexpr float card_pad = 9.0f;
constexpr float example_gap = 5.0f;
constexpr float simple_height = 54.0f;
constexpr float k_max_height = 300.0f;
constexpr int max_body_lines = 4;
constexpr int max_example_lines = 2;

// Search panel
constexpr float field_top = pad_top;
constexpr float field_h = 36.0f;
constexpr float field_gap = 10.0f;
constexpr float below_field = field_top + field_h + field_gap; // where content under the box starts
constexpr float section_h = 18.0f;                             // "Recent ... Clear" header band
constexpr float k_row_h = 32.0f;
constexpr float k_row_gap = 3.0f;
constexpr float status_h = 30.0f; // one-line states under the box
constexpr int max_rows = 4;
constexpr double caret_period = 1.0;
constexpr double caret_on = 0.55;

constexpr float rows_top(bool header)
{
    return below_field + (header ? section_h : 0.0f);
}

constexpr float search_height(int rows, bool header)
{
    return rows <= 0 ? below_field + status_h + pad_bottom
                     : rows_top(header) + rows * k_row_h + (rows - 1) * k_row_gap + pad_bottom;
}

constexpr float status_height()
{
    return below_field + status_h + pad_bottom;
}

}
}

namespace island
{

// Input state of the search box. Render thread only (the window procedure and the
// render loop share that thread), so it needs no lock.
struct LookupUiState
{
    bool open = false;
    std::wstring text;
    size_t caret = 0;
    bool select_all = false;
    int selected = 0;               // highlighted row
    double caret_reset_at = 0.0;    // blink restarts on every edit
    std::wstring clipboard_query;   // clipboard suggestion captured when the panel opened
    HWND prev_foreground = nullptr; // window to hand focus back to
};

inline LookupUiState g_lookup_ui;

struct SharedState
{
    MediaSnapshot media;
    std::vector<MediaSourceInfo> media_sources; // stable dock order
    ClipboardSnapshot clipboard;
    NotificationSnapshot notification;
    VolumeSnapshot volume;
    BrightnessSnapshot brightness;
    CapsLockSnapshot caps_lock;
    TimerSnapshot timer;
    DeviceSnapshot device;
    BluetoothDeviceSnapshot bluetooth_device;
    DoNotDisturbSnapshot do_not_disturb;
    BatterySnapshot battery;
    ProgressSnapshot progress;
    SystemSnapshot system;
    WeatherSnapshot weather;
    GitHubSnapshot github;
    LyricsSnapshot lyrics;
    LookupSnapshot lookup;
    std::vector<LookupRecent> lookup_recent; // newest first, guarded by g_stateMutex
    std::array<float, 48> waveform{};
    size_t waveform_write = 0;
    std::array<float, spectrum_bands> bands{}; // smoothed 0..1 magnitude per band (low -> high)
    bool muted = false;
    std::vector<FileTrayItem> file_tray_items;
    std::shared_ptr<const notification_center::History> notification_center;
    notification_center::View notification_center_view;
};

struct SpringValue
{
    float value = 0.0f;
    float velocity = 0.0f;
    float target = 0.0f;

    void reset(float v)
    {
        value = target = v;
        velocity = 0.0f;
    }

    void step(float total_dt, float stiffness, float damping)
    {
        const float fixed_dt = 0.0005f;
        while (total_dt > 0.0f)
        {
            float dt = std::min(total_dt, fixed_dt);
            const float displacement = value - target;
            const float acceleration = -stiffness * displacement - damping * velocity;
            velocity += acceleration * dt;
            value += velocity * dt;
            total_dt -= dt;
        }

        if (std::fabs(value - target) < 0.01f && std::fabs(velocity) < 0.01f)
        {
            value = target;
            velocity = 0.0f;
        }
    }
};

inline Settings g_settings;

}
