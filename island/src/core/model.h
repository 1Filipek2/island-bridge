#pragma once

#include "core/common.h"
#include "notifications/notification_center.h"
#include "core/pixels.h"
namespace island
{


// The conversion and hit-test helpers live further down, just after Clamp().

enum class IslandKind {
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

enum class BluetoothDeviceCategory {
    Headphones,
    Speaker,
    Mouse,
    Keyboard,
    Phone,
    Generic,
};

enum class Position {
    TopCenter,
    TopLeft,
    TopRight,
    BottomCenter,
    BottomLeft,
    BottomRight,
};

// Anything anchored to the bottom edge shares the same vertical placement and
// cannot use the top-edge macOS notch shape.
constexpr bool IsBottomPosition(Position position) {
    return position == Position::BottomCenter ||
           position == Position::BottomLeft ||
           position == Position::BottomRight;
}

enum class AccentMode {
    Auto,
    System,
    Custom,
};

enum class AnimationStyle {
    Smooth,
    Default,
    Bouncy,
    Snappy,
};

enum class ProgressStyle {
    Slim,
    Wavy,
    Squiggle,
    Bar,
};

enum class SpectrumStyle {
    Bars,
    Orb,
    Plasma,
    Led,
};

constexpr int kSpectrumBands = 24;  // log-spaced bands from SpectrumAnalyzer, stored in SharedState::bands

enum class CalendarAccentMode {
    Red,
    Accent,  // persisted as "red", from when it was a hardcoded red
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
enum class ThemePreset {
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
struct ThemePalette {
    const wchar_t* id;      // value stored in Themes.ThemePreset
    const wchar_t* label;   // right-click menu label
    const wchar_t* bg;
    const wchar_t* fg;
    const wchar_t* sec;
    const wchar_t* border;
};

static constexpr ThemePalette kThemePalettes[] = {
    {L"obsidian",  L"Obsidian (Default)", L"#08080A", L"#FFFFFF", L"#9B9BA5", L"#1E1E22"},
    {L"graphite",  L"Graphite",           L"#1C1C1E", L"#FFFFFF", L"#A8A8AE", L"#323236"},
    {L"slate",     L"Slate",              L"#111721", L"#E9EEF6", L"#92A2B8", L"#232C3A"},
    {L"nord",      L"Nord",               L"#2E3440", L"#ECEFF4", L"#A7B0C0", L"#3B4252"},
    {L"evergreen", L"Evergreen",          L"#0C1512", L"#E4F1EA", L"#8CAE9D", L"#1A2A23"},
    {L"espresso",  L"Espresso",           L"#1A1512", L"#F6EEE7", L"#B7A395", L"#2E2420"},
    {L"plum",      L"Plum",               L"#16111C", L"#F1EAF6", L"#AC9BB9", L"#281F33"},
    {L"porcelain", L"Porcelain (Light)",  L"#F5F6F8", L"#14161A", L"#5B6270", L"#D8DBE1"},
};

// Custom sits one past the last real palette. Derived, so adding a palette
// cannot leave a stale literal behind.
static constexpr int kCustomThemeIndex = static_cast<int>(ARRAYSIZE(kThemePalettes));

// Persisted under a new key. Retiring the original palettes renumbered these
// integers, and reusing "ColorTheme" would have silently reinterpreted a saved
// 4 (previously Custom) as the palette that now sits at index 4.
static constexpr const wchar_t* kThemeValueName = L"ColorThemeV2";
static constexpr const wchar_t* kLegacyThemeValueName = L"ColorTheme";

// Base menu command id for the theme submenu; entries occupy
// [kThemeMenuIdBase, kThemeMenuIdBase + kCustomThemeIndex].
static constexpr UINT kThemeMenuIdBase = 20;

// Maps a preset id to its palette index, accepting the retired ids too: a
// user's stored setting keeps its old string after the option disappears from
// the dropdown, so each legacy id is pointed at the new palette closest to it.
inline int ThemeIndexFromId(std::wstring_view id) {
    auto iequals = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) return false;
        }
        return true;
    };

    for (int i = 0; i < kCustomThemeIndex; ++i) {
        if (iequals(id, kThemePalettes[i].id)) return i;
    }
    if (iequals(id, L"custom")) return kCustomThemeIndex;

    if (iequals(id, L"oled-black")) return 0;     // -> Obsidian
    if (iequals(id, L"fluent") ||
        iequals(id, L"mica") ||
        iequals(id, L"dark-gray")) return 1;      // -> Graphite
    if (iequals(id, L"midnight-blue")) return 2;  // -> Slate
    if (iequals(id, L"deep-purple")) return 6;    // -> Plum
    return -1;
}

// Real Windows blur / acrylic painted behind the island (#59).
enum class BackdropMaterial {
    None,
    Blur,
    Acrylic,
};

enum class ContourBorderMode {
    Default,
    Auto,
    Borderless,
};

struct Settings {
    Position position = Position::TopCenter;
    int targetMonitor = 0;
    int offsetX = 0;
    int offsetY = 0;
    bool separateExpandedOffsetY = false;  // #83
    int offsetYExpanded = 0;               // #83
    float sizeScale = 1.0f;
    float baseSizeScale = 1.0f;
    std::wstring fontFamily;
    AccentMode accentMode = AccentMode::Auto;
    D2D1_COLOR_F customAccent = D2D1::ColorF(0x4cc9f0);
    int targetFps = 0; // 0 = Auto
    AnimationStyle animationStyle = AnimationStyle::Default;
    ProgressStyle progressStyle = ProgressStyle::Slim;
    SpectrumStyle spectrumStyle = SpectrumStyle::Bars;
    bool expandedMediaAnim = false;   // hover player: text/cover flip/controls transitions
    bool pillCoverAnim = false;      // collapsed pill: cover spin/drop
    float animationSpeed = 1.0f;
    bool media = true;
    bool mediaAutoExpand = false;
    bool clipboard = true;
    bool statusCountdownProgress = false;
    bool battery = true;
    bool progress = true;
    bool volume = true;
    bool brightness = true;
    CalendarAccentMode calendarAccent = CalendarAccentMode::Accent;
    bool privacyDots = true;
    bool privacyDotsMic = true;
    bool privacyDotsCam = true;
    bool privacyDotsPulse = true;
    // Modules.CapsLock -- fixes windhawk-mods#4352, which asked for a way to turn
    // the Caps Lock / Num Lock indicator off.
    //
    // Enforced in three places, because any one of them alone leaks:
    //   ChooseActivities        - the pill is never selected for display
    //   WM_APP_CAPSLOCK handler - no state is recorded, no nudge is triggered
    //   CapsLockHookWanted      - WH_KEYBOARD_LL is not installed at all
    // The activity gate is the one that actually hides the pill; the others stop
    // the work leading up to it.
    bool capsLock = true;
    bool timerEnabled = true;
    bool hideShowHotkeyEnabled = true;
    UINT hideShowModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    UINT hideShowVk = 'D';

    // Quick Lookup: hotkey-triggered dictionary / Wikipedia card.
    bool quickLookup = false;
    UINT lookupModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    UINT lookupVk = VK_SPACE;
    bool bluetoothIndicator = true;
    bool bluetoothShowBattery = true;
    D2D1_COLOR_F privacyDotsMicHex = D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f); // #FF9500
    D2D1_COLOR_F privacyDotsCamHex = D2D1::ColorF(0.133f, 0.776f, 0.239f, 1.0f); // #10B981
    float tintOpacity = 0.72f;
    float pillOpacity = 0.96f;
    bool gameOverlay = false;
    bool showMetricText = true;
    bool weather = true;
    std::wstring weatherCity;
    bool weatherFahrenheit = false;
    bool lyricsVisualizer = true;
    bool karaokeLyrics = true;
    bool karaokeLetterGlow = true;
    bool lyricsNeighborLinesVisible = false;
    int lyricsOffsetMs = 0;      // Modules.LyricsOffsetMs, speakers / wired, ms
    int lyricsOffsetBtMs = 0;    // Modules.LyricsOffsetBluetoothMs, Bluetooth output, ms
    bool collapsedLyrics = false;  // lyrics inside the collapsed media pill (right-click toggle)
    bool lyricsCache = true;       // Modules.LyricsCache
    int lyricsCacheMaxMB = 25;     // Modules.LyricsCacheMaxMB; 0 = disk cache off
    bool mediaTabLyrics = false;   // lyrics side panel in the expanded media tab
    int autoHideIdleSeconds = 0;
    bool autoHideFullscreen = true;
    bool borderMergedMode = false;
    bool unhideOnHover = true;
    bool alwaysOnTop = true;
    bool expandOnHover = true;
    bool autoDpiScale = true;
    bool w11Style = false;
    bool notchStyle = false;
    // Color customization
    ThemePreset themePreset = ThemePreset::Obsidian;
    D2D1_COLOR_F pillBgColor = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f); // #08080A
    D2D1_COLOR_F textPrimaryColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f); // #FFFFFF
    D2D1_COLOR_F textSecondaryColor = D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f); // #9B9BA5
    ContourBorderMode contourBorderMode = ContourBorderMode::Default;
    bool contourBorderEnabled = true;
    D2D1_COLOR_F contourBorderColor = D2D1::ColorF(0.200f, 0.200f, 0.220f, 1.0f); // #333338
    bool clockAccentGlow = true;
    bool privacyDotsEnabled = true;
    bool privacyDotPulsing = true;
    D2D1_COLOR_F micDotColor = D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f); // #FF9500
    D2D1_COLOR_F camDotColor = D2D1::ColorF(0.204f, 0.780f, 0.349f, 1.0f); // #34C759
    bool hardwareMonitorModule = true;
    bool githubModule = true;
    bool doNotDisturbIndicator = true;
    bool notificationRespectDnD = true;

    // ── Premium material / redesign ──────────────────────────────────────────
    bool materialDepth = true;      // downward depth shading + accent bloom
    bool dropShadow = true;         // soft shadow under the island
    float accentBloom = 1.0f;       // 0..2 multiplier on the accent wash
    float textScale = 1.0f;         // independent typography scale (#41)
    BackdropMaterial backdropMaterial = BackdropMaterial::None;  // #59
    float backdropTint = 0.55f;     // acrylic tint strength, 0..1
    float backdropFillAlpha = 0.45f;  // how opaque the pill's own fill stays

    // ── Clock / date presentation (#61) ──────────────────────────────────────
    bool showSeconds = false;
    bool use24HourClock = false;    // false = follow system locale
    bool clockFollowSystem = true;
    std::wstring dateFormat;        // empty = locale default
    bool mediaPillClock = false;    // show the clock inside the collapsed media pill
    bool dateFirst = false;
    int firstDayOfWeek = -1;        // 0 = Sunday .. 6 = Saturday, -1 = follow Windows

    // ── Localization (#35) ───────────────────────────────────────────────────
    std::wstring language = L"auto";

    // ── File tray (#33) ──────────────────────────────────────────────────────
    bool fileTrayModule = false;
    int fileTrayMaxItems = 10;
    bool fileTrayPersist = true;   // Modules.FileTrayPersist
    bool notificationCenterModule = true;

    // ── Media auto-expand exclusions (#62) ───────────────────────────────────
    std::vector<std::wstring> mediaExpandBlocklist;

    // ── Game overlay options (#25) ───────────────────────────────────────────
    bool gameOverlayShowFps = true;
    bool gameOverlayShowCpu = true;
    bool gameOverlayShowGpu = true;
    bool gameOverlayShowRam = true;
    bool gameOverlayShowDisk = true;
    bool gameOverlayCompact = false;
};

struct MediaSnapshot {
    bool available = false;
    bool playing = false;
    std::wstring title;
    std::wstring artist;
    std::wstring albumTitle;
    std::wstring sourceAppUserModelId;
    std::wstring sourceName;
    std::wstring sourceBadge;
    PixelsRef art;
    PixelsRef sourceIcon;
    uint64_t artGeneration = 0;
    uint64_t sourceIconGeneration = 0;
    double artChangedAt = 0.0;
    double titleChangedAt = 0.0;
    int64_t positionTicks = 0;
    int64_t endTicks = 0;
    int64_t lastUpdatedTicks = 0;
};

// One SMTC media session the dock can switch to.
struct MediaSourceInfo {
    std::wstring aumid;
    std::wstring name;
    std::wstring badge;
    bool playing = false;
    PixelsRef icon;
};

struct ClipboardSnapshot {
    bool active = false;
    bool image = false;
    std::wstring text;
    std::wstring appName;
    PixelsRef appIcon;
    PixelsRef imagePreview;  // decoded thumbnail when the clipboard holds an image
    double expiresAt = 0.0;
};

struct BatterySnapshot {
    bool active = false;
    bool low = false;
    bool charging = false;
    int percent = 100;
    DWORD secondsRemaining = BATTERY_LIFE_UNKNOWN;
    double expiresAt = 0.0;
};

struct ProgressSnapshot {
    bool active = false;
    int percent = 0;
};

struct NotificationSnapshot {
    bool active = false;
    std::wstring app;
    std::wstring title;
    std::wstring body;
    PixelsRef icon;
    double expiresAt = 0.0;
};

struct VolumeSnapshot {
    bool active = false;
    int percent = 0;
    bool muted = false;
    std::wstring deviceName;
    double expiresAt = 0.0;
    float displayPercent = 0.0f;  // animated bar level, set only on the render loop's snapshot copy
};

struct BrightnessSnapshot {
    bool active = false;
    int percent = 0;
    double expiresAt = 0.0;
};

struct CapsLockSnapshot {
    bool active = false;
    bool capsOn = false;
    bool numOn = false;
    bool isNumEvent = false;
    double expiresAt = 0.0;
};

struct TimerSnapshot {
    bool active = false;          // a session exists (running or paused)
    bool running = false;         // currently counting down
    bool isBreak = false;         // work vs break session
    int totalSeconds = 0;
    double endsAt = 0.0;          // NowSeconds() at completion, while running
    double remainingAtPause = 0.0;
    bool justFinished = false;
    double finishedExpiresAt = 0.0;
};

enum class DeviceEventType {
    Connected,
    Disconnected,
};

struct DeviceSnapshot {
    bool active = false;
    DeviceEventType eventType = DeviceEventType::Connected;
    std::wstring deviceName;  // e.g. "USB Drive" or "Bluetooth Device"
    bool isBluetoothLike = false;
    double expiresAt = 0.0;
};

struct BluetoothDeviceSnapshot {
    bool active = false;
    bool connected = true;   // true = just connected, false = just disconnected
    std::wstring deviceName;
    int batteryPercent = -1; // -1 = unknown/unavailable
    BluetoothDeviceCategory category = BluetoothDeviceCategory::Generic;
    double expiresAt = 0.0;
};

struct SystemSnapshot {
    int volumePercent = 0;
    bool volumeMuted = false;
    int cpuPercent = 0;
    int memoryPercent = 0;
    float memoryUsedGB = 0.0f;
    float memoryTotalGB = 0.0f;
    int diskFreePercent = 0;
    int renderFps = 0;
    int gpuPercent = -1;
    float netUpMbps = 0.0f;
    float netDownMbps = 0.0f;
    bool charging = false;
    bool micActive = false;      // orange dot: microphone in use
    bool cameraActive = false;   // green dot: camera in use
    std::wstring foregroundTitle;
    std::wstring micApp;
    std::wstring cameraApp;
};

struct Activity {
    IslandKind kind = IslandKind::Idle;
    float width = 120.0f;
    float height = 36.0f;
};

struct WeatherSnapshot {
    bool hasData = false;
    float temperature = 0.0f;
    int weatherCode = 0;
    std::wstring city;
    std::wstring weatherDesc;
    std::wstring windSpeed;
    std::wstring windDir;
    std::wstring humidity;
    std::wstring feelsLike;
    double lastUpdated = 0.0;
};

struct GitHubSnapshot {
    static constexpr int kWeeks = 26;
    bool hasData = false;
    std::wstring user;
    int todayCount = 0;
    int yearTotal = 0;
    int streak = 0;
    std::array<signed char, kWeeks * 7> cells{};  // -1 = no such day yet
    double lastUpdated = 0.0;
};

struct DoNotDisturbSnapshot {
    bool active = false;
    bool enabled = false;
    double expiresAt = 0.0;
};

// One file parked on the island's shelf (#33). Kept deliberately small: the
// tray holds references, never copies of the files themselves.
struct FileTrayItem {
    std::wstring path;
    std::wstring name;
    uint64_t sizeBytes = 0;
    bool isDirectory = false;
    bool isText = false;     // a pasted text snippet rather than a file reference
    std::wstring text;       // full snippet when isText is set
    PixelsRef icon;
};

struct LyricsLine {
    int64_t timeMs = -1;  // -1 for unsynced (plain) lines
    std::wstring text;
};

// Immutable, shared lyric lines. Same read API as the vector it replaces, so
// the drawing code (empty / size / operator[]) is unchanged.
class LyricsLines {
   public:
    LyricsLines() = default;
    explicit LyricsLines(std::vector<LyricsLine>&& v)
        : p_(std::make_shared<const std::vector<LyricsLine>>(std::move(v))) {}

    bool empty() const { return !p_ || p_->empty(); }
    size_t size() const { return p_ ? p_->size() : 0; }
    const LyricsLine& operator[](size_t i) const { return (*p_)[i]; }
    const std::vector<LyricsLine>& vec() const {
        static const std::vector<LyricsLine> none;
        return p_ ? *p_ : none;
    }

   private:
    std::shared_ptr<const std::vector<LyricsLine>> p_;
};

struct LyricsSnapshot {
    bool hasData = false;
    bool synced = false;
    bool notFound = false;
    bool fetching = false;
    std::wstring matchedTitle;
    std::wstring matchedArtist;
    LyricsLines lines;
};

// ── Quick Lookup state ───────────────────────────────────────────────────────
enum class LookupStatus {
    Search,    // panel open: search box + recent list
    Loading,   // request running
    Found,     // title / subtitle / body (/ example) are filled in
    NoResult,  // nothing found, or the network failed
    NoText,    // legacy, no longer produced
};

struct LookupSnapshot {
    bool active = false;
    LookupStatus status = LookupStatus::Loading;
    std::wstring query;      // the normalised text being looked up
    std::wstring title;      // headword / article title
    std::wstring subtitle;   // "phonetic · part of speech", or the Wikipedia short description
    std::wstring body;       // definition / first sentences of the extract
    std::wstring example;    // dictionary example sentence, if any
    std::wstring source;     // "Dictionary" / "Wikipedia"
    double expiresAt = 0.0;
};

// Shared by Renderer::MeasureLookupCard / DrawLookup and ActivityForKind so the
// size the island animates to and the layout painted inside it cannot drift.
// One finished lookup, kept in memory only (never written to disk).
struct LookupRecent {
    std::wstring query;   // what was searched (normalised)
    std::wstring title;   // headword / article title that came back
    std::wstring detail;  // one-line definition preview
    std::wstring source;
};

}

namespace island
{
namespace LookupLayout
{

    constexpr float kWidth = 380.0f;
    constexpr float kPadX = 16.0f;
    constexpr float kPadTop = 12.0f;
    constexpr float kPadBottom = 12.0f;
    constexpr float kTitleH = 20.0f;
    constexpr float kSubtitleH = 15.0f;
    constexpr float kCardGap = 6.0f;
    constexpr float kCardPad = 9.0f;
    constexpr float kExampleGap = 5.0f;
    constexpr float kSimpleHeight = 54.0f;
    constexpr float kMaxHeight = 300.0f;
    constexpr int kMaxBodyLines = 4;
    constexpr int kMaxExampleLines = 2;

    // Search panel
    constexpr float kFieldTop = kPadTop;
    constexpr float kFieldH = 36.0f;
    constexpr float kFieldGap = 10.0f;
    constexpr float kBelowField = kFieldTop + kFieldH + kFieldGap;  // where content under the box starts
    constexpr float kSectionH = 18.0f;   // "Recent ... Clear" header band
    constexpr float kRowH = 32.0f;
    constexpr float kRowGap = 3.0f;
    constexpr float kStatusH = 30.0f;    // one-line states under the box
    constexpr int kMaxRows = 4;
    constexpr double kCaretPeriod = 1.0;
    constexpr double kCaretOn = 0.55;

    constexpr float RowsTop(bool header) { return kBelowField + (header ? kSectionH : 0.0f); }
    constexpr float SearchHeight(int rows, bool header) {
        return rows <= 0 ? kBelowField + kStatusH + kPadBottom
                         : RowsTop(header) + rows * kRowH + (rows - 1) * kRowGap + kPadBottom;
    }
    constexpr float StatusHeight() { return kBelowField + kStatusH + kPadBottom; }

}
}

namespace island
{


// Input state of the search box. Render thread only (the window procedure and the
// render loop share that thread), so it needs no lock.
struct LookupUiState {
    bool open = false;
    std::wstring text;
    size_t caret = 0;
    bool selectAll = false;
    int selected = 0;               // highlighted row
    double caretResetAt = 0.0;      // blink restarts on every edit
    std::wstring clipboardQuery;    // clipboard suggestion captured when the panel opened
    HWND prevForeground = nullptr;  // window to hand focus back to
};
inline LookupUiState g_lookupUi;

struct SharedState {
    MediaSnapshot media;
    std::vector<MediaSourceInfo> mediaSources;  // stable dock order
    ClipboardSnapshot clipboard;
    NotificationSnapshot notification;
    VolumeSnapshot volume;
    BrightnessSnapshot brightness;
    CapsLockSnapshot capsLock;
    TimerSnapshot timer;
    DeviceSnapshot device;
    BluetoothDeviceSnapshot bluetoothDevice;
    DoNotDisturbSnapshot doNotDisturb;
    BatterySnapshot battery;
    ProgressSnapshot progress;
    SystemSnapshot system;
    WeatherSnapshot weather;
    GitHubSnapshot github;
    LyricsSnapshot lyrics;
    LookupSnapshot lookup;
    std::vector<LookupRecent> lookupRecent;  // newest first, guarded by g_stateMutex
    std::array<float, 48> waveform{};
    size_t waveformWrite = 0;
    std::array<float, kSpectrumBands> bands{};  // smoothed 0..1 magnitude per band (low -> high)
    bool muted = false;
    std::vector<FileTrayItem> fileTrayItems;
    std::shared_ptr<const notification_center::History> notificationCenter;
    notification_center::View notificationCenterView;
};

struct SpringValue {
    float value = 0.0f;
    float velocity = 0.0f;
    float target = 0.0f;

    void Reset(float v) {
        value = target = v;
        velocity = 0.0f;
    }

    void Step(float totalDt, float stiffness, float damping) {
        const float kFixedDt = 0.0005f;
        while (totalDt > 0.0f) {
            float dt = std::min(totalDt, kFixedDt);
            const float displacement = value - target;
            const float acceleration = -stiffness * displacement - damping * velocity;
            velocity += acceleration * dt;
            value += velocity * dt;
            totalDt -= dt;
        }

        if (std::fabs(value - target) < 0.01f && std::fabs(velocity) < 0.01f) {
            value = target;
            velocity = 0.0f;
        }
    }
};

inline Settings g_settings;

}
