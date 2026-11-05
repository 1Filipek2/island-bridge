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


// ── Weather iconography ──────────────────────────────────────────────────────
// The dashboard used colour emoji (☀️ ⛅ 🌡️) rendered with ENABLE_COLOR_FONT,
// which clashed badly with the island's monochrome, accent-tinted UI and varied
// in size and baseline between glyphs. These are drawn as vectors instead: they
// inherit the accent colour, scale cleanly, sit on a predictable baseline, and
// can never fall back to a missing-glyph box the way a font-dependent icon can.
enum class WeatherVisual {
    Clear,
    PartlyCloudy,
    Cloudy,
    Fog,
    Storm,
    Rain,
    Snow,
    Unknown,
};

// Groupings mirror GetWeatherIconAndText so both stay keyed off the same WWO
// weather codes.
WeatherVisual WeatherVisualFromCode(int code) {
    switch (code) {
        case 113:
            return WeatherVisual::Clear;
        case 116:
            return WeatherVisual::PartlyCloudy;
        case 119: case 122:
            return WeatherVisual::Cloudy;
        case 143: case 248: case 260:
            return WeatherVisual::Fog;
        case 200: case 386: case 389: case 392: case 395:
            return WeatherVisual::Storm;
        case 176: case 263: case 266: case 281: case 284: case 293: case 296:
        case 299: case 302: case 305: case 308: case 311: case 314: case 353:
        case 356: case 359:
            return WeatherVisual::Rain;
        case 179: case 182: case 185: case 227: case 230: case 317: case 320:
        case 323: case 326: case 329: case 332: case 335: case 338: case 350:
        case 362: case 365: case 368: case 371:
            return WeatherVisual::Snow;
        default:
            return WeatherVisual::Unknown;
    }
}

// Cache for DrawKaraokeLetterPop: the layout and per-cluster hit rects only
// depend on the active lyric line's text/format/box size, never on time, so
// they are rebuilt only when the line changes. Rects are stored relative to a
// (0,0) layout origin.
struct KaraokeLayoutCache {
    struct ClusterInfo {
        UINT32 textStart = 0;
        UINT32 textLength = 0;
        bool isWhitespace = true;
        D2D1_RECT_F rect{};
    };

    std::wstring text;
    IDWriteTextFormat* format = nullptr;
    float layoutWidth = 0.0f;
    float layoutHeight = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    std::vector<ClusterInfo> clusters;
    bool valid = false;
};

struct MarqueeLayoutCache {
    std::wstring text;
    IDWriteTextFormat* format = nullptr;
    float wrapWidth = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
};

// Measured geometry of the collapsed idle strip, produced by
// Renderer::MeasureIdleStrip. The render loop uses totalWidth to size the
// island; DrawIdleDashboard uses the per-slot widths to place the clock, divider
// and weather reading inside it.
struct IdleStripMetrics {
    float clockWidth = 0.0f;
    float weatherWidth = 0.0f;
    bool hasWeather = false;
    bool hasPrivacy = false;
    bool hasUnread = false;
    float totalWidth = IdleStripLayout::kMinWidth;
};

bool NotificationCenterUnread(const Settings& settings) {
    return settings.notificationCenterModule && g_notificationCenterUnread.load(std::memory_order_relaxed);
}

float IdleDotReserve(const IdleStripMetrics& metrics) {
    if (!metrics.hasPrivacy && !metrics.hasUnread) {
        return 0.0f;
    }
    return IdleStripLayout::kPrivacyReserve +
           (metrics.hasPrivacy && metrics.hasUnread ? notification_center::unread_dot_gap : 0.0f);
}

// One visible character of the song title, used by the title-change animation.
struct TitleGlyph {
    wchar_t ch0 = 0;       // first UTF-16 unit (used to match letters between titles)
    wchar_t ch1 = 0;       // second unit of a surrogate pair, otherwise 0
    float x = 0.0f;        // left edge inside the full title layout
    float sx = 0.0f;       // on-screen left edge (relative to the title rect) when the animation started
    bool  eligible = true; // visible when the animation started (only these get paired)
    float w = 0.0f;
    float h = 0.0f;
    float seed = 0.0f;     // stable 0..1 value so each letter's smoke is unique
    int match = -1;        // index of the paired glyph in the other title, or -1
    ComPtr<IDWriteTextLayout> layout;  // this character alone, same format as the title
};

// Marquee state of one title at the moment the animation starts, mirroring what
// DrawMarqueeText does (offset = fmod(now * speed, cycle)).
struct TitleScroll {
    bool scrolls = false;
    float width = 0.0f;
    float cycle = 0.0f;
    float offset0 = 0.0f;
};

// State for the gentle swap animation of the artist / album lines.
struct SwapTextState {
    std::wstring shown;               // text currently on screen
    std::wstring prev;                // text that is fading out
    double start = -1.0;              // animation start, -1 = idle
    double lastDrawn = -1.0;          // last frame this row was drawn
    MarqueeLayoutCache prevCache;     // layout cache for the outgoing text
};

struct MediaPillMetrics {
    float clockWidth = 0.0f;
    float sectionWidth = 0.0f;                       // clock + divider block on the left
    float totalWidth = MediaPillLayout::kBaseWidth;  // content space, before sizeScale
};

// Substitutes '0' for every decimal digit before measuring.
//
// Proportional faces give '1' a visibly narrower advance than '0', so measuring
// the live clock would change the pill's width as the time changed -- with
// seconds enabled that is a layered-window resize and reposition every second,
// and a visible twitch. Normalising to the widest digit makes the width stable
// for a given digit count, and since '0' is never narrower than the digit it
// replaces, the real string is guaranteed to fit the box we reserve.
std::wstring WidestDigitForm(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'0' && c <= L'9') {
            c = L'0';
        }
    }
    return text;
}

// ── Collapsed-pill lyrics ────────────────────────────────────────────────────
// Width of the collapsed media pill while it carries a lyric line (content px).
constexpr float kCollapsedLyricsWidth = 320.0f;

// True when the collapsed pill should be widened for lyrics. "fetching" counts
// too, so the pill does not shrink and regrow on every track change.
bool CollapsedLyricsActive(const SharedState& state, const Settings& settings) {
    // Stays wide for as long as media is available. If there are no lyrics to
    // show, DrawCollapsedLyrics shows the media title instead.
    return settings.collapsedLyrics && settings.media && state.media.available;
}

// One pre-built lyric line. Built once when the line changes, then reused every
// frame, so animating it costs a DrawTextLayout and nothing else.
struct CollapsedLyricLine {
    std::wstring text;
    ComPtr<IDWriteTextLayout> layout;
    float width = 0.0f;
    float height = 0.0f;
    bool isTitle = false;  // true when this is the media-title fallback, not a lyric line
};

class Renderer {
   public:
    bool Initialize(HWND hwnd) {
        hwnd_ = hwnd;

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       __uuidof(ID2D1Factory),
                                       reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

        hr = d2dFactory_->CreateDCRenderTarget(&props, &target_);
        if (FAILED(hr)) {
            return false;
        }

        const Settings initialSettings = GetSettingsCopy();
        EnsureTextFormats(initialSettings.sizeScale, initialSettings.fontFamily,
                          initialSettings.textScale);

        return CreateBackingBitmap(520, 140);
    }

    bool Render(const SharedState& state, const Settings& settings, const Activity& primary,
                const std::optional<Activity>& secondary, float width, float height,
                float nudge, bool hover, bool pinned, double now) {
        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);

        width = std::max(width, 0.0f);
        if (UsesFixedStage(settings)) {
            // A pill can never be bigger than the stage it lives in.
            width = std::min(width, kStageContentWidth * settings.sizeScale);
            height = std::min(height, kStageContentHeight * settings.sizeScale);
        }

        const StageLayout stage = ComputeStageLayout(settings, width, height);
        const int pixelWidth = stage.width;
        const int pixelHeight = stage.height;

        const bool flushTop = settings.notchStyle || settings.borderMergedMode;
        const float top = stage.pillTop + (flushTop ? std::max(0.0f, nudge) : nudge);
        const float left = stage.pillLeft;

        const BYTE alpha = static_cast<BYTE>(Clamp(settings.pillOpacity, 0.35f, 1.0f) * 255.0f);
        const bool sizeChanged = pixelWidth != bitmapWidth_ || pixelHeight != bitmapHeight_;
        const bool moved = stage.origin.x != lastOrigin_.x || stage.origin.y != lastOrigin_.y;

        // Full repaint when anything about the window itself changed, plus once a
        // second as a safety net (sleep/resume, display changes).
        const bool fullPaint = !kUseDirtyRect || !stageValid_ || sizeChanged || moved ||
                               alpha != lastAlpha_ || (now - lastFullPaint_) > 1.0;

        if (sizeChanged) {
            if (!CreateBackingBitmap(pixelWidth, pixelHeight)) {
                return false;
            }
        } else if (g_layoutDirty.exchange(false)) {
            upkeepPending_ = true;
        }
        if (upkeepPending_) {
            if (now - lastUpkeepAt_ > 0.25) {
                upkeepPending_ = false;
                lastUpkeepAt_ = now;
                ApplyOverlayZOrder(hwnd_, pixelWidth, pixelHeight);
            } else {
                g_layoutDirty = true;  // retry once the throttle expires
            }
        }

        // Region that can contain pixels this frame: the pill plus its shadow and
        // nudge slack, unioned with where it was last frame so the old pixels get erased.
        RECT bounds;
        bounds.left = std::max(0, static_cast<int>(std::floor(left - kRenderPadX - 4.0f)));
        bounds.top = std::max(0, static_cast<int>(std::floor(top - kRenderPadY - 12.0f)));
        bounds.right = std::min(pixelWidth, static_cast<int>(std::ceil(left + width + kRenderPadX + 4.0f)));
        bounds.bottom = std::min(pixelHeight, static_cast<int>(std::ceil(top + height + kRenderPadY + 12.0f)));

        RECT dirty = {0, 0, pixelWidth, pixelHeight};
        if (!fullPaint) {
            UnionRect(&dirty, &lastBounds_, &bounds);
        }

        if (IsRectEmpty(&dirty)) {
            dirty = RECT{0, 0, pixelWidth, pixelHeight};
        }

        // Bind only the region being repainted. Binding the whole stage made D2D copy
        // all of it in at BeginDraw and out at EndDraw every frame, even for a small pill.
        // The bound rect's top-left becomes (0,0); the translation restores stage
        // coordinates, and every draw routine composes with the current transform.
        RECT rc = dirty;
        HRESULT hr = target_->BindDC(memDc_, &rc);
        if (FAILED(hr)) {
            return false;
        }

        pillCount_ = 0;  // DrawPill records each pill it draws for the backdrop windows

        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(dirty.left),
                                                            -static_cast<float>(dirty.top)));
        // Clear() honours the active clip, so this only wipes the dirty region.
        target_->PushAxisAlignedClip(
            D2D1::RectF(static_cast<float>(dirty.left), static_cast<float>(dirty.top),
                        static_cast<float>(dirty.right), static_cast<float>(dirty.bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        target_->Clear(D2D1::ColorF(0, 0.0f));
        BeginFrameResources();

        EnsureBrushes(settings, state, now);
        settingsOpacity_ = settings.pillOpacity;

        const bool gameMetricsPresent = primary.kind == IslandKind::Idle &&
            (settings.gameOverlay || g_gameOverlayPinned.load());
        // True while the island is collapsing to nothing (auto-hide / fullscreen).
        islandHiding_ = primary.width < 1.0f && primary.height < 1.0f && !secondary;

        const float hoverScale = (settings.expandOnHover && ((hover && !gameMetricsPresent) || pinned)) ? 1.025f : 1.0f;
        const float scale = hoverScale;
        hoverScale_ = scale;


        if (width >= 2.0f && height >= 2.0f) {
            if (secondary) {
                const float gap = 12.0f * settings.sizeScale;
                const float maxH = std::max(primary.height, secondary->height);
                // Notch / border-merged islands hang from the top edge, so the
                // shorter pill stays flush with it instead of being centred
                // inside the taller one's height (which pushed it down).
                const bool hangFromTop = settings.notchStyle || settings.borderMergedMode;
                const float pTop = hangFromTop ? top : top + (maxH - primary.height) * 0.5f;
                const float sTop = hangFromTop ? top : top + (maxH - secondary->height) * 0.5f;

                DrawPill(state, settings, primary,
                         D2D1::RectF(left, pTop, left + primary.width, pTop + primary.height),
                         scale, now);
                DrawPill(state, settings, *secondary,
                         D2D1::RectF(left + primary.width + gap, sTop,
                                      left + primary.width + gap + secondary->width,
                                      sTop + secondary->height),
                         scale, now);
            } else {
                DrawPill(state, settings, primary,
                         D2D1::RectF(left, top, left + width, top + height), scale, now);
            }
        }

        target_->PopAxisAlignedClip();
        hr = target_->EndDraw();
        if (FAILED(hr)) {
            stageValid_ = false;
            return false;
        }

        POINT src = {0, 0};
        SIZE size = {bitmapWidth_, bitmapHeight_};
        POINT dst = stage.origin;  // constant while the pill animates

        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = alpha;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UPDATELAYEREDWINDOWINFO info = {};
        info.cbSize = sizeof(info);
        info.pptDst = &dst;
        info.psize = &size;
        info.hdcSrc = memDc_;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = fullPaint ? nullptr : &dirty;  // only this rect is re-composited

        const bool presented = UpdateLayeredWindowIndirect(hwnd_, &info) != FALSE;
        if (presented) {
            stageValid_ = true;
            lastOrigin_ = stage.origin;
            lastBounds_ = bounds;
            lastAlpha_ = alpha;
            if (fullPaint) lastFullPaint_ = now;
            g_pillClientRect = PillClientRect{left, top, left + width, top + height, true};
        } else {
            stageValid_ = false;
        }

        if (presented) {
            DwmBackdrop::Sync(hwnd_, stage.origin, pillRects_, pillRadii_, pillCount_,
                              settings.notchStyle, settings.backdropMaterial, pillBgColor_,
                              settings.backdropTint);
        }
        return presented;
    }

    void Shutdown() {
        ReleaseCachedResources();
        marqueeTitleCache_.layout.Reset();
        marqueeArtistCache_.layout.Reset();
        marqueeAlbumCache_.layout.Reset();
        marqueeClipboardCache_.layout.Reset();
        marqueeNotificationCache_.layout.Reset();
        scratchColorBrush_.Reset();
        karaokeCache_.layout.Reset();

        artBitmap_.Reset();
        notificationIconBitmap_.Reset();
        for (ComPtr<ID2D1Bitmap>& bitmap : notificationCenterBitmaps_) {
            bitmap.Reset();
        }
        notificationCenterGenerations_.fill(0);
        mediaSourceIconBitmap_.Reset();
        clipboardIconBitmap_.Reset();
        clipboardImageBitmap_.Reset();
        accentBrush_.Reset();
        redBrush_.Reset();
        textBrush_.Reset();
        mutedBrush_.Reset();
        tintBrush_.Reset();
        shadowBrush_.Reset();
        micDotBrush_.Reset();
        unreadDotBrush_.Reset();
        micGlowBrush_.Reset();
        camDotBrush_.Reset();
        camGlowBrush_.Reset();
        weatherDescFormat_.Reset();
        micGlowBrush_.Reset();
        camDotBrush_.Reset();
        camGlowBrush_.Reset();
        target_.Reset();
        textFormat_.Reset();
        smallTextFormat_.Reset();
        boldTextFormat_.Reset();
        hugeTextFormat_.Reset();
        clockFormat_.Reset();
        iconFormat_.Reset();
        mediaPlayIconFormat_.Reset();
        mediaNavIconFormat_.Reset();
        idleTextFormat_.Reset();
        calDayLargeFormat_.Reset();
        calGridFormat_.Reset();
        dwriteFactory_.Reset();
        skipTriFull_.Reset();
        skipTriNotch_.Reset();
        titleOld_.clear();
        titleNew_.clear();
        titleAnimActive_ = false;
        roundJoinStyle_.Reset();
        artFlipOld_.Reset();
        pillArtOld_.Reset();
        pillArtLast_.Reset();
        pillArtStart_ = -1.0;
        pillArtInit_ = false;
        swapArtist_.prevCache.layout.Reset();
        swapAlbum_.prevCache.layout.Reset();
        d2dFactory_.Reset();

        if (oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (memDc_) {
            DeleteDC(memDc_);
            memDc_ = nullptr;
        }
    }

    // Measures the collapsed idle strip so the render loop can size the island to
    // the text it is actually about to paint, instead of the old fixed 96/170px.
    // DrawIdleDashboard calls this too and lays the slots out from the same
    // numbers, so the pill can never be sized for a clock width the painter is
    // not using.
    IdleStripMetrics MeasureIdleStrip(const SharedState& state, const Settings& settings,
                                      double now) {
        // Idempotent and cheap when nothing changed, but necessary here: textScale
        // drives the idle font size, so measuring before the formats are rebuilt
        // would size the pill for the previous Text size setting.
        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);

        IdleStripMetrics metrics;
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring clock = FormatIslandTime(local, settings.clockFollowSystem,
                                                    settings.use24HourClock, settings.showSeconds);
        metrics.clockWidth =
            MeasureTextWidthCached(WidestDigitForm(clock), fmt, idleClockWidthCache_);

        metrics.hasWeather = settings.weather;
        if (metrics.hasWeather) {
            // Mirrors the label DrawIdleDashboard builds, including the no-data
            // placeholder, so the reserved slot matches what gets drawn.
            const bool hasData = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
            wchar_t label[32] = {};
            if (hasData) {
                std::wstring icon = L"\U0001F321\uFE0F";
                std::wstring desc = state.weather.weatherDesc;
                GetWeatherIconAndText(state.weather.weatherCode, icon, desc);
                swprintf_s(label, L"%s %.0f\x00B0", icon.c_str(), state.weather.temperature);
            } else {
                wcscpy_s(label, ARRAYSIZE(label), L"\U0001F321\uFE0F --\x00B0");
            }
            metrics.weatherWidth =
                MeasureTextWidthCached(WidestDigitForm(label), fmt, idleWeatherWidthCache_);
        }

        metrics.hasPrivacy =
            (state.system.micActive && settings.privacyDots && settings.privacyDotsMic) ||
            (state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam);

        float total = IdleStripLayout::kPadX * 2.0f + metrics.clockWidth;
        if (metrics.hasWeather) {
            total += IdleStripLayout::kSlotGap * 2.0f + IdleStripLayout::kDividerWidth +
                     metrics.weatherWidth;
        }
        metrics.hasUnread = NotificationCenterUnread(settings);
        total += IdleDotReserve(metrics);

        total = std::ceil(total / IdleStripLayout::kWidthQuantum) * IdleStripLayout::kWidthQuantum;
        metrics.totalWidth = Clamp(total, IdleStripLayout::kMinWidth, IdleStripLayout::kMaxWidth);
        return metrics;
    }

    // Collapsed media pill, with the optional clock. Returns content-space
    // sizes; the render loop multiplies by sizeScale like every other collapsed
    // width. Measured in the widest-digit form so the width does not wobble as
    // the digits change.
    MediaPillMetrics MeasureMediaPill(const SharedState& state, const Settings& settings) {
        (void)state;
        MediaPillMetrics metrics;
        if (!settings.mediaPillClock) {
            return metrics;
        }

        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring clock = FormatIslandTime(local, settings.clockFollowSystem,
                                                    settings.use24HourClock, settings.showSeconds);
        metrics.clockWidth =
            MeasureTextWidthCached(WidestDigitForm(clock), fmt, idleClockWidthCache_);

        float section = MediaPillLayout::kClockPadLeft + metrics.clockWidth +
                        MediaPillLayout::kSlotGap + MediaPillLayout::kDividerWidth;
        section = std::ceil(section / MediaPillLayout::kWidthQuantum) * MediaPillLayout::kWidthQuantum;
        metrics.sectionWidth = section;
        metrics.totalWidth = MediaPillLayout::kBaseWidth + section;
        return metrics;
    }

   private:
    // Keyed on the string alone. EnsureTextFormats invalidates both caches
    // whenever it rebuilds the formats, so a cached width can never outlive the
    // font size it was measured at -- comparing the IDWriteTextFormat pointer
    // would not be enough, since a rebuilt format can land on the freed address.
    struct TextWidthCache {
        std::wstring key;
        float width = 0.0f;
        bool valid = false;
    };
    TextWidthCache idleClockWidthCache_;
    TextWidthCache idleWeatherWidthCache_;

    float MeasureTextWidthCached(const std::wstring& text, IDWriteTextFormat* fmt,
                                 TextWidthCache& cache) {
        if (cache.valid && cache.key == text) {
            return cache.width;
        }

        float width = 0.0f;
        if (fmt && dwriteFactory_ && !text.empty()) {
            ComPtr<IDWriteTextLayout> layout;
            // Effectively unbounded wrap width: the idle formats are NO_WRAP, and
            // we want the natural advance, not a wrapped block.
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    text.c_str(), static_cast<UINT32>(text.size()), fmt,
                    4096.0f, IdleStripLayout::kHeight, &layout)) &&
                layout) {
                DWRITE_TEXT_METRICS tm = {};
                if (SUCCEEDED(layout->GetMetrics(&tm))) {
                    width = std::max(tm.width, tm.widthIncludingTrailingWhitespace);
                }
            }
        }

        cache.key = text;
        cache.width = width;
        cache.valid = true;
        return width;
    }

    bool CreateBackingBitmap(int width, int height) {
        const int logicalW = width;
        const int logicalH = height;
        // Expand/collapse changes the needed size almost every frame. Reuse the
        // existing DIB whenever it is big enough; UpdateLayeredWindow only reads
        // the (0,0,width,height) region, so a larger DIB is fine.
        if (dib_ && width <= bitmapCapW_ && height <= bitmapCapH_) {
            bitmapWidth_ = width;
            bitmapHeight_ = height;
            return true;
        }

        // Round up with headroom so growing animations rarely reallocate.
        width = ((width + 63) / 64) * 64 + 64;
        height = ((height + 31) / 32) * 32 + 32;

        if (oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (!memDc_) {
            HDC screen = GetDC(nullptr);
            memDc_ = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!memDc_) {
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
        dib_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib_) {
            return false;
        }

        oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, dib_));
        bitmapCapW_ = width;
        bitmapCapH_ = height;
        bitmapWidth_ = logicalW;
        bitmapHeight_ = logicalH;
        return true;
    }

    float lastFontScale_ = 0.0f;
    float lastTextScale_ = 0.0f;
    std::wstring lastFontFamily_;
    // textScale is an independent typography multiplier (the Text size setting):
    // sizeScale magnifies the whole island via a transform, whereas this changes
    // only the type, so the island can stay compact while the clock and labels
    // get bigger.
    void EnsureTextFormats(float scale, const std::wstring& fontFamily, float textScale) {
        if (std::abs(scale - lastFontScale_) < 0.001f && fontFamily == lastFontFamily_ &&
            std::abs(textScale - lastTextScale_) < 0.001f) {
            return;
        }
        lastTextScale_ = textScale;
        const float ts = Clamp(textScale, 0.7f, 1.6f);

        // Every format below is about to be recreated at a new size, so any
        // width measured against the old ones is stale.
        idleClockWidthCache_.valid = false;
        idleWeatherWidthCache_.valid = false;

        textFormat_ = nullptr;
        smallTextFormat_ = nullptr;
        clockFormat_ = nullptr;
        boldTextFormat_ = nullptr;
        hugeTextFormat_ = nullptr;
        iconFormat_ = nullptr;
        mediaPlayIconFormat_ = nullptr;
        mediaNavIconFormat_ = nullptr;
        idleTextFormat_ = nullptr;
        calDayLargeFormat_ = nullptr;
        calGridFormat_ = nullptr;
        timeDashboardFormat_ = nullptr;
        dateDashboardFormat_ = nullptr;

        const wchar_t* defaultDisplay = L"Segoe UI Variable Display";
        const wchar_t* defaultSmall = L"Segoe UI Variable Small";
        const wchar_t* mainFamily = fontFamily.empty() ? defaultDisplay : fontFamily.c_str();
        const wchar_t* smallFamily = fontFamily.empty() ? defaultSmall : fontFamily.c_str();

        auto createFormat = [&](const wchar_t* family, const wchar_t* fallback,
                                DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                                DWRITE_FONT_STRETCH stretch, float size,
                                ComPtr<IDWriteTextFormat>& out) {
            HRESULT hr = dwriteFactory_->CreateTextFormat(
                family, nullptr, weight, style, stretch, size, L"", &out);
            if (FAILED(hr) || !out) {
                if (fallback && wcscmp(family, fallback) != 0) {
                    hr = dwriteFactory_->CreateTextFormat(
                        fallback, nullptr, weight, style, stretch, size, L"", &out);
                }
                if (FAILED(hr) || !out) {
                    dwriteFactory_->CreateTextFormat(
                        L"Segoe UI", nullptr, weight, style, stretch, size, L"", &out);
                }
            }
        };

        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 13.5f * ts, textFormat_);
        createFormat(smallFamily, defaultSmall,
                     DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 11.0f * ts, smallTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 18.0f * ts, clockFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 12.0f * ts, boldTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 42.0f * ts, hugeTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 13.0f * ts, idleTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 63.0f * ts, calDayLargeFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 14.4f * ts, calGridFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 56.0f * ts, timeDashboardFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 16.0f * ts, dateDashboardFormat_);

        usingFluentIcons_ = false;
        ComPtr<IDWriteFontCollection> sysFonts;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->GetSystemFontCollection(&sysFonts, FALSE)) && sysFonts) {
            UINT32 fontIdx = 0;
            BOOL fontFound = FALSE;
            if (SUCCEEDED(sysFonts->FindFamilyName(L"Segoe Fluent Icons", &fontIdx, &fontFound)) && fontFound) {
                usingFluentIcons_ = true;
            }
        }

        const wchar_t* iconFontFamily = usingFluentIcons_ ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

        HRESULT hrIcon = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", &iconFormat_);
        if (FAILED(hrIcon) || !iconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", &iconFormat_);
        }

        HRESULT hrPlay = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &mediaPlayIconFormat_);
        if (FAILED(hrPlay) || !mediaPlayIconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &mediaPlayIconFormat_);
        }

        HRESULT hrNav = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &mediaNavIconFormat_);
        if (FAILED(hrNav) || !mediaNavIconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &mediaNavIconFormat_);
        }

        if (textFormat_) {
            textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (smallTextFormat_) {
            smallTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (boldTextFormat_) {
            boldTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            // Match every other centered format: without this the text hugs the
            // top of its layout rect, which left the weather dashboard's city /
            // icon / temperature visually "stuck" near the top of the island.
            boldTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (hugeTextFormat_) {
            hugeTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            hugeTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (clockFormat_) {
            clockFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            clockFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            clockFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (idleTextFormat_) {
            idleTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            idleTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            idleTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (calDayLargeFormat_) {
            calDayLargeFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            calDayLargeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            calDayLargeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (calGridFormat_) {
            calGridFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            calGridFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            calGridFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (timeDashboardFormat_) {
            timeDashboardFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            timeDashboardFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            timeDashboardFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (dateDashboardFormat_) {
            dateDashboardFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            dateDashboardFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dateDashboardFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (iconFormat_) {
            iconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (mediaPlayIconFormat_) {
            mediaPlayIconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            mediaPlayIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            mediaPlayIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (mediaNavIconFormat_) {
            mediaNavIconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            mediaNavIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            mediaNavIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        if (fontFamily != lastFontFamily_) {
            marqueeTitleCache_.layout.Reset();
            marqueeArtistCache_.layout.Reset();
            marqueeAlbumCache_.layout.Reset();
            marqueeClipboardCache_.layout.Reset();
            marqueeNotificationCache_.layout.Reset();
            weatherDescFormat_.Reset();
            weatherDescFormatSize_ = -1.0f;
        }

        karaokeCache_.layout.Reset();
        karaokeCache_.valid = false;

        // Collapsed lyric layouts were built from the old font; rebuild them.
        collapsedLyricCur_ = CollapsedLyricLine{};
        collapsedLyricPrev_ = CollapsedLyricLine{};
        collapsedLyricIdx_ = -2;

        lastFontScale_ = scale;
        lastFontFamily_ = fontFamily;
    }

    void EnsureBrushes(const Settings& settings, const SharedState& state, double now) {
        D2D1_COLOR_F targetAccent = settings.customAccent;
        if (settings.accentMode == AccentMode::System) {
            targetAccent = GetSystemAccentColor();
        } else if (settings.accentMode == AccentMode::Auto && state.media.art) {
            // Already contrast-corrected when the art was decoded.
            targetAccent = state.media.art->sampledAccent;
        } else {
            // A custom or system accent was never checked against the island's
            // own background. That did not matter while every theme was dark,
            // but it does now that one is light: the default cyan sits at about
            // 1.6:1 on Porcelain. Corrected before the smoothing lerp so the
            // value is stable rather than re-derived every frame.
            targetAccent = EnsureContrastAgainstBackground(targetAccent, settings.pillBgColor);
        }

        // Smooth accent transition: exponential lerp toward target, ~300ms half-life.
        // On the very first frame (lastAccentTime_ < 0) snap immediately so there's
        // no fade-in from the default color on startup.
        if (lastAccentTime_ < 0.0) {
            currentAccent_ = targetAccent;
            lastAccentTime_ = now;
        } else {
            const double dt = std::max(0.0, std::min(now - lastAccentTime_, 0.1));  // cap at 100ms
            lastAccentTime_ = now;
            // k = 1 - exp(-dt / tau), tau ≈ 0.18s → reaches 95% in ~350ms
            const float k = 1.0f - static_cast<float>(std::exp(-dt / 0.18));
            currentAccent_.r += (targetAccent.r - currentAccent_.r) * k;
            currentAccent_.g += (targetAccent.g - currentAccent_.g) * k;
            currentAccent_.b += (targetAccent.b - currentAccent_.b) * k;
            currentAccent_.a = 1.0f;
        }

        const D2D1_COLOR_F accent = currentAccent_;

        if (!accentBrush_) target_->CreateSolidColorBrush(accent, &accentBrush_);
        else accentBrush_->SetColor(accent);

        if (!redBrush_) target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.27f, 0.27f, 1.0f), &redBrush_);

        // Use user-configured text colors.
        D2D1_COLOR_F primary = settings.textPrimaryColor;
        primary.a = 0.98f;
        if (!textBrush_) target_->CreateSolidColorBrush(primary, &textBrush_);
        else textBrush_->SetColor(primary);

        D2D1_COLOR_F secondary = settings.textSecondaryColor;
        secondary.a = 0.90f;
        if (!mutedBrush_) target_->CreateSolidColorBrush(secondary, &mutedBrush_);
        else mutedBrush_->SetColor(secondary);

        // Keep the authored alpha. An 8-digit #RRGGBBAA background is how a
        // translucent island is requested independently of the global pill
        // transparency slider; DrawPillSurface combines the two.
        pillBgColor_ = settings.pillBgColor;

        D2D1_COLOR_F tintColor = D2D1::ColorF(0.010f, 0.010f, 0.012f, settings.tintOpacity);
        if (!tintBrush_) target_->CreateSolidColorBrush(tintColor, &tintBrush_);
        else tintBrush_->SetColor(tintColor);

        if (!shadowBrush_) target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.70f), &shadowBrush_);

        BuildMaterialTokens(settings);
    }

    // ── Premium material system ─────────────────────────────────────────────
    // Every surface in the island is built from one small token set so the whole
    // UI shares a single visual language. Tokens are recomputed once per frame
    // from the resolved theme, and adapt to background luminance so a light
    // custom background gets dark separators instead of washed-out white ones.
    struct MaterialTokens {
        D2D1_COLOR_F base{};          // the pill's own fill
        D2D1_COLOR_F raised{};        // inner cards, chips, wells
        D2D1_COLOR_F raisedStrong{};  // pressed / active chips
        D2D1_COLOR_F hairline{};      // 1px separators between content
        D2D1_COLOR_F stroke{};        // outer contour
        D2D1_COLOR_F shadow{};        // drop shadow tint
        D2D1_COLOR_F textPrimary{};
        D2D1_COLOR_F textSecondary{};
        D2D1_COLOR_F textTertiary{};
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F accentSoft{};
        bool onDark = true;
        float opacity = 1.0f;
    };

    static D2D1_COLOR_F MixColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
        return D2D1::ColorF(a.r + (b.r - a.r) * t,
                            a.g + (b.g - a.g) * t,
                            a.b + (b.b - a.b) * t,
                            a.a + (b.a - a.a) * t);
    }

    static D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float alpha) {
        c.a = Clamp(alpha, 0.0f, 1.0f);
        return c;
    }

    // ── Cached Direct2D resources ───────────────────────────────────────────
    // Nothing below is created per frame once it has warmed up. Render thread only.
    struct LinearGradientSlot { ComPtr<ID2D1LinearGradientBrush> brush; uint64_t key = 0; };
    struct RadialGradientSlot { ComPtr<ID2D1RadialGradientBrush> brush; uint64_t key = 0; };

    struct ShapeKey {
        float l = 0, t = 0, r = 0, b = 0, radius = 0;
        bool notch = false;
        bool operator==(const ShapeKey& o) const {
            return l == o.l && t == o.t && r == o.r && b == o.b &&
                   radius == o.radius && notch == o.notch;
        }
    };
    struct ShapeEntry {
        ShapeKey key;
        ComPtr<ID2D1Geometry> geom;
        uint64_t lastUsed = 0;
    };
    static constexpr size_t kShapeCacheSize = 32;

    static uint32_t PackColor8(const D2D1_COLOR_F& c) {
        auto q = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
    }

    static uint64_t HashStops(const D2D1_GRADIENT_STOP* s, UINT32 n) {
        uint64_t h = 1469598103934665603ull;
        for (UINT32 i = 0; i < n; ++i) {
            h = (h ^ PackColor8(s[i].color)) * 1099511628211ull;
            h = (h ^ static_cast<uint64_t>(s[i].position * 1000.0f + 0.5f)) * 1099511628211ull;
        }
        return h;
    }

    // Call once per frame, right after BeginDraw().
    void BeginFrameResources() {
        brushPoolUsed_ = 0;
        layerPoolUsed_ = 0;
    }

    // Returns a brush that is unique for the rest of this frame. Colour and opacity are
    // reset, so a previous user's SetOpacity() can never leak into the next one.
    ID2D1SolidColorBrush* AcquireBrush(const D2D1_COLOR_F& color) {
        if (brushPoolUsed_ >= brushPool_.size()) {
            ComPtr<ID2D1SolidColorBrush> b;
            if (FAILED(target_->CreateSolidColorBrush(color, &b)) || !b) {
                return nullptr;
            }
            brushPool_.push_back(std::move(b));
        }
        ID2D1SolidColorBrush* brush = brushPool_[brushPoolUsed_++].Get();
        brush->SetColor(color);
        brush->SetOpacity(1.0f);
        return brush;
    }

    // Same idea for layers. Each Acquire is distinct within a frame, so nested
    // PushLayer calls are safe, and the layer's backing store is kept between frames.
    ID2D1Layer* AcquireLayer() {
        if (layerPoolUsed_ >= layerPool_.size()) {
            ComPtr<ID2D1Layer> l;
            if (FAILED(target_->CreateLayer(&l)) || !l) {
                return nullptr;
            }
            layerPool_.push_back(std::move(l));
        }
        return layerPool_[layerPoolUsed_++].Get();
    }

    void SetPersistentBrush(ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& c) {
        if (!brush) {
            target_->CreateSolidColorBrush(c, &brush);
        } else {
            brush->SetColor(c);
        }
    }

    // The collection + brush are rebuilt only when the stops change; start/end move per call.
    ID2D1LinearGradientBrush* UseLinearGradient(LinearGradientSlot& slot,
                                                const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                                D2D1_POINT_2F start, D2D1_POINT_2F end) {
        const uint64_t key = HashStops(stops, count);
        if (!slot.brush || slot.key != key) {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                             D2D1_EXTEND_MODE_CLAMP, &coll)) || !coll) {
                return nullptr;
            }
            if (FAILED(target_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(start, end), coll.Get(), &slot.brush)) ||
                !slot.brush) {
                slot.brush.Reset();
                return nullptr;
            }
            slot.key = key;
        }
        slot.brush->SetStartPoint(start);
        slot.brush->SetEndPoint(end);
        return slot.brush.Get();
    }

    ID2D1RadialGradientBrush* UseRadialGradient(RadialGradientSlot& slot,
                                                const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                                D2D1_POINT_2F center, float rx, float ry) {
        const uint64_t key = HashStops(stops, count);
        if (!slot.brush || slot.key != key) {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                             D2D1_EXTEND_MODE_CLAMP, &coll)) || !coll) {
                return nullptr;
            }
            if (FAILED(target_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), rx, ry),
                    coll.Get(), &slot.brush)) ||
                !slot.brush) {
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

    // Rounded-rect or notch geometry, cached by rect + radius. Returns a ref-counted
    // pointer, so a caller holding it is safe even if the entry is evicted meanwhile.
    ComPtr<ID2D1Geometry> GetShapeGeometry(D2D1_RECT_F rect, float radius, bool notch) {
        const ShapeKey key{rect.left, rect.top, rect.right, rect.bottom, radius, notch};
        ShapeEntry* victim = &shapeCache_[0];
        for (ShapeEntry& e : shapeCache_) {
            if (e.geom && e.key == key) {
                e.lastUsed = ++shapeClock_;
                return e.geom;
            }
            if (e.lastUsed < victim->lastUsed) {
                victim = &e;  // empty entries have lastUsed == 0, so they go first
            }
        }

        ComPtr<ID2D1Geometry> geom;
        if (notch) {
            ComPtr<ID2D1PathGeometry> path = CreateNotchGeometry(rect, radius);
            if (path) path.As(&geom);
        }
        if (!geom) {  // not a notch, or the notch failed: same fallback as before
            ComPtr<ID2D1RoundedRectangleGeometry> rr;
            if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(rect, radius, radius), &rr)) && rr) {
                rr.As(&geom);
            }
        }
        if (!geom) return nullptr;

        victim->key = key;
        victim->geom = geom;
        victim->lastUsed = ++shapeClock_;
        return geom;
    }

    void ReleaseCachedResources() {
        brushPool_.clear();
        layerPool_.clear();
        brushPoolUsed_ = 0;
        layerPoolUsed_ = 0;
        raisedBrush_.Reset();
        raisedStrongBrush_.Reset();
        depthGrad_ = LinearGradientSlot{};
        trackGrad_ = LinearGradientSlot{};
        bloomGrad_ = RadialGradientSlot{};
        swapGroupLayer_.Reset();
        for (ShapeEntry& e : shapeCache_) e = ShapeEntry{};
    }

    void BuildMaterialTokens(const Settings& settings) {
        MaterialTokens t;
        t.opacity = settingsOpacity_;
        t.base = pillBgColor_;

        // Light surfaces need dark scrims/edges, dark surfaces need light ones.
        const bool onDark = RelativeLuminance(t.base) < 0.45;
        t.onDark = onDark;

        const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        const D2D1_COLOR_F black = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
        const D2D1_COLOR_F lift = onDark ? white : black;

        // Cards carry a slightly stronger fill than before. They used to be
        // outlined with a hairline ring, and losing that ring is what the fill
        // now has to compensate for so a card still reads as a distinct surface.
        t.raised = WithAlpha(lift, onDark ? 0.085f : 0.062f);
        t.raisedStrong = WithAlpha(lift, onDark ? 0.150f : 0.105f);
        t.hairline = WithAlpha(lift, onDark ? 0.100f : 0.085f);
        t.shadow = WithAlpha(black, 0.55f);

        t.stroke = settings.contourBorderColor;
        t.accent = currentAccent_;
        t.accentSoft = WithAlpha(currentAccent_, 0.18f);

        t.textPrimary = WithAlpha(settings.textPrimaryColor, 0.98f);
        t.textSecondary = WithAlpha(settings.textSecondaryColor, 0.88f);
        // Derived rather than configured, so a third tier always reads as
        // quieter than "secondary" whatever the user picked.
        t.textTertiary = WithAlpha(MixColor(settings.textSecondaryColor, t.base, 0.35f), 0.72f);

        material_ = t;
        SetPersistentBrush(raisedBrush_, t.raised);
        SetPersistentBrush(raisedStrongBrush_, t.raisedStrong);
    }

    void DrawPill(const SharedState& state, const Settings& settings, const Activity& activity,
                  D2D1_RECT_F rect, float scale, double now) {
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float w = (rect.right - rect.left) * scale;
        const float h = (rect.bottom - rect.top) * scale;
        rect = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);

        float radius = settings.w11Style ? 8.0f * settings.sizeScale : (rect.bottom - rect.top) * 0.5f;
        if (settings.notchStyle) {
            radius = 16.0f * settings.sizeScale;
        } else if (!settings.w11Style) {
            radius = std::min(radius, 44.0f * settings.sizeScale);
        }
        if (pillCount_ < DwmBackdrop::kMaxPills) {
            pillRects_[pillCount_] = rect;
            pillRadii_[pillCount_] = radius;
            ++pillCount_;
        }
        DrawSoftShadow(rect, radius);

        DrawPillSurface(rect, radius, activity.kind, settings);

        if (activity.kind == IslandKind::Progress) {
            DrawProgressRing(rect, state.progress.percent);
        }

        if (activity.kind == IslandKind::BatteryLow) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 2.1));
            redBrush_->SetOpacity(0.45f + 0.45f * pulse);
            DrawIslandShape(rect, radius, settings.w11Style, settings.notchStyle, redBrush_.Get(), 2.0f);
            redBrush_->SetOpacity(1.0f);
        }

        // No inner highlight ring here. A second bright hairline just inside the
        // contour read as a light rim tracing the whole island, which is exactly
        // the edge glow this design drops. The contour stroke alone defines the
        // silhouette now.

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        D2D1_POINT_2F pillCenter = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        target_->SetTransform(D2D1::Matrix3x2F::Scale(settings.sizeScale, settings.sizeScale, pillCenter) * oldTransform);

        float invScale = 1.0f / settings.sizeScale;
        float unW = (rect.right - rect.left) * invScale;
        float unH = (rect.bottom - rect.top) * invScale;
        D2D1_RECT_F unscaledRect = D2D1::RectF(pillCenter.x - unW * 0.5f, pillCenter.y - unH * 0.5f, pillCenter.x + unW * 0.5f, pillCenter.y + unH * 0.5f);

        switch (activity.kind) {
            case IslandKind::Media:
                DrawMedia(state, unscaledRect, settings, now);
                break;
            case IslandKind::Clipboard:
                DrawClipboard(state, unscaledRect);
                break;
            case IslandKind::Notification:
                DrawNotification(state, unscaledRect);
                break;
            case IslandKind::Volume:
                DrawVolume(state, unscaledRect);
                break;
            case IslandKind::Brightness:
                DrawBrightness(state, unscaledRect);
                break;
            case IslandKind::CapsLock:
                DrawCapsLock(state, unscaledRect);
                break;
            case IslandKind::Device:
                DrawDevice(state, unscaledRect);
                break;
            case IslandKind::Bluetooth:
                DrawBluetoothDevice(state, unscaledRect);
                break;
            case IslandKind::Timer:
                DrawTimer(state, unscaledRect);
                break;
            case IslandKind::BatteryLow:
                DrawBattery(state, unscaledRect);
                break;
            case IslandKind::Progress:
                DrawProgress(state, unscaledRect);
                break;
            case IslandKind::DoNotDisturb:
                DrawDoNotDisturb(state, unscaledRect);
                break;
            case IslandKind::Lookup:
                DrawLookup(state, unscaledRect, settings, now);
                break;
            case IslandKind::Idle:
            default:
                DrawIdleDashboard(state, unscaledRect, settings, now);
                break;
        }

        // ── Apple-style privacy indicator dots ───────────────────────────────
        // Green dot = camera in use, Orange dot = mic in use.
        // Drawn in top-right corner of pill, outside content area.
        DrawPrivacyDots(state, settings, unscaledRect, now);

        target_->SetTransform(oldTransform);
    }

    // A real soft shadow, approximated by stacking concentric rounded shapes
    // with a quadratic alpha falloff. There is no GPU device here (this renders
    // to a DC-bound target for UpdateLayeredWindow), so a Gaussian blur effect
    // is not available -- but the render padding around the pill leaves room to
    // fake it convincingly and cheaply.
    void DrawSoftShadow(D2D1_RECT_F rect, float radius) {
        // The island sits on top of the backdrop, so a shadow here would darken the blur.
        if (!g_settings.dropShadow || g_settings.backdropMaterial != BackdropMaterial::None) {
            return;
        }

        const float spread = Clamp(14.0f * g_settings.sizeScale, 6.0f, kRenderPadY - 4.0f);
        const float yOffset = spread * 0.35f;
        constexpr int kSteps = 7;

        ID2D1SolidColorBrush* brush = AcquireBrush(material_.shadow);
        if (!brush) {
            return;
        }

        for (int i = kSteps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float grow = spread * t;
            // Quadratic falloff keeps the core dense and the outer edge feathered.
            const float alpha = material_.shadow.a * (1.0f - t) * (1.0f - t) * 0.55f * settingsOpacity_;
            if (alpha <= 0.002f) {
                continue;
            }
            brush->SetOpacity(alpha);

            const D2D1_RECT_F shadowRect = D2D1::RectF(
                rect.left - grow, rect.top - grow * 0.55f + yOffset,
                rect.right + grow, rect.bottom + grow + yOffset);
            FillIslandShape(shadowRect, radius + grow, g_settings.w11Style,
                            g_settings.notchStyle, brush);
        }
        brush->SetOpacity(1.0f);
    }

    // Vertical depth shading. This used to open with a white sheen across the
    // top, which on a dark island read as a lit strip along the upper edge --
    // the same edge-glow look the rest of this design removes. What is left is
    // a single downward shade that grounds the surface without lighting any
    // edge: neutral at the top, gradually deeper toward the bottom.
    void FillSurfaceDepth(D2D1_RECT_F rect, float radius, bool strong) {
        const float h = rect.bottom - rect.top;
        if (h <= 2.0f) {
            return;
        }

        // Light backgrounds need less of it: the same alpha over near-white
        // turns into a visible grey wash rather than subtle depth.
        const float botA = (strong ? 0.085f : 0.060f) *
                           (material_.onDark ? 1.0f : 0.55f) * settingsOpacity_;

        // Stops are constant; `botA` is applied as brush opacity (identical result,
        // since every stop alpha was just a multiple of botA). One brush forever.
        static const D2D1_GRADIENT_STOP kStops[3] = {
            {0.0f,  D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
            {0.45f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f)},
            {1.0f,  D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)},
        };
        ID2D1LinearGradientBrush* brush = UseLinearGradient(
            depthGrad_, kStops, 3,
            D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom));
        if (!brush) {
            return;
        }
        brush->SetOpacity(Clamp(botA, 0.0f, 1.0f));
        FillIslandShape(rect, radius, g_settings.w11Style, g_settings.notchStyle, brush);
    }

    // A wide, very soft accent wash bled in from the top of the surface. Driven
    // by the album-art accent, this is what makes the media surface feel alive
    // without tinting the whole pill.
    void FillAccentBloom(D2D1_RECT_F rect, float radius, float strength) {
        if (strength <= 0.01f) {
            return;
        }

        const float w = rect.right - rect.left;
        const float h = rect.bottom - rect.top;
        if (w <= 2.0f || h <= 2.0f) {
            return;
        }

        // Only the accent colour is baked into the stops. Strength is brush opacity, so
        // the gradient is rebuilt only while the accent is actually changing colour.
        const D2D1_COLOR_F a = material_.accent;
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(a.r, a.g, a.b, 1.0f)},
            {1.0f, D2D1::ColorF(a.r, a.g, a.b, 0.0f)},
        };
        const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f, rect.top);
        ID2D1RadialGradientBrush* brush =
            UseRadialGradient(bloomGrad_, stops, 2, center, w * 0.75f, h * 1.15f);
        if (!brush) {
            return;
        }
        brush->SetOpacity(Clamp(strength * settingsOpacity_, 0.0f, 1.0f));
        FillIslandShape(rect, radius, g_settings.w11Style, g_settings.notchStyle, brush);
    }

    // Rounded inner card used by every dashboard, so panels across the media,
    // calendar, weather, hardware and file-tray surfaces match exactly.
    //
    // Fill only, no outline. Each card used to also get a hairline ring, and six
    // of those side by side in the hardware grid turned into a mesh of bright
    // edges competing with the content. The fill alpha in BuildMaterialTokens
    // was raised to carry the separation on its own.
    void DrawCard(D2D1_RECT_F rect, float radius, bool active = false) {
        ID2D1SolidColorBrush* fill = active ? raisedStrongBrush_.Get() : raisedBrush_.Get();
        if (fill) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), fill);
        }
    }

    // Accent-filled progress track shared by the scrubber, volume, battery and
    // timer, so "progress" looks the same everywhere.
    void DrawAccentTrack(D2D1_RECT_F track, float progress, float radius) {
        if (raisedStrongBrush_) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), raisedStrongBrush_.Get());
        }

        const float span = (track.right - track.left) * Clamp(progress, 0.0f, 1.0f);
        if (span <= 0.5f) {
            return;
        }

        const D2D1_RECT_F fillRect = D2D1::RectF(track.left, track.top, track.left + span, track.bottom);

        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, WithAlpha(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.30f), 0.95f)},
            {1.0f, WithAlpha(material_.accent, 1.0f)},
        };
        if (ID2D1LinearGradientBrush* grad = UseLinearGradient(
                trackGrad_, stops, 2,
                D2D1::Point2F(fillRect.left, fillRect.top),
                D2D1::Point2F(fillRect.right, fillRect.top))) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(fillRect, radius, radius), grad);
        }
    }

    void DrawPrivacyDots(const SharedState& state, const Settings& settings, D2D1_RECT_F rect, double now) {
        UNREFERENCED_PARAMETER(now);
        const float height = rect.bottom - rect.top;
        if (height > 55.0f) return;

        const bool mic = state.system.micActive && settings.privacyDots && settings.privacyDotsMic;
        const bool cam = state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam;
        const bool unread = NotificationCenterUnread(settings);
        if (!mic && !cam && !unread) return;

        const float dotR   = 4.0f;
        const float margin = 16.0f;
        const float dotY   = rect.top + (rect.bottom - rect.top) * 0.5f;

        const float x = rect.right - margin - dotR;

        if (unread) {
            D2D1_COLOR_F unreadColor = material_.accent;
            unreadColor.a = settingsOpacity_;
            if (!unreadDotBrush_) target_->CreateSolidColorBrush(unreadColor, &unreadDotBrush_);
            else unreadDotBrush_->SetColor(unreadColor);

            if (unreadDotBrush_) {
                const float unreadX = (mic || cam) ? x - notification_center::unread_dot_gap : x;
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(unreadX, dotY),
                                                   notification_center::unread_dot_radius,
                                                   notification_center::unread_dot_radius),
                                     unreadDotBrush_.Get());
            }
        }

        if (cam) {
            D2D1_COLOR_F camColor = settings.privacyDotsCamHex;
            camColor.a = settingsOpacity_;
            if (!camDotBrush_) target_->CreateSolidColorBrush(camColor, &camDotBrush_);
            else camDotBrush_->SetColor(camColor);

            if (camDotBrush_) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, dotY), dotR, dotR), camDotBrush_.Get());
            }
        } else if (mic) {
            D2D1_COLOR_F micColor = settings.privacyDotsMicHex;
            micColor.a = settingsOpacity_;
            if (!micDotBrush_) target_->CreateSolidColorBrush(micColor, &micDotBrush_);
            else micDotBrush_->SetColor(micColor);

            if (micDotBrush_) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, dotY), dotR, dotR), micDotBrush_.Get());
            }
        }
    }

    ComPtr<ID2D1PathGeometry> CreateNotchGeometry(D2D1_RECT_F rect, float radius) {
        ComPtr<ID2D1PathGeometry> geom;
        if (FAILED(d2dFactory_->CreatePathGeometry(&geom))) return nullptr;

        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geom->Open(&sink))) return nullptr;

        float r = std::min({radius, (rect.right - rect.left) * 0.5f, (rect.bottom - rect.top) * 0.5f});
        if (r < 0.0f) r = 0.0f;

        sink->BeginFigure(D2D1::Point2F(rect.left, rect.top), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(rect.right, rect.top));
        sink->AddLine(D2D1::Point2F(rect.right, rect.bottom - r));
        if (r > 0.0f) {
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(rect.right - r, rect.bottom),
                D2D1::SizeF(r, r), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(rect.left + r, rect.bottom));
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(rect.left, rect.bottom - r),
                D2D1::SizeF(r, r), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        } else {
            sink->AddLine(D2D1::Point2F(rect.left, rect.bottom));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();

        return geom;
    }

    ComPtr<ID2D1Geometry> CreateIslandMaskGeometry(D2D1_RECT_F rect, float radius, bool notchStyle) {
        return GetShapeGeometry(rect, radius, notchStyle);
    }

    void FillIslandShape(D2D1_RECT_F rect, float radius, bool w11Style, bool notchStyle, ID2D1Brush* brush) {
        if (!brush) return;
        if (notchStyle) {
            ComPtr<ID2D1Geometry> geom = GetShapeGeometry(rect, radius, true);
            if (geom) {
                target_->FillGeometry(geom.Get(), brush);
                return;
            }
        }
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    }

    void DrawIslandShape(D2D1_RECT_F rect, float radius, bool w11Style, bool notchStyle, ID2D1Brush* brush, float strokeWidth) {
        if (!brush) return;
        if (notchStyle) {
            auto geom = CreateNotchGeometry(rect, radius);
            if (geom) {
                target_->DrawGeometry(geom.Get(), brush, strokeWidth);
                return;
            }
        }
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, strokeWidth);
    }

    // The island's material, composed bottom-up:
    //   tint scrim -> base fill -> depth shading -> accent bloom -> contour
    // Each layer is individually subtle; together they give the pill depth
    // instead of the flat single-fill look it had before. Deliberately absent:
    // any bright rim, hairline or sheen tracing the island's edge.
    void DrawPillSurface(D2D1_RECT_F rect, float radius, IslandKind kind, const Settings& settings) {
        // The dark tint scrim exists to deepen an opaque background. With a real
        // backdrop enabled it would just mud up the blur, so it is skipped.
        if (tintBrush_ && settings.backdropMaterial == BackdropMaterial::None) {
            FillIslandShape(rect, radius, settings.w11Style, settings.notchStyle, tintBrush_.Get());
        }

        // Blur / acrylic and tint come from the DWM window below (see DwmBackdrop);
        // only the fill is drawn here.

        // User-defined pill background color. The authored alpha is combined
        // with the global pill transparency, so a plain 6-digit hex behaves
        // exactly as before (alpha 1.0) while #RRGGBBAA stays translucent.
        D2D1_COLOR_F bg = pillBgColor_;
        bg.a = Clamp(bg.a * settingsOpacity_, 0.0f, 1.0f);
        if (settings.backdropMaterial != BackdropMaterial::None) {
            // An opaque fill would hide the backdrop, so cap it.
            bg.a = std::min(bg.a, settings.backdropFillAlpha);
        }
        if (ID2D1SolidColorBrush* fillBrush = AcquireBrush(bg)) {
            FillIslandShape(rect, radius, settings.w11Style, settings.notchStyle, fillBrush);
        }

        if (settings.materialDepth) {
            FillSurfaceDepth(rect, radius, kind == IslandKind::Media || kind == IslandKind::Idle);

            // Media leans on the album-art accent; other surfaces get a whisper
            // of it so the whole UI still feels connected to what is playing.
            const float bloom = (kind == IslandKind::Media) ? 0.115f
                                : (kind == IslandKind::Idle) ? 0.055f
                                                             : 0.075f;
            FillAccentBloom(rect, radius, bloom * settings.accentBloom);
        }

        if (settings.contourBorderMode != ContourBorderMode::Borderless && settings.contourBorderEnabled) {
            D2D1_COLOR_F borderColor = settings.contourBorderColor;
            float strokeWidth = settings.w11Style ? 1.0f : 0.8f;

            if (settings.contourBorderMode == ContourBorderMode::Auto) {
                if (currentAccent_.a > 0.0f) {
                    borderColor = currentAccent_;
                    borderColor.a = std::min(1.0f, (kind == IslandKind::Idle ? 0.35f : 0.60f) * settingsOpacity_);
                    strokeWidth = 1.0f;
                } else {
                    borderColor.a = std::min(1.0f, borderColor.a * settingsOpacity_);
                }
            } else {
                borderColor.a = std::min(1.0f, borderColor.a * settingsOpacity_);
            }

            if (ID2D1SolidColorBrush* border = AcquireBrush(borderColor)) {
                D2D1_RECT_F borderRect = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f,
                                                     rect.right - 0.5f, rect.bottom - 0.5f);
                DrawIslandShape(borderRect, radius, settings.w11Style, settings.notchStyle, border, strokeWidth);
            }
        }
    }


    static void GetWeatherIconAndText(int code, std::wstring& icon, std::wstring& text) {
        switch (code) {
            case 113: icon = L"☀️"; break;
            case 116: icon = L"⛅"; break;
            case 119: case 122: icon = L"☁️"; break;
            case 143: case 248: case 260: icon = L"🌫️"; break;
            case 200: case 386: case 389: case 392: case 395: icon = L"⛈️"; break;
            case 176: case 263: case 266: case 281: case 284: case 293: case 296: case 299: case 302: case 305: case 308: case 311: case 314: case 353: case 356: case 359: icon = L"🌧️"; break;
            case 179: case 182: case 185: case 227: case 230: case 317: case 320: case 323: case 326: case 329: case 332: case 335: case 338: case 350: case 362: case 365: case 368: case 371: icon = L"❄️"; break;
            default: icon = L"🌡️"; break;
        }
    }

    static int GetDaysInMonth(int year, int month) {
        if (month == 2) {
            bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            return leap ? 29 : 28;
        }
        if (month == 4 || month == 6 || month == 9 || month == 11) return 30;
        return 31;
    }

    static int GetDayOfWeek(int year, int month, int day) {
        if (month < 3) { month += 12; year -= 1; }
        int k = year % 100;
        int j = year / 100;
        int h = (day + 13 * (month + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
        return (h + 6) % 7;
    }



    void DrawCalendarDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, SYSTEMTIME& local) {
        (void)state;
        (void)now;

        // ── Hero column ──────────────────────────────────────────────────────
        // Today's date as an editorial block: month over a large day figure over
        // the weekday. Uses the shared DrawCard so it matches every other panel
        // instead of being a one-off translucent slab.
        const D2D1_RECT_F hero = D2D1::RectF(rect.left + 22.0f * scale, rect.top + 16.0f * scale,
                                             rect.left + 115.0f * scale, rect.bottom - 20.0f * scale);
        DrawCard(hero, 12.0f * scale);

        // The accent is the album-art / system accent like everywhere else. This
        // used to fall back to a hardcoded red (#D94A38) whenever the mode wasn't
        // "System", which was the one colour in the whole island that answered to
        // nothing -- it clashed with the accent on every other surface.
        D2D1_COLOR_F accentColor = (settings.calendarAccent == CalendarAccentMode::System)
            ? GetSystemAccentColor()
            : material_.accent;
        accentColor.a = 0.95f * settingsOpacity_;

        ComPtr<ID2D1SolidColorBrush> accent;
        target_->CreateSolidColorBrush(accentColor, &accent);

        if (calendarCachedDate_.wYear != local.wYear || calendarCachedDate_.wMonth != local.wMonth ||
            calendarCachedDate_.wDay != local.wDay ||
            calendarCachedFirstDaySetting_ != settings.firstDayOfWeek) {
            wchar_t monthNameBuf[32] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, L"MMMM", monthNameBuf, ARRAYSIZE(monthNameBuf), nullptr);
            // No longer uppercased. towupper is per-character and locale-blind:
            // it turns Turkish "i" into "I" rather than "İ", and does nothing at
            // all for CJK month names, so the effect was inconsistent by locale.
            calendarCachedMonthName_ = monthNameBuf;

            wchar_t weekdayNameBuf[32] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, L"dddd", weekdayNameBuf, ARRAYSIZE(weekdayNameBuf), nullptr);
            calendarCachedWeekdayName_ = weekdayNameBuf;

            calendarCachedFirstDay_ = settings.firstDayOfWeek;
            if (calendarCachedFirstDay_ < 0) {
                DWORD localeFirstDay = 0;  // locale counts 0 = Monday
                calendarCachedFirstDay_ =
                    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                                    reinterpret_cast<LPWSTR>(&localeFirstDay),
                                    sizeof(localeFirstDay) / sizeof(wchar_t)) > 0
                        ? static_cast<int>((localeFirstDay + 1) % 7)
                        : 0;
            }

            // Same locale as the month and weekday names above, so the grid
            // headers can't end up in a different language from the hero.
            static const wchar_t* const kFallbackInitials[] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
            for (int weekday = 0; weekday < 7; ++weekday) {
                wchar_t initialBuf[16] = {};
                // CAL_SSHORTESTDAYNAME1 is Monday; weekday 0 here is Sunday.
                const CALTYPE calType = CAL_SSHORTESTDAYNAME1 + static_cast<CALTYPE>((weekday + 6) % 7);
                calendarCachedInitials_[weekday] =
                    GetCalendarInfoEx(LOCALE_NAME_USER_DEFAULT, CAL_GREGORIAN, nullptr, calType,
                                      initialBuf, ARRAYSIZE(initialBuf), nullptr) > 0
                        ? initialBuf
                        : kFallbackInitials[weekday];
            }
            calendarCachedFirstDaySetting_ = settings.firstDayOfWeek;

            calendarCachedDate_ = local;
        }

        // Month and year stay on separate lines. Putting them on one ("September
        // 2026") needs about 95px at 12px bold and the hero column only offers
        // 85, so any long month name clipped -- and month names are exactly the
        // strings that get long once localized.
        if (boldTextFormat_) {
            target_->DrawTextW(calendarCachedMonthName_.c_str(),
                               static_cast<UINT32>(calendarCachedMonthName_.size()),
                               boldTextFormat_.Get(),
                               D2D1::RectF(hero.left + 3.0f * scale, hero.top + 6.0f * scale,
                                           hero.right - 3.0f * scale, hero.top + 22.0f * scale),
                               accent.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        wchar_t yearStr[16] = {};
        swprintf_s(yearStr, L"%d", local.wYear);
        mutedBrush_->SetOpacity(0.62f);
        if (smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(yearStr, static_cast<UINT32>(wcslen(yearStr)), smallTextFormat_.Get(),
                               D2D1::RectF(hero.left, hero.top + 21.0f * scale,
                                           hero.right, hero.top + 36.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        wchar_t dayStr[16] = {};
        swprintf_s(dayStr, L"%d", local.wDay);
        textBrush_->SetOpacity(0.97f);
        target_->DrawTextW(dayStr, static_cast<UINT32>(wcslen(dayStr)),
                           calDayLargeFormat_ ? calDayLargeFormat_.Get() : hugeTextFormat_.Get(),
                           D2D1::RectF(hero.left, hero.top + 34.0f * scale,
                                       hero.right, hero.bottom - 26.0f * scale),
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        mutedBrush_->SetOpacity(0.70f);
        if (boldTextFormat_) {
            target_->DrawTextW(calendarCachedWeekdayName_.c_str(),
                               static_cast<UINT32>(calendarCachedWeekdayName_.size()),
                               boldTextFormat_.Get(),
                               D2D1::RectF(hero.left + 4.0f * scale, hero.bottom - 24.0f * scale,
                                           hero.right - 4.0f * scale, hero.bottom - 6.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // ── Month grid ───────────────────────────────────────────────────────
        const float gridStart = rect.left + 134.0f * scale;
        const float gridTop = rect.top + 16.0f * scale;
        const float colW = 28.0f * scale;
        const float headerH = 18.0f * scale;
        const int firstDay = calendarCachedFirstDay_;

        const int startDayIdx = (GetDayOfWeek(local.wYear, local.wMonth, 1) - firstDay + 7) % 7;
        const int monthDays = GetDaysInMonth(local.wYear, local.wMonth);
        const int rowCount = (startDayIdx + monthDays + 6) / 7;  // 5 or 6

        // Row height is derived from the space actually available rather than
        // fixed at 26px. At 26 a six-row month ran to y=197 inside a 184px
        // island, so the last row was silently clipped by the island mask --
        // visible every month that starts late in the week.
        const float datesTop = gridTop + headerH + 7.0f * scale;
        const float gridBottom = rect.bottom - 14.0f * scale;
        const float rowH = (gridBottom - datesTop) / static_cast<float>(rowCount > 0 ? rowCount : 1);

        IDWriteTextFormat* gridFmt = calGridFormat_ ? calGridFormat_.Get() : boldTextFormat_.Get();

        // Weekday initials are uniformly quiet. They used to paint S and S in the
        // accent, which put three competing accent marks in the grid (both
        // weekend headers plus today) and made the headers look selected.
        mutedBrush_->SetOpacity(0.55f);
        for (int i = 0; i < 7; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(gridStart + i * colW, gridTop,
                                                 gridStart + (i + 1) * colW, gridTop + headerH);
            const std::wstring& initial = calendarCachedInitials_[(firstDay + i) % 7];
            target_->DrawTextW(initial.c_str(), static_cast<UINT32>(initial.size()), gridFmt, cell,
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // Hairline under the weekday headers.
        const float hrY = gridTop + headerH + 3.0f * scale;
        ComPtr<ID2D1SolidColorBrush> hrBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(
                WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_), &hrBrush)) && hrBrush) {
            target_->DrawLine(D2D1::Point2F(gridStart + 2.0f * scale, hrY),
                              D2D1::Point2F(gridStart + 7.0f * colW - 2.0f * scale, hrY),
                              hrBrush.Get(), 1.0f * scale);
        }

        // Today's marker is sized from the cell it has to live in, so it stays a
        // circle around the figure instead of a fixed 12px disc that swallowed
        // the digits once the rows got shorter.
        const float markerR = std::min(colW, rowH) * 0.5f - 1.5f * scale;

        int row = 0;
        int col = startDayIdx;
        for (int d = 1; d <= monthDays; ++d) {
            const D2D1_RECT_F cell = D2D1::RectF(gridStart + col * colW, datesTop + row * rowH,
                                                 gridStart + (col + 1) * colW, datesTop + (row + 1) * rowH);
            const std::wstring dayText = std::to_wstring(d);
            const int weekday = (firstDay + col) % 7;

            if (d == local.wDay) {
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cell.left + colW * 0.5f, cell.top + rowH * 0.5f),
                                  markerR, markerR),
                    accent.Get());
                // Today's figure is drawn against the accent fill, so it needs the
                // primary text colour at full strength rather than the 0.85 the
                // other days use.
                textBrush_->SetOpacity(1.0f);
                target_->DrawTextW(dayText.c_str(), static_cast<UINT32>(dayText.size()), gridFmt,
                                   cell, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            } else if (weekday == 0 || weekday == 6) {
                // Weekends recede instead of taking the accent, leaving today as
                // the only accented thing in the grid.
                mutedBrush_->SetOpacity(0.62f);
                target_->DrawTextW(dayText.c_str(), static_cast<UINT32>(dayText.size()), gridFmt,
                                   cell, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            } else {
                textBrush_->SetOpacity(0.88f);
                target_->DrawTextW(dayText.c_str(), static_cast<UINT32>(dayText.size()), gridFmt,
                                   cell, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            if (++col > 6) { col = 0; ++row; }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Fixes windhawk-mods#4352: wind direction was printed as compass
    // initialisms (WSW, NE), which is meteorologist shorthand rather than
    // something glanceable. These are flow arrows, not bearing arrows -- a wind
    // *from* the north-east is drawn as an arrow pointing south-west, matching
    // the convention weather apps use.
    std::wstring WindDirToArrow(const std::wstring& dir) {
        if (dir == L"N") return L"\x2193";
        if (dir == L"NNE" || dir == L"NE" || dir == L"ENE") return L"\x2199";
        if (dir == L"E") return L"\x2190";
        if (dir == L"ESE" || dir == L"SE" || dir == L"SSE") return L"\x2196";
        if (dir == L"S") return L"\x2191";
        if (dir == L"SSW" || dir == L"SW" || dir == L"WSW") return L"\x2197";
        if (dir == L"W") return L"\x2192";
        if (dir == L"WNW" || dir == L"NW" || dir == L"NNW") return L"\x2198";
        return dir;
    }

    // ── Vector weather icons ────────────────────────────────────────────────
    // All sized relative to `s` (the icon's box width) and centred on `c`, so a
    // single call site controls scale. Built from ellipses and rounded rects
    // rather than path geometry wherever possible, to keep per-frame allocation
    // down.

    void DrawCloudShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush) {
        const D2D1_RECT_F base = D2D1::RectF(c.x - 0.44f * s, c.y + 0.02f * s,
                                             c.x + 0.44f * s, c.y + 0.21f * s);
        target_->FillRoundedRectangle(D2D1::RoundedRect(base, 0.10f * s, 0.10f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.02f * s), 0.18f * s, 0.18f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.01f * s, c.y - 0.10f * s), 0.24f * s, 0.24f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + 0.25f * s, c.y + 0.03f * s), 0.17f * s, 0.17f * s), brush);
    }

    void DrawSunShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush, float coreRadius = 0.26f) {
        target_->FillEllipse(D2D1::Ellipse(c, coreRadius * s, coreRadius * s), brush);

        const float inner = (coreRadius + 0.09f) * s;
        const float outer = (coreRadius + 0.21f) * s;
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            target_->DrawLine(D2D1::Point2F(c.x + ca * inner, c.y + sa * inner),
                              D2D1::Point2F(c.x + ca * outer, c.y + sa * outer),
                              brush, std::max(1.2f, 0.055f * s));
        }
    }

    void DrawLightningShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush) {
        ComPtr<ID2D1PathGeometry> bolt;
        if (FAILED(d2dFactory_->CreatePathGeometry(&bolt)) || !bolt) {
            return;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(bolt->Open(&sink)) || !sink) {
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

    void DrawWeatherIcon(D2D1_POINT_2F center, float size, WeatherVisual visual,
                         ID2D1Brush* strong, ID2D1Brush* soft) {
        const float s = size;
        const float stroke = std::max(1.3f, 0.06f * s);

        switch (visual) {
            case WeatherVisual::Clear:
                DrawSunShape(center, s, strong, 0.28f);
                break;

            case WeatherVisual::PartlyCloudy: {
                // Sun peeking out behind the cloud's upper-left.
                DrawSunShape(D2D1::Point2F(center.x - 0.20f * s, center.y - 0.20f * s), s * 0.62f, soft, 0.30f);
                DrawCloudShape(D2D1::Point2F(center.x + 0.05f * s, center.y + 0.06f * s), s * 0.92f, strong);
                break;
            }

            case WeatherVisual::Cloudy:
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.06f * s), s, soft);
                DrawCloudShape(D2D1::Point2F(center.x + 0.04f * s, center.y + 0.06f * s), s * 0.86f, strong);
                break;

            case WeatherVisual::Fog: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.16f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const float y = center.y + (0.24f + 0.15f * static_cast<float>(i)) * s;
                    const float half = (0.34f - 0.05f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(center.x - half, y),
                                      D2D1::Point2F(center.x + half, y), soft, stroke);
                }
                break;
            }

            case WeatherVisual::Storm:
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.20f * s), s * 0.92f, soft);
                DrawLightningShape(center, s, strong);
                break;

            case WeatherVisual::Rain: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const float x = center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    const float y = center.y + 0.24f * s;
                    target_->DrawLine(D2D1::Point2F(x + 0.05f * s, y),
                                      D2D1::Point2F(x - 0.03f * s, y + 0.26f * s), soft, stroke);
                }
                break;
            }

            case WeatherVisual::Snow: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const D2D1_POINT_2F f = D2D1::Point2F(
                        center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s,
                        center.y + (0.34f + (i == 1 ? 0.06f : 0.0f)) * s);
                    const float r = 0.075f * s;
                    for (int k = 0; k < 3; ++k) {
                        const float a = static_cast<float>(k) * 3.14159265f / 3.0f;
                        target_->DrawLine(D2D1::Point2F(f.x - std::cos(a) * r, f.y - std::sin(a) * r),
                                          D2D1::Point2F(f.x + std::cos(a) * r, f.y + std::sin(a) * r),
                                          soft, std::max(1.0f, 0.035f * s));
                    }
                }
                break;
            }

            case WeatherVisual::Unknown:
            default: {
                // Thermometer: stem, bulb, and a couple of gradation ticks.
                const float stemW = 0.11f * s;
                const D2D1_RECT_F stem = D2D1::RectF(center.x - stemW, center.y - 0.40f * s,
                                                     center.x + stemW, center.y + 0.18f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, stemW, stemW), soft);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + 0.26f * s), 0.19f * s, 0.19f * s), strong);
                const D2D1_RECT_F mercury = D2D1::RectF(center.x - stemW * 0.55f, center.y - 0.12f * s,
                                                        center.x + stemW * 0.55f, center.y + 0.20f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(mercury, stemW * 0.55f, stemW * 0.55f), strong);
                break;
            }
        }
    }

    // Small vector glyphs shared by the weather, hardware and game-overlay cards.
    //   0 wind   1 thermometer   2 droplet
    //   3 cpu    4 memory        5 gpu       6 upload   7 download   8 disk
    //   9 gauge (frame rate)
    //
    // The game overlay used to carry its own icon set (DrawGameIcon) drawn at
    // different stroke weights and proportions, so the same CPU appeared as two
    // different symbols depending on which surface you were looking at. Every
    // surface now draws from this one family.
    void DrawMetricGlyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush) {
        const float stroke = std::max(1.1f, 0.11f * s);

        if (kind == 9) {  // gauge: dial arc, ticks and a needle
            const float r = 0.40f * s;
            target_->DrawEllipse(D2D1::Ellipse(c, r, r), brush, stroke);
            for (int i = 0; i < 5; ++i) {
                const float a = -3.14159265f * 0.8f + static_cast<float>(i) * 3.14159265f * 0.4f;
                const float ca = std::cos(a);
                const float sa = std::sin(a);
                target_->DrawLine(D2D1::Point2F(c.x + ca * r, c.y + sa * r),
                                  D2D1::Point2F(c.x + ca * (r - 0.10f * s), c.y + sa * (r - 0.10f * s)),
                                  brush, stroke * 0.7f);
            }
            target_->FillEllipse(D2D1::Ellipse(c, 0.075f * s, 0.075f * s), brush);
            const float na = -3.14159265f * 0.25f;
            target_->DrawLine(c, D2D1::Point2F(c.x + std::cos(na) * r * 0.82f,
                                               c.y + std::sin(na) * r * 0.82f),
                              brush, stroke);
            return;
        }

        // Arrow used by the upload/download glyphs; `dir` is -1 up, +1 down.
        auto drawArrow = [&](float dir) {
            target_->DrawLine(D2D1::Point2F(c.x, c.y - 0.38f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.34f * s * dir), brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x + 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
        };

        switch (kind) {
            case 3: {  // cpu: chip body with pins on all four sides
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.28f * s, c.y - 0.28f * s,
                                                     c.x + 0.28f * s, c.y + 0.28f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.07f * s, 0.07f * s), brush, stroke);
                const D2D1_RECT_F core = D2D1::RectF(c.x - 0.11f * s, c.y - 0.11f * s,
                                                     c.x + 0.11f * s, c.y + 0.11f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(core, 0.03f * s, 0.03f * s), brush);
                for (int i = -1; i <= 1; ++i) {
                    const float o = static_cast<float>(i) * 0.15f * s;
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y - 0.28f * s), D2D1::Point2F(c.x + o, c.y - 0.42f * s), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y + 0.28f * s), D2D1::Point2F(c.x + o, c.y + 0.42f * s), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x - 0.28f * s, c.y + o), D2D1::Point2F(c.x - 0.42f * s, c.y + o), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + 0.28f * s, c.y + o), D2D1::Point2F(c.x + 0.42f * s, c.y + o), brush, stroke * 0.8f);
                }
                break;
            }
            case 4: {  // memory: module with contact notches along the bottom
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.42f * s, c.y - 0.26f * s,
                                                     c.x + 0.42f * s, c.y + 0.22f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                for (int i = -2; i <= 2; ++i) {
                    const float x = c.x + static_cast<float>(i) * 0.16f * s;
                    target_->DrawLine(D2D1::Point2F(x, c.y - 0.10f * s), D2D1::Point2F(x, c.y + 0.06f * s), brush, stroke * 0.85f);
                }
                target_->DrawLine(D2D1::Point2F(c.x - 0.20f * s, c.y + 0.34f * s),
                                  D2D1::Point2F(c.x + 0.20f * s, c.y + 0.34f * s), brush, stroke);
                break;
            }
            case 5: {  // gpu: board with a fan
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.44f * s, c.y - 0.24f * s,
                                                     c.x + 0.44f * s, c.y + 0.26f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.15f * s, 0.15f * s), brush, stroke * 0.9f);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.045f * s, 0.045f * s), brush);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y - 0.10f * s), D2D1::Point2F(c.x + 0.32f * s, c.y - 0.10f * s), brush, stroke * 0.8f);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y + 0.04f * s), D2D1::Point2F(c.x + 0.32f * s, c.y + 0.04f * s), brush, stroke * 0.8f);
                break;
            }
            case 6:  // upload
                drawArrow(-1.0f);
                break;
            case 7:  // download
                drawArrow(1.0f);
                break;
            case 8: {  // disk: stacked platters
                for (int i = 0; i < 3; ++i) {
                    const float y = c.y - 0.22f * s + static_cast<float>(i) * 0.22f * s;
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, y), 0.36f * s, 0.12f * s), brush, stroke * 0.9f);
                }
                break;
            }
            default:
                break;
        }
        if (kind >= 3) {
            return;
        }

        switch (kind) {
            case 0: {  // wind: three streaming lines
                const float lens[3] = {0.46f, 0.30f, 0.38f};
                for (int i = 0; i < 3; ++i) {
                    const float y = c.y + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(c.x - 0.44f * s, y),
                                      D2D1::Point2F(c.x - 0.44f * s + lens[i] * s * 1.9f, y),
                                      brush, stroke);
                }
                break;
            }
            case 1: {  // thermometer
                const float w = 0.13f * s;
                const D2D1_RECT_F stem = D2D1::RectF(c.x - w, c.y - 0.42f * s, c.x + w, c.y + 0.12f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, w, w), brush);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y + 0.24f * s), 0.22f * s, 0.22f * s), brush);
                break;
            }
            case 2:
            default: {  // droplet: circle body with a tapered tip
                ComPtr<ID2D1PathGeometry> drop;
                if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&drop)) && drop) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(drop->Open(&sink)) && sink) {
                        sink->BeginFigure(D2D1::Point2F(c.x, c.y - 0.44f * s), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(c.x + 0.34f * s, c.y - 0.02f * s),
                            D2D1::Point2F(c.x + 0.30f * s, c.y + 0.36f * s),
                            D2D1::Point2F(c.x, c.y + 0.38f * s)));
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(c.x - 0.30f * s, c.y + 0.36f * s),
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

    void DrawWeatherDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, bool hasWeather, const std::wstring& wIcon, const std::wstring& wText) {
        wchar_t wTemp[32] = {};
        if (hasWeather) swprintf_s(wTemp, L"%.0f\x00B0", state.weather.temperature);
        else wcscpy_s(wTemp, L"--\x00B0");

        std::wstring city = hasWeather ? state.weather.city : std::wstring(Loc(L"Locating..."));
        std::wstring desc = wText;

        // ── Layout ───────────────────────────────────────────────────────────
        // Left is an editorial hero block (place / reading / condition), left
        // aligned so the city, temperature and description share one optical
        // margin. Right is a stack of metric cards using the same card primitive
        // as every other dashboard. The old version centred each element in its
        // own box, which is what left the icon and the temperature floating apart
        // with a dead gap between them.
        const float heroLeft = rect.left + 26.0f * scale;
        const float heroRight = rect.left + 188.0f * scale;
        const float dividerX = rect.left + 200.0f * scale;
        const float metricLeft = rect.left + 214.0f * scale;
        const float metricRight = rect.right - 22.0f * scale;

        // Accent-tinted icon brushes: `strong` carries the shape, `soft` the
        // secondary detail (rain, rays, fog bands).
        ComPtr<ID2D1SolidColorBrush> iconStrong;
        ComPtr<ID2D1SolidColorBrush> iconSoft;
        target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.95f * settingsOpacity_), &iconStrong);
        target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.42f * settingsOpacity_), &iconSoft);

        // Place label.
        textBrush_->SetOpacity(0.55f);
        if (smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(city.c_str(), static_cast<UINT32>(city.length()), smallTextFormat_.Get(),
                               D2D1::RectF(heroLeft, rect.top + 26.0f * scale, heroRight, rect.top + 44.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Icon and temperature share one band and sit adjacent, so they read as
        // a single unit instead of two centred items.
        const float readingCenterY = rect.top + 82.0f * scale;
        if (hasWeather && iconStrong && iconSoft) {
            DrawWeatherIcon(D2D1::Point2F(heroLeft + 22.0f * scale, readingCenterY), 44.0f * scale,
                            WeatherVisualFromCode(state.weather.weatherCode),
                            iconStrong.Get(), iconSoft.Get());
        } else if (iconStrong && iconSoft) {
            DrawWeatherIcon(D2D1::Point2F(heroLeft + 22.0f * scale, readingCenterY), 44.0f * scale,
                            WeatherVisual::Unknown, iconStrong.Get(), iconSoft.Get());
        }

        textBrush_->SetOpacity(0.98f);
        if (hugeTextFormat_) {
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(wTemp, static_cast<UINT32>(wcslen(wTemp)), hugeTextFormat_.Get(),
                               D2D1::RectF(heroLeft + 52.0f * scale, readingCenterY - 32.0f * scale,
                                           heroRight, readingCenterY + 32.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const float descTop = rect.top + 120.0f * scale;
        const float descBottom = rect.top + 162.0f * scale;

        // Description
        const size_t descLength = desc.length();
        float descFontSize = 13.5f;
        if (descLength > 58) descFontSize = 9.8f;
        else if (descLength > 44) descFontSize = 10.5f;
        else if (descLength > 32) descFontSize = 11.5f;
        else if (descLength > 22) descFontSize = 12.5f;
        descFontSize *= scale;

        if (std::fabs(weatherDescFormatSize_ - descFontSize) > 0.01f || !weatherDescFormat_) {
            weatherDescFormat_.Reset();
            if (dwriteFactory_) {
                bool created = false;
                if (!settings.fontFamily.empty()) {
                    HRESULT hr = dwriteFactory_->CreateTextFormat(
                        settings.fontFamily.c_str(), nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        descFontSize, L"", &weatherDescFormat_);
                    if (SUCCEEDED(hr)) created = true;
                }
                if (!created) {
                    HRESULT hr = dwriteFactory_->CreateTextFormat(
                        L"Segoe UI Variable Display", nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        descFontSize, L"", &weatherDescFormat_);
                    if (FAILED(hr)) {
                        dwriteFactory_->CreateTextFormat(
                            L"Segoe UI", nullptr,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL,
                            descFontSize, L"", &weatherDescFormat_);
                    }
                }
            }
            if (weatherDescFormat_) {
                weatherDescFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                // Left aligned to share the hero column's margin with the city
                // and temperature.
                weatherDescFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                weatherDescFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
            weatherDescFormatSize_ = descFontSize;
        }

        textBrush_->SetOpacity(0.82f);
        target_->DrawTextW(desc.c_str(), static_cast<UINT32>(desc.length()),
                           weatherDescFormat_ ? weatherDescFormat_.Get() : textFormat_.Get(),
                           D2D1::RectF(heroLeft, descTop, heroRight, descBottom),
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        textBrush_->SetOpacity(0.96f);

        // Hairline divider, inset from both ends so it reads as a separator
        // rather than a full-height rule.
        ComPtr<ID2D1SolidColorBrush> divider;
        target_->CreateSolidColorBrush(WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_), &divider);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(dividerX, rect.top + 34.0f * scale,
                                          dividerX + 1.0f * scale, rect.bottom - 34.0f * scale),
                              0.5f * scale, 0.5f * scale), divider.Get());

        // ── Metric cards ─────────────────────────────────────────────────────
        // Each card is icon + label on one line with the value beneath, which
        // keeps long localized labels from colliding with the value the way a
        // single "Label: value" line did.
        struct Metric {
            int glyph;
            const wchar_t* label;
            std::wstring value;
        };

        const std::wstring windUnit = settings.weatherFahrenheit ? L" mph" : L" km/h";
        const Metric metrics[3] = {
            {0, Loc(L"Wind"),
             hasWeather ? state.weather.windSpeed + windUnit + L" " + WindDirToArrow(state.weather.windDir)
                        : std::wstring(L"--")},
            {1, Loc(L"Feels Like"),
             hasWeather ? state.weather.feelsLike + L"\x00B0" : std::wstring(L"--")},
            {2, Loc(L"Humidity"),
             hasWeather ? state.weather.humidity + L"%" : std::wstring(L"--")},
        };

        const float cardH = 40.0f * scale;
        const float cardGap = 7.0f * scale;
        const float stackH = cardH * 3.0f + cardGap * 2.0f;
        float cardTop = (rect.top + rect.bottom) * 0.5f - stackH * 0.5f;

        for (const Metric& m : metrics) {
            const D2D1_RECT_F card = D2D1::RectF(metricLeft, cardTop, metricRight, cardTop + cardH);
            DrawCard(card, 10.0f * scale);

            if (iconSoft) {
                DrawMetricGlyph(D2D1::Point2F(card.left + 17.0f * scale, card.top + 14.0f * scale),
                                14.0f * scale, m.glyph, iconSoft.Get());
            }

            const float textLeft = card.left + 30.0f * scale;
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.62f);
                target_->DrawTextW(m.label, static_cast<UINT32>(wcslen(m.label)), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 4.0f * scale,
                                               card.right - 8.0f * scale, card.top + 20.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.95f);
                target_->DrawTextW(m.value.c_str(), static_cast<UINT32>(m.value.size()), textFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 19.0f * scale,
                                               card.right - 8.0f * scale, card.bottom - 3.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            cardTop += cardH + cardGap;
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Cute little bin for the File Tray header: lid with handle, tapered body, two slats.
    void DrawTrayBinButton(D2D1_RECT_F box, bool enabled, bool hovered) {
        const float s = box.right - box.left;
        const D2D1_POINT_2F c = D2D1::Point2F((box.left + box.right) * 0.5f,
                                              (box.top + box.bottom) * 0.5f);
        const D2D1_COLOR_F danger = D2D1::ColorF(1.0f, 0.38f, 0.36f, 1.0f);

        if (enabled && hovered) {
            ComPtr<ID2D1SolidColorBrush> bg;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(danger, 0.20f * settingsOpacity_), &bg)) && bg) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(box, s * 0.32f, s * 0.32f), bg.Get());
            }
        }

        D2D1_COLOR_F ink = enabled ? (hovered ? danger : material_.textSecondary)
                                   : WithAlpha(material_.textSecondary, 0.35f);
        ink.a *= settingsOpacity_;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush) {
            return;
        }

        ComPtr<ID2D1StrokeStyle> round;
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);

        const float stroke = std::max(1.2f, 0.075f * s);

        // Lid and handle.
        target_->DrawLine(D2D1::Point2F(c.x - 0.31f * s, c.y - 0.17f * s),
                          D2D1::Point2F(c.x + 0.31f * s, c.y - 0.17f * s),
                          brush.Get(), stroke, round.Get());
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.30f * s,
                                          c.x + 0.11f * s, c.y - 0.17f * s),
                              0.04f * s, 0.04f * s),
            brush.Get(), stroke, round.Get());

        // Tapered body.
        ComPtr<ID2D1PathGeometry> body;
        if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&body)) && body) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(body->Open(&sink)) && sink) {
                sink->BeginFigure(D2D1::Point2F(c.x - 0.23f * s, c.y - 0.07f * s), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(c.x - 0.18f * s, c.y + 0.30f * s));
                sink->AddLine(D2D1::Point2F(c.x + 0.18f * s, c.y + 0.30f * s));
                sink->AddLine(D2D1::Point2F(c.x + 0.23f * s, c.y - 0.07f * s));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                target_->DrawGeometry(body.Get(), brush.Get(), stroke, round.Get());
            }
        }

        // Slats.
        for (int i = -1; i <= 1; i += 2) {
            const float x = c.x + static_cast<float>(i) * 0.08f * s;
            target_->DrawLine(D2D1::Point2F(x, c.y + 0.03f * s), D2D1::Point2F(x, c.y + 0.20f * s),
                              brush.Get(), stroke * 0.85f, round.Get());
        }
    }

    // Little clipboard for the File Tray header: board, clip and two text lines.
    void DrawTrayPasteButton(D2D1_RECT_F box, bool hovered) {
        const float s = box.right - box.left;
        const D2D1_POINT_2F c = D2D1::Point2F((box.left + box.right) * 0.5f,
                                              (box.top + box.bottom) * 0.5f);

        if (hovered) {
            ComPtr<ID2D1SolidColorBrush> bg;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.accent, 0.20f * settingsOpacity_), &bg)) && bg) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(box, s * 0.32f, s * 0.32f), bg.Get());
            }
        }

        D2D1_COLOR_F ink = hovered ? material_.accent : material_.textSecondary;
        ink.a *= settingsOpacity_;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush) {
            return;
        }

        ComPtr<ID2D1StrokeStyle> round;
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);

        const float stroke = std::max(1.2f, 0.075f * s);

        // Board.
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.25f * s, c.y - 0.24f * s,
                                          c.x + 0.25f * s, c.y + 0.31f * s),
                              0.07f * s, 0.07f * s),
            brush.Get(), stroke, round.Get());

        // Clip.
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(c.x - 0.11f * s, c.y - 0.32f * s,
                                          c.x + 0.11f * s, c.y - 0.15f * s),
                              0.05f * s, 0.05f * s),
            brush.Get());

        // Text lines.
        target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.00f * s),
                          D2D1::Point2F(c.x + 0.12f * s, c.y + 0.00f * s),
                          brush.Get(), stroke * 0.85f, round.Get());
        target_->DrawLine(D2D1::Point2F(c.x - 0.12f * s, c.y + 0.14f * s),
                          D2D1::Point2F(c.x + 0.05f * s, c.y + 0.14f * s),
                          brush.Get(), stroke * 0.85f, round.Get());
    }

    // Small round "x" at the right end of a File Tray row.
    void DrawTrayRemoveButton(D2D1_POINT_2F c, float size, bool hovered, bool rowHovered) {
        const D2D1_COLOR_F danger = D2D1::ColorF(1.0f, 0.38f, 0.36f, 1.0f);

        if (hovered) {
            ComPtr<ID2D1SolidColorBrush> bg;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(danger, 0.22f * settingsOpacity_), &bg)) && bg) {
                target_->FillEllipse(D2D1::Ellipse(c, size * 0.5f, size * 0.5f), bg.Get());
            }
        }

        D2D1_COLOR_F ink = hovered ? danger
                                   : WithAlpha(material_.textSecondary, rowHovered ? 0.85f : 0.55f);
        ink.a *= settingsOpacity_;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(ink, &brush)) || !brush) {
            return;
        }

        ComPtr<ID2D1StrokeStyle> round;
        d2dFactory_->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round);

        const float r = size * 0.22f;
        const float stroke = std::max(1.2f, size * 0.085f);
        target_->DrawLine(D2D1::Point2F(c.x - r, c.y - r), D2D1::Point2F(c.x + r, c.y + r),
                          brush.Get(), stroke, round.Get());
        target_->DrawLine(D2D1::Point2F(c.x - r, c.y + r), D2D1::Point2F(c.x + r, c.y - r),
                          brush.Get(), stroke, round.Get());
    }

    // File Tray (#33): a shelf for files dragged onto the island. Rows are built
    // from the shared DrawCard primitive so the shelf matches every other
    // dashboard, with the real Explorer icon for each file.
    void DrawFileTrayDashboard(const SharedState& state, D2D1_RECT_F rect,
                               const Settings& settings, float scale) {
        const float padX = FileTrayLayout::kPadX * scale;

        // Header: title on the left, count chip on the right.
        const D2D1_RECT_F headerRect = D2D1::RectF(rect.left + padX, rect.top + 18.0f * scale,
                                                   rect.right - padX, rect.top + 38.0f * scale);
        textBrush_->SetOpacity(0.96f);
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = Loc(L"File Tray");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), boldTextFormat_.Get(),
                               headerRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const size_t total = state.fileTrayItems.size();

        const float clearSize = FileTrayLayout::kClearBtnSize * scale;
        const float clearTop = rect.top + FileTrayLayout::kClearBtnTop * scale;
        const D2D1_RECT_F clearBox = D2D1::RectF(rect.right - padX - clearSize, clearTop,
                                                 rect.right - padX, clearTop + clearSize);
        const float pasteGap = FileTrayLayout::kPasteBtnGap * scale;
        const D2D1_RECT_F pasteBox = D2D1::RectF(clearBox.left - pasteGap - clearSize, clearTop,
                                                 clearBox.left - pasteGap, clearTop + clearSize);

        if (total > 0 && smallTextFormat_) {
            wchar_t countBuf[48] = {};
            swprintf_s(countBuf, L"%zu %s", total,
                       Loc(total == 1 ? L"item" : L"items"));
            const D2D1_RECT_F countRect = D2D1::RectF(headerRect.left, headerRect.top,
                                                      pasteBox.left - 8.0f * scale, headerRect.bottom);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(countBuf, static_cast<UINT32>(wcslen(countBuf)),
                               smallTextFormat_.Get(), countRect, mutedBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        DrawTrayPasteButton(pasteBox, g_hoveredFileTrayAction.load(std::memory_order_relaxed) == -3);
        DrawTrayBinButton(clearBox, total > 0,
                          g_hoveredFileTrayAction.load(std::memory_order_relaxed) == -2);

        const float listTop = rect.top + FileTrayLayout::kListTop * scale;
        const float listBottom = rect.bottom - FileTrayLayout::kListBottomInset * scale;

        if (state.fileTrayItems.empty()) {
            // Empty state: a dashed drop target rather than a bare label, so it
            // reads as an invitation.
            const D2D1_RECT_F dropRect = D2D1::RectF(rect.left + padX, listTop,
                                                      rect.right - padX, listBottom);
            ComPtr<ID2D1SolidColorBrush> dash;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * 1.6f), &dash)) && dash) {
                ComPtr<ID2D1StrokeStyle> dashStyle;
                D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
                props.dashStyle = D2D1_DASH_STYLE_DASH;
                props.dashCap = D2D1_CAP_STYLE_ROUND;
                d2dFactory_->CreateStrokeStyle(props, nullptr, 0, &dashStyle);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(dropRect, 12.0f * scale, 12.0f * scale),
                                              dash.Get(), 1.4f, dashStyle.Get());
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                const wchar_t* hint = Loc(L"Drag files here");
                mutedBrush_->SetOpacity(0.80f);
                target_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), textFormat_.Get(),
                                   dropRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        const float rowH = FileTrayLayout::kRowHeight * scale;
        const float rowGap = FileTrayLayout::kRowGap * scale;
        const int capacity = FileTrayLayout::VisibleRowCapacity((rect.bottom - rect.top) / scale);

        // Newest first: the file just dropped is the one the user wants.
        int drawn = 0;
        for (auto it = state.fileTrayItems.rbegin();
             it != state.fileTrayItems.rend() && drawn < capacity; ++it, ++drawn) {
            const FileTrayItem& item = *it;
            const float rowTop = listTop + drawn * (rowH + rowGap);
            const D2D1_RECT_F row = D2D1::RectF(rect.left + padX, rowTop, rect.right - padX, rowTop + rowH);

            const bool hovered = (g_hoveredFileTrayRow.load(std::memory_order_relaxed) == drawn);
            DrawCard(row, 9.0f * scale, hovered);

            // Real shell icon when we managed to extract one, otherwise a glyph.
            const D2D1_RECT_F iconRect = D2D1::RectF(row.left + 8.0f * scale, row.top + 6.0f * scale,
                                                      row.left + 26.0f * scale, row.bottom - 6.0f * scale);
            if (item.icon) {
                DrawBitmapPixels(*item.icon, iconRect, fileTrayIconBitmap_, fileTrayIconGeneration_, 0.98f);
            } else if (iconFormat_) {
                const wchar_t* glyph = item.isText ? L"\uE8D2"
                                                   : (item.isDirectory ? L"\uE8B7" : L"\uE7C3");
                accentBrush_->SetOpacity(0.85f);
                target_->DrawTextW(glyph, 1, iconFormat_.Get(), iconRect, accentBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
                accentBrush_->SetOpacity(1.0f);
            }

            wchar_t sizeBuf[40] = {};
            if (item.isText) {
                swprintf_s(sizeBuf, L"%zu ch", item.text.size());
            } else if (item.isDirectory) {
                wcscpy_s(sizeBuf, L"—");
            } else if (item.sizeBytes >= 1073741824ull) {
                swprintf_s(sizeBuf, L"%.1f GB", static_cast<double>(item.sizeBytes) / 1073741824.0);
            } else if (item.sizeBytes >= 1048576ull) {
                swprintf_s(sizeBuf, L"%.1f MB", static_cast<double>(item.sizeBytes) / 1048576.0);
            } else if (item.sizeBytes >= 1024ull) {
                swprintf_s(sizeBuf, L"%.0f KB", static_cast<double>(item.sizeBytes) / 1024.0);
            } else {
                swprintf_s(sizeBuf, L"%llu B", static_cast<unsigned long long>(item.sizeBytes));
            }

            const float removeSize = FileTrayLayout::kRemoveBtnSize * scale;
            const float removeMargin = FileTrayLayout::kRemoveBtnMargin * scale;
            const float removeSpace = removeSize + removeMargin + 4.0f * scale;
            const bool removeHovered =
                (g_hoveredFileTrayAction.load(std::memory_order_relaxed) == drawn);
            DrawTrayRemoveButton(D2D1::Point2F(row.right - removeMargin - removeSize * 0.5f,
                                               (row.top + row.bottom) * 0.5f),
                                 removeSize, removeHovered, hovered);

            const float sizeW = 66.0f * scale;
            const D2D1_RECT_F nameRect = D2D1::RectF(row.left + 32.0f * scale, row.top,
                                                      row.right - removeSpace - sizeW - 4.0f * scale, row.bottom);
            if (smallTextFormat_) {
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.94f);
                target_->DrawTextW(item.name.c_str(), static_cast<UINT32>(item.name.size()),
                                   smallTextFormat_.Get(), nameRect, textBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);

                const D2D1_RECT_F sizeRect = D2D1::RectF(row.right - removeSpace - sizeW, row.top,
                                                          row.right - removeSpace, row.bottom);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                mutedBrush_->SetOpacity(0.70f);
                target_->DrawTextW(sizeBuf, static_cast<UINT32>(wcslen(sizeBuf)),
                                   smallTextFormat_.Get(), sizeRect, mutedBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }

        // "+N more" when the shelf holds more than fits.
        if (static_cast<int>(total) > capacity && smallTextFormat_) {
            wchar_t moreBuf[32] = {};
            swprintf_s(moreBuf, L"+%d", static_cast<int>(total) - capacity);
            const D2D1_RECT_F moreRect = D2D1::RectF(rect.left + padX, listBottom - 12.0f * scale,
                                                      rect.right - padX, listBottom);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            mutedBrush_->SetOpacity(0.65f);
            target_->DrawTextW(moreBuf, static_cast<UINT32>(wcslen(moreBuf)), smallTextFormat_.Get(),
                               moreRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    std::size_t notification_center_slot(uint64_t generation) {
        for (std::size_t slot = 0; slot < notificationCenterGenerations_.size(); ++slot) {
            if (generation != 0 && notificationCenterGenerations_[slot] == generation && notificationCenterBitmaps_[slot]) {
                return slot;
            }
        }
        const std::size_t slot = notificationCenterNextSlot_;
        notificationCenterNextSlot_ = (notificationCenterNextSlot_ + 1) % notificationCenterGenerations_.size();
        notificationCenterGenerations_[slot] = 0;
        return slot;
    }

    void DrawNotificationCenterIcon(const notification_center::Group& group, D2D1_RECT_F badge, float radius) {
        ComPtr<ID2D1SolidColorBrush> plate;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &plate)) && plate) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(badge, radius, radius), plate.Get());
        }

        if (group.icon) {
            const std::size_t slot = notification_center_slot(group.icon->generation);
            DrawRoundedBitmapPixels(*group.icon, badge, radius, notificationCenterBitmaps_[slot],
                                    notificationCenterGenerations_[slot], 1.0f);
            return;
        }

        const wchar_t initial = notification_initial(group.app);
        if (initial == 0 || !boldTextFormat_) {
            return;
        }
        const DWRITE_TEXT_ALIGNMENT text_alignment = boldTextFormat_->GetTextAlignment();
        const DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment = boldTextFormat_->GetParagraphAlignment();
        boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        boldTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(&initial, 1, boldTextFormat_.Get(), badge, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        boldTextFormat_->SetTextAlignment(text_alignment);
        boldTextFormat_->SetParagraphAlignment(paragraph_alignment);
    }

    void DrawNotificationCenterBack(D2D1_RECT_F box, float scale, bool hovered) {
        if (hovered) {
            DrawCard(box, 7.0f * scale, true);
        }
        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        const float arm = 4.5f * scale;
        const float stroke = 1.8f * scale;
        textBrush_->SetOpacity(hovered ? 1.0f : 0.85f);
        target_->DrawLine(D2D1::Point2F(cx + arm * 0.5f, cy - arm), D2D1::Point2F(cx - arm * 0.5f, cy),
                          textBrush_.Get(), stroke);
        target_->DrawLine(D2D1::Point2F(cx - arm * 0.5f, cy), D2D1::Point2F(cx + arm * 0.5f, cy + arm),
                          textBrush_.Get(), stroke);
    }

    void DrawNotificationCenterDetail(const notification_center::Group& group, std::size_t offset,
                                      D2D1_RECT_F rect, float scale) {
        const int capacity = notification_center::visible_rows((rect.bottom - rect.top) / scale);
        g_notificationCenterDetailRows.store(capacity, std::memory_order_relaxed);
        const std::size_t rows = static_cast<std::size_t>(capacity);
        const std::size_t count = group.entries.size();
        const std::size_t limit = notification_center::max_offset(count, capacity);
        const std::size_t first = std::min(offset, limit);
        const int hoveredAction = g_hoveredNotificationAction.load(std::memory_order_relaxed);

        const float padX = notification_center::pad_x * scale;
        const float headTop = rect.top + notification_center::clear_top * scale;
        const float clearSize = notification_center::clear_size * scale;
        const float backSize = notification_center::back_size * scale;
        const D2D1_RECT_F clearBox = D2D1::RectF(rect.right - padX - clearSize, headTop,
                                                 rect.right - padX, headTop + clearSize);
        const D2D1_RECT_F backBox = D2D1::RectF(rect.left + padX, headTop,
                                                rect.left + padX + backSize, headTop + backSize);
        DrawNotificationCenterBack(backBox, scale, hoveredAction == kNotificationCenterBack);

        const float iconSize = 20.0f * scale;
        const float iconTop = (backBox.top + backBox.bottom - iconSize) * 0.5f;
        const D2D1_RECT_F badge = D2D1::RectF(backBox.right + 6.0f * scale, iconTop,
                                              backBox.right + 6.0f * scale + iconSize, iconTop + iconSize);
        DrawNotificationCenterIcon(group, badge, 6.0f * scale);

        const float textTop = rect.top + 18.0f * scale;
        const float textBottom = rect.top + 38.0f * scale;
        float headingRight = clearBox.left - 8.0f * scale;

        if (count > rows && smallTextFormat_) {
            wchar_t rangeBuf[48] = {};
            swprintf_s(rangeBuf, L"%zu\u2013%zu / %zu", first + 1, std::min(count, first + rows), count);
            const D2D1_RECT_F rangeRect = D2D1::RectF(headingRight - 72.0f * scale, textTop, headingRight, textBottom);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(rangeBuf, static_cast<UINT32>(wcslen(rangeBuf)), smallTextFormat_.Get(),
                               rangeRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            headingRight = rangeRect.left;
        }

        if (boldTextFormat_) {
            std::wstring heading = group.app;
            if (count > 1) {
                heading += L" \u00B7 " + std::to_wstring(count);
            }
            const DWRITE_TEXT_ALIGNMENT alignment = boldTextFormat_->GetTextAlignment();
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textBrush_->SetOpacity(0.96f);
            target_->DrawTextW(heading.c_str(), static_cast<UINT32>(heading.size()), boldTextFormat_.Get(),
                               D2D1::RectF(badge.right + 8.0f * scale, textTop, headingRight - 6.0f * scale, textBottom),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            boldTextFormat_->SetTextAlignment(alignment);
        }

        DrawTrayBinButton(clearBox, true, hoveredAction == kNotificationCenterClear);

        const float listTop = rect.top + notification_center::list_top * scale;
        const float rowH = notification_center::row_height * scale;
        const float rowGap = notification_center::row_gap * scale;
        const float timeW = 56.0f * scale;

        if (smallTextFormat_) {
            const DWRITE_WORD_WRAPPING wrapping = smallTextFormat_->GetWordWrapping();
            for (std::size_t index = 0; index < rows; ++index) {
                const notification_center::Entry* entry = notification_center::entry_at(group, first, index);
                if (!entry) {
                    break;
                }
                const float rowTop = listTop + static_cast<float>(index) * (rowH + rowGap);
                const D2D1_RECT_F row = D2D1::RectF(rect.left + padX, rowTop, rect.right - padX, rowTop + rowH);
                DrawCard(row, 9.0f * scale, false);

                const float textLeft = row.left + 10.0f * scale;
                const float textRight = row.right - 10.0f * scale;

                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                mutedBrush_->SetOpacity(0.65f);
                target_->DrawTextW(entry->time.c_str(), static_cast<UINT32>(entry->time.size()), smallTextFormat_.Get(),
                                   D2D1::RectF(textRight - timeW, row.top + 3.0f * scale, textRight, row.top + 19.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

                smallTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                textBrush_->SetOpacity(0.90f);
                target_->DrawTextW(entry->preview.c_str(), static_cast<UINT32>(entry->preview.size()), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, row.top + 3.0f * scale, textRight - timeW - 4.0f * scale,
                                               row.bottom - 2.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetWordWrapping(wrapping);
            }
        }

        if (count > rows && limit > 0) {
            const float trackTop = listTop;
            const float trackHeight = static_cast<float>(rows) * (rowH + rowGap) - rowGap;
            const float thumbHeight = std::max(10.0f * scale, trackHeight * static_cast<float>(rows) / static_cast<float>(count));
            const float thumbTop = trackTop + (trackHeight - thumbHeight) * static_cast<float>(first) / static_cast<float>(limit);
            const float trackX = rect.right - padX + 7.0f * scale;
            const float thumbWidth = 3.0f * scale;
            mutedBrush_->SetOpacity(0.55f);
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(trackX, thumbTop, trackX + thumbWidth, thumbTop + thumbHeight),
                                  thumbWidth * 0.5f, thumbWidth * 0.5f),
                mutedBrush_.Get());
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    void DrawNotificationCenterDashboard(const SharedState& state, D2D1_RECT_F rect, float scale) {
        g_notificationCenterUnread.store(false, std::memory_order_relaxed);

        if (const notification_center::Group* open = OpenNotificationGroup(state)) {
            DrawNotificationCenterDetail(*open, state.notificationCenterView.offset, rect, scale);
            return;
        }

        const float padX = notification_center::pad_x * scale;
        const D2D1_RECT_F headerRect = D2D1::RectF(rect.left + padX, rect.top + 18.0f * scale,
                                                   rect.right - padX, rect.top + 38.0f * scale);
        textBrush_->SetOpacity(0.96f);
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = Loc(L"Notifications");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), boldTextFormat_.Get(),
                               headerRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const notification_center::History* history = state.notificationCenter.get();
        const std::size_t groupCount = history ? history->groups().size() : 0;
        const int capacity = notification_center::visible_rows((rect.bottom - rect.top) / scale);

        const float clearSize = notification_center::clear_size * scale;
        const float clearTop = rect.top + notification_center::clear_top * scale;
        const D2D1_RECT_F clearBox = D2D1::RectF(rect.right - padX - clearSize, clearTop,
                                                 rect.right - padX, clearTop + clearSize);

        if (groupCount > 0 && smallTextFormat_) {
            wchar_t countBuf[48] = {};
            const int hidden = static_cast<int>(groupCount) - capacity;
            if (hidden > 0) {
                swprintf_s(countBuf, L"%zu \u00B7 +%d", history->total(), hidden);
            } else {
                swprintf_s(countBuf, L"%zu", history->total());
            }
            const D2D1_RECT_F countRect = D2D1::RectF(headerRect.left, headerRect.top,
                                                      clearBox.left - 8.0f * scale, headerRect.bottom);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(countBuf, static_cast<UINT32>(wcslen(countBuf)), smallTextFormat_.Get(),
                               countRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        DrawTrayBinButton(clearBox, groupCount > 0,
                          g_hoveredNotificationAction.load(std::memory_order_relaxed) == -2);

        const float listTop = rect.top + notification_center::list_top * scale;
        const float listBottom = rect.bottom - notification_center::list_bottom_inset * scale;

        if (groupCount == 0) {
            if (textFormat_) {
                const D2D1_RECT_F emptyRect = D2D1::RectF(rect.left + padX, listTop, rect.right - padX, listBottom);
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const wchar_t* hint = Loc(L"No notifications");
                mutedBrush_->SetOpacity(0.80f);
                target_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), textFormat_.Get(),
                                   emptyRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        const float rowH = notification_center::row_height * scale;
        const float rowGap = notification_center::row_gap * scale;
        const float removeSize = notification_center::remove_size * scale;
        const float removeMargin = notification_center::remove_margin * scale;
        const float removeSpace = removeSize + removeMargin + 4.0f * scale;
        const float timeW = 56.0f * scale;
        const int hoveredRow = g_hoveredNotificationRow.load(std::memory_order_relaxed);
        const int hoveredAction = g_hoveredNotificationAction.load(std::memory_order_relaxed);

        int drawn = 0;
        for (const notification_center::Group& group : history->groups()) {
            if (drawn >= capacity) {
                break;
            }
            const float rowTop = listTop + static_cast<float>(drawn) * (rowH + rowGap);
            const D2D1_RECT_F row = D2D1::RectF(rect.left + padX, rowTop, rect.right - padX, rowTop + rowH);
            const bool hovered = hoveredRow == drawn;
            DrawCard(row, 9.0f * scale, hovered);

            const float iconSize = 24.0f * scale;
            const float iconTop = (row.top + row.bottom - iconSize) * 0.5f;
            const D2D1_RECT_F badge = D2D1::RectF(row.left + 7.0f * scale, iconTop,
                                                  row.left + 7.0f * scale + iconSize, iconTop + iconSize);
            DrawNotificationCenterIcon(group, badge, 7.0f * scale);

            DrawTrayRemoveButton(D2D1::Point2F(row.right - removeMargin - removeSize * 0.5f,
                                               (row.top + row.bottom) * 0.5f),
                                 removeSize, hoveredAction == drawn, hovered);

            if (smallTextFormat_) {
                const float textLeft = badge.right + 9.0f * scale;
                const float textRight = row.right - removeSpace;
                const float midY = (row.top + row.bottom) * 0.5f;

                std::wstring heading = group.app;
                if (group.entries.size() > 1) {
                    heading += L" \u00B7 " + std::to_wstring(group.entries.size());
                }
                textBrush_->SetOpacity(0.94f);
                target_->DrawTextW(heading.c_str(), static_cast<UINT32>(heading.size()), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, row.top + 3.0f * scale, textRight - timeW, midY + 1.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                mutedBrush_->SetOpacity(0.65f);
                const notification_center::Entry& latest = group.latest();
                target_->DrawTextW(latest.time.c_str(), static_cast<UINT32>(latest.time.size()), smallTextFormat_.Get(),
                                   D2D1::RectF(textRight - timeW, row.top + 3.0f * scale, textRight, midY + 1.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

                mutedBrush_->SetOpacity(0.78f);
                target_->DrawTextW(latest.preview.c_str(), static_cast<UINT32>(latest.preview.size()), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, midY, textRight, row.bottom - 2.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            ++drawn;
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Load colour is *semantic*, not decorative: the accent while a component is
    // comfortable, amber under pressure, red when saturated. The previous design
    // assigned a fixed rainbow colour per metric, which carried no information
    // (CPU and NET UP were both green, GPU and NET DOWN both cyan) and fought the
    // album-art accent.
    D2D1_COLOR_F LoadStateColor(float fraction) const {
        if (fraction >= 0.90f) {
            return D2D1::ColorF(1.0f, 0.35f, 0.32f, 1.0f);
        }
        if (fraction >= 0.75f) {
            return D2D1::ColorF(1.0f, 0.69f, 0.13f, 1.0f);
        }
        return material_.accent;
    }

    void DrawGitHubDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale) {
        (void)settings;

        const GitHubSnapshot& github = state.github;
        const float padX = 24.0f * scale;

        textBrush_->SetOpacity(0.96f);
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(L"GitHub", 6, boldTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX, rect.top + 16.0f * scale,
                                           rect.right - padX, rect.top + 32.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (!github.hasData) {
            const wchar_t* loading = Loc(L"Loading...");
            textBrush_->SetOpacity(0.55f);
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                target_->DrawTextW(loading, static_cast<UINT32>(wcslen(loading)), smallTextFormat_.Get(),
                                   D2D1::RectF(rect.left + padX, rect.top + 80.0f * scale,
                                               rect.right - padX, rect.top + 104.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            textBrush_->SetOpacity(1.0f);
            return;
        }

        const std::wstring handle = L"@" + github.user;
        textBrush_->SetOpacity(0.55f);
        if (smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            target_->DrawTextW(handle.c_str(), static_cast<UINT32>(handle.length()), smallTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX, rect.top + 17.0f * scale,
                                           rect.right - padX, rect.top + 33.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Empty days use the shared raised surface; active days are the accent
        // colour, so the grid follows the album art like every other panel.
        static constexpr float kLevelAlpha[] = {0.30f, 0.52f, 0.76f, 1.0f};
        ComPtr<ID2D1SolidColorBrush> levelBrushes[4];
        for (int i = 0; i < 4; ++i) {
            target_->CreateSolidColorBrush(WithAlpha(material_.accent, kLevelAlpha[i] * settingsOpacity_),
                                           &levelBrushes[i]);
        }

        const float cell = 10.0f * scale;
        const float pitch = 12.0f * scale;
        const float gridWidth = pitch * GitHubSnapshot::kWeeks - (pitch - cell);
        const float gridLeft = (rect.left + rect.right - gridWidth) * 0.5f;
        const float gridTop = rect.top + 40.0f * scale;
        const float radius = 2.5f * scale;

        for (int week = 0; week < GitHubSnapshot::kWeeks; ++week) {
            for (int day = 0; day < 7; ++day) {
                const int level = github.cells[week * 7 + day];
                if (level < 0) continue;
                const float x = gridLeft + week * pitch;
                const float y = gridTop + day * pitch;
                const D2D1_ROUNDED_RECT square =
                    D2D1::RoundedRect(D2D1::RectF(x, y, x + cell, y + cell), radius, radius);
                ID2D1SolidColorBrush* brush =
                    level == 0 ? raisedBrush_.Get() : levelBrushes[std::min(level, 4) - 1].Get();
                if (brush) target_->FillRoundedRectangle(square, brush);
            }
        }

        wchar_t todayText[32], yearText[32], streakText[32];
        swprintf_s(todayText, L"%d today", github.todayCount);
        swprintf_s(yearText, L"%d this year", github.yearTotal);
        swprintf_s(streakText, L"%d day streak", github.streak);

        const float statsTop = gridTop + 7.0f * pitch + 6.0f * scale;
        const float statsBottom = statsTop + 18.0f * scale;
        textBrush_->SetOpacity(0.72f);
        if (smallTextFormat_) {
            const struct {
                const wchar_t* text;
                DWRITE_TEXT_ALIGNMENT alignment;
            } stats[] = {{todayText, DWRITE_TEXT_ALIGNMENT_LEADING},
                         {yearText, DWRITE_TEXT_ALIGNMENT_CENTER},
                         {streakText, DWRITE_TEXT_ALIGNMENT_TRAILING}};
            for (const auto& stat : stats) {
                smallTextFormat_->SetTextAlignment(stat.alignment);
                target_->DrawTextW(stat.text, static_cast<UINT32>(wcslen(stat.text)), smallTextFormat_.Get(),
                                   D2D1::RectF(gridLeft, statsTop, gridLeft + gridWidth, statsBottom),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        textBrush_->SetOpacity(1.0f);
    }

    void DrawHardwareMonitorDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale) {
        (void)settings;

        const float padX = 24.0f * scale;

        // Header, left aligned to match the File Tray and weather dashboards.
        textBrush_->SetOpacity(0.96f);
        const wchar_t* hwTitle = Loc(L"Hardware Monitor");
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(hwTitle, static_cast<UINT32>(wcslen(hwTitle)), boldTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX, rect.top + 16.0f * scale,
                                           rect.right - padX, rect.top + 32.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        wchar_t cpuBuf[32], ramBuf[40], gpuBuf[32], upBuf[32], downBuf[32], diskBuf[32];
        swprintf_s(cpuBuf, L"%d%%", state.system.cpuPercent);
        swprintf_s(ramBuf, L"%.1f / %.1f GB", state.system.memoryUsedGB, state.system.memoryTotalGB);
        if (state.system.gpuPercent >= 0) {
            swprintf_s(gpuBuf, L"%d%%", state.system.gpuPercent);
        } else {
            wcscpy_s(gpuBuf, L"--");
        }
        swprintf_s(upBuf, L"%.1f Mbps", state.system.netUpMbps);
        swprintf_s(downBuf, L"%.1f Mbps", state.system.netDownMbps);
        const int diskUsed = 100 - state.system.diskFreePercent;
        swprintf_s(diskBuf, L"%d%%", diskUsed);

        const float ramFraction = (state.system.memoryTotalGB > 0.01f)
            ? Clamp(state.system.memoryUsedGB / state.system.memoryTotalGB, 0.0f, 1.0f)
            : -1.0f;

        struct HwMetric {
            int glyph;
            const wchar_t* label;
            const wchar_t* value;
            float fraction;  // negative means "no meaningful 0-100 scale"
        };

        // Network throughput has no natural ceiling, so those two cards get no
        // load bar rather than a bar against an invented maximum.
        const HwMetric metrics[6] = {
            {3, L"CPU",      cpuBuf,  Clamp(state.system.cpuPercent / 100.0f, 0.0f, 1.0f)},
            {6, L"NET UP",   upBuf,   -1.0f},
            {4, L"RAM",      ramBuf,  ramFraction},
            {7, L"NET DOWN", downBuf, -1.0f},
            {5, L"GPU",      gpuBuf,  state.system.gpuPercent >= 0
                                          ? Clamp(state.system.gpuPercent / 100.0f, 0.0f, 1.0f)
                                          : -1.0f},
            {8, L"DISK",     diskBuf, Clamp(diskUsed / 100.0f, 0.0f, 1.0f)},
        };

        // 2 x 3 grid of cards. Cards give the grid its structure, so the old
        // full-height centre divider is gone.
        const float colGap = 9.0f * scale;
        const float colW = (rect.right - rect.left - padX * 2.0f - colGap) * 0.5f;
        const float rowH = 38.0f * scale;
        const float rowGap = 6.0f * scale;
        const float gridTop = rect.top + 38.0f * scale;

        for (int i = 0; i < 6; ++i) {
            const HwMetric& m = metrics[i];
            const int col = i % 2;
            const int row = i / 2;

            const float cardLeft = rect.left + padX + static_cast<float>(col) * (colW + colGap);
            const float cardTop = gridTop + static_cast<float>(row) * (rowH + rowGap);
            const D2D1_RECT_F card = D2D1::RectF(cardLeft, cardTop, cardLeft + colW, cardTop + rowH);

            DrawCard(card, 9.0f * scale);

            const bool hasBar = m.fraction >= 0.0f;
            const D2D1_COLOR_F tint = hasBar ? LoadStateColor(m.fraction) : material_.accent;

            if (ID2D1SolidColorBrush* glyphBrush = AcquireBrush(WithAlpha(tint, 0.85f * settingsOpacity_))) {
                DrawMetricGlyph(D2D1::Point2F(card.left + 16.0f * scale, card.top + 15.0f * scale),
                                15.0f * scale, m.glyph, glyphBrush);
            }

            const float textLeft = card.left + 30.0f * scale;
            const float textRight = card.right - 9.0f * scale;

            // Label sits above the value rather than sharing a line with it.
            // A single "Label: value" line collides once localized -- German
            // "Luftfeuchte" or Russian "Влажность" leave no room for the number.
            // The three bands (label / value / bar) are kept adjacent but
            // non-overlapping so nothing clips into its neighbour.
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.60f);
                target_->DrawTextW(m.label, static_cast<UINT32>(wcslen(m.label)), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 2.0f * scale,
                                               textRight, card.top + 15.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.95f);
                // Without a bar the value takes the band the bar would have
                // used, so the two card shapes stay optically balanced.
                const float valueBottom = hasBar ? card.top + 30.0f * scale : card.bottom - 4.0f * scale;
                target_->DrawTextW(m.value, static_cast<UINT32>(wcslen(m.value)), textFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 15.0f * scale, textRight, valueBottom),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (hasBar) {
                const D2D1_RECT_F track = D2D1::RectF(textLeft, card.bottom - 6.0f * scale,
                                                      textRight, card.bottom - 3.5f * scale);
                ComPtr<ID2D1SolidColorBrush> trackBrush;
                if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                    target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale), trackBrush.Get());
                }

                const float span = (track.right - track.left) * m.fraction;
                if (span > 0.5f) {
                    if (ID2D1SolidColorBrush* fillBrush = AcquireBrush(WithAlpha(tint, 0.95f))) {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                              1.25f * scale, 1.25f * scale),
                            fillBrush);
                    }
                }
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

        // Karaoke effect: a soft wave rides the wipe boundary across the active line.
    // The layout and per-cluster rects are cached (karaokeCache_) and only rebuilt
    // when the line changes.
    void DrawKaraokeLetterPop(const wchar_t* text, UINT32 textLength, IDWriteTextFormat* format,
                              D2D1_RECT_F lineRect, float wipeX, float baseOpacity,
                              float unsungOpacity, bool glowEnabled, float glowStrength = 1.0f) {
        if (!text || textLength == 0 || !format || baseOpacity <= 0.01f) {
            return;
        }

        const float layoutWidth = lineRect.right - lineRect.left;
        const float layoutHeight = lineRect.bottom - lineRect.top;
        if (layoutWidth <= 0.0f || layoutHeight <= 0.0f) {
            return;
        }

        const std::wstring textView(text, textLength);
        if (karaokeCache_.text != textView || karaokeCache_.format != format ||
            std::fabs(karaokeCache_.layoutWidth - layoutWidth) > 0.5f ||
            std::fabs(karaokeCache_.layoutHeight - layoutHeight) > 0.5f ||
            !karaokeCache_.layout) {
            karaokeCache_.layout.Reset();
            karaokeCache_.clusters.clear();
            karaokeCache_.valid = false;
            karaokeCache_.text = textView;
            karaokeCache_.format = format;
            karaokeCache_.layoutWidth = layoutWidth;
            karaokeCache_.layoutHeight = layoutHeight;

            dwriteFactory_->CreateTextLayout(text, textLength, format, layoutWidth, layoutHeight,
                                             &karaokeCache_.layout);
            if (karaokeCache_.layout) {
                // Walk shaped glyph clusters (not raw UTF-16 units) so scripts like
                // Devanagari, where one letter is several code units, stay intact.
                UINT32 clusterCount = 0;
                karaokeCache_.layout->GetClusterMetrics(nullptr, 0, &clusterCount);
                if (clusterCount > 0) {
                    std::vector<DWRITE_CLUSTER_METRICS> rawClusters(clusterCount);
                    if (SUCCEEDED(karaokeCache_.layout->GetClusterMetrics(rawClusters.data(), clusterCount,
                                                                          &clusterCount))) {
                        karaokeCache_.clusters.reserve(clusterCount);
                        UINT32 clusterStart = 0;
                        for (UINT32 c = 0; c < clusterCount; ++c) {
                            const UINT32 clusterLength = rawClusters[c].length;
                            const UINT32 i = clusterStart;
                            clusterStart += clusterLength;

                            KaraokeLayoutCache::ClusterInfo info;
                            info.textStart = i;
                            info.textLength = clusterLength;
                            info.isWhitespace = rawClusters[c].isWhitespace || clusterLength == 0;

                            if (!info.isWhitespace) {
                                DWRITE_HIT_TEST_METRICS hit{};
                                UINT32 hitCount = 0;
                                if (SUCCEEDED(karaokeCache_.layout->HitTestTextRange(
                                        i, clusterLength, 0.0f, 0.0f, &hit, 1, &hitCount)) &&
                                    hitCount > 0 && hit.width > 0.0f) {
                                    info.rect = D2D1::RectF(hit.left, hit.top, hit.left + hit.width,
                                                            hit.top + hit.height);
                                } else {
                                    info.isWhitespace = true;
                                }
                            }

                            karaokeCache_.clusters.push_back(info);
                        }
                        karaokeCache_.valid = true;
                    }
                }
            }
        }

        if (!karaokeCache_.valid) {
            // Plain two-pass wipe if the layout could not be built.
            mutedBrush_->SetOpacity(baseOpacity * unsungOpacity);
            target_->DrawTextW(text, textLength, format, lineRect, mutedBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (wipeX > lineRect.left) {
                D2D1_RECT_F sungClip = D2D1::RectF(lineRect.left, lineRect.top,
                                                   std::min(wipeX, lineRect.right), lineRect.bottom);
                target_->PushAxisAlignedClip(sungClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                textBrush_->SetOpacity(baseOpacity);
                target_->DrawTextW(text, textLength, format, lineRect, textBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                target_->PopAxisAlignedClip();
            }
            return;
        }

        constexpr float kWaveTrailWidth = 150.0f;
        constexpr float kWaveLeadWidth = 46.0f;
        constexpr int kGlowStampCount = 4;
        constexpr float kGlowRadius = 0.85f;
        constexpr float kGlowAlpha = 0.075f;

        for (const auto& cluster : karaokeCache_.clusters) {
            if (cluster.isWhitespace) {
                continue;
            }

            const D2D1_RECT_F hitRect = D2D1::RectF(
                cluster.rect.left + lineRect.left, cluster.rect.top + lineRect.top,
                cluster.rect.right + lineRect.left, cluster.rect.bottom + lineRect.top);

            const float charCenterX = (hitRect.left + hitRect.right) * 0.5f;
            const float distFromWipe = charCenterX - wipeX;  // <= 0 means already sung

            float pop;
            if (distFromWipe <= 0.0f) {
                const float t = Clamp(-distFromWipe / kWaveTrailWidth, 0.0f, 1.0f);
                pop = 0.5f * (1.0f + std::cos(t * 3.14159265f));
            } else {
                const float t = Clamp(distFromWipe / kWaveLeadWidth, 0.0f, 1.0f);
                pop = 0.5f * (1.0f + std::cos(t * 3.14159265f));
            }

            const bool sung = distFromWipe <= 0.0f;
            D2D1_RECT_F charRect = D2D1::RectF(hitRect.left, lineRect.top, lineRect.right, lineRect.bottom);
            const wchar_t* clusterText = text + cluster.textStart;

            if (glowEnabled && glowStrength > 0.01f && pop > 0.02f) {
                const float alpha = kGlowAlpha * pop * baseOpacity * glowStrength;
                if (alpha > 0.003f) {
                    textBrush_->SetOpacity(alpha);
                    for (int s = 0; s < kGlowStampCount; ++s) {
                        const float a = (2.0f * 3.14159265f * s) / kGlowStampCount;
                        const float ox = std::cos(a) * kGlowRadius;
                        const float oy = std::sin(a) * kGlowRadius;
                        D2D1_RECT_F stampRect = D2D1::RectF(
                            charRect.left + ox, charRect.top + oy,
                            charRect.right + ox, charRect.bottom + oy);
                        target_->DrawTextW(clusterText, cluster.textLength, format, stampRect,
                                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    textBrush_->SetOpacity(baseOpacity);
                }
            }

            if (sung) {
                textBrush_->SetOpacity(baseOpacity);
                target_->DrawTextW(clusterText, cluster.textLength, format, charRect, textBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            } else {
                mutedBrush_->SetOpacity(baseOpacity * unsungOpacity);
                target_->DrawTextW(clusterText, cluster.textLength, format, charRect, mutedBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    void DrawTimeDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, SYSTEMTIME& local) {
        (void)now;

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;

        if (settings.clockAccentGlow) {
            D2D1_COLOR_F accentColor = accentBrush_ ? accentBrush_->GetColor() : D2D1::ColorF(0x4cc9f0);
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.24f)},
                {1.0f, D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> glowStops;
            target_->CreateGradientStopCollection(stops, 2, &glowStops);

            if (glowStops) {
                const D2D1_POINT_2F glowCenter = D2D1::Point2F(cx, cy - 8.0f * scale);
                ComPtr<ID2D1RadialGradientBrush> glowBrush;
                target_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(glowCenter, D2D1::Point2F(0, 0),
                                                         150.0f * scale, 70.0f * scale),
                    glowStops.Get(), &glowBrush);
                if (glowBrush) {
                    target_->FillEllipse(D2D1::Ellipse(glowCenter, 150.0f * scale, 70.0f * scale), glowBrush.Get());
                }
            }
        }

        // Time and date as a single typographic block. `dateFirst` promotes the
        // date to the headline for people who care about it more than the clock
        // (#61); either way the pair stays optically centred as a unit.
        const std::wstring timeText = FormatIslandTime(local, settings.clockFollowSystem,
                                                       settings.use24HourClock, settings.showSeconds);
        const std::wstring dateText = FormatIslandDate(local, settings.dateFormat, L"dddd, MMMM d");

        IDWriteTextFormat* bigFmt = timeDashboardFormat_ ? timeDashboardFormat_.Get() : hugeTextFormat_.Get();
        IDWriteTextFormat* smallFmt = dateDashboardFormat_ ? dateDashboardFormat_.Get() : boldTextFormat_.Get();

        const std::wstring& headline = settings.dateFirst ? dateText : timeText;
        const std::wstring& subline = settings.dateFirst ? timeText : dateText;
        IDWriteTextFormat* headlineFmt = settings.dateFirst ? smallFmt : bigFmt;
        IDWriteTextFormat* sublineFmt = settings.dateFirst ? bigFmt : smallFmt;

        if (settings.dateFirst) {
            // Date on top reads as a label, so give the big clock the lower slot.
            const D2D1_RECT_F headRect = D2D1::RectF(rect.left, cy - 46.0f * scale, rect.right, cy - 22.0f * scale);
            textBrush_->SetOpacity(0.92f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headlineFmt,
                               headRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F subRect = D2D1::RectF(rect.left, cy - 20.0f * scale, rect.right, cy + 44.0f * scale);
            textBrush_->SetOpacity(0.98f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), sublineFmt,
                               subRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        } else {
            const D2D1_RECT_F headRect = D2D1::RectF(rect.left, cy - 44.0f * scale, rect.right, cy + 20.0f * scale);
            textBrush_->SetOpacity(0.98f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headlineFmt,
                               headRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F subRect = D2D1::RectF(rect.left, cy + 22.0f * scale, rect.right, cy + 46.0f * scale);
            mutedBrush_->SetOpacity(0.85f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), sublineFmt,
                               subRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        textBrush_->SetOpacity(0.98f);

        const bool micActive = state.system.micActive && settings.privacyDots && settings.privacyDotsMic;
        const bool camActive = state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam;

        if (camActive || micActive) {
            std::wstring label;
            D2D1_COLOR_F dotColor;

            if (camActive && micActive) {
                dotColor = settings.privacyDotsCamHex;
                if (!state.system.cameraApp.empty() && !state.system.micApp.empty() && state.system.cameraApp == state.system.micApp) {
                    label = state.system.cameraApp + L" is using camera & microphone";
                } else if (!state.system.cameraApp.empty() && !state.system.micApp.empty()) {
                    label = state.system.cameraApp + L" & " + state.system.micApp + L" using camera & mic";
                } else if (!state.system.cameraApp.empty()) {
                    label = state.system.cameraApp + L" is using camera & microphone";
                } else if (!state.system.micApp.empty()) {
                    label = state.system.micApp + L" is using camera & microphone";
                } else {
                    label = L"Camera & microphone in use";
                }
            } else if (camActive) {
                dotColor = settings.privacyDotsCamHex;
                if (!state.system.cameraApp.empty()) {
                    label = state.system.cameraApp + L" is using your camera";
                } else {
                    label = L"Camera in use";
                }
            } else {
                dotColor = settings.privacyDotsMicHex;
                if (!state.system.micApp.empty()) {
                    label = state.system.micApp + L" is using your microphone";
                } else {
                    label = L"Microphone in use";
                }
            }

            IDWriteTextFormat* fmt = smallTextFormat_ ? smallTextFormat_.Get() : textFormat_.Get();
            if (fmt && dwriteFactory_) {
                ComPtr<IDWriteTextLayout> textLayout;
                HRESULT hr = dwriteFactory_->CreateTextLayout(
                    label.c_str(), static_cast<UINT32>(label.size()),
                    fmt, 500.0f, 30.0f, &textLayout);

                if (SUCCEEDED(hr) && textLayout) {
                    DWRITE_TEXT_METRICS tm = {};
                    textLayout->GetMetrics(&tm);

                    const float pillH = 22.0f * scale;
                    const float pillW = tm.width + 28.0f * scale;
                    const float pillX = cx - pillW * 0.5f;
                    const float pillY = rect.bottom - 18.0f * scale - pillH;

                    ComPtr<ID2D1SolidColorBrush> badgeBg;
                    target_->CreateSolidColorBrush(WithAlpha(material_.raised, material_.raised.a * settingsOpacity_), &badgeBg);
                    if (badgeBg) {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH), pillH * 0.5f, pillH * 0.5f),
                            badgeBg.Get());
                    }

                    ComPtr<ID2D1SolidColorBrush> dotBrush;
                    dotColor.a = settingsOpacity_;
                    target_->CreateSolidColorBrush(dotColor, &dotBrush);
                    if (dotBrush) {
                        const float badgeDotR = 3.5f * scale;
                        target_->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(pillX + 10.0f * scale, pillY + pillH * 0.5f), badgeDotR, badgeDotR),
                            dotBrush.Get());
                    }

                    mutedBrush_->SetOpacity(0.90f);
                    target_->DrawTextLayout(
                        D2D1::Point2F(pillX + 18.0f * scale, pillY + (pillH - tm.height) * 0.5f),
                        textLayout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                    mutedBrush_->SetOpacity(0.75f);
                }
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    void DrawIdleDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                           double now) {
        if (settings.gameOverlay || g_gameOverlayPinned.load()) {
            DrawGameOverlay(state, rect, 1.0f);
            return;
        }
        if (!clockFormat_) return;

        // Clip to the island's real silhouette (pill / notch / w11 rounded
        // rect), not its bounding box — a plain rect clip leaves the corners
        // outside the rounded shape unclipped, which is what was showing up
        // as a faint square "border" around the round island while collapsing.
        // Publish the same content-space geometry the media surface does, so the
        // File Tray's row hit test works while the island is idle too.
        PublishContentGeometry(rect);

        const float dashHeight = rect.bottom - rect.top;
        const float dashRadius = ContentIslandRadius(dashHeight);
        ComPtr<ID2D1Geometry> dashMask = CreateIslandMaskGeometry(rect, dashRadius, g_settings.notchStyle);
        ID2D1Layer* dashLayer = AcquireLayer();
        const bool haveDashMask = dashMask && dashLayer;
        if (haveDashMask) {
            target_->PushLayer(D2D1::LayerParameters(rect, dashMask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), dashLayer);
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring collapsedTime = FormatIslandTime(local, settings.clockFollowSystem,
                                                            settings.use24HourClock,
                                                            settings.showSeconds);
        const wchar_t* timeBuf = collapsedTime.c_str();

        const float scale = 1.0f;
        const float width = rect.right - rect.left;

        bool hasWeather = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
        std::wstring wIcon = L"🌡️";
        std::wstring wText = Loc(L"Loading...");
        if (hasWeather) {
            wText = state.weather.weatherDesc;
            GetWeatherIconAndText(state.weather.weatherCode, wIcon, wText);
        }

        // Cross-fade the collapsed status-bar view and the expanded tab view
        // across a small width band around the switchover point instead of
        // a hard cut. Previously this was a plain if/else on width, so the
        // clock's glow (and everything else in the expanded view) just got
        // clipped smaller as the pill shrank and then vanished outright the
        // instant width crossed the threshold — a pop, not a fade. Blending
        // both views by opacity (with an eased curve) makes it a dissolve.
        (void)width;
        auto SmoothFade = [](float t) {
            t = Clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        // Height-driven, like the media surface: collapsed content is gone by
        // ~70px, expanded content starts at ~56px and is solid by ~120px.
        const float collapsedAlpha = SmoothFade((70.0f - dashHeight) / 30.0f);
        const float expandedAlpha = SmoothFade((dashHeight - 56.0f) / 64.0f);

        ID2D1Layer* dashFadeLayer = AcquireLayer();

        if (collapsedAlpha > 0.01f) {
            if (dashFadeLayer) {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), collapsedAlpha, nullptr,
                                                          D2D1_LAYER_OPTIONS_NONE),
                                   dashFadeLayer);
            }

            // Collapsed Mode (Apple Dynamic Island status bar).
            //
            // Slots are placed from the same measurements that sized the pill, so
            // the clock gets exactly the room it needs and the divider sits between
            // the two strings instead of at an arbitrary geometric centre. The old
            // version split the pill 50/50 at centerX and padded both ends by 6px,
            // which is what produced the dead air on short strings
            // (windhawk-mods#5086).
            const IdleStripMetrics idleMetrics = MeasureIdleStrip(state, settings, now);
            IDWriteTextFormat* idleFmt =
                idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

            // The dot is anchored to the right edge by DrawPrivacyDots, so reserve
            // its lane rather than shrinking the text box out from under the clock.
            const float privacyReserve = IdleDotReserve(idleMetrics) * scale;
            const float innerLeft = rect.left + IdleStripLayout::kPadX * scale;
            const float innerRight =
                rect.right - IdleStripLayout::kPadX * scale - privacyReserve;

            float blockWidth = idleMetrics.clockWidth;
            if (idleMetrics.hasWeather) {
                blockWidth += (IdleStripLayout::kSlotGap * 2.0f +
                               IdleStripLayout::kDividerWidth) * scale +
                              idleMetrics.weatherWidth;
            }

            // Centre the block in whatever room the pill currently has. Mid-spring
            // the rect is wider or narrower than the natural width, and clamping
            // the slack at zero stops the slots from inverting when it is narrower.
            const float slack = std::max(0.0f, (innerRight - innerLeft) - blockWidth);
            float slotX = innerLeft + slack * 0.5f;

            // Each slot is exactly its measured width, and the measurement used the
            // widest-digit form, so the live string is centred inside a box it is
            // guaranteed to fit rather than drifting as the digits change.
            textBrush_->SetOpacity(0.96f);
            const D2D1_RECT_F timeRect =
                D2D1::RectF(slotX, rect.top, slotX + idleMetrics.clockWidth, rect.bottom);
            target_->DrawTextW(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), idleFmt,
                               timeRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            slotX += idleMetrics.clockWidth;

            if (idleMetrics.hasWeather) {
                slotX += IdleStripLayout::kSlotGap * scale;

                ComPtr<ID2D1SolidColorBrush> divider;
                target_->CreateSolidColorBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_),
                    &divider);
                if (divider) {
                    const float divW = IdleStripLayout::kDividerWidth * scale;
                    const float divTop = rect.top + IdleStripLayout::kDividerInsetY * scale;
                    const float divBottom = rect.bottom - IdleStripLayout::kDividerInsetY * scale;
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(slotX, divTop, slotX + divW, divBottom),
                                          divW * 0.5f, divW * 0.5f), divider.Get());
                }
                slotX += (IdleStripLayout::kDividerWidth + IdleStripLayout::kSlotGap) * scale;

                wchar_t weatherLabel[32] = {};
                if (hasWeather) swprintf_s(weatherLabel, L"%s %.0f\x00B0", wIcon.c_str(), state.weather.temperature);
                else wcscpy_s(weatherLabel, ARRAYSIZE(weatherLabel), L"\U0001F321\uFE0F --\x00B0");

                const D2D1_RECT_F wRect =
                    D2D1::RectF(slotX, rect.top, slotX + idleMetrics.weatherWidth, rect.bottom);
                target_->DrawTextW(weatherLabel, static_cast<UINT32>(wcslen(weatherLabel)), idleFmt,
                                   wRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
            textBrush_->SetOpacity(1.0f);

            if (dashFadeLayer) {
                target_->PopLayer();
            }
        }

        if (expandedAlpha > 0.01f) {
            if (dashFadeLayer) {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), expandedAlpha, nullptr,
                                                          D2D1_LAYER_OPTIONS_NONE),
                                   dashFadeLayer);
            }

            // Expanded Mode. Order must match ActiveTabCount()/FileTrayTabIndex().
            std::vector<int> activeTabs;
            activeTabs.push_back(3); // Time (shown first when the island expands/on hover)
            if (settings.notificationCenterModule) activeTabs.push_back(7);
            if (settings.weather) activeTabs.push_back(1); // Weather
            activeTabs.push_back(0); // Calendar
            if (settings.githubModule) activeTabs.push_back(5);
            if (settings.hardwareMonitorModule) activeTabs.push_back(2);
            if (settings.fileTrayModule) activeTabs.push_back(4);

            int maxTabs = static_cast<int>(activeTabs.size());
            int tabIdx = NormalizedTabIndex(settings);
            if (tabIdx >= maxTabs) tabIdx = maxTabs - 1;

            // Lay the expanded view out at its final size, centred horizontally and
            // pinned to the top, so growing the pill only reveals it (same as media)
            // instead of re-centring and rescaling the content every frame.
            const float fullW = std::max(MediaLayout::kExpandedWidth, rect.right - rect.left);
            const float fullH = std::max(MediaLayout::kExpandedHeight, rect.bottom - rect.top);
            const float fullCx = (rect.left + rect.right) * 0.5f;
            const D2D1_RECT_F fullRect = D2D1::RectF(fullCx - fullW * 0.5f, rect.top,
                                                     fullCx + fullW * 0.5f, rect.top + fullH);

            DrawTabTransition(1, tabIdx, activeTabs, fullRect, now, [&](int activeTabId) {
                if (activeTabId == 3) DrawTimeDashboard(state, fullRect, settings, now, scale, local);
                else if (activeTabId == 0) DrawCalendarDashboard(state, fullRect, settings, now, scale, local);
                else if (activeTabId == 1) DrawWeatherDashboard(state, fullRect, settings, now, scale, hasWeather, wIcon, wText);
                else if (activeTabId == 5) DrawGitHubDashboard(state, fullRect, settings, scale);
                else if (activeTabId == 2) DrawHardwareMonitorDashboard(state, fullRect, settings, scale);
                else if (activeTabId == 4) DrawFileTrayDashboard(state, fullRect, settings, scale);
                else if (activeTabId == 7) DrawNotificationCenterDashboard(state, fullRect, scale);
            });

            DrawTabPagination(fullRect, maxTabs);

            if (dashFadeLayer) {
                target_->PopLayer();
            }
        }

        if (expandedAlpha <= 0.01f && !g_trayDragOver.load(std::memory_order_relaxed)) {
            g_idleTab = 0;
            ResetTabTransition();
        }

        if (haveDashMask) target_->PopLayer(); else target_->PopAxisAlignedClip();
    }

    void DrawGameOverlay(const SharedState& state, D2D1_RECT_F rect, float unused_scale) {
        (void)unused_scale;
        const float scale = 1.0f;
        const bool compact = g_settings.gameOverlayCompact;
        const GameOverlayLayout::Metrics m = GameOverlayLayout::For(compact);

        const float cardTop = rect.top + m.padY;
        const float cardBottom = rect.bottom - m.padY;
        float cursorX = rect.left + m.padX;

        // ── FPS, given hero treatment ────────────────────────────────────────
        // Frame rate is the number a player actually watches, so it gets a wider
        // card and a larger figure than the percentages beside it. It has no
        // 0-100 ceiling, so it gets no load bar and keeps the plain accent.
        if (g_settings.gameOverlayShowFps) {
            const D2D1_RECT_F fpsCard = D2D1::RectF(cursorX, cardTop, cursorX + m.fpsW, cardBottom);
            DrawCard(fpsCard, m.radius);

            ComPtr<ID2D1SolidColorBrush> fpsIcon;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.accent, 0.90f * settingsOpacity_), &fpsIcon)) && fpsIcon) {
                DrawMetricGlyph(D2D1::Point2F(fpsCard.left + 15.0f * scale, fpsCard.top + 15.0f * scale),
                                15.0f * scale, 9, fpsIcon.Get());
            }

            const float textLeft = fpsCard.left + 28.0f * scale;
            const float textRight = fpsCard.right - 9.0f * scale;

            if (g_settings.showMetricText && smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.58f);
                target_->DrawTextW(L"FPS", 3, smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, fpsCard.top + 2.0f * scale,
                                               textRight, fpsCard.top + 16.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            wchar_t fpsValue[16] = {};
            swprintf_s(fpsValue, L"%d", state.system.renderFps);
            IDWriteTextFormat* fpsFmt = clockFormat_ ? clockFormat_.Get() : textFormat_.Get();
            if (fpsFmt) {
                // clockFormat_ is centre-aligned by default; the strip reads as a
                // left-aligned column, so override for this draw and restore.
                fpsFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textBrush_->SetOpacity(0.97f);
                // Shares the label's left margin so the glyph sits in its own
                // column and the text column has one straight edge, rather than
                // the value hanging out to the left of the label above it.
                const float top = g_settings.showMetricText ? fpsCard.top + 16.0f * scale : cardTop;
                target_->DrawTextW(fpsValue, static_cast<UINT32>(wcslen(fpsValue)), fpsFmt,
                                   D2D1::RectF(textLeft, top,
                                               textRight, cardBottom - 3.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                fpsFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            }

            cursorX = fpsCard.right + m.fpsGap;
        }

        // ── Load cards ───────────────────────────────────────────────────────
        // Laid out from whichever metrics are enabled (#25), so turning some off
        // closes the gap instead of leaving a hole. Icons come from the shared
        // DrawMetricGlyph family, so CPU here is the same symbol as CPU on the
        // hardware dashboard.
        struct GameCard {
            const wchar_t* label;
            int percent;
            int glyph;
            bool enabled;
        };
        const GameCard cards[] = {
            {L"CPU", state.system.cpuPercent,              3, g_settings.gameOverlayShowCpu},
            {L"RAM", state.system.memoryPercent,           4, g_settings.gameOverlayShowRam},
            {L"GPU", state.system.gpuPercent,              5, g_settings.gameOverlayShowGpu},
            {L"DISK", 100 - state.system.diskFreePercent,  8, g_settings.gameOverlayShowDisk},
        };

        for (const GameCard& card : cards) {
            if (!card.enabled) {
                continue;
            }
            if (cursorX + m.cardW > rect.right - m.padX + 0.5f) {
                break;  // ran out of room
            }
            DrawGameMetricCard(D2D1::RectF(cursorX, cardTop, cursorX + m.cardW, cardBottom),
                               card.label, card.percent, card.glyph, m.radius);
            cursorX += m.cardW + m.gap;
        }

        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawGameMetricCard(D2D1_RECT_F rect, const wchar_t* label, int percent, int glyph, float radius) {
        const float scale = 1.0f;

        // Same semantic load colour the hardware dashboard uses: accent while a
        // component is comfortable, amber under pressure, red when saturated.
        // This replaces a per-metric rainbow (cyan CPU, magenta RAM, green GPU,
        // orange disk) that encoded nothing and fought the album-art accent.
        const bool known = percent >= 0;
        const float pct = known ? Clamp(percent / 100.0f, 0.0f, 1.0f) : 0.0f;
        const D2D1_COLOR_F tint = known ? LoadStateColor(pct) : material_.accent;

        // Fill only. This card used to carry a hairline border *and* a
        // metric-coloured ring on top of it -- two bright outlines per card,
        // four cards across, which is the edge lighting this design drops.
        DrawCard(rect, radius);

        ComPtr<ID2D1SolidColorBrush> glyphBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(
                WithAlpha(tint, 0.85f * settingsOpacity_), &glyphBrush)) && glyphBrush) {
            DrawMetricGlyph(D2D1::Point2F(rect.left + 15.0f * scale, rect.top + 15.0f * scale),
                            14.0f * scale, glyph, glyphBrush.Get());
        }

        const float textLeft = rect.left + 28.0f * scale;
        const float textRight = rect.right - 8.0f * scale;

        // Label above value above bar, matching the hardware dashboard exactly.
        if (g_settings.showMetricText && smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            mutedBrush_->SetOpacity(0.58f);
            target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), smallTextFormat_.Get(),
                               D2D1::RectF(textLeft, rect.top + 2.0f * scale,
                                           textRight, rect.top + 16.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        wchar_t value[16] = {};
        if (known) {
            swprintf_s(value, L"%d%%", percent);
        } else {
            wcscpy_s(value, ARRAYSIZE(value), L"--");
        }

        if (textFormat_) {
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textBrush_->SetOpacity(0.95f);
            // Aligned with the label above it, so the glyph owns the left column
            // and label/value share one straight edge.
            const float valueTop = g_settings.showMetricText ? rect.top + 17.0f * scale
                                                             : rect.top + 4.0f * scale;
            target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                               D2D1::RectF(textLeft, valueTop,
                                           textRight, rect.bottom - 8.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // The bar spans the card rather than just the text column: at this size it
        // reads as the card's own meter, and a 38px stub under the value did not.
        const D2D1_RECT_F track = D2D1::RectF(rect.left + 13.0f * scale, rect.bottom - 7.0f * scale,
                                              rect.right - 13.0f * scale, rect.bottom - 4.5f * scale);
        ComPtr<ID2D1SolidColorBrush> trackBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale), trackBrush.Get());
        }
        const float span = (track.right - track.left) * pct;
        if (span > 0.5f) {
            ComPtr<ID2D1SolidColorBrush> fillBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(tint, 0.95f), &fillBrush)) && fillBrush) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                      1.25f * scale, 1.25f * scale),
                    fillBrush.Get());
            }
        }

        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    // ── Tab transition (spring-driven carousel) ─────────────────────────────
    // tabPos_ is a continuous "which tab is in view" value that a damped spring
    // chases toward the selected index. Every visible tab is drawn from its
    // distance to that position (slide + shrink + fade), so the motion is
    // interruptible, keeps its momentum on fast scrolling, and never restarts.
    void ResetTabTransition() {
        tabInit_ = false;
        tabAnimating_ = false;
        g_tabAnimating.store(false, std::memory_order_relaxed);
        g_tabEdgeBump.store(0, std::memory_order_relaxed);
    }

    template <typename DrawFn>
    void DrawTabTransition(int context, int tabIdx, const std::vector<int>& tabIds,
                           D2D1_RECT_F rect, double now, DrawFn&& drawTab) {
        const int count = static_cast<int>(tabIds.size());
        if (count <= 0) {
            ResetTabTransition();
            return;
        }
        tabIdx = ClampInt(tabIdx, 0, count - 1);

        // ── Feel: tweak these ──
        constexpr float kStiffness = 250.0f;        // higher = faster (settles in ~0.29s)
        constexpr float kDamping = 31.0f;           // ~0.92 damping ratio: no visible overshoot
        constexpr float kTravelPx = 34.0f;          // vertical gap between neighbouring tabs
        constexpr float kMinScale = 0.965f;         // size of a tab that is fully out of view
        constexpr float kFadePower = 1.4f;          // >1 thins the mid-point slightly so text never ghosts

        float dt = static_cast<float>(now - tabLastDrawn_);
        tabLastDrawn_ = now;
        // First frame of a motion: the previous paint may be long ago, so don't
        // let a big gap swallow the start of the animation.
        dt = tabAnimating_ ? Clamp(dt, 0.001f, 0.05f) : (1.0f / 120.0f);

        // Snap (no animation) when the tab view was just opened, switched between
        // media/idle views, or the number of tabs changed.
        if (!tabInit_ || context != tabContext_ || count != tabCount_) {
            tabInit_ = true;
            tabContext_ = context;
            tabCount_ = count;
            tabPos_ = static_cast<float>(tabIdx);
            tabTarget_ = tabPos_;
            tabVel_ = 0.0f;
            tabDir_ = 1.0f;
            tabAnimating_ = false;
        }
        const float newTarget = static_cast<float>(tabIdx);
        if (newTarget != tabTarget_) {
            tabDir_ = newTarget > tabTarget_ ? 1.0f : -1.0f;  // remembered for the whole move
            tabTarget_ = newTarget;
        }

        // The first and last tab are hard stops: there is no edge bump any more.
        g_tabEdgeBump.store(0, std::memory_order_relaxed);

        if (tabPos_ != tabTarget_ || tabVel_ != 0.0f) {
            float remaining = dt;
            while (remaining > 0.0f) {
                const float h = std::min(remaining, 0.004f);
                tabVel_ += (-kStiffness * (tabPos_ - tabTarget_) - kDamping * tabVel_) * h;
                tabPos_ += tabVel_ * h;

                // Never travel past the first or last tab, and kill any velocity
                // pushing further out, so the edge tabs cannot overshoot or twitch.
                const float maxTabPos = static_cast<float>(count - 1);
                if (tabPos_ < 0.0f) {
                    tabPos_ = 0.0f;
                    if (tabVel_ < 0.0f) tabVel_ = 0.0f;
                } else if (tabPos_ > maxTabPos) {
                    tabPos_ = maxTabPos;
                    if (tabVel_ > 0.0f) tabVel_ = 0.0f;
                }
                remaining -= h;
            }
            if (std::fabs(tabPos_ - tabTarget_) < 0.0008f && std::fabs(tabVel_) < 0.02f) {
                tabPos_ = tabTarget_;
                tabVel_ = 0.0f;
            }
        }

        const bool moving = (tabPos_ != tabTarget_ || tabVel_ != 0.0f);
        tabAnimating_ = moving;
        g_tabAnimating.store(moving, std::memory_order_relaxed);

        if (!moving) {
            drawTab(tabIds[tabIdx]);
            return;
        }
        g_tabLastPaintAt.store(now, std::memory_order_relaxed);  // render loop keeps frames coming until it settles

        const D2D1_POINT_2F center =
            D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);

        if (!tabLayer_) {
            target_->CreateLayer(&tabLayer_);  // cached: re-creating it every frame is costly
        }

        // Every tab is a pure function of its distance to tabPos_, with no
        // "arriving" / "leaving" roles. Motion is therefore continuous even when you
        // reverse mid-scroll, and the tabs travel as one rigid strip (a fixed gap
        // apart), which is what reads as real scrolling rather than two fading cards.
        auto drawLayer = [&](int index) {
            if (index < 0 || index >= count) {
                return;
            }
            const float d = static_cast<float>(index) - tabPos_;  // <0 above, >0 below
            const float ad = Clamp(std::fabs(d), 0.0f, 1.0f);
            const float eased = SmoothStep01(ad);

            const float alpha = std::pow(1.0f - eased, kFadePower);
            if (alpha <= 0.004f) {
                return;
            }

            const float scale = 1.0f - (1.0f - kMinScale) * eased;
            const float dy = d * kTravelPx;

            target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) *
                                  D2D1::Matrix3x2F::Translation(0.0f, dy) * oldTransform);
            if (alpha < 0.995f && tabLayer_) {
                target_->PushLayer(
                    D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), alpha, nullptr,
                                          D2D1_LAYER_OPTIONS_NONE),
                    tabLayer_.Get());
                drawTab(tabIds[index]);
                target_->PopLayer();
            } else {
                drawTab(tabIds[index]);  // fully opaque: skip the layer, much cheaper
            }
            target_->SetTransform(oldTransform);
        };

        // Farther tab first, so the one closer to centre ends up on top.
        const int lo = static_cast<int>(std::floor(tabPos_));
        if ((tabPos_ - static_cast<float>(lo)) < 0.5f) {
            drawLayer(lo + 1);
            drawLayer(lo);
        } else {
            drawLayer(lo);
            drawLayer(lo + 1);
        }
    }

    // Vertical pagination dots on the right edge. The active dot stretches into a
    // pill and slides with the transition instead of jumping.
    void DrawTabPagination(D2D1_RECT_F rect, int tabCount) {
        if (tabCount <= 1) {
            return;
        }
        ID2D1SolidColorBrush* dotBrush = AcquireBrush(WithAlpha(material_.textPrimary, 1.0f));
        if (!dotBrush) {
            return;
        }

        const float dotX = rect.right - 10.0f;
        const float spacing = 9.0f;
        const float r = 2.5f;
        const float startY = (rect.top + rect.bottom) * 0.5f - spacing * (tabCount - 1) * 0.5f;

        // Resting dots. The one under the indicator fades out, so the pill reads
        // as travelling over the track instead of sitting on top of a dot.
        for (int i = 0; i < tabCount; ++i) {
            const float under = SmoothStep01(Clamp(1.0f - std::fabs(tabPos_ - static_cast<float>(i)), 0.0f, 1.0f));
            dotBrush->SetOpacity(0.22f * (1.0f - under) * settingsOpacity_);
            target_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(dotX, startY + spacing * static_cast<float>(i)), r, r),
                dotBrush);
        }

        // Travelling indicator: follows the spring exactly (so it also gives a little
        // at the ends) and stretches slightly with speed.
        const float pos = Clamp(tabPos_, -0.3f, static_cast<float>(tabCount) - 0.7f);
        const float stretch = Clamp(std::fabs(tabVel_) * 0.55f, 0.0f, 3.5f);
        const float halfH = r + 2.2f + stretch;
        const float cy = startY + spacing * pos;
        dotBrush->SetOpacity(0.90f * settingsOpacity_);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(dotX - r, cy - halfH, dotX + r, cy + halfH), r, r),
            dotBrush);
    }

    // Publishes the content-space rect currently being painted so
    // OverlayWndProc hit-tests against the real geometry rather than assuming
    // the pill is centered in the client area. Seqlock write: bump to odd,
    // store, bump to even, so a reader can never mix two frames.
    void PublishContentGeometry(D2D1_RECT_F rect, float rightTrim = 0.0f) {
        const unsigned seq = g_mediaHitSeq.load(std::memory_order_relaxed);
        g_mediaHitSeq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        g_mediaHitLeft.store(rect.left, std::memory_order_relaxed);
        g_mediaHitTop.store(rect.top, std::memory_order_relaxed);
        g_mediaHitRight.store(rect.right, std::memory_order_relaxed);
        g_mediaHitBottom.store(rect.bottom, std::memory_order_relaxed);
        g_mediaHitScale.store(g_settings.sizeScale, std::memory_order_relaxed);
        g_mediaHitTrim.store(rightTrim, std::memory_order_relaxed);
        g_mediaHitStamp.store(GetTickCount64(), std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
        g_mediaHitSeq.store(seq + 2, std::memory_order_relaxed);
    }

    // Corner radius for the island mask in *content* space -- the coordinate
    // system DrawMedia and DrawIdleDashboard are handed, which DrawPill scales
    // by sizeScale afterwards. Because that scale is applied later, these radii
    // must not be pre-multiplied by sizeScale. They previously were, so the
    // stadium corner arc grew with the size scale until it cut across the album
    // art's top-left corner (visible from roughly 2x upwards).
    float ContentIslandRadius(float contentHeight) const {
        if (g_settings.notchStyle) {
            return 16.0f;
        }
        if (g_settings.w11Style) {
            return 8.0f;
        }
        return std::min(contentHeight * 0.5f, 44.0f);
    }

    // Takes settings by reference from Render()'s private copy rather than
    // reaching for g_settings. The dashboards it delegates to read std::wstring
    // members (DrawCalendarDashboard -> settings.dateFormat), which would
    // otherwise race with LoadSettings() replacing the struct mid-frame.
    void DrawMedia(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                   double now) {
        const float height = rect.bottom - rect.top;

        // Island resize tracking: "settled" once the size has stopped changing for 0.15s.
        // Small per-frame drifts accumulate against the last latched size, so the
        // spring's tail can't sneak past the check.
        {
            const float curW = rect.right - rect.left;
            if (std::fabs(curW - islandSizeLastW_) > 0.25f ||
                std::fabs(height - islandSizeLastH_) > 0.25f) {
                islandSizeLastW_ = curW;
                islandSizeLastH_ = height;
                islandSizeMovedAt_ = now;
            }
            islandSettled_ = (now - islandSizeMovedAt_) > 0.15;
        }

        expandedAnim_ = settings.expandedMediaAnim;
        pillCoverAnim_ = settings.pillCoverAnim;

        // The old side panel is replaced by the immersive lyrics view, so the island never widens.
        const bool sideLyricsOn = false;
        // 0 = normal player, 1 = immersive lyrics (always 0 unless "Lyrics in the media tab" is on).
        const float immersiveMix = UpdateImmersiveMix(settings, height, now);
        g_immersiveMixPub.store(immersiveMix, std::memory_order_relaxed);
        const float sideLyricsExtra =
            sideLyricsOn ? std::max(0.0f, (rect.right - rect.left) - MediaLayout::kExpandedWidth)
                         : 0.0f;

        PublishContentGeometry(rect, sideLyricsExtra);

        const float radius = ContentIslandRadius(height);
        ComPtr<ID2D1Geometry> mask = CreateIslandMaskGeometry(rect, radius, settings.notchStyle);
        ID2D1Layer* layer = AcquireLayer();

        float expandedAlpha = std::clamp((height - MediaLayout::kExpandedMinHeight) / 60.0f, 0.0f, 1.0f);
        float collapsedAlpha = std::clamp((80.0f - height) / 30.0f, 0.0f, 1.0f);

        // ── Shared album art ─────────────────────────────────────────────────
        // One art element that morphs between the collapsed geometry (small circle
        // on the left) and the expanded geometry (64px rounded square) as the island
        // grows or shrinks, instead of cross-fading two different shapes.
        // artMorph: 0 = collapsed pill, 1 = fully expanded (reached at 120px, where
        // the expanded layer is already fully opaque and takes over drawing the art).
        constexpr float kCollapsedPillHeight = 44.0f;
        constexpr float kArtMorphEndHeight = 120.0f;
        constexpr float kCollapsedArtPad = 6.0f;
        constexpr float kCollapsedArtSize = kCollapsedPillHeight - kCollapsedArtPad * 2.0f;
        // Raw progress straight from the island's height.
        const float artMorphRaw = Clamp((height - kCollapsedPillHeight) /
                                        (kArtMorphEndHeight - kCollapsedPillHeight), 0.0f, 1.0f);

        // Shrinking follows the height exactly (unchanged). Expanding is not
        // allowed to run faster than kArtExpandSec for the full circle -> square
        // change, because the expand spring is so fast that the morph would
        // otherwise be over in a few frames.
        constexpr float kArtExpandSec = 0.26f;
        // The art only travels to its expanded spot on the Media tab. On any other
        // first tab (e.g. pinned lyrics) it just follows the height, with no throttle.
        const bool artOnMediaTab = NormalizedTabIndex(settings) == 0;
        const bool freshMorph = artMorphLast_ >= 0.0 && (now - artMorphLast_) < 0.25;
        const float dtMorph = freshMorph ? Clamp(static_cast<float>(now - artMorphLast_), 0.001f, 0.05f) : 0.0f;
        artMorphLast_ = now;

        if (!freshMorph || artMorphRaw <= artMorphLin_ || !artOnMediaTab) {
            artMorphLin_ = artMorphRaw;
        } else {
            artMorphLin_ = std::min(artMorphRaw, artMorphLin_ + dtMorph / kArtExpandSec);
            g_layoutDirty = true;  // keep frames coming until the morph catches up
        }
        const float artMorph = SmoothStep01(artMorphLin_);

        // Source badge: hidden during the morph, fades in once the art hands over
        // to the expanded layer (this used to pop in instantly).
        if (artMorph >= 1.0f) {
            if (!freshMorph) {
                artBadgeAlpha_ = 1.0f;
            } else if (artBadgeAlpha_ < 1.0f) {
                artBadgeAlpha_ = std::min(1.0f, artBadgeAlpha_ + dtMorph / 0.20f);
                g_layoutDirty = true;
            }
        } else {
            artBadgeAlpha_ = 0.0f;
        }
        float pillClockSection = 0.0f;
        if (settings.mediaPillClock && idleTextFormat_) {
            pillClockSection = MeasureMediaPill(state, settings).sectionWidth;
        }
        const D2D1_RECT_F collapsedArtRect = D2D1::RectF(
            rect.left + pillClockSection + kCollapsedArtPad, rect.top + kCollapsedArtPad,
            rect.left + pillClockSection + kCollapsedArtPad + kCollapsedArtSize,
            rect.top + kCollapsedArtPad + kCollapsedArtSize);
        auto lerpF = [](float a, float b, float t) { return a + (b - a) * t; };
        const D2D1_RECT_F normalArtRect = D2D1::RectF(
            rect.left + MediaLayout::kArtInsetX, rect.top + MediaLayout::kArtInsetY,
            rect.left + MediaLayout::kArtInsetX + MediaLayout::kArtSize,
            rect.top + MediaLayout::kArtInsetY + MediaLayout::kArtSize);
        const D2D1_RECT_F immersiveArtRect = D2D1::RectF(
            rect.left + ImmersiveLayout::kArtLeft, rect.top + ImmersiveLayout::kArtTop,
            rect.left + ImmersiveLayout::kArtLeft + ImmersiveLayout::kArtSize,
            rect.top + ImmersiveLayout::kArtTop + ImmersiveLayout::kArtSize);
        // Equals the original rect / radius exactly while immersiveMix == 0.
        const D2D1_RECT_F expandedArtRect = D2D1::RectF(
            lerpF(normalArtRect.left, immersiveArtRect.left, immersiveMix),
            lerpF(normalArtRect.top, immersiveArtRect.top, immersiveMix),
            lerpF(normalArtRect.right, immersiveArtRect.right, immersiveMix),
            lerpF(normalArtRect.bottom, immersiveArtRect.bottom, immersiveMix));
        const float expandedArtRadius =
            lerpF(16.0f, ImmersiveLayout::kArtSize * 0.5f, immersiveMix);
        const D2D1_RECT_F morphArtRect = D2D1::RectF(
            lerpF(collapsedArtRect.left, expandedArtRect.left, artMorph),
            lerpF(collapsedArtRect.top, expandedArtRect.top, artMorph),
            lerpF(collapsedArtRect.right, expandedArtRect.right, artMorph),
            lerpF(collapsedArtRect.bottom, expandedArtRect.bottom, artMorph));
        const float morphArtRadius = lerpF(kCollapsedArtSize * 0.5f, expandedArtRadius, artMorph);

        // Expanded UI
        if (expandedAlpha > 0.01f && mask && layer) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), expandedAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE), layer);

            // Order must match ActiveTabCount()/FileTrayTabIndex().
            std::vector<int> activeTabs;
            activeTabs.push_back(0); // Media
            if (settings.notificationCenterModule) activeTabs.push_back(7);
            if (settings.weather) activeTabs.push_back(2); // Weather
            activeTabs.push_back(1); // Calendar
            if (settings.githubModule) activeTabs.push_back(6);
            if (settings.hardwareMonitorModule) activeTabs.push_back(3);
            if (settings.fileTrayModule) activeTabs.push_back(4);

            // Same settings object as the list above, so the index can't be
            // normalised against a tab set that no longer matches.
            int maxTabs = static_cast<int>(activeTabs.size());
            int tabIdx = NormalizedTabIndex(settings);
            if (tabIdx >= maxTabs) tabIdx = maxTabs - 1;

            auto drawTabContent = [&](int activeTabId) {
            if (activeTabId == 0) {
                // With side lyrics on, the player lives in the left part of the island and
                // the rest of this branch (which uses `rect`) lays out exactly as before.
                const D2D1_RECT_F fullIslandRect = rect;
                const D2D1_RECT_F rect = D2D1::RectF(fullIslandRect.left, fullIslandRect.top,
                                                     fullIslandRect.right - sideLyricsExtra,
                                                     fullIslandRect.bottom);
                // Source switch envelope for everything that changes with the source.
                float swapAlpha = 1.0f, swapDx = 0.0f;
                UpdateSourceSwap(state, now, &swapAlpha, &swapDx);

                // Art, text and spectrum slide + fade as one group (closed in 13c).
                const bool contentGrouped = BeginSwapGroup(swapAlpha, swapDx);
                if (swapAlpha > 0.003f) {
                // Expanded Apple DI media: large square art on left, text center.
                const float artSize = MediaLayout::kArtSize;
                D2D1_RECT_F artRect = D2D1::RectF(rect.left + MediaLayout::kArtInsetX,
                                                  rect.top + MediaLayout::kArtInsetY,
                                                  rect.left + MediaLayout::kArtInsetX + artSize,
                                                  rect.top + MediaLayout::kArtInsetY + artSize);
                // Until the morph has finished, the shared art pass at the bottom of
                // DrawMedia owns the cover, so it is never drawn twice.
                if (artMorph >= 1.0f) {
                    DrawAlbumArtFlip(state.media, expandedArtRect, now, expandedArtRadius,
                                     immersiveMix < 0.01f);
                }

                // Immersive: title / artist / spectrum / source line cross-fade out.
                const bool normalFaded = BeginImmersiveFade(1.0f - immersiveMix);
                if (immersiveMix < 0.999f) {

                const float waveW = 32.0f;
                const float waveH = 20.0f;
                D2D1_RECT_F waveRect = D2D1::RectF(rect.right - 24.0f - waveW,
                                                   rect.top + 20.0f + (artSize - waveH) * 0.5f,
                                                   rect.right - 24.0f,
                                                   rect.top + 20.0f + (artSize + waveH) * 0.5f);

                const float textLeft = artRect.right + 18.0f;
                const float textRight = waveRect.left - 16.0f;

                // Title — bold, prominent.
                D2D1_RECT_F titleRect = D2D1::RectF(textLeft, rect.top + 34.0f, textRight, rect.top + 54.0f);
                const std::wstring titleText =
                    state.media.title.empty() ? std::wstring(Loc(L"Unknown")) : state.media.title;
                if (!DrawTitleTransition(titleText, titleRect, textBrush_.Get(), now, !state.media.title.empty())) {
                    DrawMarqueeText(titleText, titleRect, textFormat_.Get(), textBrush_.Get(), now, 42.0f,
                                    marqueeTitleCache_);
                }

                // Artist — muted below title.
                D2D1_RECT_F artistRect = D2D1::RectF(textLeft, rect.top + 54.0f, textRight, rect.top + 74.0f);
                mutedBrush_->SetOpacity(0.80f);
                DrawSwapMarquee(swapArtist_, state.media.artist, artistRect, smallTextFormat_.Get(),
                                mutedBrush_.Get(), now, 30.0f, marqueeArtistCache_, 0.06f);
                mutedBrush_->SetOpacity(0.75f);

                if (!state.media.albumTitle.empty()) {
                    D2D1_RECT_F albumRect = D2D1::RectF(textLeft, rect.top + 68.0f, textRight, rect.top + 84.0f);
                    mutedBrush_->SetOpacity(0.70f);
                    DrawSwapMarquee(swapAlbum_, state.media.albumTitle, albumRect, smallTextFormat_.Get(),
                                    mutedBrush_.Get(), now, 28.0f, marqueeAlbumCache_, 0.12f);
                    mutedBrush_->SetOpacity(0.75f);
                }

                if (settings.spectrumStyle == SpectrumStyle::Bars) {
                    DrawSpectrum(state, waveRect, settings, now, state.media.playing);
                } else if (state.media.playing) {
                    DrawSpectrum(state, waveRect, settings, now);
                } else {
                    const float gap = 2.5f;
                    const float availableW = waveRect.right - waveRect.left;
                    const int count = 7;
                    const float barWidth = (availableW - gap * (count - 1)) / count;
                    const float centerY = (waveRect.top + waveRect.bottom) * 0.5f;
                    mutedBrush_->SetOpacity(0.5f);
                    for (int i = 0; i < count; ++i) {
                        const float dotX = waveRect.left + i * (barWidth + gap) + barWidth * 0.5f;
                        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, centerY), 1.2f, 1.2f), mutedBrush_.Get());
                    }
                }

                // Source line, under the artist (or under the album when there is one).
                const float srcLineTop = rect.top + (state.media.albumTitle.empty() ? 70.0f : 85.0f);
                DrawSourceLine(state.media, D2D1::RectF(textLeft, srcLineTop, textRight, srcLineTop + 14.0f), now);
                }  // end of `if (immersiveMix < 0.999f)`
                EndImmersiveFade(normalFaded);
                DrawImmersiveLyricsView(state, rect, settings, now, immersiveMix);
                }  // end of `if (swapAlpha > 0.003f)` opened in 13b
                EndSwapGroup(contentGrouped);

                // Immersive lyrics: draw the transport block a little smaller so the
                // lyrics get the room. Restored after the lyrics button below.
                D2D1_MATRIX_3X2_F playerZoneOldTransform;
                target_->GetTransform(&playerZoneOldTransform);
                if (immersiveMix > 0.001f) {
                    const float ps = 1.0f + (ImmersiveLayout::kPlayerScale - 1.0f) * immersiveMix;
                    target_->SetTransform(
                        D2D1::Matrix3x2F::Scale(
                            ps, ps,
                            D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                          rect.top + ImmersiveLayout::kPlayerPivotY)) *
                        playerZoneOldTransform);
                }

                // Scrubber + time labels fade (no slide).
                const bool timelineGrouped = BeginSwapGroup(swapAlpha, 0.0f);

                // Timeline (Scrubber)
                const float scrubberY = rect.top + MediaLayout::kScrubberY;
                double currentPosition = state.media.positionTicks / 10000000.0;
                double duration = state.media.endTicks / 10000000.0;
                if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                    currentPosition += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
                }
                currentPosition = std::max(0.0, std::min(currentPosition, duration));

                const bool isDraggingThisBar = g_scrubbing.load(std::memory_order_relaxed);
                float progress;
                if (isDraggingThisBar) {
                    progress = Clamp(g_scrubDragFraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    currentPosition = duration * progress;
                } else {
                    progress = duration > 0.0 ? static_cast<float>(currentPosition / duration) : 0.0f;
                }

                auto FormatTime = [](double seconds) -> std::wstring {
                    if (seconds <= 0.0 || _isnan(seconds)) return L"0:00";
                    int m = static_cast<int>(seconds) / 60;
                    int s = static_cast<int>(seconds) % 60;
                    wchar_t buf[16];
                    swprintf_s(buf, L"%d:%02d", m, s);
                    return buf;
                };

                std::wstring elapsedStr = FormatTime(currentPosition);
                std::wstring remainStr = L"-" + FormatTime(duration - currentPosition);

                const float scrubLeft = rect.left + MediaLayout::kScrubMargin;
                const float scrubRight = rect.right - MediaLayout::kScrubMargin;

                const float barLeft = scrubLeft + MediaLayout::kScrubBarLeftInset;
                const float barRight = scrubRight - MediaLayout::kScrubBarRightInset;
                const float timeGap = 8.0f;

                D2D1_RECT_F elRect = D2D1::RectF(scrubLeft, scrubberY - 10.0f, barLeft - timeGap, scrubberY + 10.0f);
                D2D1_RECT_F remRect = D2D1::RectF(barRight + timeGap, scrubberY - 10.0f, scrubRight, scrubberY + 10.0f);

                mutedBrush_->SetOpacity(0.8f);
                if (smallTextFormat_) {
                    smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    target_->DrawTextW(elapsedStr.c_str(), static_cast<UINT32>(elapsedStr.size()), smallTextFormat_.Get(), elRect, mutedBrush_.Get());

                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    target_->DrawTextW(remainStr.c_str(), static_cast<UINT32>(remainStr.size()), smallTextFormat_.Get(), remRect, mutedBrush_.Get());

                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }

                // The scrubber uses the shared accent track, so it matches the
                // volume, battery and timer bars exactly. The bar thickens while
                // dragging, which is the standard cue that it is grabbable.
                if (settings.progressStyle != ProgressStyle::Slim) {
                    DrawStyledProgress(settings.progressStyle, barLeft, barRight, scrubberY, progress,
                                       isDraggingThisBar, state.media.playing, now);
                } else {
                const float barHalf = isDraggingThisBar ? 3.5f : 2.5f;
                DrawAccentTrack(D2D1::RectF(barLeft, scrubberY - barHalf, barRight, scrubberY + barHalf),
                                progress, barHalf);

                const D2D1_COLOR_F scrubColor =
                    (currentAccent_.a > 0.0f) ? currentAccent_ : D2D1::ColorF(0x4cc9f0);
                const float scrubW = (barRight - barLeft) * progress;
                const float thumbX = barLeft + scrubW;
                const float thumbR = isDraggingThisBar ? 6.5f : 4.5f;

                // Halo first so the thumb sits on top of it.
                if (isDraggingThisBar) {
                    ComPtr<ID2D1SolidColorBrush> thumbHalo;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(scrubColor, 0.22f), &thumbHalo)) &&
                        thumbHalo) {
                        target_->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR * 2.2f, thumbR * 2.2f),
                            thumbHalo.Get());
                    }
                }

                // A white thumb with an accent ring reads more precisely against
                // album art than a solid accent dot.
                ComPtr<ID2D1SolidColorBrush> thumbFill;
                if (SUCCEEDED(target_->CreateSolidColorBrush(
                        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), &thumbFill)) && thumbFill) {
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR, thumbR),
                                         thumbFill.Get());
                }
                ComPtr<ID2D1SolidColorBrush> thumbRing;
                if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(scrubColor, 0.85f), &thumbRing)) &&
                    thumbRing) {
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR, thumbR),
                                         thumbRing.Get(), 1.6f);
                }
                }  // end Slim style

                EndSwapGroup(timelineGrouped);

                // Controls. Positions come from MediaLayout so the hit test in
                // OverlayWndProc stays in lockstep with what is drawn here.
                const float cy = rect.top + MediaLayout::kControlsY;
                const float cx = (rect.left + rect.right) * 0.5f;
                DrawMediaControls(state.media.playing,
                                  D2D1::Point2F(cx - MediaLayout::kControlSpacing, cy),
                                  D2D1::Point2F(cx, cy),
                                  D2D1::Point2F(cx + MediaLayout::kControlSpacing, cy),
                                  now);
                DrawSourceDock(state, rect, now);  // stays still during a switch
                if (settings.mediaTabLyrics) {
                    DrawImmersiveLyricsButton(rect, immersiveMix);
                }
                target_->SetTransform(playerZoneOldTransform);
                if (sideLyricsExtra > 1.0f) {
                    DrawMediaSideLyrics(state,
                                        D2D1::RectF(rect.right, fullIslandRect.top,
                                                    fullIslandRect.right, fullIslandRect.bottom),
                                        settings, now);
                }
            } else if (activeTabId == 1) {
                SYSTEMTIME local = {}; GetLocalTime(&local);
                DrawCalendarDashboard(state, rect, settings, now, 1.0f, local);
            } else if (activeTabId == 2) {
                bool hasWeather = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
                std::wstring wIcon = L"🌡️"; std::wstring wText = Loc(L"Loading...");
                if (hasWeather) {
                    wText = state.weather.weatherDesc;
                    GetWeatherIconAndText(state.weather.weatherCode, wIcon, wText);
                }
                DrawWeatherDashboard(state, rect, settings, now, 1.0f, hasWeather, wIcon, wText);
            } else if (activeTabId == 6) {
                DrawGitHubDashboard(state, rect, settings, 1.0f);
            } else if (activeTabId == 3) {
                DrawHardwareMonitorDashboard(state, rect, settings, 1.0f);
            } else if (activeTabId == 4) {
                DrawFileTrayDashboard(state, rect, settings, 1.0f);
            } else if (activeTabId == 7) {
                DrawNotificationCenterDashboard(state, rect, 1.0f);
            }
            };  // end drawTabContent

            DrawTabTransition(0, tabIdx, activeTabs, rect, now, drawTabContent);
            DrawTabPagination(rect, maxTabs);

            target_->PopLayer();
        }

        // Collapsed UI
        if (collapsedAlpha > 0.01f && mask && layer && !g_trayDragOver.load(std::memory_order_relaxed)) {
            g_idleTab = 0;
            ResetTabTransition();
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), collapsedAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE), layer);;

            const float cy = (rect.top + rect.bottom) * 0.5f;

            // Optional clock section on the left. Zero wide when the option is
            // off, so the cover sits exactly where it always did.
            MediaPillMetrics pillMetrics;
            if (settings.mediaPillClock && idleTextFormat_) {
                pillMetrics = MeasureMediaPill(state, settings);
            }
            const float clockSection = pillMetrics.sectionWidth;

            // The cover itself is drawn by the shared art pass at the end of
            // DrawMedia; this rect is only used to place the lyrics next to it.
            const D2D1_RECT_F artRect = collapsedArtRect;

            float shiftX = 0.0f;
            if (state.system.micActive || state.system.cameraActive) {
                shiftX = 22.0f;
            }

            D2D1_RECT_F waveRect = D2D1::RectF(rect.right - 42.0f - shiftX, cy - 10.0f,
                                               rect.right - 14.0f - shiftX, cy + 10.0f);
            if (settings.spectrumStyle == SpectrumStyle::Bars) {
                DrawSpectrum(state, waveRect, settings, now, state.media.playing);
            } else if (state.media.playing) {
                DrawSpectrum(state, waveRect, settings, now);
            } else {
                const float gap = 2.5f;
                const float availableW = waveRect.right - waveRect.left;
                const int count = std::max(1, static_cast<int>((availableW + gap) / (2.0f + gap)));
                const float barWidth = (availableW - gap * (count - 1)) / count;
                mutedBrush_->SetOpacity(0.5f);
                for (int i = 0; i < count; ++i) {
                    const float dotX = waveRect.left + i * (barWidth + gap) + barWidth * 0.5f;
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, cy), 1.2f, 1.2f), mutedBrush_.Get());
                }
            }

            // Lyrics between the album art and the visualiser. Fades in as the
            // pill widens, so there is no pop while the spring is still moving.
            if (CollapsedLyricsActive(state, settings)) {
                const float lyricsAlpha =
                    SmoothStep01(((rect.right - rect.left) - 175.0f) / 60.0f);
                if (lyricsAlpha > 0.01f) {
                    const float textLeft = artRect.right + 10.0f;
                    const float textRight = waveRect.left - 10.0f;
                    DrawCollapsedLyrics(state,
                                        D2D1::RectF(textLeft, rect.top + 4.0f, textRight, rect.bottom - 4.0f),
                                        now, lyricsAlpha);
                }
            }

            // Clock + divider on the left, same face, size and time format as the
            // idle strip. On hover the collapsed layer fades out and the normal
            // player shows.
            if (clockSection > 0.0f) {
                SYSTEMTIME local = {};
                GetLocalTime(&local);
                const std::wstring clockText = FormatIslandTime(local, settings.clockFollowSystem,
                                                                settings.use24HourClock,
                                                                settings.showSeconds);
                const float clockLeft = rect.left + MediaPillLayout::kClockPadLeft;
                textBrush_->SetOpacity(0.96f);
                target_->DrawTextW(clockText.c_str(), static_cast<UINT32>(clockText.size()),
                                   idleTextFormat_.Get(),
                                   D2D1::RectF(clockLeft, rect.top, clockLeft + pillMetrics.clockWidth, rect.bottom),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textBrush_->SetOpacity(1.0f);

                ComPtr<ID2D1SolidColorBrush> divider;
                target_->CreateSolidColorBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_), &divider);
                if (divider) {
                    const float divX = clockLeft + pillMetrics.clockWidth + MediaPillLayout::kSlotGap;
                    const float divW = MediaPillLayout::kDividerWidth;
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(divX, rect.top + MediaPillLayout::kDividerInsetY,
                                                      divX + divW, rect.bottom - MediaPillLayout::kDividerInsetY),
                                          divW * 0.5f, divW * 0.5f),
                        divider.Get());
                }
            }

            target_->PopLayer();
        }

        // ── Shared album art pass ────────────────────────────────────────────
        // Drawn once, outside both cross-fading layers, at full opacity, so the cover
        // morphs smoothly in position, size and corner radius while the island
        // resizes. Once fully expanded, the expanded layer takes over (flip animation,
        // source badge). Alpha is eased so switching tabs can't make it pop in.
        {
            const bool trayDrag = g_trayDragOver.load(std::memory_order_relaxed);
            const bool onMediaTab = NormalizedTabIndex(settings) == 0;
            const float targetAlpha = trayDrag ? 0.0f : (onMediaTab ? 1.0f : collapsedAlpha);

            const bool recent = artOuterLast_ >= 0.0 && (now - artOuterLast_) < 0.25;
            const float dtArt = recent ? Clamp(static_cast<float>(now - artOuterLast_), 0.001f, 0.05f) : 0.016f;
            artOuterLast_ = now;
            // Off the Media tab the art has no business following a slow ease; it
            // fades exactly with the collapsed pill's contents.
            if (!recent || artMorph >= 1.0f || !onMediaTab) {
                artOuterAlpha_ = targetAlpha;
            } else {
                artOuterAlpha_ += (targetAlpha - artOuterAlpha_) * (1.0f - std::exp(-dtArt / 0.06f));
            }

            // While the island is hiding, the art follows the pill: it shrinks
            // toward the pill's centre and fades, instead of staying 32px and
            // sticking out until the window stops painting.
            float hideS = 1.0f;
            if (islandHiding_) {
                const float needW = pillClockSection + kCollapsedArtPad * 2.0f + kCollapsedArtSize;
                const float curW = rect.right - rect.left;
                hideS = SmoothStep01(std::min(height / kCollapsedPillHeight, curW / needW));
            }
            // On the Media tab the cover morphs circle -> square. On any other tab it
            // stays a small circle on the collapsed pill and never flies into the
            // expanded layout, so it can't show up on top of the lyrics.
            const D2D1_RECT_F artBaseRect = onMediaTab ? morphArtRect : collapsedArtRect;
            const float artBaseRadius = onMediaTab ? morphArtRadius : kCollapsedArtSize * 0.5f;

            D2D1_RECT_F artDrawRect = artBaseRect;
            float artDrawRadius = artBaseRadius;
            if (hideS < 1.0f) {
                const float pcx = (rect.left + rect.right) * 0.5f;
                const float pcy = (rect.top + rect.bottom) * 0.5f;
                const float acx = (artBaseRect.left + artBaseRect.right) * 0.5f;
                const float acy = (artBaseRect.top + artBaseRect.bottom) * 0.5f;
                const float ncx = pcx + (acx - pcx) * hideS;
                const float ncy = pcy + (acy - pcy) * hideS;
                const float hw = (artBaseRect.right - artBaseRect.left) * 0.5f * hideS;
                const float hh = (artBaseRect.bottom - artBaseRect.top) * 0.5f * hideS;
                artDrawRect = D2D1::RectF(ncx - hw, ncy - hh, ncx + hw, ncy + hh);
                artDrawRadius = artBaseRadius * hideS;
            }
            const float artAlphaEff = artOuterAlpha_ * hideS;

            if (artMorph < 1.0f && artAlphaEff > 0.01f) {
                ID2D1Layer* artLayer = (artAlphaEff < 0.99f) ? AcquireLayer() : nullptr;
                if (artLayer) {
                    target_->PushLayer(
                        D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                              Clamp(artAlphaEff, 0.0f, 1.0f), nullptr,
                                              D2D1_LAYER_OPTIONS_NONE),
                        artLayer);
                }
                DrawAlbumArtDrop(state.media, artDrawRect, now, artDrawRadius);
                if (artLayer) {
                    target_->PopLayer();
                }
            }
        }
    }

    // ═══ Immersive lyrics (Modules.MediaTabLyrics) ═══════════════════════════
    // Cross-fades the 380x184 media card into a lyrics-focused view: small cover
    // circle, one-line header, spring-scrolled lyric lines, accent glow behind.
    // Scrubber, controls and source dock are untouched.

    // Eased 0..1 mix between normal player and lyrics view. Always 0 when the option
    // is off, and the view resets whenever the island collapses.
    float UpdateImmersiveMix(const Settings& settings, float height, double now) {
        // Option off: the feature is fully inert, and the saved choice is cleared.
        if (!settings.mediaTabLyrics) {
            if (g_immersiveLyrics.load(std::memory_order_relaxed)) {
                g_immersiveLyrics.store(false, std::memory_order_relaxed);
            }
            immersiveMixLin_ = 0.0f;
            immersiveMixLast_ = -1.0;
            return 0.0f;
        }

        const float target = g_immersiveLyrics.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        // Collapsed: keep the choice, and pre-set the mix so the next expand opens
        // straight into the same view (no fade, no flash of the normal player).
        if (height < 52.0f) {
            immersiveMixLin_ = target;
            immersiveMixLast_ = -1.0;
            return SmoothStep01(target);
        }

        const bool fresh = immersiveMixLast_ >= 0.0 && (now - immersiveMixLast_) < 0.25;
        if (!fresh) {
            immersiveMixLin_ = target;
        } else {
            const float dt = Clamp(static_cast<float>(now - immersiveMixLast_), 0.001f, 0.05f);
            const float step = dt / static_cast<float>(ImmersiveLayout::kFadeSec);
            if (target > immersiveMixLin_) {
                immersiveMixLin_ = std::min(target, immersiveMixLin_ + step);
            } else {
                immersiveMixLin_ = std::max(target, immersiveMixLin_ - step);
            }
            if (immersiveMixLin_ != target) {
                g_layoutDirty = true;  // keep frames coming until it settles
            }
        }
        immersiveMixLast_ = now;
        return SmoothStep01(immersiveMixLin_);
    }

    bool BeginImmersiveFade(float alpha) {
        if (alpha >= 0.999f) return false;
        ID2D1Layer* layer = AcquireLayer();
        if (!layer) return false;
        target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), Clamp(alpha, 0.0f, 1.0f),
                                                  nullptr, D2D1_LAYER_OPTIONS_NONE),
                           layer);
        return true;
    }

    void EndImmersiveFade(bool began) {
        if (began) target_->PopLayer();
    }

    // Speech-bubble button. Lights up in the accent colour while the lyrics view is open.
    void DrawImmersiveLyricsButton(D2D1_RECT_F rect, float mix) {
        if (!EnsureRoundJoinStyle()) return;
        const float r = ImmersiveLayout::kBtnRadius;
        const D2D1_POINT_2F c = D2D1::Point2F(rect.right - ImmersiveLayout::kBtnInsetRight - r,
                                              rect.top + MediaLayout::kControlsY);
        const bool hovered = g_hoveredLyricsBtn.load(std::memory_order_relaxed);

        const D2D1_COLOR_F restBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered ? 0.09f : 0.040f);
        const D2D1_COLOR_F onBg = D2D1::ColorF(currentAccent_.r, currentAccent_.g, currentAccent_.b,
                                               hovered ? 0.32f : 0.24f);
        if (ID2D1SolidColorBrush* bg = AcquireBrush(MixColor(restBg, onBg, mix))) {
            target_->FillEllipse(D2D1::Ellipse(c, r, r), bg);
        }

        const float restOpacity = hovered ? 0.92f : 0.62f;
        const float opacity = Clamp(restOpacity + (1.0f - restOpacity) * mix, 0.0f, 1.0f);
        const float sw = 1.4f;
        ID2D1StrokeStyle* st = roundJoinStyle_.Get();
        const D2D1_RECT_F body = D2D1::RectF(c.x - 7.0f, c.y - 7.2f, c.x + 7.0f, c.y + 3.8f);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(body, 3.0f, 3.0f);

        if (mix > 0.01f) {
            accentBrush_->SetOpacity(0.30f * mix);
            target_->FillRoundedRectangle(rr, accentBrush_.Get());
        }
        accentBrush_->SetOpacity(opacity);
        target_->DrawRoundedRectangle(rr, accentBrush_.Get(), sw, st);
        // Tail
        target_->DrawLine(D2D1::Point2F(c.x - 4.5f, body.bottom), D2D1::Point2F(c.x - 4.5f, c.y + 7.2f),
                          accentBrush_.Get(), sw, st);
        target_->DrawLine(D2D1::Point2F(c.x - 4.5f, c.y + 7.2f), D2D1::Point2F(c.x - 0.8f, body.bottom),
                          accentBrush_.Get(), sw, st);
        // Text lines
        target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 3.4f), D2D1::Point2F(c.x + 3.8f, c.y - 3.4f),
                          accentBrush_.Get(), 1.2f, st);
        target_->DrawLine(D2D1::Point2F(c.x - 3.8f, c.y - 0.2f), D2D1::Point2F(c.x + 1.4f, c.y - 0.2f),
                          accentBrush_.Get(), 1.2f, st);
        accentBrush_->SetOpacity(1.0f);
    }

    void EnsureImmersiveFormat(const Settings& settings) {
        const float ts = Clamp(settings.textScale, 0.7f, 1.6f);
        if (immFormat_ && std::fabs(immFmtScale_ - ts) < 0.001f && immFmtFamily_ == settings.fontFamily) {
            return;
        }
        immFormat_.Reset();
        immCache_.clear();
        const wchar_t* family =
            settings.fontFamily.empty() ? L"Segoe UI Variable Display" : settings.fontFamily.c_str();
        HRESULT hr = dwriteFactory_->CreateTextFormat(
            family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f * ts, L"", &immFormat_);
        if (FAILED(hr) || !immFormat_) {
            immFormat_.Reset();
            dwriteFactory_->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 15.0f * ts, L"", &immFormat_);
        }
        if (immFormat_) {
            immFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            immFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        immFmtScale_ = ts;
        immFmtFamily_ = settings.fontFamily;
    }

    void DrawImmersiveLyricsView(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                                 double now, float mix) {
        if (mix <= 0.003f || !textFormat_ || !smallTextFormat_ || !dwriteFactory_) {
            return;
        }

        const float width = rect.right - rect.left;
        const float cx = (rect.left + rect.right) * 0.5f;
        const float zoneTop = rect.top + ImmersiveLayout::kLyricsTop;
        const float zoneBottom = rect.top + ImmersiveLayout::kLyricsBottom;
        const float cy = (zoneTop + zoneBottom) * 0.5f;
        const float availW = width - ImmersiveLayout::kPadX * 2.0f;

        const bool fresh = immLastDrawn_ >= 0.0 && (now - immLastDrawn_) < 0.25;
        const float dt = fresh ? Clamp(static_cast<float>(now - immLastDrawn_), 0.001f, 0.05f) : 0.016f;
        immLastDrawn_ = now;
        g_lastLyricsViewTime.store(now, std::memory_order_relaxed);

        ID2D1Layer* fadeLayer = AcquireLayer();
        if (fadeLayer) {
            target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), Clamp(mix, 0.0f, 1.0f),
                                                      nullptr, D2D1_LAYER_OPTIONS_NONE),
                               fadeLayer);
        }

        // ── Accent glow behind the lyrics, breathing gently with the audio level ──
        {
            float level = 0.0f;
            if (settings.spectrumStyle == SpectrumStyle::Bars) {
                level = state.media.playing ? PlayingIndicatorGlow(now) : 0.0f;
            } else if (state.media.playing) {
                const size_t n = state.waveform.size();
                level = Clamp(state.waveform[(state.waveformWrite + n - 1) % n], 0.0f, 1.0f);
            }
            immGlow_ += (level - immGlow_) * (1.0f - std::exp(-dt / 0.12f));

            const D2D1_COLOR_F a = material_.accent;
            const D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(a.r, a.g, a.b, 1.0f)},
                {1.0f, D2D1::ColorF(a.r, a.g, a.b, 0.0f)},
            };
            const D2D1_POINT_2F gc = D2D1::Point2F(cx, cy + 6.0f);
            const float rx = width * 0.46f;
            const float ry = 66.0f;
            if (ID2D1RadialGradientBrush* glow = UseRadialGradient(bloomGrad_, stops, 2, gc, rx, ry)) {
                const float strength = (0.10f + 0.10f * immGlow_) * Clamp(settings.accentBloom, 0.0f, 2.0f);
                glow->SetOpacity(Clamp(strength, 0.0f, 0.8f) * settingsOpacity_);
                target_->FillEllipse(D2D1::Ellipse(gc, rx, ry), glow);
            }
        }

        // ── One-line header: "Title · Artist" next to the small cover circle ──
        {
            const float textLeft = rect.left + ImmersiveLayout::kHeaderTextLeft;
            const float textRight = rect.right - ImmersiveLayout::kHeaderRightPad;
            const float midY = rect.top + ImmersiveLayout::kArtTop + ImmersiveLayout::kArtSize * 0.5f;
            std::wstring head = state.media.title.empty() ? std::wstring(Loc(L"Unknown")) : state.media.title;
            if (!state.media.artist.empty()) {
                head += L"  \u00b7  " + state.media.artist;
            }
            textBrush_->SetOpacity(0.70f);
            DrawMarqueeText(head, D2D1::RectF(textLeft, midY - 8.0f, textRight, midY + 8.0f),
                            smallTextFormat_.Get(), textBrush_.Get(), now, 30.0f,
                            marqueeImmersiveCache_);
        }

        // ── Lyrics ──
        target_->PushAxisAlignedClip(D2D1::RectF(rect.left + 18.0f, zoneTop, rect.right - 18.0f, zoneBottom),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const auto& lines = state.lyrics.lines;
        const bool usable =
            state.lyrics.hasData && state.lyrics.synced && !lines.empty() &&
            state.lyrics.matchedTitle == state.media.title &&
            state.lyrics.matchedArtist == state.media.artist;

        if (!usable) {
            immCache_.clear();
            immTrackKey_.clear();
            const wchar_t* msg = state.lyrics.fetching ? Loc(L"Fetching lyrics...")
                                 : (state.lyrics.hasData ? Loc(L"No synced lyrics")
                                                         : Loc(L"No lyrics found"));
            const float opacity = state.lyrics.fetching
                ? 0.50f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6))
                : 0.60f;
            DWRITE_TEXT_METRICS tm{};
            if (auto layout = MakeLineLayout(msg, textFormat_.Get(), availW, &tm, true)) {
                mutedBrush_->SetOpacity(opacity);
                target_->DrawTextLayout(D2D1::Point2F(cx - tm.width * 0.5f, cy - tm.height * 0.5f),
                                        layout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        } else {
            EnsureImmersiveFormat(settings);

            // Same position math as the Lyrics tab, including the sync offset.
            double posSec = state.media.positionTicks / 10000000.0;
            if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                posSec += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
            }
            if (state.media.playing) {
                posSec += LyricsLookaheadSec();
            }
            const int64_t posMs = std::max<int64_t>(0, static_cast<int64_t>(posSec * 1000.0));

            int lo = 0;
            int hi = static_cast<int>(lines.size());
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (lines[mid].timeMs <= posMs) lo = mid + 1; else hi = mid;
            }
            const int idx = lo - 1;  // -1 = intro, before the first line

            // Spring-eased scroll toward the active line.
            const std::wstring trackKey = state.lyrics.matchedArtist + L"|" + state.lyrics.matchedTitle;
            if (!fresh || trackKey != immTrackKey_) {
                immTrackKey_ = trackKey;
                immCache_.clear();
                immScroll_ = static_cast<float>(idx);
                immVel_ = 0.0f;
            }
            const float target = static_cast<float>(idx);
            if (std::fabs(immScroll_ - target) > 8.0f) {  // big seek: don't scroll through every line
                immScroll_ = target;
                immVel_ = 0.0f;
            }
            float remaining = dt;
            while (remaining > 0.0f) {
                const float h = std::min(remaining, 0.004f);
                immVel_ += (-ImmersiveLayout::kSpringStiffness * (immScroll_ - target) -
                            ImmersiveLayout::kSpringDamping * immVel_) * h;
                immScroll_ += immVel_ * h;
                remaining -= h;
            }
            if (std::fabs(immScroll_ - target) < 0.0015f && std::fabs(immVel_) < 0.01f) {
                immScroll_ = target;
                immVel_ = 0.0f;
            }

            auto lineFor = [&](int i) -> CollapsedLyricLine& {
                auto it = immCache_.find(i);
                if (it == immCache_.end()) {
                    if (immCache_.size() > 16) immCache_.clear();
                    CollapsedLyricLine built;
                    const std::wstring text =
                        (i >= 0 && i < static_cast<int>(lines.size()) && !lines[i].text.empty())
                            ? lines[i].text : std::wstring(L"\u266A");
                    BuildCollapsedLyricLine(built, text, immFormat_.Get());
                    it = immCache_.emplace(i, std::move(built)).first;
                }
                return it->second;
            };

            const float neighbour = settings.lyricsNeighborLinesVisible ? 0.62f : 0.40f;
            auto alphaFor = [&](float ad) {
                if (ad <= 1.0f) return 1.0f + (neighbour - 1.0f) * SmoothStep01(ad);
                // Past the neighbours: fade fully out instead of leaving ghost lines
                // crammed against the zone edge.
                return neighbour * (1.0f - SmoothStep01((ad - 1.0f) / 0.45f));
            };
            auto offsetFor = [&](float d) {
                const float ad = std::fabs(d);
                const float y = ad <= 1.0f
                    ? ImmersiveLayout::kLinePitch * ad
                    : ImmersiveLayout::kLinePitch + ImmersiveLayout::kOuterPitch * (ad - 1.0f);
                return d < 0.0f ? -y : y;
            };

            const D2D1_COLOR_F tc = textBrush_->GetColor();
            const D2D1_GRADIENT_STOP wipeStops[2] = {
                {0.0f, D2D1::ColorF(tc.r, tc.g, tc.b, 1.0f)},
                {1.0f, D2D1::ColorF(tc.r, tc.g, tc.b, 0.0f)},
            };

            D2D1_MATRIX_3X2_F oldTransform;
            target_->GetTransform(&oldTransform);

            const int first = static_cast<int>(std::floor(immScroll_)) - 3;
            for (int i = first; i <= first + 7; ++i) {
                if (i < -1 || i >= static_cast<int>(lines.size())) continue;
                const float d = static_cast<float>(i) - immScroll_;
                const float ad = std::fabs(d);
                if (ad > 1.6f) continue;
                const float alpha = alphaFor(ad);
                if (alpha <= 0.01f) continue;

                CollapsedLyricLine& L = lineFor(i);
                if (!L.layout || L.width <= 0.0f) continue;

                const float focus = 1.0f - SmoothStep01(Clamp(ad, 0.0f, 1.0f));
                float s = 0.80f + 0.28f * focus;
                if (L.width * s > availW) {
                    s = std::max(availW / L.width, s * 0.62f);
                }
                const float yc = cy + offsetFor(d);
                const D2D1_POINT_2F origin = D2D1::Point2F(cx - L.width * 0.5f, yc - L.height * 0.5f);

                target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(cx, yc)) * oldTransform);

                if (settings.karaokeLyrics && i == idx && idx >= 0) {
                    // Dim base, then a soft-edged bright wipe following the song.
                    mutedBrush_->SetOpacity(alpha * (0.62f + 0.38f * (1.0f - focus)));
                    target_->DrawTextLayout(origin, L.layout.Get(), mutedBrush_.Get(),
                                            D2D1_DRAW_TEXT_OPTIONS_NONE);

                    int64_t startMs = lines[i].timeMs;
                    int64_t endMs = startMs + 4000;
                    if (i + 1 < static_cast<int>(lines.size())) {
                        endMs = lines[i + 1].timeMs;
                    } else if (state.media.endTicks > 0) {
                        endMs = state.media.endTicks / 10000;
                    }
                    const int64_t span = endMs - startMs;
                    const float p = span > 0
                        ? Clamp(static_cast<float>(posMs - startMs) / static_cast<float>(span), 0.0f, 1.0f)
                        : 1.0f;
                    const float wipeX = origin.x - 8.0f + (L.width + 16.0f) * p;
                    if (focus > 0.01f) {
                        if (ID2D1LinearGradientBrush* wipe = UseLinearGradient(
                                immKaraokeGrad_, wipeStops, 2,
                                D2D1::Point2F(wipeX - 8.0f, yc), D2D1::Point2F(wipeX + 8.0f, yc))) {
                            wipe->SetOpacity(alpha * focus);
                            target_->DrawTextLayout(origin, L.layout.Get(), wipe,
                                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
                        }
                    }
                } else {
                    if (focus < 0.999f) {
                        mutedBrush_->SetOpacity(alpha * (1.0f - focus));
                        target_->DrawTextLayout(origin, L.layout.Get(), mutedBrush_.Get(),
                                                D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                    if (focus > 0.001f) {
                        textBrush_->SetOpacity(alpha * focus);
                        target_->DrawTextLayout(origin, L.layout.Get(), textBrush_.Get(),
                                                D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                }
            }
            target_->SetTransform(oldTransform);
        }

        target_->PopAxisAlignedClip();
        if (fadeLayer) {
            target_->PopLayer();
        }
        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Builds a wrapped lyric line for the media-tab lyrics panel, shrinking the font
    // until it fits maxHeight (down to about 10px).
    void BuildSideLyricLine(CollapsedLyricLine& out, const std::wstring& text, float width,
                            float maxHeight, const Settings& settings) {
        out.text = text;
        out.layout.Reset();
        out.width = 0.0f;
        out.height = 0.0f;
        out.isTitle = false;
        if (!dwriteFactory_ || text.empty()) {
            return;
        }

        const wchar_t* family =
            settings.fontFamily.empty() ? L"Segoe UI Variable Display" : settings.fontFamily.c_str();
        const float ts = Clamp(settings.textScale, 0.7f, 1.6f);
        const float minSize = 10.0f * ts;
        float size = 16.0f * ts;

        for (int attempt = 0; attempt < 8; ++attempt) {
            ComPtr<IDWriteTextFormat> fmt;
            HRESULT hr = dwriteFactory_->CreateTextFormat(
                family, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
            if (FAILED(hr) || !fmt) {
                fmt.Reset();
                hr = dwriteFactory_->CreateTextFormat(
                    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
                if (FAILED(hr) || !fmt) {
                    return;
                }
            }
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                        fmt.Get(), width, 4096.0f, &layout)) ||
                !layout) {
                return;
            }
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            out.layout = layout;
            out.width = tm.width;
            out.height = tm.height;

            if (tm.height <= maxHeight || size <= minSize) {
                break;
            }
            size = std::max(minSize, size - 1.5f * ts);
        }
    }

    // Lyrics side panel for the expanded media tab. `area` is the strip to the right of
    // the normal player. Previous / next lines are dimmed, the current line is large and
    // wrapped, and it slides in when the line changes.
    void DrawMediaSideLyrics(const SharedState& state, D2D1_RECT_F area, const Settings& settings,
                             double now) {
        const float areaW = area.right - area.left;
        if (areaW < 8.0f || !textFormat_ || !smallTextFormat_ || !dwriteFactory_) {
            return;
        }

        // Fades in as the island widens, so nothing pops while the spring is moving.
        const float reveal = SmoothStep01((areaW - 6.0f) / (MediaLayout::kSideLyricsWidth * 0.55f));
        if (reveal <= 0.01f) {
            return;
        }

        constexpr float kPadX = 18.0f;
        constexpr float kSlidePx = 12.0f;
        constexpr double kAnimSec = 0.30;
        const float textLeft = area.left + kPadX;
        const float textW = MediaLayout::kSideLyricsWidth - kPadX * 2.0f;

        const bool fresh = sideLyricLastDrawn_ >= 0.0 && (now - sideLyricLastDrawn_) < 0.25;
        sideLyricLastDrawn_ = now;

        ID2D1Layer* fadeLayer = AcquireLayer();
        if (fadeLayer) {
            target_->PushLayer(D2D1::LayerParameters(area, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), reveal, nullptr,
                                                      D2D1_LAYER_OPTIONS_NONE),
                               fadeLayer);
        } else {
            target_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        // Thin separator between the player and the lyrics.
        if (ID2D1SolidColorBrush* divider =
                AcquireBrush(WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_))) {
            target_->FillRectangle(
                D2D1::RectF(area.left, area.top + 24.0f, area.left + 1.0f, area.bottom - 24.0f),
                divider);
        }

        const auto& lines = state.lyrics.lines;
        const bool usable =
            state.lyrics.hasData && state.lyrics.synced && !lines.empty() &&
            state.lyrics.matchedTitle == state.media.title &&
            state.lyrics.matchedArtist == state.media.artist;

        if (!usable) {
            sideLyricIdx_ = -2;
            sideLyricCur_ = CollapsedLyricLine{};
            sideLyricPrev_ = CollapsedLyricLine{};

            const wchar_t* msg =
                state.lyrics.fetching ? Loc(L"Fetching lyrics...") : Loc(L"No lyrics found");
            float opacity = 0.60f;
            if (state.lyrics.fetching) {
                opacity = 0.50f + 0.25f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 0.6));
            }
            DWRITE_TEXT_METRICS tm{};
            if (auto layout = MakeLineLayout(msg, textFormat_.Get(), textW, &tm, true)) {
                mutedBrush_->SetOpacity(opacity);
                target_->DrawTextLayout(
                    D2D1::Point2F(textLeft, (area.top + area.bottom) * 0.5f - tm.height * 0.5f),
                    layout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        } else {
            // Same position math as the Lyrics tab, including the lookahead nudge.
            double posSec = state.media.positionTicks / 10000000.0;
            if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                posSec += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
            }
            if (state.media.playing) {
                posSec += LyricsLookaheadSec();
                            }
                            const int64_t posMs = std::max<int64_t>(0, static_cast<int64_t>(posSec * 1000.0));

            int lo = 0;
            int hi = static_cast<int>(lines.size());
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (lines[mid].timeMs <= posMs) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            const int idx = lo - 1;  // -1 = before the first line

            const float midTop = area.top + 46.0f;
            const float midH = std::max(20.0f, (area.bottom - 46.0f) - midTop);

            const bool styleChanged = std::fabs(sideLyricScale_ - settings.textScale) > 0.001f ||
                                      sideLyricFamily_ != settings.fontFamily;
            const bool trackChanged = state.media.title != sideLyricTitle_ ||
                                      state.media.artist != sideLyricArtist_;

            if (idx != sideLyricIdx_ || styleChanged || trackChanged || !sideLyricCur_.layout) {
                if (styleChanged || trackChanged || !fresh || sideLyricIdx_ == -2) {
                    sideLyricPrev_ = CollapsedLyricLine{};
                    sideLyricAnimStart_ = now - 10.0;  // snap, no animation
                } else {
                    sideLyricPrev_ = std::move(sideLyricCur_);
                    sideLyricAnimStart_ = now;
                }
                sideLyricIdx_ = idx;
                sideLyricTitle_ = state.media.title;
                sideLyricArtist_ = state.media.artist;
                sideLyricScale_ = settings.textScale;
                sideLyricFamily_ = settings.fontFamily;

                const std::wstring text = (idx >= 0 && !lines[idx].text.empty())
                                              ? lines[idx].text
                                              : std::wstring(L"\u266A");
                BuildSideLyricLine(sideLyricCur_, text, textW, midH, settings);
            }

            // Ease-out-cubic transition.
            const float t = Clamp(static_cast<float>((now - sideLyricAnimStart_) / kAnimSec), 0.0f, 1.0f);
            if (t < 1.0f) {
                g_layoutDirty = true;  // keep frames coming until it settles
            }
            const float inv = 1.0f - t;
            const float e = 1.0f - inv * inv * inv;

            auto drawDim = [&](int lineIdx, float centerY) {
                if (lineIdx < 0 || lineIdx >= static_cast<int>(lines.size())) {
                    return;
                }
                const std::wstring& s = lines[lineIdx].text;
                if (s.empty()) {
                    return;
                }
                DWRITE_TEXT_METRICS tm{};
                if (auto layout = MakeLineLayout(s, smallTextFormat_.Get(), textW, &tm, true)) {
                    mutedBrush_->SetOpacity(0.50f * e);
                    target_->DrawTextLayout(D2D1::Point2F(textLeft, centerY - tm.height * 0.5f),
                                            layout.Get(), mutedBrush_.Get(),
                                            D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
            };
            drawDim(idx - 1, area.top + 26.0f);
            drawDim(idx + 1, area.bottom - 26.0f);

            target_->PushAxisAlignedClip(
                D2D1::RectF(area.left, area.top + 40.0f, area.right, area.bottom - 40.0f),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            // Outgoing line rises and fades slightly faster than the new one arrives.
            if (t < 1.0f && sideLyricPrev_.layout) {
                const CollapsedLyricLine& p = sideLyricPrev_;
                const float y = midTop + std::max(0.0f, (midH - p.height) * 0.5f) - e * kSlidePx;
                textBrush_->SetOpacity(0.90f * (1.0f - Clamp(t * 1.6f, 0.0f, 1.0f)));
                target_->DrawTextLayout(D2D1::Point2F(textLeft, y), p.layout.Get(), textBrush_.Get(),
                                        D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            if (sideLyricCur_.layout) {
                const CollapsedLyricLine& c = sideLyricCur_;
                const float y = midTop + std::max(0.0f, (midH - c.height) * 0.5f) + (1.0f - e) * kSlidePx;
                textBrush_->SetOpacity(0.97f * e);
                target_->DrawTextLayout(D2D1::Point2F(textLeft, y), c.layout.Get(), textBrush_.Get(),
                                        D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            target_->PopAxisAlignedClip();
        }

        if (fadeLayer) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Builds the cached layout for one lyric line.
    void BuildCollapsedLyricLine(CollapsedLyricLine& out, const std::wstring& text,
                                 IDWriteTextFormat* fmt) {
        out.text = text;
        out.layout.Reset();
        out.width = 0.0f;
        out.height = 0.0f;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                       fmt, 4096.0f, 40.0f, &out.layout)) &&
            out.layout) {
            // Other draw code flips the shared format's alignment temporarily;
            // pin it here so positioning below never depends on that.
            out.layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            out.layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TEXT_METRICS tm{};
            out.layout->GetMetrics(&tm);
            out.width = tm.widthIncludingTrailingWhitespace;
            out.height = tm.height;
        }
    }

    // Draws the current synced lyric line inside `area`. When the line changes,
    // the old one slides up and fades out while the new one rises in from below.
    void DrawCollapsedLyrics(const SharedState& state, D2D1_RECT_F area, double now, float alpha) {
        g_lastLyricsViewTime.store(now, std::memory_order_relaxed);
        IDWriteTextFormat* fmt = textFormat_.Get();
        if (!fmt || !dwriteFactory_ || alpha <= 0.01f) {
            return;
        }
        const float availW = area.right - area.left;
        if (availW < 40.0f) {
            return;
        }

        const auto& lines = state.lyrics.lines;

        // Lyrics only count if they belong to the track that is playing right now.
        // This also stops the previous song's lyrics showing while the lyrics
        // thread has not noticed the track change yet.
        const bool lyricsUsable =
            state.lyrics.hasData && state.lyrics.synced && !lines.empty() &&
            state.lyrics.matchedTitle == state.media.title &&
            state.lyrics.matchedArtist == state.media.artist;

        int idx = -1;  // -1 = before the first line, or no usable lyrics
        int64_t posMs = 0;
        if (lyricsUsable) {
            // Same position math as the Lyrics tab, including the lookahead nudge.
            double posSec = state.media.positionTicks / 10000000.0;
            if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                posSec += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
            }
            if (state.media.playing) {
                posSec += LyricsLookaheadSec();
                            }
                            posMs = std::max<int64_t>(0, static_cast<int64_t>(posSec * 1000.0));

            // Binary search for the last line whose timestamp has passed.
            int lo = 0;
            int hi = static_cast<int>(lines.size());
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (lines[mid].timeMs <= posMs) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            idx = lo - 1;
        }

        // The media title is only a fallback for when there are no usable lyrics
        // (still loading, not found, unsynced). Once lyrics are usable, the intro
        // and instrumental gaps show the note glyph, as before.
        constexpr int kTitleKey = -3;  // distinct from idx == -1 (before the first line)
        const bool showTitle = !lyricsUsable;
        const int showIdx = showTitle ? kTitleKey : idx;

        std::wstring fallback = state.media.title;
        if (fallback.empty()) fallback = state.media.sourceName;
        if (fallback.empty()) fallback = Loc(L"Media");

        // Rebuild layouts only when what is shown actually changed.
        const bool trackChanged = state.media.title != collapsedLyricTitle_ ||
                                  state.media.artist != collapsedLyricArtist_;
        const bool fallbackTextChanged = showTitle && collapsedLyricCur_.text != fallback;
        if (trackChanged || showIdx != collapsedLyricIdx_ || fallbackTextChanged) {
            if (trackChanged || collapsedLyricIdx_ == -2) {
                // New track or first draw: snap, no animation.
                collapsedLyricPrev_ = CollapsedLyricLine{};
                collapsedLyricAnimStart_ = now - 10.0;
            } else {
                collapsedLyricPrev_ = std::move(collapsedLyricCur_);
                collapsedLyricAnimStart_ = now;
            }
            collapsedLyricTitle_ = state.media.title;
            collapsedLyricArtist_ = state.media.artist;
            collapsedLyricIdx_ = showIdx;

            const std::wstring text = showTitle
                ? fallback
                : ((idx >= 0 && !lines[idx].text.empty()) ? lines[idx].text
                                                          : std::wstring(L"\u266A"));
            BuildCollapsedLyricLine(collapsedLyricCur_, text, fmt);
            collapsedLyricCur_.isTitle = showTitle;
        }
        if (!collapsedLyricCur_.layout) {
            return;
        }

        // Quick ease-out-cubic transition (~0.30s).
        constexpr double kAnimSec = 0.30;
        constexpr float kSlidePx = 13.0f;
        const float t = Clamp(static_cast<float>((now - collapsedLyricAnimStart_) / kAnimSec), 0.0f, 1.0f);
        const float inv = 1.0f - t;
        const float e = 1.0f - inv * inv * inv;

        const float cy = (area.top + area.bottom) * 0.5f;
        target_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Outgoing line: rises and fades a bit faster than the new one arrives.
        if (t < 1.0f && collapsedLyricPrev_.layout) {
            const CollapsedLyricLine& p = collapsedLyricPrev_;
            float x = area.left;
            if (p.width <= availW) {
                x = area.left + (availW - p.width) * 0.5f;
            } else if (!p.isTitle) {
                x = area.left - (p.width - availW);  // lyric rests at its panned end
            }
            const float y = cy - p.height * 0.5f - e * kSlidePx;
            textBrush_->SetOpacity(alpha * (1.0f - Clamp(t * 1.6f, 0.0f, 1.0f)) * 0.9f);
            target_->DrawTextLayout(D2D1::Point2F(x, y), p.layout.Get(), textBrush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // Incoming / current line.
        {
            const CollapsedLyricLine& c = collapsedLyricCur_;
            float x = area.left + (availW - c.width) * 0.5f;  // centred when it fits
            const float overflow = c.width - availW;
            float titleCycle = 0.0f;  // > 0 only when the title is scrolling as a marquee
            if (overflow > 0.0f) {
                if (c.isTitle) {
                    // Long title: scroll it in a loop, like the other marquees.
                    titleCycle = c.width + 38.0f;
                    x = area.left - std::fmod(static_cast<float>(now) * 30.0f, titleCycle);
                } else if (showIdx >= 0) {
                    // Long lyric: start at the left edge and pan across as it is sung.
                    const int64_t startMs = lines[showIdx].timeMs;
                    int64_t endMs = startMs + 4000;
                    if (showIdx + 1 < static_cast<int>(lines.size())) {
                        endMs = lines[showIdx + 1].timeMs;
                    } else if (state.media.endTicks > 0) {
                        endMs = state.media.endTicks / 10000;
                    }
                    const int64_t span = endMs - startMs;
                    const float prog = span > 0
                        ? Clamp(static_cast<float>(posMs - startMs) / static_cast<float>(span), 0.0f, 1.0f)
                        : 1.0f;
                    x = area.left - overflow * SmoothStep01((prog - 0.12f) / 0.70f);
                }
            }
            const float y = cy - c.height * 0.5f + (1.0f - e) * kSlidePx;
            textBrush_->SetOpacity(alpha * e * (c.isTitle ? 0.90f : 0.97f));
            target_->DrawTextLayout(D2D1::Point2F(x, y), c.layout.Get(), textBrush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (titleCycle > 0.0f) {
                target_->DrawTextLayout(D2D1::Point2F(x + titleCycle, y), c.layout.Get(),
                                        textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        target_->PopAxisAlignedClip();
        textBrush_->SetOpacity(0.96f);
    }

    // ── Source switch animation + source dock ────────────────────────────────
    enum { kSwapNone = 0, kSwapOut, kSwapHold, kSwapIn };

    // Out: old content slides away and fades. Hold: nothing drawn while the media
    // thread switches sessions. In: new content slides in. The request is only sent
    // once Out has finished, so old and new content are never visible together.
    void UpdateSourceSwap(const SharedState& state, double now, float* alpha, float* dx) {
        *alpha = 1.0f;
        *dx = 0.0f;

        // Media tab was off screen for a while: don't resume mid-animation.
        if (swapPhase_ != kSwapNone && swapLastUpdate_ >= 0.0 && now - swapLastUpdate_ > 0.5) {
            if (swapPhase_ == kSwapOut) RequestMediaSource(swapTarget_);
            swapPhase_ = kSwapNone;
        }
        swapLastUpdate_ = now;

        if (!expandedAnim_ && swapPhase_ != kSwapNone) {  // animation switched off mid-swap
            if (swapPhase_ == kSwapOut) RequestMediaSource(swapTarget_);
            swapPhase_ = kSwapNone;
        }

        // New request. One that arrives during slide-in waits for it to finish.
        const unsigned seq = g_switchSeq.load();
        if (seq != swapSeenSeq_ && swapPhase_ != kSwapIn) {
            swapSeenSeq_ = seq;
            {
                std::lock_guard lock(g_mediaSourceMutex);
                swapTarget_ = g_switchTarget;
            }
            swapDir_ = g_switchDir.load() >= 0 ? 1.0f : -1.0f;
            if (!expandedAnim_) {
                RequestMediaSource(swapTarget_);
            } else if (swapPhase_ == kSwapNone) {
                swapPhase_ = kSwapOut;
                swapStart_ = now;
            } else if (swapPhase_ == kSwapHold) {
                RequestMediaSource(swapTarget_);  // retarget while nothing is visible
                swapStart_ = now;
            }                                     // kSwapOut: just retargeted
        }

        switch (swapPhase_) {
            case kSwapOut: {
                const float t = Clamp(static_cast<float>((now - swapStart_) / MediaLayout::kSwitchOutSec), 0.0f, 1.0f);
                const float e = t * t;  // ease-in
                *alpha = 1.0f - e;
                *dx = -swapDir_ * MediaLayout::kSwitchShift * e;  // opposite to the switch direction
                if (t >= 1.0f) {
                    RequestMediaSource(swapTarget_);
                    swapPhase_ = kSwapHold;
                    swapStart_ = now;
                    *alpha = 0.0f;
                }
                break;
            }
            case kSwapHold:
                *alpha = 0.0f;
                *dx = -swapDir_ * MediaLayout::kSwitchShift;
                if (state.media.sourceAppUserModelId == swapTarget_ ||
                    now - swapStart_ > MediaLayout::kSwitchHoldTimeoutSec) {
                    CancelMediaChangeAnims();
                    swapPhase_ = kSwapIn;
                    swapStart_ = now;
                }
                break;
            case kSwapIn: {
                const float t = Clamp(static_cast<float>((now - swapStart_) / MediaLayout::kSwitchInSec), 0.0f, 1.0f);
                const float inv = 1.0f - t;
                const float e = 1.0f - inv * inv * inv;  // ease-out cubic
                *alpha = e;
                *dx = swapDir_ * MediaLayout::kSwitchShift * (1.0f - e);
                if (t >= 1.0f) {
                    swapPhase_ = kSwapNone;
                    *alpha = 1.0f;
                    *dx = 0.0f;
                }
                break;
            }
            default:
                break;
        }
        if (swapPhase_ != kSwapNone) g_layoutDirty = true;
    }

    // The title smoke, art flip, artist/album swap and spectrum ripple all key off "was
    // this drawn a moment ago". Forget that so the new source simply appears.
    void CancelMediaChangeAnims() {
        titleAnimActive_ = false;
        titleOld_.clear();
        titleNew_.clear();
        titleLastDrawn_ = -1.0;
        artFlipStart_ = -1.0;
        artFlipOld_.Reset();
        artFlipLastDrawn_ = -1.0;
        swapArtist_.start = -1.0;
        swapArtist_.lastDrawn = -1.0;
        swapAlbum_.start = -1.0;
        swapAlbum_.lastDrawn = -1.0;
        specStart_ = -1.0;
        specLastDrawn_ = -1.0;
    }

    // Fades (and optionally shifts) everything drawn until EndSwapGroup.
    bool BeginSwapGroup(float alpha, float dx) {
        if (alpha >= 0.999f && std::fabs(dx) < 0.01f) return false;
        if (!swapGroupLayer_ &&
            (FAILED(target_->CreateLayer(&swapGroupLayer_)) || !swapGroupLayer_)) return false;
        target_->GetTransform(&swapGroupOldTransform_);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(dx, 0.0f) * swapGroupOldTransform_);
        target_->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                  D2D1::IdentityMatrix(), Clamp(alpha, 0.0f, 1.0f), nullptr,
                                  D2D1_LAYER_OPTIONS_NONE),
            swapGroupLayer_.Get());
        return true;
    }

    void EndSwapGroup(bool began) {
        if (!began) return;
        target_->PopLayer();
        target_->SetTransform(swapGroupOldTransform_);
        swapGroupLayer_.Reset();
    }

    // "Playing from Brave" / "Paused in Spotify", 11px, tertiary text.
    void DrawSourceLine(const MediaSnapshot& media, D2D1_RECT_F area, double now) {
        if (media.sourceName.empty() || !smallTextFormat_) return;
        const std::wstring text =
            FormatWithName(Loc(media.playing ? L"Playing from %s" : L"Paused in %s"), media.sourceName);
        if (!scratchColorBrush_) {
            target_->CreateSolidColorBrush(material_.textTertiary, &scratchColorBrush_);
        } else {
            scratchColorBrush_->SetColor(material_.textTertiary);
        }
        if (!scratchColorBrush_) return;
        DrawMarqueeText(text, area, smallTextFormat_.Get(), scratchColorBrush_.Get(), now, 26.0f,
                        marqueeSourceCache_);
    }

    // One rounded pill, one circular slot per source. Shown only with 2+ sources.
    void DrawSourceDock(const SharedState& state, D2D1_RECT_F rect, double now) {
        const int sourceCount = static_cast<int>(state.mediaSources.size());
        if (sourceCount < 2) {
            dockInit_ = false;
            return;
        }

        const int liveIdx = ActiveMediaSourceIndex(state);
        const DockSlots slots = ComputeDockSlots(sourceCount, liveIdx);  // same inputs as the hit test
        if (slots.count < 2) return;

        // During a switch the ring already points at the source being switched to.
        int shownIdx = liveIdx;
        if (swapPhase_ != kSwapNone && !swapTarget_.empty()) {
            for (int i = 0; i < sourceCount; ++i) {
                if (state.mediaSources[i].aumid == swapTarget_) {
                    shownIdx = i;
                    break;
                }
            }
        }
        auto slotOf = [&](int sourceIdx) {
            for (int s = 0; s < slots.count; ++s) {
                if (slots.sourceIndex[s] == sourceIdx) return s;
            }
            return -1;
        };
        int targetSlot = slotOf(shownIdx);
        if (targetSlot < 0) targetSlot = slotOf(liveIdx);
        if (targetSlot < 0) targetSlot = 0;

        // ── Animation state ──────────────────────────────────────────────────
        const bool fresh = dockInit_ && dockLastDrawn_ >= 0.0 && (now - dockLastDrawn_) < 0.25 &&
                           dockSlotCount_ == slots.count;
        const float dt = fresh ? Clamp(static_cast<float>(now - dockLastDrawn_), 0.001f, 0.05f) : 0.016f;
        dockLastDrawn_ = now;

        if (!fresh) {
            dockInit_ = true;
            dockSlotCount_ = slots.count;
            dockIndPos_ = dockIndFrom_ = dockIndTarget_ = static_cast<float>(targetSlot);
            dockIndStart_ = -1.0;
            for (int i = 0; i < MediaLayout::kDockMaxSlots; ++i) {
                dockHover_[i] = 0.0f;
                dockPress_[i] = 0.0f;
            }
        } else if (static_cast<float>(targetSlot) != dockIndTarget_) {
            dockIndFrom_ = dockIndPos_;
            dockIndTarget_ = static_cast<float>(targetSlot);
            dockIndStart_ = now;
        }
        if (dockIndStart_ >= 0.0) {
            const float p = Clamp(static_cast<float>((now - dockIndStart_) / MediaLayout::kDockSlideSec), 0.0f, 1.0f);
            dockIndPos_ = dockIndFrom_ + (dockIndTarget_ - dockIndFrom_) * SkipEaseInOut(p);
            if (p >= 1.0f) {
                dockIndPos_ = dockIndTarget_;
                dockIndStart_ = -1.0;
            }
            g_layoutDirty = true;
        }

        const int hovered = g_hoveredSourceSlot.load(std::memory_order_relaxed);
        const int pressed = g_pressedSourceSlot.load(std::memory_order_relaxed);
        bool moving = false;
        for (int i = 0; i < slots.count; ++i) {
            const float hoverTarget = (i == hovered) ? 1.0f : 0.0f;
            dockHover_[i] += (hoverTarget - dockHover_[i]) * (1.0f - std::exp(-dt / 0.09f));
            if (std::fabs(hoverTarget - dockHover_[i]) < 0.004f) dockHover_[i] = hoverTarget; else moving = true;

            const float pressTarget = (i == pressed) ? 1.0f : 0.0f;
            const float tau = pressTarget > dockPress_[i] ? 0.025f : 0.075f;  // same feel as mediaBtnPress_
            dockPress_[i] += (pressTarget - dockPress_[i]) * (1.0f - std::exp(-dt / tau));
            if (std::fabs(pressTarget - dockPress_[i]) < 0.004f) dockPress_[i] = pressTarget; else moving = true;
        }
        if (moving) g_layoutDirty = true;

        // ── Drawing ──────────────────────────────────────────────────────────
        const float cy = rect.top + MediaLayout::kControlsY;
        const float x0 = rect.left + MediaLayout::kDockLeft;
        const D2D1_RECT_F pill = D2D1::RectF(x0, cy - MediaLayout::kDockHeight * 0.5f,
                                             x0 + MediaLayout::DockWidth(slots.count),
                                             cy + MediaLayout::kDockHeight * 0.5f);
        DrawCard(pill, MediaLayout::kDockHeight * 0.5f);

        // The pill's real colour (raised over the island base), used for the dot's ring.
        const D2D1_COLOR_F pillBg = MixColor(WithAlpha(material_.base, 1.0f),
                                             WithAlpha(material_.raised, 1.0f), material_.raised.a);

        // Active indicator: slides between slots.
        const float stride = MediaLayout::kDockSlot + MediaLayout::kDockGap;
        const float indCx = rect.left + MediaLayout::DockSlotLeft(0) + dockIndPos_ * stride +
                            MediaLayout::kDockSlot * 0.5f;
        const float indR = MediaLayout::kDockSlot * 0.5f;
        ComPtr<ID2D1SolidColorBrush> indFill;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &indFill)) && indFill) {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(indCx, cy), indR, indR), indFill.Get());
        }
        accentBrush_->SetOpacity(1.0f);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(indCx, cy), indR - 0.75f, indR - 0.75f),
                             accentBrush_.Get(), 1.5f);

        for (int i = 0; i < slots.count; ++i) {
            const D2D1_POINT_2F c = D2D1::Point2F(
                rect.left + MediaLayout::DockSlotLeft(i) + MediaLayout::kDockSlot * 0.5f, cy);
            const float active = Clamp(1.0f - std::fabs(dockIndPos_ - static_cast<float>(i)), 0.0f, 1.0f);
            const float lit = std::max(active, dockHover_[i]);
            const float opacity = 0.5f + 0.5f * lit;  // 50% idle, 100% active / hovered
            const float s = 1.0f - 0.10f * Clamp(dockPress_[i], 0.0f, 1.0f);
            const D2D1_RECT_F slotRect = D2D1::RectF(c.x - 13.0f, c.y - 13.0f, c.x + 13.0f, c.y + 13.0f);

            D2D1_MATRIX_3X2_F oldTransform;
            target_->GetTransform(&oldTransform);
            target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * oldTransform);

            const int srcIdx = slots.sourceIndex[i];
            if (srcIdx < 0) {
                wchar_t more[16] = {};
                swprintf_s(more, L"+%d", slots.overflow);
                if (boldTextFormat_) {
                    mutedBrush_->SetOpacity(0.55f + 0.45f * lit);
                    target_->DrawTextW(more, static_cast<UINT32>(wcslen(more)), boldTextFormat_.Get(),
                                       slotRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    mutedBrush_->SetOpacity(0.75f);
                }
            } else {
                const MediaSourceInfo& src = state.mediaSources[srcIdx];
                if (src.icon) {
                    DrawCircularBitmapPixels(*src.icon, c, MediaLayout::kDockIcon * 0.5f,
                                             dockIconBitmap_[i], dockIconGen_[i], opacity);
                } else if (boldTextFormat_ && !src.badge.empty()) {
                    textBrush_->SetOpacity(opacity);
                    target_->DrawTextW(src.badge.c_str(), static_cast<UINT32>(src.badge.size()),
                                       boldTextFormat_.Get(), slotRect, textBrush_.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    textBrush_->SetOpacity(0.96f);
                }

                if (src.playing) {  // 7px green dot, bottom-right, 1.5px ring in the pill colour
                    const D2D1_POINT_2F d = D2D1::Point2F(c.x + 9.5f, c.y + 9.5f);
                    ComPtr<ID2D1SolidColorBrush> ring, dot;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(pillBg, &ring)) && ring) {
                        target_->FillEllipse(D2D1::Ellipse(d, 5.0f, 5.0f), ring.Get());
                    }
                    if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f), &dot)) && dot) {
                        target_->FillEllipse(D2D1::Ellipse(d, 3.5f, 3.5f), dot.Get());
                    }
                }
            }
            target_->SetTransform(oldTransform);
        }
    }

    void UpdateMediaButtonAnimations(double now, bool playing) {
        float dt = 0.016f;
        if (lastMediaBtnTime_ > 0.0) {
            dt = static_cast<float>(std::max(0.001, std::min(now - lastMediaBtnTime_, 0.05)));
        }
        lastMediaBtnTime_ = now;

        const int pressedCmd = g_pressedMediaButton.load();
        bool isStillAnimating = false;

        for (int i = 0; i < 3; ++i) {
            const float target = (pressedCmd == i) ? 1.0f : 0.0f;
            const float tau = (target > mediaBtnPress_[i]) ? 0.025f : 0.075f;
            const float k = 1.0f - std::exp(-dt / tau);
            mediaBtnPress_[i] += (target - mediaBtnPress_[i]) * k;

            if (std::abs(mediaBtnPress_[i] - target) < 0.002f) {
                mediaBtnPress_[i] = target;
            } else {
                isStillAnimating = true;
            }
        }

        // Play/pause morph. Animates only if the button was on screen a moment
        // ago; otherwise (island was collapsed, first draw) it snaps to the state.
        {
            constexpr double kMorphDuration = 0.34;
            const bool recentlyDrawn = playMorphLastDrawn_ >= 0.0 && (now - playMorphLastDrawn_) < 0.25;
            playMorphLastDrawn_ = now;
            const float target = playing ? 0.0f : 1.0f;
            if (!playMorphInit_ || !recentlyDrawn || !expandedAnim_) {
                playMorphInit_ = true;
                playMorph_ = playMorphFrom_ = playMorphTarget_ = target;
                playMorphStart_ = -1.0;
            } else if (target != playMorphTarget_) {
                playMorphFrom_ = playMorph_;
                playMorphTarget_ = target;
                playMorphStart_ = now;
            }
            if (playMorphStart_ >= 0.0) {
                const float p = static_cast<float>((now - playMorphStart_) / kMorphDuration);
                if (p >= 1.0f) {
                    playMorph_ = playMorphTarget_;
                    playMorphStart_ = -1.0;
                } else {
                    playMorph_ = playMorphFrom_ + (playMorphTarget_ - playMorphFrom_) * SkipEaseInOut(p);
                    isStillAnimating = true;
                }
            }
        }

        // Depth-swap: start on a new click, then advance with wall-clock time.
        constexpr double kSkipDuration = 0.46;
        const unsigned trig[2] = {g_skipTriggerPrev.load(), g_skipTriggerNext.load()};
        for (int i = 0; i < 2; ++i) {
            if (trig[i] != skipSeenTrigger_[i]) {
                skipSeenTrigger_[i] = trig[i];
                lastSkipClickAt_ = now;
                lastSkipDir_ = (i == 0) ? -1.0f : 1.0f;
                // Ignore a re-click while the swap is still in its first half so
                // the motion never snaps back; the command is sent regardless.
                if (expandedAnim_ && (skipAnimStart_[i] < 0.0 || (now - skipAnimStart_[i]) / kSkipDuration >= 0.5)) {
                    skipAnimStart_[i] = now;
                }
            }
            if (skipAnimStart_[i] >= 0.0) {
                const double p = (now - skipAnimStart_[i]) / kSkipDuration;
                if (p >= 1.0) {
                    skipAnimStart_[i] = -1.0;
                    skipProgress_[i] = -1.0f;
                } else {
                    skipProgress_[i] = static_cast<float>(p);
                    isStillAnimating = true;
                }
            } else {
                skipProgress_[i] = -1.0f;
            }
        }

        if (isStillAnimating) {
            g_layoutDirty = true;
        }
    }

    // ── Skip icon: two rounded triangles + "depth swap" animation ────────────
    static constexpr float kSkipTriW = 0.626f;   // triangle width / height
    static constexpr float kSkipPitch = 0.451f;  // front-left minus back-left, / height

    HRESULT BuildRoundedTriangle(float x0, float w, float H, ComPtr<ID2D1PathGeometry>& out) {
        const D2D1_POINT_2F v[3] = {
            D2D1::Point2F(x0, -0.5f * H), D2D1::Point2F(x0, 0.5f * H), D2D1::Point2F(x0 + w, 0.0f)};
        const float d[3] = {0.11f * H, 0.11f * H, 0.14f * H};
        D2D1_POINT_2F a[3], b[3];
        for (int i = 0; i < 3; ++i) {
            const D2D1_POINT_2F p = v[(i + 2) % 3], n = v[(i + 1) % 3];
            float px = p.x - v[i].x, py = p.y - v[i].y;
            float nx = n.x - v[i].x, ny = n.y - v[i].y;
            const float pl = std::sqrt(px * px + py * py), nl = std::sqrt(nx * nx + ny * ny);
            a[i] = D2D1::Point2F(v[i].x + px / pl * d[i], v[i].y + py / pl * d[i]);
            b[i] = D2D1::Point2F(v[i].x + nx / nl * d[i], v[i].y + ny / nl * d[i]);
        }
        ComPtr<ID2D1PathGeometry> geom;
        HRESULT hr = d2dFactory_->CreatePathGeometry(&geom);
        if (FAILED(hr) || !geom) return FAILED(hr) ? hr : E_FAIL;
        ComPtr<ID2D1GeometrySink> sink;
        hr = geom->Open(&sink);
        if (FAILED(hr) || !sink) return FAILED(hr) ? hr : E_FAIL;
        sink->BeginFigure(b[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(a[1]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[1], b[1]));
        sink->AddLine(a[2]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[2], b[2]));
        sink->AddLine(a[0]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[0], b[0]));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        hr = sink->Close();
        if (FAILED(hr)) return hr;
        out = geom;
        return S_OK;
    }

    bool EnsureSkipGeometry() {
        if (skipTriFull_ && skipTriNotch_) return true;
        if (!d2dFactory_) return false;
        const float H = MediaLayout::kNavButtonRadius * 0.78f;
        const float w = kSkipTriW * H, pitch = kSkipPitch * H;

        ComPtr<ID2D1PathGeometry> full, back, front;
        if (FAILED(BuildRoundedTriangle(0.0f, w, H, full))) return false;
        if (FAILED(BuildRoundedTriangle(0.0f, w, H, back))) return false;
        if (FAILED(BuildRoundedTriangle(pitch, w, H, front))) return false;

        // The back triangle is cut by a slightly enlarged copy of the front one,
        // leaving a clean gap that works on translucent backgrounds too.
        ComPtr<ID2D1TransformedGeometry> grown;
        if (FAILED(d2dFactory_->CreateTransformedGeometry(
                front.Get(), D2D1::Matrix3x2F::Scale(1.16f, 1.16f, D2D1::Point2F(pitch + w / 3.0f, 0.0f)),
                &grown)) || !grown) return false;

        ComPtr<ID2D1PathGeometry> notch;
        if (FAILED(d2dFactory_->CreatePathGeometry(&notch)) || !notch) return false;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(notch->Open(&sink)) || !sink) return false;
        if (FAILED(back->CombineWithGeometry(grown.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.05f, sink.Get()))) {
            return false;
        }
        if (FAILED(sink->Close())) return false;

        skipTriFull_ = full;
        skipTriNotch_ = notch;
        return true;
    }

    static float SkipSmooth(float x) {            // smoothstep 0..1
        x = Clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
    static float SkipEaseInOut(float x) {         // easeInOutCubic
        x = Clamp(x, 0.0f, 1.0f);
        return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
    }

    // forward = true -> "next" (>>), false -> "previous" (<<, mirrored).
    // p < 0 = resting state, otherwise animation progress 0..1.
    void DrawSkipIcon(D2D1_POINT_2F center, bool forward, float p, float opacity, float press) {
        if (!EnsureSkipGeometry()) return;

        const float H = MediaLayout::kNavButtonRadius * 0.78f;
        const float w = kSkipTriW * H, pitch = kSkipPitch * H;
        const float pairLeft = -0.5f * (pitch + w);

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        const float sf = 1.0f - 0.13f * press;
        const D2D1_MATRIX_3X2_F base =
            D2D1::Matrix3x2F::Scale(forward ? 1.0f : -1.0f, 1.0f) *
            D2D1::Matrix3x2F::Translation(center.x, center.y) *
            D2D1::Matrix3x2F::Scale(sf, sf, center) * oldTransform;

        auto fill = [&](ID2D1Geometry* g, float x, float scale, float alpha) {
            if (alpha <= 0.003f) return;
            const D2D1_MATRIX_3X2_F m =
                D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(w / 3.0f, 0.0f)) *
                D2D1::Matrix3x2F::Translation(x, 0.0f) * base;
            target_->SetTransform(m);
            accentBrush_->SetOpacity(Clamp(opacity * alpha, 0.0f, 1.0f));
            target_->FillGeometry(g, accentBrush_.Get());
        };

        if (p < 0.0f) {
            fill(skipTriNotch_.Get(), pairLeft, 1.0f, 1.0f);
            fill(skipTriFull_.Get(), pairLeft + pitch, 1.0f, 1.0f);
        } else {
            const float e = SkipEaseInOut(p);

            // B (front): pushed forward, recedes and fades out.
            const float bFade = 1.0f - SkipSmooth(p / 0.75f);
            fill(skipTriFull_.Get(), pairLeft + pitch + pitch * 1.15f * e, 1.0f - 0.2f * e, bFade);

            // A (back): glides into B's slot; its notch dissolves as it becomes the front.
            const float s = SkipSmooth((p - 0.1f) / 0.8f);
            fill(skipTriNotch_.Get(), pairLeft + pitch * e, 1.0f, 1.0f - s);
            fill(skipTriFull_.Get(), pairLeft + pitch * e, 1.0f, s);

            // A' (new back): grows in behind, where A used to be.
            const float cIn = SkipSmooth((p - 0.3f) / 0.7f);
            fill(skipTriNotch_.Get(), pairLeft - pitch * 0.45f * (1.0f - cIn), 0.7f + 0.3f * cIn, cIn);
        }

        target_->SetTransform(oldTransform);
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawMediaControls(bool playing, D2D1_POINT_2F prev, D2D1_POINT_2F play, D2D1_POINT_2F next, double now) {
        UpdateMediaButtonAnimations(now, playing);
        DrawMediaButton(prev, MediaLayout::kNavButtonRadius, 0, false);
        DrawMediaButton(play, MediaLayout::kPlayButtonRadius, playing ? 1 : 2, true);
        DrawMediaButton(next, MediaLayout::kNavButtonRadius, 3, false);
    }

    void DrawMediaButton(D2D1_POINT_2F center, float radius, int kind, bool primary) {
        int buttonCmd = (kind == 0) ? 0 : ((kind == 1 || kind == 2) ? 1 : 2);
        bool isHovered = (g_hoveredMediaButton.load() == buttonCmd);
        const float press = (buttonCmd >= 0 && buttonCmd < 3) ? mediaBtnPress_[buttonCmd] : 0.0f;

        const float r = radius * (1.0f - 0.13f * press);

        const D2D1_COLOR_F restingBg = D2D1::ColorF(
            1.0f, 1.0f, 1.0f,
            primary ? (isHovered ? 0.16f : 0.080f) : (isHovered ? 0.09f : 0.040f)
        );
        const D2D1_COLOR_F pressedBg = D2D1::ColorF(
            currentAccent_.r, currentAccent_.g, currentAccent_.b,
            primary ? 0.28f : 0.18f
        );

        const D2D1_COLOR_F currentBg = D2D1::ColorF(
            restingBg.r + (pressedBg.r - restingBg.r) * press,
            restingBg.g + (pressedBg.g - restingBg.g) * press,
            restingBg.b + (pressedBg.b - restingBg.b) * press,
            restingBg.a + (pressedBg.a - restingBg.a) * press
        );

        if (ID2D1SolidColorBrush* bg = AcquireBrush(currentBg)) {
            target_->FillEllipse(D2D1::Ellipse(center, r, r), bg);
        }

        const float baseOpacity = primary ? (isHovered ? 1.0f : 0.88f) : (isHovered ? 0.92f : 0.62f);
        const float iconOpacity = Clamp(baseOpacity + (1.0f - baseOpacity) * press, 0.0f, 1.0f);
        accentBrush_->SetOpacity(iconOpacity);

        if (kind == 0 || kind == 3) {
            DrawSkipIcon(center, kind == 3, skipProgress_[kind == 3 ? 1 : 0], iconOpacity, press);
            accentBrush_->SetOpacity(1.0f);
            return;
        }

        if (kind == 1 || kind == 2) {
            DrawPlayPauseIcon(center, iconOpacity, press);
            accentBrush_->SetOpacity(1.0f);
            return;
        }

        const wchar_t* glyph = nullptr;
        IDWriteTextFormat* fmt = nullptr;
        if (usingFluentIcons_) {
            if (kind == 0) {
                glyph = L"\uF8AC";
                fmt = mediaNavIconFormat_.Get();
            } else if (kind == 1) {
                glyph = L"\uF8AE";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 2) {
                glyph = L"\uF5B0";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 3) {
                glyph = L"\uF8AD";
                fmt = mediaNavIconFormat_.Get();
            }
        } else {
            if (kind == 0) {
                glyph = L"\uE100";
                fmt = mediaNavIconFormat_.Get();
            } else if (kind == 1) {
                glyph = L"\uE103";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 2) {
                glyph = L"\uE102";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 3) {
                glyph = L"\uE101";
                fmt = mediaNavIconFormat_.Get();
            }
        }

        if (glyph && fmt) {
            const float offsetX = (kind == 2) ? 1.0f : 0.0f;
            D2D1_RECT_F glyphRect = D2D1::RectF(
                center.x - radius + offsetX,
                center.y - radius,
                center.x + radius + offsetX,
                center.y + radius
            );

            if (press > 0.001f) {
                D2D1_MATRIX_3X2_F oldTransform;
                target_->GetTransform(&oldTransform);
                const float scaleFactor = 1.0f - 0.13f * press;
                target_->SetTransform(D2D1::Matrix3x2F::Scale(scaleFactor, scaleFactor, center) * oldTransform);
                target_->DrawTextW(glyph, 1, fmt, glyphRect, accentBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                target_->SetTransform(oldTransform);
            } else {
                target_->DrawTextW(glyph, 1, fmt, glyphRect, accentBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        accentBrush_->SetOpacity(1.0f);
    }

    // ── Play/pause morph ─────────────────────────────────────────────────────
    // Both icons are two quads with matching vertices: the pause bars become the
    // two halves of the play triangle (the right half collapses into the tip).
    static constexpr float kPlayPauseCorner = 1.5f;

    struct PlayPauseShapes {
        D2D1_POINT_2F pause[8];
        D2D1_POINT_2F play[8];
    };

    static const PlayPauseShapes& GetPlayPauseShapes() {
        static const PlayPauseShapes shapes = [] {
            PlayPauseShapes s = {};
            const float rr = kPlayPauseCorner;
            const float bx0 = 2.1f, bx1 = 6.5f, by = 7.5f;
            // Left bar, then right bar: TL, TR, BR, BL.
            s.pause[0] = D2D1::Point2F(-bx1 + rr, -by + rr);
            s.pause[1] = D2D1::Point2F(-bx0 - rr, -by + rr);
            s.pause[2] = D2D1::Point2F(-bx0 - rr,  by - rr);
            s.pause[3] = D2D1::Point2F(-bx1 + rr,  by - rr);
            s.pause[4] = D2D1::Point2F( bx0 + rr, -by + rr);
            s.pause[5] = D2D1::Point2F( bx1 - rr, -by + rr);
            s.pause[6] = D2D1::Point2F( bx1 - rr,  by - rr);
            s.pause[7] = D2D1::Point2F( bx0 + rr,  by - rr);

            // Triangle (optically centred), split by a shared edge T-U.
            const D2D1_POINT_2F A = D2D1::Point2F(-5.5f, -7.5f);
            const D2D1_POINT_2F B = D2D1::Point2F(-5.5f,  7.5f);
            const D2D1_POINT_2F C = D2D1::Point2F( 7.5f,  0.0f);
            const D2D1_POINT_2F T = D2D1::Point2F( 1.0f, -3.75f);
            const D2D1_POINT_2F U = D2D1::Point2F( 1.0f,  3.75f);
            const float la = std::hypot(B.x - C.x, B.y - C.y);
            const float lb = std::hypot(C.x - A.x, C.y - A.y);
            const float lc = std::hypot(A.x - B.x, A.y - B.y);
            const float per = la + lb + lc;
            const float ix = (la * A.x + lb * B.x + lc * C.x) / per;
            const float iy = (la * A.y + lb * B.y + lc * C.y) / per;
            const float area = 0.5f * std::fabs((B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x));
            const float inr = 2.0f * area / per;
            const float k = (inr - rr) / inr;
            auto shrink = [&](D2D1_POINT_2F p) {
                return D2D1::Point2F(ix + k * (p.x - ix), iy + k * (p.y - iy));
            };
            s.play[0] = shrink(A); s.play[1] = shrink(T); s.play[2] = shrink(U); s.play[3] = shrink(B);
            s.play[4] = shrink(T); s.play[5] = shrink(C); s.play[6] = shrink(C); s.play[7] = shrink(U);
            return s;
        }();
        return shapes;
    }

    bool EnsureRoundJoinStyle() {
        if (roundJoinStyle_) return true;
        if (!d2dFactory_) return false;
        return SUCCEEDED(d2dFactory_->CreateStrokeStyle(
                   D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                                               D2D1_DASH_STYLE_SOLID, 0.0f),
                   nullptr, 0, &roundJoinStyle_)) && roundJoinStyle_;
    }

    void DrawPlayPauseIcon(D2D1_POINT_2F center, float opacity, float press) {
        if (!d2dFactory_) return;
        EnsureRoundJoinStyle();
        const PlayPauseShapes& sh = GetPlayPauseShapes();
        const float m = Clamp(playMorph_, 0.0f, 1.0f);

        D2D1_POINT_2F p[8];
        for (int i = 0; i < 8; ++i) {
            p[i] = D2D1::Point2F(sh.pause[i].x + (sh.play[i].x - sh.pause[i].x) * m,
                                 sh.pause[i].y + (sh.play[i].y - sh.pause[i].y) * m);
        }

        ComPtr<ID2D1PathGeometry> quad[2];
        for (int q = 0; q < 2; ++q) {
            if (FAILED(d2dFactory_->CreatePathGeometry(&quad[q])) || !quad[q]) return;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(quad[q]->Open(&sink)) || !sink) return;
            sink->BeginFigure(p[q * 4], D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(p[q * 4 + 1]);
            sink->AddLine(p[q * 4 + 2]);
            sink->AddLine(p[q * 4 + 3]);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) return;
        }

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);

        // Everything is drawn opaque inside a layer that carries the icon opacity,
        // so the overlap between fill, stroke and the two halves never double-blends.
        ComPtr<ID2D1Layer> layer;
        const bool layered = SUCCEEDED(target_->CreateLayer(nullptr, &layer)) && layer;
        if (layered) {
            target_->PushLayer(
                D2D1::LayerParameters(D2D1::RectF(center.x - 18.0f, center.y - 18.0f,
                                                  center.x + 18.0f, center.y + 18.0f),
                                      nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(), opacity, nullptr, D2D1_LAYER_OPTIONS_NONE),
                layer.Get());
        }
        accentBrush_->SetOpacity(layered ? 1.0f : opacity);

        const float sf = 1.0f - 0.13f * press;
        target_->SetTransform(D2D1::Matrix3x2F::Translation(center.x, center.y) *
                              D2D1::Matrix3x2F::Scale(sf, sf, center) * oldTransform);
        for (int q = 0; q < 2; ++q) {
            target_->FillGeometry(quad[q].Get(), accentBrush_.Get());
            target_->DrawGeometry(quad[q].Get(), accentBrush_.Get(), kPlayPauseCorner * 2.0f,
                                  roundJoinStyle_.Get());
        }
        target_->SetTransform(oldTransform);
        if (layered) target_->PopLayer();
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Song-title change animation ──────────────────────────────────────────
    // Scripts whose characters shape/reorder together can't be animated one
    // glyph at a time, so those titles simply fall back to the plain redraw.
    static bool TitleNeedsShaping(wchar_t c) {
        return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0590 && c <= 0x0DFF) ||
               (c >= 0x0E00 && c <= 0x0EFF) || c == 0x200D;
    }

    // Same formula DrawMarqueeText uses for the title (speed 42), so the hand-off
    // back to the normal marquee at the end of the animation doesn't jump.
    static float TitleScrollOffset(const TitleScroll& sc, double now) {
        return sc.scrolls ? std::fmod(static_cast<float>(now) * 42.0f, sc.cycle) : 0.0f;
    }

    // allLayouts = false keeps only the glyphs visible right now (the outgoing
    // title is frozen, so nothing else of it will ever be seen).
    bool BuildTitleGlyphs(const std::wstring& text, float wrapHeight, float available, double now,
                          bool allLayouts, std::vector<TitleGlyph>& out, TitleScroll& scroll) {
        out.clear();
        scroll = TitleScroll{};
        if (!dwriteFactory_ || !textFormat_ || text.empty() || text.size() > 120) return false;

        ComPtr<IDWriteTextLayout> full;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                    textFormat_.Get(), 2000.0f, wrapHeight, &full)) || !full) {
            return false;
        }
        DWRITE_TEXT_METRICS tm = {};
        full->GetMetrics(&tm);
        scroll.width = tm.widthIncludingTrailingWhitespace;
        scroll.scrolls = scroll.width > available;
        scroll.cycle = scroll.width + 38.0f;
        scroll.offset0 = TitleScrollOffset(scroll, now);

        for (UINT32 i = 0; i < text.size();) {
            const wchar_t c = text[i];
            if (TitleNeedsShaping(c)) return false;
            const UINT32 len = (IS_HIGH_SURROGATE(c) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) ? 2 : 1;
            if (c != L' ' && c != L'\t' && c != 0x00A0 && c != 0x3000) {
                TitleGlyph g;
                g.ch0 = c;
                g.ch1 = (len == 2) ? text[i + 1] : 0;
                float px = 0.0f, py = 0.0f;
                DWRITE_HIT_TEST_METRICS htm = {};
                if (FAILED(full->HitTestTextPosition(i, FALSE, &px, &py, &htm))) return false;
                g.x = px;
                g.sx = px - scroll.offset0;
                g.eligible = (g.sx + htm.width > 0.0f) && (g.sx < available);
                if (allLayouts || g.eligible) {
                    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str() + i, len, textFormat_.Get(),
                                                                2000.0f, wrapHeight, &g.layout)) || !g.layout) {
                        return false;
                    }
                    DWRITE_TEXT_METRICS gm = {};
                    g.layout->GetMetrics(&gm);
                    g.w = gm.widthIncludingTrailingWhitespace;
                    g.h = gm.height;
                    const float hv = std::sin(static_cast<float>(i) * 12.9898f + static_cast<float>(c) * 78.233f) * 43758.5453f;
                    g.seed = hv - std::floor(hv);
                    out.push_back(std::move(g));
                }
            }
            i += len;
        }
        return !out.empty();
    }

    // Longest common subsequence of identical, visible glyphs; ties prefer the
    // pairs that travel the least. Order is preserved, so matched letters never cross.
    static void MatchTitleGlyphs(std::vector<TitleGlyph>& a, std::vector<TitleGlyph>& b) {
        const size_t n = a.size(), m = b.size();
        std::vector<int> dp((n + 1) * (m + 1), 0);
        auto at = [&](size_t i, size_t j) -> int& { return dp[i * (m + 1) + j]; };
        auto same = [&](size_t i, size_t j) {
            return a[i].eligible && b[j].eligible && a[i].ch0 == b[j].ch0 && a[i].ch1 == b[j].ch1;
        };
        auto pairScore = [&](size_t i, size_t j) {
            return 1000 - static_cast<int>(std::min(std::fabs(a[i].sx - b[j].sx), 900.0f));
        };
        for (size_t i = 1; i <= n; ++i) {
            for (size_t j = 1; j <= m; ++j) {
                int best = std::max(at(i - 1, j), at(i, j - 1));
                if (same(i - 1, j - 1)) best = std::max(best, at(i - 1, j - 1) + pairScore(i - 1, j - 1));
                at(i, j) = best;
            }
        }
        size_t i = n, j = m;
        while (i > 0 && j > 0) {
            if (same(i - 1, j - 1) && at(i, j) == at(i - 1, j - 1) + pairScore(i - 1, j - 1)) {
                a[i - 1].match = static_cast<int>(j - 1);
                b[j - 1].match = static_cast<int>(i - 1);
                --i; --j;
            } else if (at(i - 1, j) >= at(i, j - 1)) {
                --i;
            } else {
                --j;
            }
        }
    }

    void StartTitleTransition(const std::wstring& oldText, const std::wstring& newText,
                              D2D1_RECT_F rect, double now) {
        const float h = rect.bottom - rect.top;
        const float avail = rect.right - rect.left;
        if (!BuildTitleGlyphs(oldText, h, avail, now, false, titleOld_, titleOldScroll_) ||
            !BuildTitleGlyphs(newText, h, avail, now, true, titleNew_, titleNewScroll_)) {
            titleOld_.clear();
            titleNew_.clear();
            return;
        }
        MatchTitleGlyphs(titleOld_, titleNew_);
        titleAvail_ = avail;
        titleAnimStart_ = now;
        titleAnimDur_ = 1.0f;
        titleAnimActive_ = true;
    }

    // t: 0 = solid, 1 = gone. Rises, sways, stretches upward and thins out; extra
    // translucent copies trail above it so the edge looks soft instead of cut.
    // Used in reverse (t counting down, smaller amp) to make letters condense in.
    void DrawSmokeGlyph(const TitleGlyph& g, D2D1_POINT_2F origin, float t, float amp,
                        ID2D1Brush* brush, float baseOpacity) {
        if (t >= 0.999f || !g.layout) return;
        if (t <= 0.0f) {
            brush->SetOpacity(baseOpacity);
            target_->DrawTextLayout(origin, g.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            return;
        }
        D2D1_MATRIX_3X2_F oldT;
        target_->GetTransform(&oldT);

        const float seed = g.seed;
        const float rise = 16.0f * amp * std::pow(t, 1.15f);
        const float sway = std::sin(t * (4.5f + 3.0f * seed) + seed * 6.2832f) * 4.5f * amp * std::sqrt(t);
        const float angle = (seed - 0.5f) * 50.0f * t * amp;
        const float sx = 1.0f + 0.55f * t * amp;
        const float sy = 1.0f + 1.40f * t * amp;
        const float alpha = std::pow(1.0f - t, 1.6f) * baseOpacity;
        const float soft = SkipSmooth(t / 0.25f);  // soft copies fade in as it starts to dissolve
        const float spread = t * 3.2f * amp;
        const D2D1_POINT_2F pivot = D2D1::Point2F(origin.x + g.w * 0.5f, origin.y + g.h * 0.82f);

        for (int k = 0; k < 4; ++k) {
            const float dx = (k == 1) ? -spread * 0.9f : (k == 3 ? spread * 0.9f : 0.0f);
            const float dy = -static_cast<float>(k) * spread * 0.7f;
            const float a = (k == 0) ? alpha * (1.0f - 0.55f * soft) : alpha * 0.28f * soft;
            if (a <= 0.003f) continue;
            brush->SetOpacity(Clamp(a, 0.0f, 1.0f));
            target_->SetTransform(
                D2D1::Matrix3x2F::Scale(sx, sy, pivot) *
                D2D1::Matrix3x2F::Rotation(angle * (1.0f + 0.25f * static_cast<float>(k)), pivot) *
                D2D1::Matrix3x2F::Translation(sway + dx, -rise + dy) * oldT);
            target_->DrawTextLayout(origin, g.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        target_->SetTransform(oldT);
        brush->SetOpacity(baseOpacity);
    }

    // Returns true when it drew the title itself (animation running); false means
    // the caller should draw the title the normal way.
    bool DrawTitleTransition(const std::wstring& text, D2D1_RECT_F rect, ID2D1Brush* brush,
                             double now, bool realTitle) {
        // The title is only drawn while the expanded media card is visible, so a
        // long gap means the old title was never on screen: just snap.
        const bool recentlyDrawn = titleLastDrawn_ >= 0.0 && (now - titleLastDrawn_) < 0.25;
        titleLastDrawn_ = now;

        if (text != titleShown_) {
            const std::wstring oldText = titleShown_;
            const bool oldWasReal = !titleShownPlaceholder_;
            titleShown_ = text;
            titleShownPlaceholder_ = !realTitle;
            titleAnimActive_ = false;
            titleOld_.clear();
            titleNew_.clear();
            if (expandedAnim_ && recentlyDrawn && realTitle && oldWasReal && !oldText.empty()) {
                StartTitleTransition(oldText, text, rect, now);
            }
        } else if (titleAnimActive_ && (!recentlyDrawn || !expandedAnim_)) {
            titleAnimActive_ = false;
            titleOld_.clear();
            titleNew_.clear();
        }

        if (!titleAnimActive_) return false;

        const float time = static_cast<float>(now - titleAnimStart_);
        if (time >= titleAnimDur_) {
            titleAnimActive_ = false;
            titleOld_.clear();
            titleNew_.clear();
            return false;
        }

        const float baseOpacity = brush->GetOpacity();
        const float avail = std::max(titleAvail_, 1.0f);
        const float newOffset = TitleScrollOffset(titleNewScroll_, now);
        target_->PushAxisAlignedClip(
            D2D1::RectF(rect.left - 4.0f, rect.top - 20.0f, rect.right + 4.0f, rect.bottom + 6.0f),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Old letters with no partner: dissolve into smoke, left to right. The old
        // title is frozen at the spot where it was when the track changed.
        const float oldSpan = std::max(std::min(titleOldScroll_.width, avail), 1.0f);
        for (const TitleGlyph& g : titleOld_) {
            if (g.match >= 0 || !g.eligible) continue;
            const float f = Clamp(g.sx / oldSpan, 0.0f, 1.0f);
            const float t = Clamp((time - 0.22f * f) / 0.60f, 0.0f, 1.0f);
            DrawSmokeGlyph(g, D2D1::Point2F(rect.left + g.sx, rect.top), t, 1.0f, brush, baseOpacity);
        }

        // Letters present in both titles: glide to their new position (which keeps
        // scrolling if the new title is a marquee).
        brush->SetOpacity(baseOpacity);
        for (const TitleGlyph& g : titleOld_) {
            if (g.match < 0) continue;
            const TitleGlyph& n = titleNew_[static_cast<size_t>(g.match)];
            const float e = SkipEaseInOut((time - 0.04f) / 0.55f);
            const float targetX = n.x - newOffset;
            const float x = g.sx + (targetX - g.sx) * e;
            const float lift = -1.5f * std::sin(3.14159265f * e);
            target_->DrawTextLayout(D2D1::Point2F(rect.left + x, rect.top + lift), g.layout.Get(), brush,
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // New letters with no partner: condense out of the smoke. A scrolling title
        // is drawn at its live marquee position, including the wrapped copy.
        const float newSpan = std::max(std::min(titleNewScroll_.width, avail), 1.0f);
        for (const TitleGlyph& g : titleNew_) {
            if (g.match >= 0) continue;
            const float xs = g.x - newOffset;
            const float f = Clamp(xs / newSpan, 0.0f, 1.0f);
            const float u = Clamp((time - 0.28f - 0.20f * f) / 0.50f, 0.0f, 1.0f);
            if (xs + g.w > 0.0f && xs < avail) {
                DrawSmokeGlyph(g, D2D1::Point2F(rect.left + xs, rect.top), 1.0f - u, 0.45f, brush, baseOpacity);
            }
            if (titleNewScroll_.scrolls) {
                const float xs2 = xs + titleNewScroll_.cycle;
                if (xs2 + g.w > 0.0f && xs2 < avail) {
                    DrawSmokeGlyph(g, D2D1::Point2F(rect.left + xs2, rect.top), 1.0f - u, 0.45f, brush, baseOpacity);
                }
            }
        }

        brush->SetOpacity(baseOpacity);
        target_->PopAxisAlignedClip();
        g_layoutDirty = true;
        return true;
    }

    // ── Progress bar styles (Wavy / Squiggle / Bar) ──────────────────────────
    // Slim is the original bar and keeps its own code path in DrawMedia. The
    // hit test is untouched: every shape here stays inside the scrubber's hit area.
    void DrawStyledProgress(ProgressStyle style, float barLeft, float barRight, float cy,
                            float progress, bool dragging, bool playing, double now) {
        const float width = barRight - barLeft;
        if (width <= 8.0f) return;
        progress = Clamp(progress, 0.0f, 1.0f);
        const float thumbX = barLeft + width * progress;

        ComPtr<ID2D1SolidColorBrush> accent, track;
        if (FAILED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 1.0f), &accent)) || !accent) return;
        if (FAILED(target_->CreateSolidColorBrush(material_.raisedStrong, &track)) || !track) return;
        EnsureRoundJoinStyle();

        // Wave strength eases toward 0 when paused and back to 1 when playing.
        {
            const bool fresh = progressLastTime_ >= 0.0 && (now - progressLastTime_) < 0.25;
            const float target = playing ? 1.0f : 0.0f;
            if (!fresh) {
                progressAmp_ = target;
            } else {
                const float dt = static_cast<float>(now - progressLastTime_);
                progressAmp_ += (target - progressAmp_) * (1.0f - std::exp(-dt / 0.16f));
                if (std::fabs(target - progressAmp_) < 0.004f) {
                    progressAmp_ = target;
                } else {
                    g_layoutDirty = true;
                }
            }
            progressLastTime_ = now;
        }
        const float amp = progressAmp_;

        auto drawEndDot = [&](float x) {
            ComPtr<ID2D1SolidColorBrush> dot;
            if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.9f), &dot)) && dot) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 1.7f, 1.7f), dot.Get());
            }
        };

        if (style == ProgressStyle::Bar) {
            // Thick rounded bar split by a vertical line thumb, with a dot at the end.
            const float half = dragging ? 4.5f : 4.0f;
            const float gap = dragging ? 5.0f : 4.0f;
            const float fillRight = thumbX - gap;
            const float trackLeft = thumbX + gap;
            if (fillRight - barLeft > 1.0f) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(barLeft, cy - half, fillRight, cy + half), half, half),
                    accent.Get());
            }
            if (barRight - trackLeft > 1.0f) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(trackLeft, cy - half, barRight, cy + half), half, half),
                    track.Get());
                drawEndDot(barRight - half);
            }
            const float tw = dragging ? 4.0f : 3.0f;
            const float th = dragging ? 24.0f : 20.0f;
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(thumbX - tw * 0.5f, cy - th * 0.5f, thumbX + tw * 0.5f, cy + th * 0.5f),
                                  tw * 0.5f, tw * 0.5f),
                accent.Get());
            return;
        }

        // Wavy and Squiggle: a travelling sine wave up to the thumb, a thin flat
        // line after it. Squiggle is shorter, shallower and about half the speed.
        const bool wavy = (style == ProgressStyle::Wavy);
        const float A = (wavy ? 3.2f : 1.8f) * amp;
        const float lambda = wavy ? 26.0f : 15.0f;
        const double freq = wavy ? 0.9 : 0.45;
        const float strokeW = wavy ? 3.4f : 3.0f;
        const float k = 6.2831853f / lambda;
        const float phase = static_cast<float>(std::fmod(now * freq, 1.0) * 6.2831853);
        const float endTaper = wavy ? 16.0f : 10.0f;  // flattens into the thumb

        if (thumbX - barLeft > 1.5f) {
            std::vector<D2D1_POINT_2F> pts;
            pts.reserve(static_cast<size_t>((thumbX - barLeft) / 1.25f) + 3);
            for (float x = barLeft;; x += 1.25f) {
                const float xx = std::min(x, thumbX);
                const float taper = std::min(SkipSmooth((xx - barLeft) / 10.0f),
                                             SkipSmooth((thumbX - xx) / endTaper));
                pts.push_back(D2D1::Point2F(xx, cy + A * taper * std::sin(k * (xx - barLeft) - phase)));
                if (x >= thumbX) break;
            }
            ComPtr<ID2D1PathGeometry> path;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&path)) && path) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(path->Open(&sink)) && sink) {
                    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                    if (pts.size() > 1) sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) {
                        target_->DrawGeometry(path.Get(), accent.Get(), strokeW, roundJoinStyle_.Get());
                    }
                }
            }
        }

        const float thumbR = dragging ? 7.0f : 5.5f;
        const float trackStart = thumbX + (wavy ? thumbR + 3.0f : 4.0f);
        if (barRight - trackStart > 2.0f) {
            const float tw = wavy ? 2.2f : 2.8f;
            target_->DrawLine(D2D1::Point2F(trackStart, cy), D2D1::Point2F(barRight - 2.0f, cy),
                              track.Get(), tw, roundJoinStyle_.Get());
            if (wavy) drawEndDot(barRight - 1.7f);
        }

        if (wavy) {
            if (dragging) {
                ComPtr<ID2D1SolidColorBrush> halo;
                if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.22f), &halo)) && halo) {
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbR * 2.0f, thumbR * 2.0f),
                                         halo.Get());
                }
            }
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbR, thumbR), accent.Get());
        } else {
            const float tw = dragging ? 4.0f : 3.0f;
            const float th = dragging ? 22.0f : 18.0f;
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(thumbX - tw * 0.5f, cy - th * 0.5f, thumbX + tw * 0.5f, cy + th * 0.5f),
                                  tw * 0.5f, tw * 0.5f),
                accent.Get());
        }
    }

    // ── Artist / album: gentle swap ──────────────────────────────────────────
    // The old line drifts up and fades while the new one rises into place. Only
    // runs when the line was already on screen; otherwise the text just appears.
    void DrawSwapMarquee(SwapTextState& st, const std::wstring& text, D2D1_RECT_F rect,
                         IDWriteTextFormat* format, ID2D1Brush* brush, double now, float speed,
                         MarqueeLayoutCache& cache, float delay) {
        constexpr float kDur = 0.38f;
        const bool recentlyDrawn = st.lastDrawn >= 0.0 && (now - st.lastDrawn) < 0.25;
        st.lastDrawn = now;

        if (text != st.shown) {
            if (recentlyDrawn && expandedAnim_) {
                st.prev = st.shown;
                st.start = now;
            } else {
                st.prev.clear();
                st.start = -1.0;
            }
            st.shown = text;
        } else if (st.start >= 0.0 && !recentlyDrawn) {
            st.start = -1.0;
        }

        const float elapsed = st.start >= 0.0 ? static_cast<float>(now - st.start) : -1.0f;
        if (elapsed < 0.0f || elapsed >= delay + kDur) {
            st.start = -1.0;
            DrawMarqueeText(text, rect, format, brush, now, speed, cache);
            return;
        }

        const float u = Clamp((elapsed - delay) / kDur, 0.0f, 1.0f);
        const float e = SkipEaseInOut(u);
        const float base = brush->GetOpacity();
        const float dy = 5.0f;
        D2D1_RECT_F up = rect;
        up.top -= dy * e;
        up.bottom -= dy * e;
        D2D1_RECT_F down = rect;
        down.top += dy * (1.0f - e);
        down.bottom += dy * (1.0f - e);

        brush->SetOpacity(base * (1.0f - SkipSmooth(u * 1.4f)));
        DrawMarqueeText(st.prev, up, format, brush, now, speed, st.prevCache);
        brush->SetOpacity(base * SkipSmooth((u - 0.25f) / 0.75f));
        DrawMarqueeText(text, down, format, brush, now, speed, cache);
        brush->SetOpacity(base);
        g_layoutDirty = true;
    }

    // ── Album art: flip ──────────────────────────────────────────────────────
    // The decoder assigns a fresh generation on every poll during the post-change
    // settle window even when the image is identical, so the flip is keyed on a
    // hash of the pixels instead and only fires when the picture really changed.
    uint64_t ArtContentHash(const MediaSnapshot& media) {
        if (!media.art) return 0;
        if (artHashValue_ != 0 && artHashGen_ == media.art->generation) return artHashValue_;
        uint64_t h = 1469598103934665603ull;
        h = (h ^ static_cast<uint64_t>(media.art->width)) * 1099511628211ull;
        h = (h ^ static_cast<uint64_t>(media.art->height)) * 1099511628211ull;
        const std::vector<uint8_t>& v = media.art->bgra;
        for (size_t i = 0; i < v.size(); i += 61) {
            h = (h ^ v[i]) * 1099511628211ull;
        }
        if (h == 0) h = 1;
        artHashGen_ = media.art->generation;
        artHashValue_ = h;
        return h;
    }

    void DrawArtFace(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius) {
        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        HRESULT hrMask = d2dFactory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &mask);
        ComPtr<ID2D1Layer> layer;
        HRESULT hrLayer = target_->CreateLayer(nullptr, &layer);
        const bool roundedClip = SUCCEEDED(hrMask) && SUCCEEDED(hrLayer) && mask && layer;
        if (roundedClip) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get()), layer.Get());
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        if (bitmap) {
            target_->DrawBitmap(bitmap, rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            accentBrush_->SetOpacity(0.24f);
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush_.Get());
            accentBrush_->SetOpacity(1.0f);
        }
        if (roundedClip) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
    }

    void DrawAlbumArtFlip(const MediaSnapshot& media, D2D1_RECT_F rect, double now,
                          float radius, bool drawBadge) {
        constexpr double kFlipDuration = 0.62;
        const bool recentlyDrawn = artFlipLastDrawn_ >= 0.0 && (now - artFlipLastDrawn_) < 0.25;
        artFlipLastDrawn_ = now;

        const uint64_t hash = ArtContentHash(media);
        if (!artFlipInit_ || !recentlyDrawn) {
            artFlipInit_ = true;
            artFlipHash_ = hash;
            artFlipStart_ = -1.0;
            artFlipOld_.Reset();
        } else if (hash != artFlipHash_) {
            // Mid-flip changes just retarget the face shown after the turn.
            if (expandedAnim_ && artFlipStart_ < 0.0 && hash != 0) {
                artFlipOld_ = artBitmap_;  // still the previous cover: not rebuilt yet
                artFlipStart_ = now;
            }
            artFlipHash_ = hash;
        }

        float p = -1.0f;
        if (artFlipStart_ >= 0.0) {
            p = static_cast<float>((now - artFlipStart_) / kFlipDuration);
            if (p >= 1.0f) {
                artFlipStart_ = -1.0;
                artFlipOld_.Reset();
                p = -1.0f;
            }
        }
        if (p < 0.0f) {
            DrawAlbumArt(media, rect, now, radius, drawBadge);
            return;
        }

        g_layoutDirty = true;
        const float q = SkipEaseInOut(p);
        const float c = std::fabs(std::cos(3.14159265f * q));  // 1 = facing us, 0 = edge-on
        if (c < 0.02f) return;

        const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                                   (rect.top + rect.bottom) * 0.5f);
        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        target_->SetTransform(D2D1::Matrix3x2F::Scale(c, 1.0f - 0.05f * (1.0f - c), center) * oldTransform);

        if (q < 0.5f) {
            DrawArtFace(artFlipOld_.Get(), rect, radius);
        } else {
            DrawAlbumArt(media, rect, now, radius, drawBadge);
        }

        // Darken the face as it turns away, for a sense of depth.
        ComPtr<ID2D1SolidColorBrush> shade;
        if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f * (1.0f - c)), &shade)) && shade) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), shade.Get());
        }
        target_->SetTransform(oldTransform);
    }

    // ── Album art (collapsed pill): record drop ──────────────────────────────
    // A track change plays as a record swap: the old disc spins up and is thrown
    // away while the new one drops in from above, spinning fast, then springs onto
    // the pill and slows to rest. Skipping backwards spins the other way. Same
    // cover, new track: a short press-and-rebound with the ring and glint.
    void EnsureArtBitmap(const MediaSnapshot& media) {
        if (!media.art) return;
        const BitmapPixels& art = *media.art;
        if (artGeneration_ != art.generation || !artBitmap_) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(art.width, art.height),
                                  art.bgra.data(), art.width * 4,
                                  &props, &artBitmap_);
            artGeneration_ = art.generation;
        }
    }

    // One disc: scaled about its centre, the picture spun inside a fixed round
    // mask. `lag` is how far the angle moved over the last few ms; it drives the
    // ghost copies drawn on top of the disc.
    void DrawArtDisc(ID2D1Bitmap* bitmap, D2D1_RECT_F rect, float radius,
                     float scale, float angle, float lag, float alpha) {
        if (alpha <= 0.004f || scale <= 0.02f) return;
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                              (rect.top + rect.bottom) * 0.5f);
        D2D1_MATRIX_3X2_F base;
        target_->GetTransform(&base);
        const D2D1_MATRIX_3X2_F scaled = D2D1::Matrix3x2F::Scale(scale, scale, c) * base;
        target_->SetTransform(scaled);

        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        ComPtr<ID2D1Layer> layer;
        const bool clipped =
            SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(rect, radius, radius), &mask)) &&
            SUCCEEDED(target_->CreateLayer(nullptr, &layer)) && mask && layer;
        if (clipped) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), Clamp(alpha, 0.0f, 1.0f)),
                               layer.Get());
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        const float speed = std::min(std::fabs(lag) / 16.0f, 1.0f);
        const int ghosts = (bitmap && speed > 0.05f) ? 3 : 0;
        for (int k = 0; k <= ghosts; ++k) {
            const float a = (k == 0) ? 1.0f : 0.30f * speed * (1.0f - 0.30f * static_cast<float>(k - 1));
            target_->SetTransform(
                D2D1::Matrix3x2F::Rotation(angle - lag * static_cast<float>(k), c) * scaled);
            if (bitmap) {
                target_->DrawBitmap(bitmap, rect, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                accentBrush_->SetOpacity(0.24f);
                target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush_.Get());
                accentBrush_->SetOpacity(1.0f);
            }
        }

        if (clipped) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
        target_->SetTransform(base);
    }

    // Accent ring leaving the rim + a soft glint sweeping across the disc.
    // ringP / sheenP: <0 or >1 = not running, otherwise progress 0..1.
    void DrawArtLandingFx(D2D1_RECT_F rect, float radius, float ringP, float sheenP, float dir) {
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                                              (rect.top + rect.bottom) * 0.5f);
        const float r0 = std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.5f;

        if (sheenP > 0.0f && sheenP < 1.0f) {
            const float e = SkipEaseInOut(sheenP);
            const float pos = (e * 2.0f - 1.0f) * 1.3f * r0;
            const float half = 0.55f * r0;
            const float dx = 0.82f * dir, dy = -0.57f;
            const float peak = 0.34f * std::sin(3.14159265f * sheenP);
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
                    coll.Get(), &glint)) && glint) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), glint.Get());
            }
        }

        for (int i = 0; i < 2; ++i) {
            const float p = (ringP - 0.16f * static_cast<float>(i)) / (1.0f - 0.16f * static_cast<float>(i));
            if (p <= 0.0f || p >= 1.0f) continue;
            const float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
            const float a = (i == 0 ? 0.60f : 0.28f) * (1.0f - p) * (1.0f - p);
            const float rr = r0 + 0.8f + 4.2f * e;
            accentBrush_->SetOpacity(a);
            target_->DrawEllipse(D2D1::Ellipse(c, rr, rr), accentBrush_.Get(), 0.6f + 1.4f * (1.0f - p));
        }
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawAlbumArtDrop(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius) {
        constexpr double kFullDur = 0.95;
        constexpr double kPulseDur = 0.55;
        constexpr double kTitleWait = 0.9;  // how long to wait for a new cover after a title change
        const bool recentlyDrawn = pillArtLastDrawn_ >= 0.0 && (now - pillArtLastDrawn_) < 0.25;
        pillArtLastDrawn_ = now;

        const uint64_t hash = ArtContentHash(media);
        if (!pillArtInit_ || !recentlyDrawn || !pillCoverAnim_) {
            // First frame, the pill was off screen, or the animation is turned off: snap.
            pillArtInit_ = true;
            pillArtHash_ = hash;
            pillTitle_ = media.title;
            pillArtStart_ = -1.0;
            pillTitlePendingAt_ = -1.0;
            pillArtOld_.Reset();
            pillArtLast_.Reset();
        } else {
            float dir = 1.0f;
            if (lastSkipClickAt_ >= 0.0 && (now - lastSkipClickAt_) < 6.0) dir = lastSkipDir_;

            if (hash != pillArtHash_) {
                // The picture really changed (the decoder re-issues generations
                // for identical covers, hence the hash).
                if (pillArtStart_ < 0.0 && hash != 0) {
                    pillArtOld_ = pillArtLast_;  // what was on screen last frame; may be empty
                    pillArtStart_ = now;
                    pillArtPulse_ = false;
                    pillArtDir_ = dir;
                    pillArtFullAt_ = now;
                    pillTitlePendingAt_ = -1.0;
                }
                pillArtHash_ = hash;
            }

            if (media.title != pillTitle_) {
                const bool hadTitle = !pillTitle_.empty();
                pillTitle_ = media.title;
                // Only arm the pulse if no drop is running or just finished; the
                // metadata often arrives before the new cover does.
                if (hadTitle && !media.title.empty() && pillArtStart_ < 0.0 &&
                    (now - pillArtFullAt_) > 1.6) {
                    pillTitlePendingAt_ = now;
                }
            }
            if (pillTitlePendingAt_ >= 0.0) {
                if (pillArtStart_ >= 0.0) {
                    pillTitlePendingAt_ = -1.0;
                } else if (now - pillTitlePendingAt_ >= kTitleWait) {
                    // No new cover showed up: same album, so do the gentle pulse.
                    pillTitlePendingAt_ = -1.0;
                    pillArtOld_.Reset();
                    pillArtStart_ = now;
                    pillArtPulse_ = true;
                    pillArtDir_ = dir;
                } else {
                    g_layoutDirty = true;  // keep frames coming while we wait
                }
            }
        }

        EnsureArtBitmap(media);
        ID2D1Bitmap* fresh = !media.art ? nullptr : artBitmap_.Get();

        float p = -1.0f;
        if (pillArtStart_ >= 0.0) {
            p = static_cast<float>((now - pillArtStart_) / (pillArtPulse_ ? kPulseDur : kFullDur));
            if (p >= 1.0f) {
                pillArtStart_ = -1.0;
                pillArtOld_.Reset();
                p = -1.0f;
            }
        }

        if (p < 0.0f) {
            DrawAlbumArt(media, rect, now, radius, false);
        } else {
            g_layoutDirty = true;
            const float dir = pillArtDir_;

            if (!pillArtPulse_) {
                // Old disc: spins up and is thrown away behind the new one.
                if (pillArtOld_) {
                    const float u = Clamp(p / 0.40f, 0.0f, 1.0f);
                    if (u < 1.0f) {
                        const float up = std::max(u - 0.058f, 0.0f);
                        const float k = u * u * u, kp = up * up * up;
                        DrawArtDisc(pillArtOld_.Get(), rect, radius,
                                    1.0f - 0.26f * k,
                                    dir * 150.0f * k,
                                    dir * 150.0f * (k - kp),
                                    1.0f - SkipSmooth(u / 0.9f));
                    }
                }
                // New disc: drops in big and fast-spinning, springs down, slows to rest.
                if (p > 0.14f) {
                    const float u = Clamp((p - 0.14f) / 0.86f, 0.0f, 1.0f);
                    const float up = std::max(u - 0.027f, 0.0f);
                    const float settle = 1.0f - SkipSmooth((u - 0.8f) / 0.2f);
                    const float scale = 1.0f + 0.30f * std::exp(-5.2f * u) * std::cos(7.5f * u) * settle;
                    const float angle = -dir * 220.0f * std::pow(1.0f - u, 3.0f);
                    const float anglePrev = -dir * 220.0f * std::pow(1.0f - up, 3.0f);
                    DrawArtDisc(fresh, rect, radius, scale, angle, angle - anglePrev,
                                SkipSmooth(u / 0.28f));
                }
                DrawArtLandingFx(rect, radius, (p - 0.32f) / 0.60f, (p - 0.34f) / 0.46f, dir);
            } else {
                // Same cover: press-and-rebound in place.
                auto wob = [](float x) { return std::exp(-5.5f * x) * std::sin(11.0f * x); };
                const float w = wob(p);
                const float wp = wob(std::max(p - 0.04f, 0.0f));
                DrawArtDisc(fresh, rect, radius, 1.0f - 0.20f * w, dir * 18.0f * w,
                            dir * 18.0f * (w - wp), 1.0f);
                DrawArtLandingFx(rect, radius, (p - 0.08f) / 0.62f, (p - 0.12f) / 0.55f, dir);
            }
        }

        if (fresh) {
            pillArtLast_ = artBitmap_;
        } else {
            pillArtLast_.Reset();
        }
    }

    // ── Audio spectrum styles ────────────────────────────────────────────────
    // DrawSpectrum is the single entry point used by the collapsed pill, the
    // expanded player and the Lyrics tab. It owns the track-change detection and
    // hands the animation clock to the selected style via specT_/specDir_.
    float SpecBandRange(const SharedState& state, float from, float to) const {
        // Average of the analyzer bands covering [from, to) in 0..kSpectrumBands.
        int a = std::clamp(static_cast<int>(std::floor(from)), 0, kSpectrumBands - 1);
        int b = std::clamp(static_cast<int>(std::ceil(to)), a + 1, kSpectrumBands);
        float sum = 0.0f;
        for (int i = a; i < b; ++i) sum += state.bands[static_cast<size_t>(i)];
        return sum / static_cast<float>(b - a);
    }

    void DrawSpectrum(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                      bool playing = true) {
        constexpr double kSpecDur = 1.15;
        const bool recentlyDrawn = specLastDrawn_ >= 0.0 && (now - specLastDrawn_) < 0.25;
        float dt = recentlyDrawn ? static_cast<float>(now - specLastDrawn_) : 0.016f;
        dt = Clamp(dt, 0.001f, 0.05f);
        specLastDrawn_ = now;

        const size_t key = std::hash<std::wstring>{}(state.media.title) * 31u +
                           std::hash<std::wstring>{}(state.media.artist);
        if (!specInit_ || !recentlyDrawn) {
            specInit_ = true;
            specKey_ = key;
            specStart_ = -1.0;
            if (!recentlyDrawn) {
                std::fill(std::begin(specPeak_), std::end(specPeak_), 0.0f);
                std::fill(std::begin(specPeakVel_), std::end(specPeakVel_), 0.0f);
                specBassSlow_ = 0.0f;
                specKick_ = 0.0f;
            }
        } else if (key != specKey_ && !state.media.title.empty()) {
            const bool running = specStart_ >= 0.0 && (now - specStart_) < 0.35;
            specKey_ = key;
            // Title and artist often land a few frames apart: absorb the second one.
            if (!running) {
                specStart_ = now;
                specDir_ = 1.0f;
                if (lastSkipClickAt_ >= 0.0 && (now - lastSkipClickAt_) < 6.0) specDir_ = lastSkipDir_;
                // Led: the old peaks rain down as the scanline reboots the matrix.
                for (float& v : specPeakVel_) v = 0.0f;
                for (int i = 0; i < kSpectrumBands; ++i) specPeak_[i] *= 0.6f;
            }
        }

        specT_ = -1.0f;
        if (specStart_ >= 0.0) {
            const float t = static_cast<float>((now - specStart_) / kSpecDur);
            if (t >= 1.0f) {
                specStart_ = -1.0;
            } else {
                specT_ = t;
                g_layoutDirty = true;
            }
        }

        // Low-end energy drives the orb's pulse and a beat "kick" envelope.
        const float bass = SpecBandRange(state, 0.0f, 4.0f);
        specBassSlow_ += (bass - specBassSlow_) * (1.0f - std::exp(-dt / 0.45f));
        const float beat = std::max(0.0f, bass - specBassSlow_ - 0.04f);
        specKick_ = std::max(specKick_ * std::exp(-dt / 0.18f), Clamp(beat * 3.2f, 0.0f, 1.0f));
        specBass_ += (bass - specBass_) * (1.0f - std::exp(-dt / 0.07f));
        specPhase_ += dt * (1.6f + 5.0f * specBass_ + 8.0f * specKick_);

        switch (settings.spectrumStyle) {
            case SpectrumStyle::Orb:    DrawSpectrumOrb(state, rect); break;
            case SpectrumStyle::Plasma: DrawSpectrumPlasma(state, rect); break;
            case SpectrumStyle::Led:    DrawSpectrumLed(state, rect, dt); break;
            default:                    DrawPlayingIndicator(rect, playing, now); break;
        }
    }

    // ── Pulse Orb ────────────────────────────────────────────────────────────
    void DrawSpectrumOrb(const SharedState& state, D2D1_RECT_F rect) {
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        const float R = std::min(w, h) * 0.5f + 1.0f;  // outer reach
        const float r0 = R * 0.40f;  // core radius at rest
        // Fewer, thinner spokes on a small orb so they never merge into a solid disc.
        const int kSpokes = R < 14.0f ? 16 : 24;
        const int half = kSpokes / 2;
        constexpr float kTwoPi = 6.28318531f;

        const float t = specT_;
        // Spokes are fixed (bass at the bottom, mirrored left/right) so the shape always
        // reads as a spectrum; only the track-change detonation spins the orb.
        float lenScale = 1.0f, coreScale = 1.0f, ringShrink = 0.0f, spin = 0.0f;
        float burstAlpha = 0.0f, burstR = 0.0f;
        if (t >= 0.0f) {
            if (t < 0.28f) {
                const float u = SkipSmooth(t / 0.28f);
                lenScale = 1.0f - u;
                coreScale = 1.0f - 0.35f * u;
                ringShrink = 0.30f * u;
            } else {
                const float u = Clamp((t - 0.28f) / 0.72f, 0.0f, 1.0f);
                const float spring = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
                lenScale = spring;
                coreScale = 1.0f + 0.55f * std::exp(-7.0f * u) * std::cos(6.0f * u);
                ringShrink = 0.30f * std::exp(-9.0f * u);
                spin += specDir_ * 4.2f * std::pow(1.0f - u, 3.0f);
                burstR = r0 + (R + 3.0f - r0) * SkipEaseInOut(u / 0.8f);
                burstAlpha = 0.75f * (1.0f - Clamp(u / 0.85f, 0.0f, 1.0f)) * (1.0f - Clamp(u / 0.85f, 0.0f, 1.0f));
            }
        }

        const float baseR = r0 * (1.0f - ringShrink);
        const float bassPulse = 0.18f * specBass_ + 0.22f * specKick_;

        EnsureRoundJoinStyle();
        const float strokeW = Clamp(kTwoPi * (baseR + 1.0f) / static_cast<float>(kSpokes) * 0.62f, 1.0f, R * 0.16f);

        // Thin shell ring just inside the spokes: breathes with the bass (no filled background).
        accentBrush_->SetOpacity(0.25f + 0.35f * specBass_ + 0.25f * specKick_);
        const float shellR = baseR * (0.90f + 0.18f * bassPulse);
        target_->DrawEllipse(D2D1::Ellipse(c, shellR, shellR), accentBrush_.Get(), 0.8f);

        for (int k = 0; k < kSpokes; ++k) {
            const int m = k < half ? k : kSpokes - 1 - k;  // mirrored 0..half-1
            const float lo = static_cast<float>(m) * kSpectrumBands / half;
            float v = SpecBandRange(state, lo, lo + static_cast<float>(kSpectrumBands) / half);
            v = Clamp(v * 1.1f, 0.04f, 1.0f);
            // Each spoke trails the detonation slightly so the burst reads as a wave.
            float ls = lenScale;
            if (t >= 0.28f) {
                const float u = Clamp((t - 0.28f - 0.012f * m) / 0.72f, 0.0f, 1.0f);
                ls = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
            }
            const float len = (R - baseR - 1.5f) * (0.10f + 0.90f * v) * Clamp(ls, 0.0f, 1.45f);
            // k = 0 and k = kSpokes-1 sit either side of the bottom (bass), highs meet at the top.
            const float a = spin + 1.5707963f + kTwoPi * (static_cast<float>(k) + 0.5f) / kSpokes;
            const float ca = std::cos(a), sa = std::sin(a);
            const float l = std::max(len, 0.6f);
            const D2D1_POINT_2F p0 = D2D1::Point2F(c.x + ca * (baseR + 1.0f), c.y + sa * (baseR + 1.0f));
            const D2D1_POINT_2F p1 = D2D1::Point2F(c.x + ca * (baseR + 1.0f + l), c.y + sa * (baseR + 1.0f + l));
            // Soft glow underlay, then the spoke itself.
            accentBrush_->SetOpacity(0.10f + 0.14f * v);
            target_->DrawLine(p0, p1, accentBrush_.Get(), strokeW * 2.0f, roundJoinStyle_.Get());
            accentBrush_->SetOpacity(0.45f + 0.55f * v);
            target_->DrawLine(p0, p1, accentBrush_.Get(), strokeW, roundJoinStyle_.Get());
        }

        // Core.
        accentBrush_->SetOpacity(0.95f);
        const float coreR = std::max(0.8f, baseR * 0.58f * coreScale * (1.0f + bassPulse));
        target_->FillEllipse(D2D1::Ellipse(c, coreR, coreR), accentBrush_.Get());
        ComPtr<ID2D1SolidColorBrush> hot;
        if (SUCCEEDED(target_->CreateSolidColorBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.65f), &hot)) && hot) {
            hot->SetOpacity(0.55f + 0.35f * specKick_);
            target_->FillEllipse(D2D1::Ellipse(c, coreR * 0.45f, coreR * 0.45f), hot.Get());
        }

        if (burstAlpha > 0.01f) {
            accentBrush_->SetOpacity(burstAlpha);
            target_->DrawEllipse(D2D1::Ellipse(c, burstR, burstR), accentBrush_.Get(), 0.8f + 1.4f * burstAlpha);
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Plasma Thread ────────────────────────────────────────────────────────
    void DrawSpectrumPlasma(const SharedState& state, D2D1_RECT_F rect) {
        constexpr int kPts = 34;
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float maxA = h * 0.46f;
        const float t = specT_;
        const float dir = specDir_;

        EnsureRoundJoinStyle();
        ComPtr<ID2D1SolidColorBrush> hot;
        target_->CreateSolidColorBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.6f), &hot);

        // Front position (normalised, already mirrored for "previous").
        const float front = t < 0.15f ? -0.4f : -0.4f + 1.9f * SkipEaseInOut((t - 0.15f) / 0.78f);

        for (int strand = 0; strand < 2; ++strand) {
            D2D1_POINT_2F pts[kPts];
            for (int i = 0; i < kPts; ++i) {
                const float xn = static_cast<float>(i) / (kPts - 1);
                const float xd = dir >= 0.0f ? xn : 1.0f - xn;  // distance along the sweep
                // max(): sin(pi) is slightly negative in float and pow(neg, 0.7) is NaN.
                float env = std::pow(std::max(0.0f, std::sin(3.14159265f * xn)), 0.7f);  // pinned at both ends
                float band = SpecBandRange(state, xn * (kSpectrumBands - 1), xn * (kSpectrumBands - 1) + 1.5f);
                band = Clamp(0.10f + 0.95f * band + 0.25f * specKick_, 0.0f, 1.25f);

                float ignite = 1.0f, boost = 0.0f;
                if (t >= 0.0f) {
                    if (t < 0.15f) {
                        ignite = 1.0f - SkipSmooth(t / 0.15f);
                    } else {
                        ignite = SkipSmooth((front - xd) / 0.22f + 0.15f);
                        const float g = (xd - front) / 0.16f;
                        boost = 0.95f * std::exp(-g * g);
                    }
                }
                const float ph = specPhase_ + (strand ? 3.14159265f : 0.0f);
                const float wave = std::sin(xn * (7.0f + 4.0f * band) - ph) * 0.65f +
                                   std::sin(xn * 13.0f + ph * 0.6f) * 0.35f;
                const float A = maxA * env * (band * ignite + boost * env);
                pts[i] = D2D1::Point2F(rect.left + xn * w, cy + wave * A * (strand ? -1.0f : 1.0f));
            }
            ComPtr<ID2D1PathGeometry> path;
            if (FAILED(d2dFactory_->CreatePathGeometry(&path)) || !path) continue;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(path->Open(&sink)) || !sink) continue;
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLines(pts + 1, kPts - 1);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close())) continue;

            // Glow underlay, body, hot core.
            accentBrush_->SetOpacity(strand ? 0.16f : 0.22f);
            target_->DrawGeometry(path.Get(), accentBrush_.Get(), 4.2f, roundJoinStyle_.Get());
            accentBrush_->SetOpacity(strand ? 0.62f : 0.95f);
            target_->DrawGeometry(path.Get(), accentBrush_.Get(), strand ? 1.1f : 1.6f, roundJoinStyle_.Get());
            if (hot && !strand) {
                hot->SetOpacity(0.35f + 0.4f * specKick_);
                target_->DrawGeometry(path.Get(), hot.Get(), 0.6f, roundJoinStyle_.Get());
            }
        }

        // Spark riding the wave front during the re-ignite.
        if (t >= 0.15f && hot) {
            const float xd = Clamp(front, 0.0f, 1.0f);
            const float xn = dir >= 0.0f ? xd : 1.0f - xd;
            const float a = 1.0f - Clamp((t - 0.80f) / 0.2f, 0.0f, 1.0f);
            if (front > -0.1f && front < 1.15f) {
                hot->SetOpacity(0.85f * a);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 2.2f, 2.2f), hot.Get());
                accentBrush_->SetOpacity(0.28f * a);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 5.5f, 5.5f), accentBrush_.Get());
            }
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Peak Matrix ──────────────────────────────────────────────────────────
    // Stacked LED cells per column with gravity-driven falling peak caps.
    void DrawSpectrumLed(const SharedState& state, D2D1_RECT_F rect, float dt) {
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const float colGap = 1.8f, colW = 2.6f;
        const int cols = std::clamp(static_cast<int>((w + colGap) / (colW + colGap)), 3, 14);
        const float cw = (w - colGap * (cols - 1)) / cols;
        const int rows = std::clamp(static_cast<int>(h / 3.6f), 4, 8);
        const float cellGap = 1.0f;
        const float ch = (h - cellGap * (rows - 1)) / rows;
        const float t = specT_;
        const float dir = specDir_;

        ComPtr<ID2D1SolidColorBrush> hot;
        target_->CreateSolidColorBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.7f), &hot);

        // Scan position in column units, mirrored for "previous".
        const float travel = static_cast<float>(cols) + 4.0f;
        const float scan = t < 0.0f ? -10.0f : -1.5f + travel * SkipEaseInOut(t / 0.82f);

        for (int c = 0; c < cols; ++c) {
            const int slot = dir >= 0.0f ? c : cols - 1 - c;  // order the scan reaches this column
            // Same source as Classic Bars: each column reads the level history, newest on the
            // right, so every column rises and falls with the music (no frequency tilt).
            const size_t wfSize = state.waveform.size();
            const size_t wfOffset = static_cast<size_t>(cols - c) * 3;
            const size_t wfSrc = (state.waveformWrite + wfSize * 2 - wfOffset) % wfSize;
            float v = Clamp(state.waveform[wfSrc], 0.03f, 1.0f);

            // Falling peak cap with gravity; a hit snaps it up.
            float& pk = specPeak_[c];
            float& pv = specPeakVel_[c];
            if (v >= pk) { pk = v; pv = 0.0f; }
            else { pv += 2.4f * dt; pk = std::max(v, pk - pv * dt); }

            float colAlpha = 1.0f, flash = 0.0f;
            if (t >= 0.0f) {
                const float d = scan - static_cast<float>(slot);  // >0: scanline already passed
                if (d < 0.0f) {
                    colAlpha = 0.35f;  // old song, about to be wiped
                } else {
                    flash = Clamp(1.0f - d / 3.2f, 0.0f, 1.0f);  // glowing tail behind the scanline
                    colAlpha = 0.35f + 0.65f * SkipSmooth(d / 1.2f);
                    if (d < 1.6f) { pk = std::min(pk, 0.0f + v); }      // peaks reset as the line passes
                }
            }

            const float x = rect.left + c * (cw + colGap);
            const int lit = static_cast<int>(std::round(v * rows));
            const int cap = std::clamp(static_cast<int>(std::round(pk * rows)) - 1, 0, rows - 1);
            for (int r = 0; r < rows; ++r) {
                const float yb = rect.bottom - r * (ch + cellGap);
                D2D1_RECT_F cell = D2D1::RectF(x, yb - ch, x + cw, yb);
                const float rr = std::min(cw, ch) * 0.35f;
                const float topness = rows > 1 ? static_cast<float>(r) / (rows - 1) : 0.0f;
                if (flash > 0.0f) {
                    // Whole column ignites, fading from the bottom up as the tail dies.
                    ID2D1SolidColorBrush* b = (hot && flash > 0.7f && r >= rows / 2) ? hot.Get() : accentBrush_.Get();
                    b->SetOpacity(Clamp(0.25f + 0.75f * flash, 0.0f, 1.0f) * (0.6f + 0.4f * (1.0f - topness)));
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), b);
                } else if (r < lit) {
                    accentBrush_->SetOpacity((0.50f + 0.45f * topness) * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accentBrush_.Get());
                } else if (r == cap && cap >= lit && pk > 0.05f && hot) {
                    hot->SetOpacity(0.9f * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), hot.Get());
                } else {
                    accentBrush_->SetOpacity(0.09f * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accentBrush_.Get());
                }
            }
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Brightness banner (shares the Volume banner's look) ──────────────────
    void DrawLevelBanner(D2D1_RECT_F rect, const wchar_t* glyph, const std::wstring& label,
                         const wchar_t* value, float pct, bool drained) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), raisedStrongBrush_.Get());

        textBrush_->SetOpacity(0.95f);

        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        const float tx = badge.right + 14;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 13.0f, rect.right - 58, cy + 3.0f);
        mutedBrush_->SetOpacity(0.50f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F valueRect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), smallTextFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
        if (drained) {
            ComPtr<ID2D1SolidColorBrush> trackBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f, 2.5f), trackBrush.Get());
            }
            D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                           track.left + (track.right - track.left) * pct,
                                           track.bottom);
            ComPtr<ID2D1SolidColorBrush> dim;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.textSecondary, 0.35f), &dim)) && dim) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.5f, 2.5f), dim.Get());
            }
        } else {
            DrawAccentTrack(track, pct, 2.5f);
        }
        accentBrush_->SetOpacity(1.0f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawBrightness(const SharedState& state, D2D1_RECT_F rect) {
        wchar_t value[32] = {};
        swprintf_s(value, L"%d%%", state.brightness.percent);
        DrawLevelBanner(rect, L"\uE706", Loc(L"Brightness"), value,
                        Clamp(state.brightness.percent / 100.0f, 0.0f, 1.0f), false);
    }

    void DrawAlbumArt(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius = 9.0f, bool drawBadge = true) {
        ComPtr<ID2D1Geometry> mask = GetShapeGeometry(rect, radius, false);
        ID2D1Layer* layer = AcquireLayer();
        const bool roundedClip = mask && layer;
        if (roundedClip) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get()), layer);
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        if (media.art) {
            EnsureArtBitmap(media);

            D2D1_RECT_F dst = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
            target_->DrawBitmap(artBitmap_.Get(), dst, 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            accentBrush_->SetOpacity(0.24f);
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush_.Get());
            accentBrush_->SetOpacity(1.0f);
            if (media.sourceIcon) {
                D2D1_RECT_F iconRect = D2D1::RectF(rect.left + 11, rect.top + 11,
                                                  rect.right - 11, rect.bottom - 11);
                DrawBitmapPixels(*media.sourceIcon, iconRect, mediaSourceIconBitmap_,
                                 mediaSourceIconGeneration_, 0.95f);
            } else {
                target_->DrawTextW(media.sourceBadge.empty() ? L"\u25b6" : media.sourceBadge.c_str(),
                                   static_cast<UINT32>(media.sourceBadge.empty() ? 1 : media.sourceBadge.size()),
                                   textFormat_.Get(), rect, textBrush_.Get());
            }
        }

        if (drawBadge && media.sourceIcon && artBadgeAlpha_ > 0.01f) {
            D2D1_RECT_F badge = D2D1::RectF(rect.right - 24, rect.bottom - 22,
                                           rect.right - 3, rect.bottom - 3);
            DrawCircularBitmapPixels(*media.sourceIcon,
                                     D2D1::Point2F((badge.left + badge.right) * 0.5f,
                                                   (badge.top + badge.bottom) * 0.5f),
                                     9.5f, mediaSourceIconBitmap_,
                                     mediaSourceIconGeneration_, 0.98f * artBadgeAlpha_);
        }

        if (roundedClip) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
    }

    void DrawBitmapPixels(const BitmapPixels& pixels, D2D1_RECT_F rect,
                          ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                          float opacity = 1.0f) {
        if (pixels.bgra.empty()) {
            return;
        }

        if (cachedGeneration != pixels.generation || !cache) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height),
                                  pixels.bgra.data(), pixels.width * 4,
                                  &props, &cache);
            cachedGeneration = pixels.generation;
        }

        if (cache) {
            target_->DrawBitmap(cache.Get(), rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    // Draws a bitmap filling a rounded-rect badge with a small inset for polish.
    void DrawRoundedBitmapPixels(const BitmapPixels& pixels, D2D1_RECT_F badge,
                                 float cornerRadius,
                                 ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                                 float opacity = 1.0f) {
        if (pixels.bgra.empty()) return;

        // 2px inset so the icon has clean edges inside the badge.
        const float pad = 2.0f;
        D2D1_RECT_F iconRect = D2D1::RectF(badge.left + pad, badge.top + pad,
                                           badge.right - pad, badge.bottom - pad);
        const float innerR = std::max(0.0f, cornerRadius - pad);

        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        d2dFactory_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(iconRect, innerR, innerR), &mask);
        ComPtr<ID2D1Layer> layer;
        target_->CreateLayer(nullptr, &layer);

        if (mask && layer) {
            target_->PushLayer(
                D2D1::LayerParameters(iconRect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                layer.Get());
            DrawBitmapPixels(pixels, iconRect, cache, cachedGeneration, opacity);
            target_->PopLayer();
        } else {
            DrawBitmapPixels(pixels, iconRect, cache, cachedGeneration, opacity);
        }
    }

    // Like DrawRoundedBitmapPixels, but center-crops (cover-fit) instead of
    // stretching, so a non-square clipboard image isn't squashed into the
    // square badge.
    void DrawCoverFitBitmapPixels(const BitmapPixels& pixels, D2D1_RECT_F badge,
                                  float cornerRadius,
                                  ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                                  float opacity = 1.0f) {
        if (pixels.bgra.empty() || !pixels.width || !pixels.height) return;

        const float pad = 2.0f;
        D2D1_RECT_F iconRect = D2D1::RectF(badge.left + pad, badge.top + pad,
                                           badge.right - pad, badge.bottom - pad);
        const float innerR = std::max(0.0f, cornerRadius - pad);
        const float boxW = iconRect.right - iconRect.left;
        const float boxH = iconRect.bottom - iconRect.top;
        if (boxW <= 0.0f || boxH <= 0.0f) return;

        const float srcAspect = static_cast<float>(pixels.width) / static_cast<float>(pixels.height);
        const float boxAspect = boxW / boxH;
        float drawW = boxW;
        float drawH = boxH;
        if (srcAspect > boxAspect) {
            drawH = boxH;
            drawW = boxH * srcAspect;
        } else {
            drawW = boxW;
            drawH = boxW / srcAspect;
        }
        const float cx = (iconRect.left + iconRect.right) * 0.5f;
        const float cy = (iconRect.top + iconRect.bottom) * 0.5f;
        D2D1_RECT_F drawRect = D2D1::RectF(cx - drawW * 0.5f, cy - drawH * 0.5f,
                                           cx + drawW * 0.5f, cy + drawH * 0.5f);

        ComPtr<ID2D1RoundedRectangleGeometry> mask;
        d2dFactory_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(iconRect, innerR, innerR), &mask);
        ComPtr<ID2D1Layer> layer;
        target_->CreateLayer(nullptr, &layer);

        if (mask && layer) {
            target_->PushLayer(
                D2D1::LayerParameters(iconRect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                layer.Get());
            DrawBitmapPixels(pixels, drawRect, cache, cachedGeneration, opacity);
            target_->PopLayer();
        } else {
            DrawBitmapPixels(pixels, drawRect, cache, cachedGeneration, opacity);
        }
    }

    void DrawCircularBitmapPixels(const BitmapPixels& pixels, D2D1_POINT_2F center, float radius,
                                  ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                                  float opacity = 1.0f) {
        if (pixels.bgra.empty()) {
            return;
        }

        D2D1_RECT_F rect = D2D1::RectF(center.x - radius, center.y - radius,
                                      center.x + radius, center.y + radius);
        ComPtr<ID2D1EllipseGeometry> ellipse;
        d2dFactory_->CreateEllipseGeometry(D2D1::Ellipse(center, radius, radius), &ellipse);
        ComPtr<ID2D1Layer> layer;
        target_->CreateLayer(nullptr, &layer);

        if (ellipse && layer) {
            target_->PushLayer(D2D1::LayerParameters(
                                  rect, ellipse.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                              layer.Get());
            DrawBitmapPixels(pixels, rect, cache, cachedGeneration, opacity);
            target_->PopLayer();
        } else {
            DrawBitmapPixels(pixels, rect, cache, cachedGeneration, opacity);
        }

        ComPtr<ID2D1SolidColorBrush> border;
        target_->CreateSolidColorBrush(material_.hairline, &border);
        if (border) {
            target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), border.Get(), 1.0f);
        }
    }

    void DrawMarqueeText(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                         ID2D1Brush* brush, double now, float speed, MarqueeLayoutCache& cache) {
        if (!format || !brush || text.empty()) {
            return;
        }

        const float wrapHeight = rect.bottom - rect.top;
        // Only rebuild the layout when text/format/height actually changed.
        // Scroll offset is applied via the translated draw origin below, so
        // it never invalidates the cache — this is what lets the marquee
        // scroll every frame without calling CreateTextLayout every frame.
        if (cache.text != text || cache.format != format ||
            std::fabs(cache.wrapWidth - wrapHeight) > 0.01f || !cache.layout) {
            cache.layout.Reset();
            dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                             format, 2000.0f, wrapHeight, &cache.layout);
            cache.text = text;
            cache.format = format;
            cache.wrapWidth = wrapHeight;
            cache.metrics = {};
            if (cache.layout) {
                cache.layout->GetMetrics(&cache.metrics);
            }
        }

        if (!cache.layout) {
            return;
        }

        const float available = rect.right - rect.left;

        D2D1_RECT_F clipRect = rect;
        clipRect.top -= 10.0f;
        clipRect.bottom += 10.0f;
        target_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (cache.metrics.widthIncludingTrailingWhitespace <= available) {
            target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), cache.layout.Get(), brush,
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        } else {
            const float cycle = cache.metrics.widthIncludingTrailingWhitespace + 38.0f;
            const float offset = std::fmod(static_cast<float>(now) * speed, cycle);
            target_->DrawTextLayout(D2D1::Point2F(rect.left - offset, rect.top), cache.layout.Get(),
                                    brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            target_->DrawTextLayout(D2D1::Point2F(rect.left - offset + cycle, rect.top),
                                    cache.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        target_->PopAxisAlignedClip();
    }

    float PlayingIndicatorLevel(int bar, double now) const {
        const double t = std::fmod(now, 3600.0);
        const double slow = std::sin(6.283185307179586 * (0.62 + 0.09 * bar) * t + 1.9 * bar);
        const double fast = std::sin(6.283185307179586 * (1.03 + 0.05 * bar) * t + 0.7 * bar * bar);
        const float wave = static_cast<float>(0.6 * slow + 0.4 * fast);
        return SmoothStep01(0.5f + 0.5f * wave);
    }

    float PlayingIndicatorGlow(double now) const {
        const float breath = 0.5f + 0.5f * static_cast<float>(std::sin(6.283185307179586 * 0.45 * std::fmod(now, 3600.0)));
        return 0.22f + 0.14f * breath;
    }

    void DrawPlayingIndicator(D2D1_RECT_F rect, bool playing, double now) {
        const bool recentlyDrawn = indicatorLastDrawn_ >= 0.0 && (now - indicatorLastDrawn_) < 0.25;
        const float dt = recentlyDrawn ? Clamp(static_cast<float>(now - indicatorLastDrawn_), 0.0f, 0.05f) : 0.0f;
        indicatorLastDrawn_ = now;
        const float target = playing ? 1.0f : 0.0f;
        if (!recentlyDrawn) {
            indicatorMix_ = target;
        } else {
            indicatorMix_ += (target - indicatorMix_) * (1.0f - std::exp(-dt / 0.22f));
            if (std::fabs(target - indicatorMix_) < 0.002f) {
                indicatorMix_ = target;
            } else {
                g_layoutDirty = true;
            }
        }

        const float gap = 2.5f;
        const float dot = 2.4f;
        const float availableW = rect.right - rect.left;
        const int count = std::clamp(static_cast<int>((availableW + gap) / (2.0f + gap)), 1, 32);
        const float slotWidth = (availableW - gap * (count - 1)) / count;
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        const float maxH = (rect.bottom - rect.top) * 0.78f;
        const float mix = SmoothStep01(indicatorMix_);
        const float width = dot + (slotWidth - dot) * mix;
        const float t = specT_;

        for (int i = 0; i < count; ++i) {
            const float level = 0.28f + 0.72f * PlayingIndicatorLevel(i, now);
            float h = dot + (std::max(width, level * maxH) - dot) * mix;
            if (t >= 0.0f && count > 1 && playing) {
                float f = static_cast<float>(i) / static_cast<float>(count - 1);
                if (specDir_ < 0.0f) f = 1.0f - f;
                const float u = Clamp((t - 0.34f * f) / 0.66f, 0.0f, 1.0f);
                const float dip = std::sin(3.14159265f * u);
                h = std::max(width, h * (1.0f - 0.6f * dip * dip));
            }

            const float cx = rect.left + i * (slotWidth + gap) + slotWidth * 0.5f;
            const D2D1_RECT_F bar = D2D1::RectF(cx - width * 0.5f, centerY - h * 0.5f,
                                                cx + width * 0.5f, centerY + h * 0.5f);
            const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(bar, width * 0.5f, width * 0.5f);
            if (mix < 0.999f) {
                mutedBrush_->SetOpacity(0.5f * (1.0f - mix));
                target_->FillRoundedRectangle(shape, mutedBrush_.Get());
            }
            if (mix > 0.001f) {
                accentBrush_->SetOpacity(mix * (0.6f + 0.3f * level));
                target_->FillRoundedRectangle(shape, accentBrush_.Get());
            }
        }
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawCountdownProgress(float left, float right, float bottom, float progress) {
        if (!g_settings.statusCountdownProgress) return;
        const float h = 2.5f;
        D2D1_RECT_F track = D2D1::RectF(left, bottom - h, right, bottom);
        ComPtr<ID2D1SolidColorBrush> trackBrush;
        target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), &trackBrush);
        target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f, 1.25f), trackBrush.Get());
        D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                       track.left + (track.right - track.left) * Clamp(progress, 0.0f, 1.0f),
                                       track.bottom);
        accentBrush_->SetOpacity(0.70f);
        target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 1.25f, 1.25f), accentBrush_.Get());
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawClipboard(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 40.0f || rect.right - rect.left < 100.0f) return;
        const double now = NowSeconds();
        const float ttl = 2.5f;
        const float remaining = Clamp(static_cast<float>(state.clipboard.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14.0f, cy - badgeSz * 0.5f,
                                        rect.left + 14.0f + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        if (state.clipboard.image && state.clipboard.imagePreview) {
            DrawCoverFitBitmapPixels(*state.clipboard.imagePreview,
                                     badge, br,
                                     clipboardImageBitmap_,
                                     clipboardImageGeneration_, 1.0f);
        } else if (state.clipboard.appIcon) {
            DrawRoundedBitmapPixels(*state.clipboard.appIcon,
                                    badge, br,
                                    clipboardIconBitmap_,
                                    clipboardIconGeneration_, 0.96f);
        } else {
            const wchar_t* glyph = state.clipboard.image
                ? (usingFluentIcons_ ? L"\uE91B" : L"\uE114")
                : (usingFluentIcons_ ? L"\uF0E3" : L"\uE8C8");
            textBrush_->SetOpacity(0.95f);

            if (iconFormat_) {
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target_->DrawTextW(glyph,
                                   static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            textBrush_->SetOpacity(0.90f);
        }

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        D2D1_RECT_F titleRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        mutedBrush_->SetOpacity(0.48f);
        const std::wstring clipTitle =
            state.clipboard.appName.empty()
                ? (state.clipboard.image ? std::wstring(L"Image copied") : std::wstring(L"Clipboard"))
                : state.clipboard.appName + L"  \u00b7  Clipboard";
        target_->DrawTextW(clipTitle.c_str(), static_cast<UINT32>(clipTitle.size()),
                           smallTextFormat_.Get(), titleRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F textRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        DrawMarqueeText(state.clipboard.text.empty() ? std::wstring(Loc(L"Copied")) : state.clipboard.text,
                        textRect, textFormat_.Get(), textBrush_.Get(), now, 34.0f, marqueeClipboardCache_);

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawNotification(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 48.0f || rect.right - rect.left < 120.0f) return;
        const double now = NowSeconds();
        const float ttl = 4.0f;
        const float remaining = Clamp(static_cast<float>(state.notification.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        const float cy = (rect.top + rect.bottom) * 0.5f;
        // Apple DI: app icon is a large iOS-style rounded square.
        const float iconSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - iconSz * 0.5f,
                                        rect.left + 14 + iconSz, cy + iconSz * 0.5f);
        const float br = iconSz * 0.35f;  // Softer iOS superellipse-like squircle.

        // Icon background plate.
        ComPtr<ID2D1SolidColorBrush> plateBrush;
        target_->CreateSolidColorBrush(material_.raisedStrong, &plateBrush);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), plateBrush.Get());

        if (state.notification.icon) {
            DrawRoundedBitmapPixels(*state.notification.icon, badge, br,
                                    notificationIconBitmap_, notificationIconGeneration_, 1.0f);

            // Draw a red dot (badge) at the top-right of the app icon
            ComPtr<ID2D1SolidColorBrush> badgeColor;
            target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f), &badgeColor);

            const float dotR = iconSz * 0.13f;
            const float dotX = badge.right - dotR * 0.5f;
            const float dotY = badge.top + dotR * 0.5f;

            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR), badgeColor.Get());

            ComPtr<ID2D1SolidColorBrush> badgeBorder;
            target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &badgeBorder);
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR), badgeBorder.Get(), 0.9f);
        } else if (const wchar_t initial = notification_initial(state.notification.app); initial != 0 && boldTextFormat_) {
            const DWRITE_TEXT_ALIGNMENT text_alignment = boldTextFormat_->GetTextAlignment();
            const DWRITE_PARAGRAPH_ALIGNMENT paragraph_alignment = boldTextFormat_->GetParagraphAlignment();
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            boldTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textBrush_->SetOpacity(0.95f);
            target_->DrawTextW(&initial, 1, boldTextFormat_.Get(), badge, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            boldTextFormat_->SetTextAlignment(text_alignment);
            boldTextFormat_->SetParagraphAlignment(paragraph_alignment);
            textBrush_->SetOpacity(0.90f);
        } else {
            const wchar_t* glyph = usingFluentIcons_ ? L"\uEA8F" : L"\uE7E7";
            textBrush_->SetOpacity(0.95f);
            if (iconFormat_) {
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target_->DrawTextW(glyph,
                                   static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
            textBrush_->SetOpacity(0.90f);
        }

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        D2D1_RECT_F appRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        mutedBrush_->SetOpacity(0.75f);
        target_->DrawTextW(state.notification.app.c_str(),
                           static_cast<UINT32>(state.notification.app.size()),
                           smallTextFormat_.Get(), appRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F titleRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        textBrush_->SetOpacity(0.95f);
        DrawMarqueeText(state.notification.title.empty() ? L"Notification" : state.notification.title,
                        titleRect, textFormat_.Get(), textBrush_.Get(), now, 28.0f, marqueeNotificationCache_);
        textBrush_->SetOpacity(0.90f);

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.50f);
    }

    void DrawVolume(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const bool muted = state.volume.muted || state.volume.percent == 0;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f; // Softer squircle corners

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const wchar_t* glyph = muted ? L"\uE74F" : (usingFluentIcons_ ? L"\uE767" : L"\uE993");
        textBrush_->SetOpacity(0.95f);

        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        const float tx = badge.right + 14;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 13.0f, rect.right - 58, cy + 3.0f);
        mutedBrush_->SetOpacity(0.50f);
        const std::wstring deviceLabel =
            state.volume.deviceName.empty() ? std::wstring(Loc(L"Volume")) : state.volume.deviceName;
        target_->DrawTextW(deviceLabel.c_str(), static_cast<UINT32>(deviceLabel.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[32] = {};
        if (muted) {
            wcscpy_s(value, ARRAYSIZE(value), Loc(L"Muted"));
        } else {
            swprintf_s(value, L"%d%%", state.volume.percent);
        }
        D2D1_RECT_F valueRect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), smallTextFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
        // Shared accent track, so the volume bar matches the media scrubber.
        const float pct = Clamp(state.volume.displayPercent / 100.0f, 0.0f, 1.0f);
        if (muted) {
            // Muted still shows the level, just drained of colour.
            ComPtr<ID2D1SolidColorBrush> trackBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f, 2.5f), trackBrush.Get());
            }
            D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                           track.left + (track.right - track.left) * pct,
                                           track.bottom);
            ComPtr<ID2D1SolidColorBrush> dim;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.textSecondary, 0.35f), &dim)) && dim) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.5f, 2.5f), dim.Get());
            }
        } else {
            DrawAccentTrack(track, pct, 2.5f);
        }
        accentBrush_->SetOpacity(1.0f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawTimer(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const double now = NowSeconds();
        double remaining = state.timer.justFinished
            ? 0.0
            : (state.timer.running ? std::max(0.0, state.timer.endsAt - now)
                                    : state.timer.remainingAtPause);
        const int totalSec = state.timer.totalSeconds > 0 ? state.timer.totalSeconds : 1;
        const float progress = state.timer.justFinished
            ? 1.0f
            : Clamp(1.0f - static_cast<float>(remaining / totalSec), 0.0f, 1.0f);

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const float ringR = badgeSz * 0.30f;
        D2D1_POINT_2F ringCenter = D2D1::Point2F((badge.left + badge.right) * 0.5f,
                                                  (badge.top + badge.bottom) * 0.5f);
        ComPtr<ID2D1SolidColorBrush> ringTrack;
        target_->CreateSolidColorBrush(material_.raisedStrong, &ringTrack);
        target_->DrawEllipse(D2D1::Ellipse(ringCenter, ringR, ringR), ringTrack.Get(), 2.0f);

        ComPtr<ID2D1PathGeometry> geometry;
        d2dFactory_->CreatePathGeometry(&geometry);
        ComPtr<ID2D1GeometrySink> sink;
        geometry->Open(&sink);
        const float start = -3.14159265f * 0.5f;
        const float sweep = 2.0f * 3.14159265f * progress;
        const int segments = std::max(2, static_cast<int>(40 * progress));
        auto pointAt = [&](float a) {
            return D2D1::Point2F(ringCenter.x + std::cos(a) * ringR,
                                  ringCenter.y + std::sin(a) * ringR);
        };
        sink->BeginFigure(pointAt(start), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= segments; ++i) {
            sink->AddLine(pointAt(start + sweep * i / segments));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        ComPtr<ID2D1SolidColorBrush> ringFg;
        D2D1_COLOR_F fgColor = state.timer.isBreak
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f, 0.58f, 0.0f, 1.0f);
        if (state.timer.justFinished) {
            fgColor = D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f);
        }
        target_->CreateSolidColorBrush(fgColor, &ringFg);
        target_->DrawGeometry(geometry.Get(), ringFg.Get(), 2.4f);

        if (state.timer.active && !state.timer.running) {
            ComPtr<ID2D1SolidColorBrush> pauseBrush;
            target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &pauseBrush);
            const float h = ringR * 0.7f;
            target_->FillRectangle(
                D2D1::RectF(ringCenter.x - 2.6f, ringCenter.y - h * 0.5f,
                            ringCenter.x - 0.8f, ringCenter.y + h * 0.5f), pauseBrush.Get());
            target_->FillRectangle(
                D2D1::RectF(ringCenter.x + 0.8f, ringCenter.y - h * 0.5f,
                            ringCenter.x + 2.6f, ringCenter.y + h * 0.5f), pauseBrush.Get());
        }

        const float tx = badge.right + 14;
        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = state.timer.justFinished
            ? (state.timer.isBreak ? L"Break Complete" : L"Focus Complete")
            : (state.timer.isBreak ? L"Break Timer" : L"Focus Timer");
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[32] = {};
        int rem = static_cast<int>(std::ceil(remaining));
        swprintf_s(value, L"%d:%02d", rem / 60, rem % 60);
        D2D1_RECT_F valueRect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawCapsLock(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 110.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const wchar_t* glyph = nullptr;
        std::wstring label;
        bool isOn = false;

        if (state.capsLock.isNumEvent) {
            glyph = L"1";
            label = Loc(L"Num Lock");
            isOn = state.capsLock.numOn;
        } else {
            glyph = L"A";
            label = Loc(L"Caps Lock");
            isOn = state.capsLock.capsOn;
        }

        // Draw central bold keycap glyph, vertically and horizontally centered
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), clockFormat_.Get(), badge,
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Draw physical glowing status LED inside the keycap at top-right with padding
        ComPtr<ID2D1SolidColorBrush> ledBrush;
        D2D1_COLOR_F ledColor = isOn
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)   // Green glowing LED for ON
            : D2D1::ColorF(1.0f,  1.0f,  1.0f,  0.22f);  // Dim white for OFF
        target_->CreateSolidColorBrush(ledColor, &ledBrush);

        const float ledR = 2.2f;
        D2D1_POINT_2F ledCenter = D2D1::Point2F(badge.right - 5.5f, badge.top + 5.5f);
        target_->FillEllipse(D2D1::Ellipse(ledCenter, ledR, ledR), ledBrush.Get());

        // Draw label text ("Caps Lock" / "Num Lock") - increased font size and vertically centered
        const float tx = badge.right + 14.0f;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 9.0f, rect.right - 46.0f, cy + 11.0f);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           textFormat_.Get(), labelRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Draw status string (ON/OFF) - increased font size and vertically centered
        std::wstring status = isOn ? Loc(L"On") : Loc(L"Off");
        D2D1_RECT_F statusRect = D2D1::RectF(rect.right - 44.0f, cy - 9.0f, rect.right - 14.0f, cy + 11.0f);
        if (isOn) {
            textBrush_->SetOpacity(0.95f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void DrawDevice(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 100.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const bool connected = (state.device.eventType == DeviceEventType::Connected);

        // Badge circle with colored dot
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const wchar_t* glyph = usingFluentIcons_ ? L"\uECF0" : L"\uE88E";
        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        textBrush_->SetOpacity(0.90f);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        D2D1_COLOR_F dotColor = connected
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f,  0.27f, 0.22f, 1.0f);
        target_->CreateSolidColorBrush(dotColor, &dotBrush);

        D2D1_POINT_2F dotCenter = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
        target_->FillEllipse(D2D1::Ellipse(dotCenter, 4.5f, 4.5f), dotBrush.Get());

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        const double now = NowSeconds();
        const float ttl = 3.0f;
        const float remaining = Clamp(static_cast<float>(state.device.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = connected ? L"Device Connected" : L"Device Removed";
        D2D1_RECT_F labelRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rect.right - 14.0f, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        textBrush_->SetOpacity(0.95f);
        const std::wstring& name = state.device.deviceName.empty()
            ? (state.device.isBluetoothLike ? std::wstring(L"Bluetooth") : std::wstring(L"USB Device"))
            : state.device.deviceName;
        D2D1_RECT_F nameRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rect.right - 14.0f, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                           textFormat_.Get(), nameRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawDoNotDisturb(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 110.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14.0f, cy - badgeSz * 0.5f,
                                        rect.left + 14.0f + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        const bool isOn = state.doNotDisturb.enabled;
        const wchar_t* glyph = isOn ? L"\uE7ED" : L"\uEA8F";
        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        ComPtr<ID2D1SolidColorBrush> ledBrush;
        D2D1_COLOR_F ledColor = isOn
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f,  1.0f,  1.0f,  0.22f);
        target_->CreateSolidColorBrush(ledColor, &ledBrush);

        const float ledR = 2.2f;
        D2D1_POINT_2F ledCenter = D2D1::Point2F(badge.right - 5.5f, badge.top + 5.5f);
        target_->FillEllipse(D2D1::Ellipse(ledCenter, ledR, ledR), ledBrush.Get());

        const float tx = badge.right + 14.0f;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 9.0f, rect.right - 46.0f, cy + 11.0f);
        textBrush_->SetOpacity(0.95f);
        std::wstring label = Loc(L"Do Not Disturb");
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           textFormat_.Get(), labelRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        std::wstring status = isOn ? Loc(L"On") : Loc(L"Off");
        D2D1_RECT_F statusRect = D2D1::RectF(rect.right - 44.0f, cy - 9.0f, rect.right - 14.0f, cy + 11.0f);
        if (isOn) {
            textBrush_->SetOpacity(0.95f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), textFormat_.Get(),
                               statusRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const double now = NowSeconds();
        const float ttl = 3.0f;
        const float remaining = Clamp(static_cast<float>(state.doNotDisturb.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;
        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 4.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
        textBrush_->SetOpacity(0.90f);
    }

    void DrawBluetoothCategoryIcon(D2D1_RECT_F badge, BluetoothDeviceCategory category) {
        const wchar_t* glyph = L"\uE702";
        switch (category) {
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

        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        textBrush_->SetOpacity(0.90f);
    }

    void DrawBluetoothDevice(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const double now = NowSeconds();
        const float ttl = 4.0f;
        const float remaining = Clamp(static_cast<float>(state.bluetoothDevice.expiresAt - now), 0.0f, ttl);
        const float progress = remaining / ttl;

        const bool connected = state.bluetoothDevice.connected;
        const int battery = state.bluetoothDevice.batteryPercent;
        const bool hasBattery = g_settings.bluetoothShowBattery && battery >= 0;

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        DrawBluetoothCategoryIcon(badge, state.bluetoothDevice.category);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        D2D1_COLOR_F dotColor = connected
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f, 0.27f, 0.22f, 1.0f);
        target_->CreateSolidColorBrush(dotColor, &dotBrush);
        D2D1_POINT_2F dotCenter = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
        target_->FillEllipse(D2D1::Ellipse(dotCenter, 4.5f, 4.5f), dotBrush.Get());

        const float tx = badge.right + 14.0f;
        const bool showBar = g_settings.statusCountdownProgress;
        const float rightEdge = hasBattery ? rect.right - 62.0f : rect.right - 14.0f;

        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = std::wstring(Loc(L"Bluetooth")) + L" " + (connected ? Loc(L"Connected") : Loc(L"Disconnected"));
        D2D1_RECT_F labelRect = showBar
            ? D2D1::RectF(tx, cy - 18.0f, rightEdge, cy - 2.0f)
            : D2D1::RectF(tx, cy - 16.0f, rightEdge, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        textBrush_->SetOpacity(0.95f);
        const std::wstring& name = state.bluetoothDevice.deviceName.empty()
            ? std::wstring(L"Bluetooth Device")
            : state.bluetoothDevice.deviceName;
        D2D1_RECT_F nameRect = showBar
            ? D2D1::RectF(tx, cy - 2.0f, rightEdge, cy + 15.0f)
            : D2D1::RectF(tx, cy - 1.0f, rightEdge, cy + 16.0f);
        target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                           textFormat_.Get(), nameRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (hasBattery) {
            const float colCenter = rect.right - 31.0f;
            const float bw = 16.0f;
            const float bh = 8.5f;
            const float nubW = 1.6f;
            const float totalW = bw + nubW;
            const float batLeft = colCenter - totalW * 0.5f;
            const float batY = showBar ? cy - 10.0f : cy - 9.0f;

            D2D1_RECT_F batRect = D2D1::RectF(batLeft, batY - bh * 0.5f,
                                              batLeft + bw, batY + bh * 0.5f);
            ComPtr<ID2D1SolidColorBrush> batBorder;
            target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.75f), &batBorder);
            target_->DrawRoundedRectangle(D2D1::RoundedRect(batRect, 1.5f, 1.5f), batBorder.Get(), 1.2f);
            D2D1_RECT_F nub = D2D1::RectF(batRect.right, batY - 2.0f, batRect.right + nubW, batY + 2.0f);
            target_->FillRectangle(nub, batBorder.Get());

            const float pct = Clamp(battery / 100.0f, 0.0f, 1.0f);
            D2D1_RECT_F fill = D2D1::RectF(batRect.left + 1.5f, batRect.top + 1.5f,
                                           batRect.left + 1.5f + (bw - 3.0f) * pct, batRect.bottom - 1.5f);
            ComPtr<ID2D1SolidColorBrush> fillBrush;
            D2D1_COLOR_F fillColor = battery <= 20 ? D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f)
                                                    : D2D1::ColorF(1, 1, 1, 0.92f);
            target_->CreateSolidColorBrush(fillColor, &fillBrush);
            target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 0.8f, 0.8f), fillBrush.Get());

            wchar_t pctBuf[16] = {};
            swprintf_s(pctBuf, L"%d%%", battery);
            D2D1_RECT_F pctRect = showBar
                ? D2D1::RectF(colCenter - 25.0f, cy - 2.0f, colCenter + 25.0f, cy + 14.0f)
                : D2D1::RectF(colCenter - 25.0f, cy - 1.0f, colCenter + 25.0f, cy + 15.0f);
            textBrush_->SetOpacity(0.85f);
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                target_->DrawTextW(pctBuf, static_cast<UINT32>(wcslen(pctBuf)), smallTextFormat_.Get(),
                                   pctRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }

        DrawCountdownProgress(tx, rect.right - 14.0f, rect.bottom - 6.0f, progress);
        mutedBrush_->SetOpacity(0.58f);
        textBrush_->SetOpacity(0.90f);
    }

    // ── Quick Lookup card ────────────────────────────────────────────────────
   public:
    // Content-space height the Lookup card needs for the current result. Called
    // from the render loop (like MeasureIdleStrip) so the island animates to
    // exactly the size DrawLookup will paint.
    float MeasureLookupCard(const SharedState& state, const Settings& settings) {
        const LookupSnapshot& lk = state.lookup;
        if (lk.status == LookupStatus::Search) {
            const std::vector<LookupRow> rows = BuildLookupRows(state.lookupRecent, g_lookupUi);
            return LookupLayout::SearchHeight(static_cast<int>(rows.size()),
                                              LookupHasHeader(g_lookupUi, rows));
        }
        if (lk.status == LookupStatus::Found) {
            const float card = LookupCardHeight(lk, EnsureLookupFit(lk, settings), LookupGrow(settings));
            return Clamp(card + LookupLayout::kFieldH + LookupLayout::kFieldGap,
                         LookupLayout::kSimpleHeight, LookupLayout::kMaxHeight);
        }
        return LookupLayout::StatusHeight();
    }

   private:
    // Body / example text after being fitted to its line cap, plus measured heights.
    struct LookupFit {
        std::wstring key;
        std::wstring body;
        std::wstring example;
        float bodyH = 0.0f;
        float exampleH = 0.0f;
        bool valid = false;
    };

    // Title / subtitle rows grow with the Text size setting (never shrink).
    float LookupGrow(const Settings& settings) const {
        return std::max(1.0f, Clamp(settings.textScale, 0.7f, 1.6f));
    }

    float LookupCardHeight(const LookupSnapshot& lk, const LookupFit& fit, float grow) const {
        float inner = fit.bodyH;
        if (!fit.example.empty()) {
            inner += LookupLayout::kExampleGap + fit.exampleH;
        }
        const float h = LookupLayout::kPadTop + LookupLayout::kTitleH * grow +
                        (lk.subtitle.empty() ? 2.0f : LookupLayout::kSubtitleH * grow) +
                        LookupLayout::kCardGap + inner + LookupLayout::kCardPad * 2.0f +
                        LookupLayout::kPadBottom;
        return Clamp(h, LookupLayout::kSimpleHeight, LookupLayout::kMaxHeight);
    }

    void EnsureLookupFormats(const Settings& settings) {
        const float ts = Clamp(settings.textScale, 0.7f, 1.6f);
        if (lookupBodyFormat_ && lookupExampleFormat_ &&
            std::fabs(lookupFormatScale_ - ts) < 0.001f && lookupFormatFamily_ == settings.fontFamily) {
            return;
        }
        lookupBodyFormat_.Reset();
        lookupExampleFormat_.Reset();
        lookupFit_.valid = false;

        const wchar_t* family =
            settings.fontFamily.empty() ? L"Segoe UI Variable Small" : settings.fontFamily.c_str();
        auto make = [&](DWRITE_FONT_STYLE style, float size, ComPtr<IDWriteTextFormat>& out) {
            HRESULT hr = dwriteFactory_->CreateTextFormat(
                family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, style, DWRITE_FONT_STRETCH_NORMAL,
                size, L"", &out);
            if (FAILED(hr) || !out) {
                out.Reset();
                dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 style, DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
            }
            if (out) {
                out->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        };
        make(DWRITE_FONT_STYLE_NORMAL, 12.0f * ts, lookupBodyFormat_);
        make(DWRITE_FONT_STYLE_ITALIC, 11.5f * ts, lookupExampleFormat_);
        lookupFormatScale_ = ts;
        lookupFormatFamily_ = settings.fontFamily;
    }

    // Wraps `text` to `width`; if it needs more than maxLines, cuts it at the end of
    // the last allowed line and ends it with an ellipsis. Reports the final height.
    std::wstring FitTextToLines(const std::wstring& text, IDWriteTextFormat* fmt, float width,
                                int maxLines, float* outHeight) {
        *outHeight = 0.0f;
        if (text.empty() || !fmt || !dwriteFactory_) {
            return text;
        }

        auto measure = [&](const std::wstring& s, UINT32* lines, float* height) -> bool {
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwriteFactory_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()),
                                                        fmt, width, 4096.0f, &layout)) || !layout) {
                return false;
            }
            DWRITE_TEXT_METRICS tm = {};
            if (FAILED(layout->GetMetrics(&tm))) {
                return false;
            }
            *lines = tm.lineCount;
            *height = tm.height;
            return true;
        };

        UINT32 lineCount = 0;
        float height = 0.0f;
        if (!measure(text, &lineCount, &height)) {
            return text;
        }
        if (static_cast<int>(lineCount) <= maxLines) {
            *outHeight = height;
            return text;
        }
        const float oneLine = lineCount > 0 ? height / static_cast<float>(lineCount) : 0.0f;

        // Where does the last allowed line end?
        size_t keep = text.size();
        {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                           fmt, width, 4096.0f, &layout)) && layout) {
                UINT32 n = 0;
                layout->GetLineMetrics(nullptr, 0, &n);
                std::vector<DWRITE_LINE_METRICS> lines(n);
                if (n > 0 && SUCCEEDED(layout->GetLineMetrics(lines.data(), n, &n))) {
                    size_t total = 0;
                    for (UINT32 i = 0; i < n && i < static_cast<UINT32>(maxLines); ++i) {
                        total += lines[i].length;
                    }
                    keep = std::min(total, text.size());
                }
            }
        }

        std::wstring cut = text.substr(0, keep);
        for (int attempt = 0; attempt < 60 && !cut.empty(); ++attempt) {
            while (!cut.empty() && (iswspace(cut.back()) || cut.back() == L',' || cut.back() == L';')) {
                cut.pop_back();
            }
            const std::wstring candidate = cut + L"\u2026";
            UINT32 lines = 0;
            float h = 0.0f;
            if (measure(candidate, &lines, &h) && static_cast<int>(lines) <= maxLines) {
                *outHeight = h;
                return candidate;
            }
            if (cut.empty()) {
                break;
            }
            cut.pop_back();
            if (!cut.empty() && IS_HIGH_SURROGATE(cut.back())) {
                cut.pop_back();
            }
        }
        *outHeight = oneLine;
        return L"\u2026";
    }

    // Cached: the text only changes when a new result arrives, so the layouts are
    // built once per result instead of once per frame.
    const LookupFit& EnsureLookupFit(const LookupSnapshot& lk, const Settings& settings) {
        EnsureLookupFormats(settings);
        const std::wstring key = lk.body + L"\x1F" + lk.example;
        if (lookupFit_.valid && lookupFit_.key == key) {
            return lookupFit_;
        }
        const float textW = LookupLayout::kWidth - LookupLayout::kPadX * 2.0f -
                            LookupLayout::kCardPad * 2.0f;
        LookupFit fit;
        fit.key = key;
        fit.body = FitTextToLines(lk.body, lookupBodyFormat_.Get(), textW,
                                  LookupLayout::kMaxBodyLines, &fit.bodyH);
        fit.example = FitTextToLines(lk.example, lookupExampleFormat_.Get(), textW,
                                     LookupLayout::kMaxExampleLines, &fit.exampleH);
        fit.valid = true;
        lookupFit_ = std::move(fit);
        return lookupFit_;
    }

    // ── Quick Lookup panel ───────────────────────────────────────────────────
    ComPtr<IDWriteTextLayout> MakeLineLayout(const std::wstring& text, IDWriteTextFormat* fmt,
                                             float maxWidth, DWRITE_TEXT_METRICS* metrics,
                                             bool ellipsis = false) {
        ComPtr<IDWriteTextLayout> layout;
        if (!fmt || !dwriteFactory_) return layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                    fmt, std::max(1.0f, maxWidth), 200.0f,
                                                    &layout)) || !layout) {
            layout.Reset();
            return layout;
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (ellipsis) {
            ComPtr<IDWriteInlineObject> sign;
            if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(layout.Get(), &sign)) && sign) {
                DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trimming, sign.Get());
            }
        }
        if (metrics) layout->GetMetrics(metrics);
        return layout;
    }

    ID2D1StrokeStyle* RoundStroke() {
        if (!roundStroke_ && d2dFactory_) {
            d2dFactory_->CreateStrokeStyle(
                D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                nullptr, 0, &roundStroke_);
        }
        return roundStroke_.Get();
    }

    // kind: 0 magnifier, 1 history clock, 2 clipboard, 3 return arrow.
    void DrawLookupGlyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush) {
        ID2D1StrokeStyle* round = RoundStroke();
        const float stroke = std::max(1.3f, 0.10f * s);
        auto P = [](float x, float y) { return D2D1::Point2F(x, y); };

        switch (kind) {
            case 0: {
                const float r = 0.30f * s;
                const D2D1_POINT_2F lens = P(c.x - 0.08f * s, c.y - 0.08f * s);
                target_->DrawEllipse(D2D1::Ellipse(lens, r, r), brush, stroke, round);
                target_->DrawLine(P(lens.x + r * 0.72f, lens.y + r * 0.72f),
                                  P(c.x + 0.40f * s, c.y + 0.40f * s), brush, stroke, round);
                break;
            }
            case 1: {
                const float r = 0.40f * s;
                target_->DrawEllipse(D2D1::Ellipse(c, r, r), brush, stroke, round);
                target_->DrawLine(P(c.x, c.y - 0.22f * s), c, brush, stroke, round);
                target_->DrawLine(c, P(c.x + 0.17f * s, c.y + 0.10f * s), brush, stroke, round);
                break;
            }
            case 2: {
                target_->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(c.x - 0.30f * s, c.y - 0.28f * s,
                                                  c.x + 0.30f * s, c.y + 0.40f * s),
                                      0.08f * s, 0.08f * s),
                    brush, stroke, round);
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(c.x - 0.13f * s, c.y - 0.40f * s,
                                                  c.x + 0.13f * s, c.y - 0.20f * s),
                                      0.05f * s, 0.05f * s),
                    brush);
                target_->DrawLine(P(c.x - 0.14f * s, c.y + 0.06f * s), P(c.x + 0.14f * s, c.y + 0.06f * s), brush, stroke * 0.85f, round);
                target_->DrawLine(P(c.x - 0.14f * s, c.y + 0.22f * s), P(c.x + 0.05f * s, c.y + 0.22f * s), brush, stroke * 0.85f, round);
                break;
            }
            default: {  // return arrow
                const D2D1_POINT_2F tip = P(c.x - 0.36f * s, c.y + 0.08f * s);
                target_->DrawLine(P(c.x + 0.34f * s, c.y - 0.28f * s), P(c.x + 0.34f * s, c.y + 0.08f * s), brush, stroke, round);
                target_->DrawLine(P(c.x + 0.34f * s, c.y + 0.08f * s), tip, brush, stroke, round);
                target_->DrawLine(tip, P(c.x - 0.14f * s, c.y - 0.12f * s), brush, stroke, round);
                target_->DrawLine(tip, P(c.x - 0.14f * s, c.y + 0.28f * s), brush, stroke, round);
                break;
            }
        }
    }

    // The search box: accent magnifier, text with a real caret, key hint on the right.
    void DrawLookupField(D2D1_RECT_F rect, double now) {
        const LookupUiState& ui = g_lookupUi;
        const D2D1_RECT_F field = D2D1::RectF(
            rect.left + LookupLayout::kPadX, rect.top + LookupLayout::kFieldTop,
            rect.right - LookupLayout::kPadX,
            rect.top + LookupLayout::kFieldTop + LookupLayout::kFieldH);
        DrawCard(field, 12.0f, true);
        const float midY = (field.top + field.bottom) * 0.5f;

        accentBrush_->SetOpacity(0.95f);
        DrawLookupGlyph(D2D1::Point2F(field.left + 19.0f, midY), 16.0f, 0, accentBrush_.Get());
        accentBrush_->SetOpacity(1.0f);

        // Key hint chip: "Esc" while empty, the return arrow once there is text.
        const D2D1_RECT_F chip = D2D1::RectF(field.right - 44.0f, midY - 10.0f,
                                             field.right - 12.0f, midY + 10.0f);
        ComPtr<ID2D1SolidColorBrush> chipFill;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.hairline, &chipFill)) && chipFill) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(chip, 6.0f, 6.0f), chipFill.Get());
        }
        mutedBrush_->SetOpacity(0.75f);
        if (ui.text.empty()) {
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                target_->DrawTextW(L"Esc", 3, smallTextFormat_.Get(), chip, mutedBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        } else {
            DrawLookupGlyph(D2D1::Point2F((chip.left + chip.right) * 0.5f, midY), 12.0f, 3,
                            mutedBrush_.Get());
        }

        const float areaLeft = field.left + 38.0f;
        const float areaRight = chip.left - 10.0f;
        const float areaW = std::max(8.0f, areaRight - areaLeft);
        float caretX = 0.0f;
        float scroll = 0.0f;

        target_->PushAxisAlignedClip(D2D1::RectF(areaLeft, field.top, areaRight, field.bottom),
                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DWRITE_TEXT_METRICS tm{};
        if (ui.text.empty()) {
            auto hint = MakeLineLayout(Loc(L"Search a word or phrase"), textFormat_.Get(), 4096.0f, &tm);
            if (hint) {
                mutedBrush_->SetOpacity(0.50f);
                target_->DrawTextLayout(D2D1::Point2F(areaLeft + 6.0f, midY - tm.height * 0.5f),
                                        hint.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        } else if (auto layout = MakeLineLayout(ui.text, textFormat_.Get(), 4096.0f, &tm)) {
            const UINT32 caretIdx = static_cast<UINT32>(std::min(ui.caret, ui.text.size()));
            float caretY = 0.0f;
            DWRITE_HIT_TEST_METRICS hm{};
            layout->HitTestTextPosition(caretIdx, FALSE, &caretX, &caretY, &hm);
            if (caretX > areaW - 6.0f) scroll = caretX - (areaW - 6.0f);  // keep the caret in view

            const float ox = areaLeft - scroll;
            if (ui.selectAll) {
                ComPtr<ID2D1SolidColorBrush> sel;
                if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.32f), &sel)) && sel) {
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(ox - 2.0f, midY - 10.0f,
                                                      ox + tm.widthIncludingTrailingWhitespace + 2.0f,
                                                      midY + 10.0f), 4.0f, 4.0f),
                        sel.Get());
                }
            }
            textBrush_->SetOpacity(0.98f);
            target_->DrawTextLayout(D2D1::Point2F(ox, midY - tm.height * 0.5f), layout.Get(),
                                    textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        if (!ui.selectAll && LookupCaretVisible(now, ui.caretResetAt)) {
            const float cx = areaLeft + 1.0f + caretX - scroll;
            accentBrush_->SetOpacity(0.95f);
            target_->DrawLine(D2D1::Point2F(cx, midY - 9.0f), D2D1::Point2F(cx, midY + 9.0f),
                              accentBrush_.Get(), 1.6f, RoundStroke());
            accentBrush_->SetOpacity(1.0f);
        }
        target_->PopAxisAlignedClip();
        mutedBrush_->SetOpacity(0.75f);
    }

    // Clipboard suggestion + recents (box empty) or Look up + matching recents (typing).
    void DrawLookupRows(D2D1_RECT_F rect, const std::vector<LookupRow>& rows, bool header) {
        const LookupUiState& ui = g_lookupUi;
        const float left = rect.left + LookupLayout::kPadX;
        const float right = rect.right - LookupLayout::kPadX;
        float y = rect.top + LookupLayout::kBelowField;

        if (rows.empty()) {
            DWRITE_TEXT_METRICS tm{};
            auto hint = MakeLineLayout(Loc(L"Type a word or phrase, then press Enter"),
                                       smallTextFormat_.Get(), right - left - 8.0f, &tm, true);
            if (hint) {
                mutedBrush_->SetOpacity(0.60f);
                target_->DrawTextLayout(D2D1::Point2F(left + 4.0f, y + (LookupLayout::kStatusH - tm.height) * 0.5f),
                                        hint.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            mutedBrush_->SetOpacity(0.75f);
            return;
        }

        if (header && smallTextFormat_) {
            const D2D1_RECT_F band = D2D1::RectF(left + 4.0f, y, right - 4.0f, y + LookupLayout::kSectionH);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            mutedBrush_->SetOpacity(0.55f);
            const wchar_t* title = Loc(L"Recent");
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), smallTextFormat_.Get(), band,
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (LookupHasRecentRows(rows)) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                const wchar_t* clear = Loc(L"Clear");
                target_->DrawTextW(clear, static_cast<UINT32>(wcslen(clear)), smallTextFormat_.Get(), band,
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            y += LookupLayout::kSectionH;
        }

        for (size_t i = 0; i < rows.size(); ++i) {
            const LookupRow& row = rows[i];
            const D2D1_RECT_F rr = D2D1::RectF(left, y, right, y + LookupLayout::kRowH);
            const bool selected = static_cast<int>(i) == ui.selected;
            const float midY = y + LookupLayout::kRowH * 0.5f;
            if (selected) {
                DrawCard(rr, 10.0f, false);
            }

            const int glyph = row.kind == LookupRowKind::Typed ? 0
                              : row.kind == LookupRowKind::Clipboard ? 2 : 1;
            ID2D1SolidColorBrush* glyphBrush = (row.kind == LookupRowKind::Recent) ? mutedBrush_.Get()
                                                                                    : accentBrush_.Get();
            glyphBrush->SetOpacity(row.kind == LookupRowKind::Recent ? 0.70f : 0.92f);
            DrawLookupGlyph(D2D1::Point2F(rr.left + 17.0f, midY), 14.0f, glyph, glyphBrush);
            glyphBrush->SetOpacity(row.kind == LookupRowKind::Recent ? 0.75f : 1.0f);

            const float x0 = rr.left + 34.0f;
            const float x1 = rr.right - (selected ? 34.0f : 10.0f);
            const float areaW = std::max(10.0f, x1 - x0);

            const std::wstring label = row.kind == LookupRowKind::Typed
                ? std::wstring(Loc(L"Look up")) + L" \u201c" + row.label + L"\u201d"
                : row.label;

            DWRITE_TEXT_METRICS lm{};
            MakeLineLayout(label, textFormat_.Get(), 4096.0f, &lm);  // natural width
            const float labelW = std::min(lm.widthIncludingTrailingWhitespace,
                                          areaW * (row.detail.empty() ? 1.0f : 0.62f));
            if (auto labelLayout = MakeLineLayout(label, textFormat_.Get(), labelW + 0.5f, &lm, true)) {
                textBrush_->SetOpacity(0.96f);
                target_->DrawTextLayout(D2D1::Point2F(x0, midY - lm.height * 0.5f), labelLayout.Get(),
                                        textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            if (!row.detail.empty()) {
                const float dx = x0 + labelW + 10.0f;
                const float dw = x1 - dx;
                if (dw > 40.0f) {
                    DWRITE_TEXT_METRICS dm{};
                    if (auto detail = MakeLineLayout(row.detail, smallTextFormat_.Get(), dw, &dm, true)) {
                        ID2D1SolidColorBrush* b = (row.kind == LookupRowKind::Clipboard) ? accentBrush_.Get()
                                                                                          : mutedBrush_.Get();
                        b->SetOpacity(row.kind == LookupRowKind::Clipboard ? 0.85f : 0.62f);
                        target_->DrawTextLayout(D2D1::Point2F(dx, midY - dm.height * 0.5f), detail.Get(), b,
                                                D2D1_DRAW_TEXT_OPTIONS_NONE);
                        b->SetOpacity(b == accentBrush_.Get() ? 1.0f : 0.75f);
                    }
                }
            }

            if (selected) {
                mutedBrush_->SetOpacity(0.80f);
                DrawLookupGlyph(D2D1::Point2F(rr.right - 17.0f, midY), 12.0f, 3, mutedBrush_.Get());
                mutedBrush_->SetOpacity(0.75f);
            }
            y += LookupLayout::kRowH + LookupLayout::kRowGap;
        }
        textBrush_->SetOpacity(0.96f);
    }

    // "Looking up..." / "No result found" under the box.
    void DrawLookupStatus(D2D1_RECT_F rect, const LookupSnapshot& lk) {
        const float left = rect.left + LookupLayout::kPadX + 4.0f;
        const float right = rect.right - LookupLayout::kPadX - 4.0f;
        const float y = rect.top + LookupLayout::kBelowField;
        const wchar_t* msg = lk.status == LookupStatus::Loading ? Loc(L"Looking up...")
                                                                : Loc(L"No result found");
        DWRITE_TEXT_METRICS tm{};
        if (auto layout = MakeLineLayout(msg, textFormat_.Get(), right - left, &tm, true)) {
            mutedBrush_->SetOpacity(0.75f);
            target_->DrawTextLayout(D2D1::Point2F(left, y + (LookupLayout::kStatusH - tm.height) * 0.5f),
                                    layout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    // The definition / summary card under the box (the old "Found" layout, shifted down).
    void DrawLookupResult(const SharedState& state, D2D1_RECT_F rect, const Settings& settings) {
        const LookupSnapshot& lk = state.lookup;
        const LookupFit& fit = EnsureLookupFit(lk, settings);
        const float grow = LookupGrow(settings);
        const float titleH = LookupLayout::kTitleH * grow;
        const float left = rect.left + LookupLayout::kPadX;
        const float right = rect.right - LookupLayout::kPadX;
        float y = rect.top + LookupLayout::kBelowField;

        constexpr float kTagW = 74.0f;
        textBrush_->SetOpacity(0.97f);
        target_->DrawTextW(lk.title.c_str(), static_cast<UINT32>(lk.title.size()),
                           textFormat_.Get(), D2D1::RectF(left, y, right - kTagW - 6.0f, y + titleH),
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!lk.source.empty() && smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            accentBrush_->SetOpacity(0.90f);
            target_->DrawTextW(lk.source.c_str(), static_cast<UINT32>(lk.source.size()),
                               smallTextFormat_.Get(),
                               D2D1::RectF(right - kTagW, y + 3.0f, right, y + titleH),
                               accentBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            accentBrush_->SetOpacity(1.0f);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        y += titleH;

        if (!lk.subtitle.empty()) {
            const float subH = LookupLayout::kSubtitleH * grow;
            mutedBrush_->SetOpacity(0.78f);
            target_->DrawTextW(lk.subtitle.c_str(), static_cast<UINT32>(lk.subtitle.size()),
                               smallTextFormat_.Get(), D2D1::RectF(left, y - 2.0f, right, y + subH - 2.0f),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += subH;
        } else {
            y += 2.0f;
        }
        y += LookupLayout::kCardGap;

        float inner = fit.bodyH;
        if (!fit.example.empty()) {
            inner += LookupLayout::kExampleGap + fit.exampleH;
        }
        const float pad = LookupLayout::kCardPad;
        const D2D1_RECT_F card = D2D1::RectF(left, y, right, y + inner + pad * 2.0f);
        DrawCard(card, 10.0f);

        float ty = card.top + pad;
        textBrush_->SetOpacity(0.92f);
        target_->DrawTextW(fit.body.c_str(), static_cast<UINT32>(fit.body.size()),
                           lookupBodyFormat_.Get(),
                           D2D1::RectF(card.left + pad, ty, card.right - pad, ty + fit.bodyH + 2.0f),
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        ty += fit.bodyH;

        if (!fit.example.empty()) {
            ty += LookupLayout::kExampleGap;
            mutedBrush_->SetOpacity(0.70f);
            target_->DrawTextW(fit.example.c_str(), static_cast<UINT32>(fit.example.size()),
                               lookupExampleFormat_.Get(),
                               D2D1::RectF(card.left + pad, ty, card.right - pad, ty + fit.exampleH + 2.0f),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void DrawLookup(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        const LookupSnapshot& lk = state.lookup;
        const float h = rect.bottom - rect.top;
        const float w = rect.right - rect.left;
        if (h < 24.0f || w < 100.0f) return;

        // Hit tests (clicks, hover, cursor) map through this rect.
        PublishContentGeometry(rect);

        const float need = MeasureLookupCard(state, settings);
        const float alpha = SmoothStep01((h - need * 0.55f) / (need * 0.45f));
        if (alpha <= 0.01f) return;  // island still too small; content fades in as it opens

        ComPtr<ID2D1Geometry> mask =
            CreateIslandMaskGeometry(rect, ContentIslandRadius(h), settings.notchStyle);
        ID2D1Layer* layer = AcquireLayer();
        const bool masked = mask && layer;
        if (masked) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), alpha, nullptr,
                                                      D2D1_LAYER_OPTIONS_NONE),
                               layer);
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        DrawLookupField(rect, now);

        if (lk.status == LookupStatus::Search) {
            const std::vector<LookupRow> rows = BuildLookupRows(state.lookupRecent, g_lookupUi);
            DrawLookupRows(rect, rows, LookupHasHeader(g_lookupUi, rows));
        } else if (lk.status == LookupStatus::Found) {
            DrawLookupResult(state, rect, settings);
        } else {
            DrawLookupStatus(rect, lk);
        }

        if (masked) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    ComPtr<ID2D1StrokeStyle> roundStroke_;
    ComPtr<IDWriteTextFormat> lookupBodyFormat_;
    ComPtr<IDWriteTextFormat> lookupExampleFormat_;
    float lookupFormatScale_ = -1.0f;
    std::wstring lookupFormatFamily_;
    LookupFit lookupFit_;

    void DrawBattery(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ComPtr<ID2D1SolidColorBrush> badgeBg;
        target_->CreateSolidColorBrush(material_.raisedStrong, &badgeBg);
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg.Get());

        // Draw battery vector icon
        const float bx = badge.left + badgeSz * 0.25f;
        const float by = cy - badgeSz * 0.22f;
        const float bw = badgeSz * 0.45f;
        const float bh = badgeSz * 0.44f;
        D2D1_RECT_F batRect = D2D1::RectF(bx, by, bx + bw, by + bh);

        ComPtr<ID2D1SolidColorBrush> batBorder;
        target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.85f), &batBorder);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(batRect, 2, 2), batBorder.Get(), 1.5f);

        // Battery Terminal (Nub)
        D2D1_RECT_F nubRect = D2D1::RectF(batRect.right, cy - 3, batRect.right + 2.5f, cy + 3);
        target_->FillRectangle(nubRect, batBorder.Get());

        // Battery Fill
        const float pct = Clamp(state.battery.percent / 100.0f, 0.0f, 1.0f);
        D2D1_RECT_F fillRect = D2D1::RectF(batRect.left + 2, batRect.top + 2,
                                           batRect.left + 2 + (bw - 4) * pct, batRect.bottom - 2);

        ComPtr<ID2D1SolidColorBrush> batFill;
        if (state.battery.low) {
            target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f), &batFill); // Red
        } else if (state.battery.charging) {
            target_->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f), &batFill); // Green
        } else {
            target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), &batFill); // White
        }
        target_->FillRoundedRectangle(D2D1::RoundedRect(fillRect, 1, 1), batFill.Get());

        // Text Labels
        const float tx = badge.right + 14;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = state.battery.charging ? L"Power Connected" : L"Battery Alert";
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[128] = {};
        if (state.battery.secondsRemaining != BATTERY_LIFE_UNKNOWN && !state.battery.charging) {
            const DWORD minutes = state.battery.secondsRemaining / 60;
            swprintf_s(value, ARRAYSIZE(value), L"%d%% \u2022 %luh %02lum left",
                       state.battery.percent, minutes / 60, minutes % 60);
        } else {
            swprintf_s(value, ARRAYSIZE(value), L"%d%%", state.battery.percent);
        }

        D2D1_RECT_F valueRect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);
    }

    void DrawProgress(const SharedState& state, D2D1_RECT_F rect) {
        wchar_t buffer[64] = {};
        swprintf_s(buffer, L"Progress %d%%", state.progress.percent);
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : textFormat_.Get();
        target_->DrawTextW(buffer, static_cast<UINT32>(wcslen(buffer)), fmt,
                           rect,
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawProgressRing(D2D1_RECT_F rect, int percent) {
        ComPtr<ID2D1PathGeometry> geometry;
        d2dFactory_->CreatePathGeometry(&geometry);
        ComPtr<ID2D1GeometrySink> sink;
        geometry->Open(&sink);

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float rx = (rect.right - rect.left) * 0.5f + 6.0f;
        const float ry = (rect.bottom - rect.top) * 0.5f + 6.0f;
        const float start = -3.14159265f * 0.5f;
        const float sweep = 2.0f * 3.14159265f * Clamp(percent / 100.0f, 0.0f, 1.0f);
        const int segments = std::max(2, static_cast<int>(48 * percent / 100.0f));

        auto pointAt = [&](float a) {
            return D2D1::Point2F(cx + std::cos(a) * rx, cy + std::sin(a) * ry);
        };

        sink->BeginFigure(pointAt(start), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= segments; ++i) {
            const float a = start + sweep * i / segments;
            sink->AddLine(pointAt(a));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        accentBrush_->SetOpacity(0.92f);
        target_->DrawGeometry(geometry.Get(), accentBrush_.Get(), 3.0f);
        accentBrush_->SetOpacity(1.0f);
    }

    HWND hwnd_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    int bitmapWidth_ = 0;     // logical size of the frame being drawn
    int bitmapHeight_ = 0;
    int bitmapCapW_ = 0;      // allocated size of the DIB (>= logical size)
    int bitmapCapH_ = 0;

    // Fixed-stage bookkeeping (see Render)
    POINT lastOrigin_ = {0, 0};
    RECT lastBounds_ = {0, 0, 0, 0};
    BYTE lastAlpha_ = 0;
    bool stageValid_ = false;
    double lastFullPaint_ = -1e9;

    // Pills drawn this frame, in client px, for the DWM backdrop windows (#59).
    D2D1_RECT_F pillRects_[DwmBackdrop::kMaxPills] = {};
    float pillRadii_[DwmBackdrop::kMaxPills] = {};
    int pillCount_ = 0;

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> smallTextFormat_;
    ComPtr<IDWriteTextFormat> boldTextFormat_;
    ComPtr<IDWriteTextFormat> hugeTextFormat_;
    ComPtr<IDWriteTextFormat> clockFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    ComPtr<IDWriteTextFormat> mediaPlayIconFormat_;
    ComPtr<IDWriteTextFormat> mediaNavIconFormat_;
    bool usingFluentIcons_ = true;
    ComPtr<IDWriteTextFormat> idleTextFormat_;
    ComPtr<IDWriteTextFormat> calDayLargeFormat_;
    ComPtr<IDWriteTextFormat> calGridFormat_;
    ComPtr<IDWriteTextFormat> timeDashboardFormat_;
    ComPtr<IDWriteTextFormat> dateDashboardFormat_;
    ComPtr<ID2D1SolidColorBrush> accentBrush_;
    ComPtr<ID2D1SolidColorBrush> redBrush_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    ComPtr<ID2D1SolidColorBrush> tintBrush_;
    ComPtr<ID2D1SolidColorBrush> shadowBrush_;
    ComPtr<ID2D1SolidColorBrush> micDotBrush_;
    ComPtr<ID2D1SolidColorBrush> unreadDotBrush_;
    ComPtr<ID2D1SolidColorBrush> micGlowBrush_;
    ComPtr<ID2D1SolidColorBrush> camDotBrush_;
    ComPtr<ID2D1SolidColorBrush> camGlowBrush_;
    ComPtr<ID2D1SolidColorBrush> scratchColorBrush_;
    MarqueeLayoutCache marqueeTitleCache_;
    MarqueeLayoutCache marqueeArtistCache_;
    MarqueeLayoutCache marqueeAlbumCache_;
    MarqueeLayoutCache marqueeClipboardCache_;
    MarqueeLayoutCache marqueeNotificationCache_;
    ComPtr<ID2D1Bitmap> artBitmap_;
    ComPtr<ID2D1Bitmap> notificationIconBitmap_;
    ComPtr<ID2D1Bitmap> mediaSourceIconBitmap_;
    ComPtr<ID2D1Bitmap> clipboardIconBitmap_;
    ComPtr<ID2D1Bitmap> clipboardImageBitmap_;
    uint64_t artGeneration_ = 0;
    SYSTEMTIME calendarCachedDate_{};
    std::wstring calendarCachedMonthName_;
    std::wstring calendarCachedWeekdayName_;
    std::array<std::wstring, 7> calendarCachedInitials_;  // indexed 0 = Sunday
    int calendarCachedFirstDay_ = 0;                       // resolved, 0 = Sunday
    int calendarCachedFirstDaySetting_ = -2;               // -2 forces the first fill
    ComPtr<IDWriteTextFormat> weatherDescFormat_;
    float weatherDescFormatSize_ = -1.0f;
    uint64_t notificationIconGeneration_ = 0;
    uint64_t mediaSourceIconGeneration_ = 0;
    uint64_t clipboardIconGeneration_ = 0;
    uint64_t clipboardImageGeneration_ = 0;
    ComPtr<ID2D1Bitmap> fileTrayIconBitmap_;
    uint64_t fileTrayIconGeneration_ = 0;
    std::array<ComPtr<ID2D1Bitmap>, notification_center::max_apps> notificationCenterBitmaps_;
    std::array<uint64_t, notification_center::max_apps> notificationCenterGenerations_{};
    std::size_t notificationCenterNextSlot_ = 0;

    // ── Lyrics tab state ─────────────────────────────────────────────────────
    KaraokeLayoutCache karaokeCache_;

    // ── Collapsed lyrics state ───────────────────────────────────────────────
    CollapsedLyricLine collapsedLyricCur_;
    CollapsedLyricLine collapsedLyricPrev_;
    int collapsedLyricIdx_ = -2;            // -2 = nothing built yet (forces a snap)
    std::wstring collapsedLyricTitle_;
    std::wstring collapsedLyricArtist_;
    double collapsedLyricAnimStart_ = -10.0;

    // ── Media-tab lyrics panel state ─────────────────────────────────────────
    CollapsedLyricLine sideLyricCur_;
    CollapsedLyricLine sideLyricPrev_;
    int sideLyricIdx_ = -2;                 // -2 = nothing built yet (forces a snap)
    std::wstring sideLyricTitle_;
    std::wstring sideLyricArtist_;
    std::wstring sideLyricFamily_;
    float sideLyricScale_ = -1.0f;
    double sideLyricAnimStart_ = -10.0;
    double sideLyricLastDrawn_ = -1.0;

    // ── Immersive lyrics state ───────────────────────────────────────────────
    float immersiveMixLin_ = 0.0f;
    double immersiveMixLast_ = -1.0;
    double immLastDrawn_ = -1.0;
    float immScroll_ = 0.0f;
    float immVel_ = 0.0f;
    float immGlow_ = 0.0f;
    std::wstring immTrackKey_;
    std::unordered_map<int, CollapsedLyricLine> immCache_;
    ComPtr<IDWriteTextFormat> immFormat_;
    float immFmtScale_ = -1.0f;
    std::wstring immFmtFamily_;
    MarqueeLayoutCache marqueeImmersiveCache_;
    LinearGradientSlot immKaraokeGrad_;
    float  hoverScale_ = 1.0f;       // pill scale set by Render(); gives the resting expanded size
    float  islandSizeLastW_ = 0.0f;  // island resize tracking (see DrawMedia)
    float  islandSizeLastH_ = 0.0f;
    double islandSizeMovedAt_ = -1e9;
    bool   islandSettled_ = true;    // false while the island is still springing to a new size
    float settingsOpacity_ = 0.96f;
    D2D1_COLOR_F pillBgColor_ = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f);
    // Design tokens for the current frame, rebuilt by EnsureBrushes.
    MaterialTokens material_{};
    // Accent color lerp: smoothly transition between successive sampled accents
    // so track changes don't produce a jarring instant color pop.
    D2D1_COLOR_F currentAccent_ = D2D1::ColorF(0x4cc9f0);
    double       lastAccentTime_ = -1.0;  // -1 = not yet set (will snap on first frame)
    float        mediaBtnPress_[3] = {0.0f, 0.0f, 0.0f};
    double       lastMediaBtnTime_ = -1.0;
    ComPtr<ID2D1PathGeometry> skipTriFull_;   // skip icons: rounded triangle, left edge at x=0
    ComPtr<ID2D1PathGeometry> skipTriNotch_;  // same, with the front triangle's gap cut out
    unsigned     skipSeenTrigger_[2] = {0, 0};
    double       skipAnimStart_[2] = {-1.0, -1.0};
    float        skipProgress_[2] = {-1.0f, -1.0f};  // index 0 = previous, 1 = next; <0 = idle, else 0..1
    double       lastSkipClickAt_ = -1.0;
    float        lastSkipDir_ = 1.0f;
    float        playMorph_ = 0.0f;  // play/pause morph: 0 = pause bars, 1 = play triangle
    float        playMorphFrom_ = 0.0f;
    float        playMorphTarget_ = 0.0f;
    double       playMorphStart_ = -1.0;
    double       playMorphLastDrawn_ = -1.0;
    bool         playMorphInit_ = false;
    ComPtr<ID2D1StrokeStyle> roundJoinStyle_;
    std::vector<TitleGlyph> titleOld_;  // title change: shared letters slide, the rest dissolve
    std::vector<TitleGlyph> titleNew_;
    std::wstring titleShown_;
    bool         titleShownPlaceholder_ = false;
    bool         titleAnimActive_ = false;
    double       titleAnimStart_ = 0.0;
    double       titleLastDrawn_ = -1.0;
    float        titleAnimDur_ = 1.0f;
    float        titleAvail_ = 0.0f;
    TitleScroll  titleOldScroll_;
    TitleScroll  titleNewScroll_;
    SwapTextState swapArtist_;
    SwapTextState swapAlbum_;
    ComPtr<ID2D1Bitmap> artFlipOld_;  // album art flip
    double       artFlipStart_ = -1.0;
    double       artFlipLastDrawn_ = -1.0;
    uint64_t     artFlipHash_ = 0;
    uint64_t     artHashGen_ = 0;
    uint64_t     artHashValue_ = 0;
    bool         artFlipInit_ = false;
    ComPtr<ID2D1Bitmap> pillArtOld_;    // pill cover change: outgoing disc during a drop
    ComPtr<ID2D1Bitmap> pillArtLast_;   // bitmap drawn in the previous pill frame
    double       pillArtStart_ = -1.0;  // -1 = idle
    double       pillArtLastDrawn_ = -1.0;
    double       pillArtFullAt_ = -1.0; // when the last full drop started
    double       artOuterLast_ = -1.0;  // shared art pass: last frame it ran
    float        artOuterAlpha_ = 1.0f; // shared art pass: eased opacity
    bool         upkeepPending_ = false;  // z-order upkeep requested, throttled in Render()
    double       lastUpkeepAt_ = -1e9;    // last time ApplyOverlayZOrder actually ran
    bool         islandHiding_ = false; // target size is 0 (auto-hide / fullscreen): art shrinks with the pill
    float        artMorphLin_ = 0.0f;   // art circle->square progress (linear, before easing)
    double       artMorphLast_ = -1.0;
    float        artBadgeAlpha_ = 1.0f; // source badge opacity on the expanded art
    double       pillTitlePendingAt_ = -1.0;
    uint64_t     pillArtHash_ = 0;
    std::wstring pillTitle_;
    float        pillArtDir_ = 1.0f;    // +1 clockwise (next), -1 counter-clockwise (previous)
    bool         pillArtPulse_ = false; // same cover, new track
    bool         pillArtInit_ = false;
    bool         expandedAnim_ = false;   // Settings::expandedMediaAnim, latched per frame
    bool         pillCoverAnim_ = false;  // Settings::pillCoverAnim, latched per frame
    float        indicatorMix_ = 0.0f;
    double       indicatorLastDrawn_ = -1.0;
    bool         specInit_ = false;     // audio spectrum state
    double       specLastDrawn_ = -1.0;
    double       specStart_ = -1.0;     // -1 = no track-change animation running
    size_t       specKey_ = 0;
    float        specT_ = -1.0f;        // 0..1 animation progress, <0 when idle
    float        specDir_ = 1.0f;
    float        specPhase_ = 0.0f;
    float        specBass_ = 0.0f;
    float        specBassSlow_ = 0.0f;
    float        specKick_ = 0.0f;
    float        specPeak_[kSpectrumBands] = {};
    float        specPeakVel_[kSpectrumBands] = {};
    float        progressAmp_ = 1.0f;   // wavy/squiggle bar: wave strength, eases to 0 while paused
    double       progressLastTime_ = -1.0;

    // ── Source switch + dock ────────────────────────────────────────────────
    int          swapPhase_ = 0;        // kSwapNone
    double       swapStart_ = 0.0;
    double       swapLastUpdate_ = -1.0;
    float        swapDir_ = 1.0f;
    unsigned     swapSeenSeq_ = 0;
    std::wstring swapTarget_;
    ComPtr<ID2D1Layer> swapGroupLayer_;
    D2D1_MATRIX_3X2_F swapGroupOldTransform_ = D2D1::Matrix3x2F::Identity();
    bool         dockInit_ = false;
    int          dockSlotCount_ = 0;
    double       dockLastDrawn_ = -1.0;
    float        dockIndPos_ = 0.0f;
    float        dockIndFrom_ = 0.0f;
    float        dockIndTarget_ = 0.0f;
    double       dockIndStart_ = -1.0;
    float        dockHover_[MediaLayout::kDockMaxSlots] = {};
    float        dockPress_[MediaLayout::kDockMaxSlots] = {};
    ComPtr<ID2D1Bitmap> dockIconBitmap_[MediaLayout::kDockMaxSlots];
    uint64_t     dockIconGen_[MediaLayout::kDockMaxSlots] = {};
    MarqueeLayoutCache marqueeSourceCache_;

    // ── Tab transition state ────────────────────────────────────────────────
    int          tabContext_ = -1;      // 0 = media view, 1 = idle view (ids mean different things)
    int          tabCount_ = 0;
    bool         tabInit_ = false;      // false = next draw snaps to the selected tab
    bool         tabAnimating_ = false;
    float        tabPos_ = 0.0f;        // continuous position the spring moves
    float        tabTarget_ = 0.0f;
    float        tabVel_ = 0.0f;
    float        tabDir_ = 1.0f;        // +1 = last move went forward, -1 = backward
    double       tabLastDrawn_ = -1.0;
    ComPtr<ID2D1Layer> tabLayer_;       // reused layer for the tab fade

    // ── Cached resources (see "Cached Direct2D resources") ──────────────────
    std::vector<ComPtr<ID2D1SolidColorBrush>> brushPool_;
    size_t brushPoolUsed_ = 0;
    std::vector<ComPtr<ID2D1Layer>> layerPool_;
    size_t layerPoolUsed_ = 0;
    ComPtr<ID2D1SolidColorBrush> raisedBrush_;        // DrawCard fill
    ComPtr<ID2D1SolidColorBrush> raisedStrongBrush_;  // active card, badges, tracks
    LinearGradientSlot depthGrad_;
    LinearGradientSlot trackGrad_;
    RadialGradientSlot bloomGrad_;
    ShapeEntry shapeCache_[kShapeCacheSize];
    uint64_t shapeClock_ = 0;
};

Activity ActivityForKind(IslandKind kind, const Settings& settings, const SharedState& state) {
    Activity activity;
    activity.kind = kind;

    switch (kind) {
        case IslandKind::Media:
            activity.width = MediaPillLayout::kBaseWidth +
                (state.system.micActive || state.system.cameraActive ? MediaPillLayout::kPrivacyExtraWidth : 0.0f);
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
            // Width is fixed; the real height is measured per result by
            // Renderer::MeasureLookupCard in the render loop. This is only a seed.
            activity.width = LookupLayout::kWidth;
            activity.height = 42.0f;
            break;
        case IslandKind::Idle:
        default:
            if (settings.autoHideIdleSeconds == -1 && !state.system.micActive && !state.system.cameraActive) {
                activity.width = 0.0f;
                activity.height = 0.0f;
            } else {
                // Seed only. The real collapsed width is measured from the clock
                // and weather strings by Renderer::MeasureIdleStrip and applied in
                // the render loop -- this function has no DWrite access. A fixed
                // width here was the whole bug behind windhawk-mods#5086: dead
                // air around a short "9:41", and clipping on "10:41:32 PM" once
                // Text size reached 140.
                activity.width = settings.weather ? 170.0f : 96.0f;
                activity.height = IdleStripLayout::kHeight;
            }
            break;
    }

    activity.width *= settings.sizeScale;
    activity.height *= settings.sizeScale;
    return activity;
}

}



std::vector<IslandKind> ChooseActivities(const SharedState& state, const Settings& settings, double now) {
    std::vector<IslandKind> activities;

    if (settings.quickLookup && state.lookup.active && now < state.lookup.expiresAt) {
        activities.push_back(IslandKind::Lookup);
    }

    if (state.clipboard.active && now < state.clipboard.expiresAt) {
        activities.push_back(IslandKind::Clipboard);
    }
    // settings.capsLock is checked here, like every other module above and below.
    // Leaving it out was what made the Caps Lock toggle not actually work: the
    // handler gate stopped the nudge but the pill was still selected and drawn.
    if (settings.capsLock && state.capsLock.active && now < state.capsLock.expiresAt) {
        activities.push_back(IslandKind::CapsLock);
    }
    if (state.device.active && now < state.device.expiresAt) {
        activities.push_back(IslandKind::Device);
    }
    if (settings.bluetoothIndicator && state.bluetoothDevice.active &&
        now < state.bluetoothDevice.expiresAt) {
        activities.push_back(IslandKind::Bluetooth);
    }
    if (settings.doNotDisturbIndicator && state.doNotDisturb.active &&
        now < state.doNotDisturb.expiresAt) {
        activities.push_back(IslandKind::DoNotDisturb);
    }
    if (settings.volume && state.volume.active && now < state.volume.expiresAt) {
        activities.push_back(IslandKind::Volume);
    }
    if (settings.brightness && state.brightness.active && now < state.brightness.expiresAt) {
        activities.push_back(IslandKind::Brightness);
    }
    if (state.notification.active && now < state.notification.expiresAt) {
        activities.push_back(IslandKind::Notification);
    }
    if (settings.battery && state.battery.active && now < state.battery.expiresAt) {
        activities.push_back(IslandKind::BatteryLow);
    }
    if (settings.progress && state.progress.active) {
        activities.push_back(IslandKind::Progress);
    }
    if (settings.timerEnabled &&
        ((state.timer.justFinished && now < state.timer.finishedExpiresAt) ||
         state.timer.active)) {
        activities.push_back(IslandKind::Timer);
    }
    if (settings.media && state.media.available) {
        activities.push_back(IslandKind::Media);
    }

    if (activities.empty()) {
        activities.push_back(IslandKind::Idle);
    }

    return activities;
}

DWORD WINAPI RenderThreadProc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClass, L"Dynamic Island for Windows", WS_POPUP, 0, 0, 520, 140,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        host::log(L"Failed to create Dynamic Island overlay window.");
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    g_hwnd = hwnd;
    if (g_shellHookMessage == 0) g_shellHookMessage = RegisterWindowMessageW(L"SHELLHOOK");
    if (g_taskbarCreatedMessage == 0) g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    using ChangeWindowMessageFilterEx_t = BOOL(WINAPI*)(HWND, UINT, DWORD, PVOID);
    static auto pChangeWindowMessageFilterEx = reinterpret_cast<ChangeWindowMessageFilterEx_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"));
    if (pChangeWindowMessageFilterEx) {
        if (g_shellHookMessage) pChangeWindowMessageFilterEx(hwnd, g_shellHookMessage, 1 /*MSGFLT_ALLOW*/, nullptr);
        if (g_taskbarCreatedMessage) pChangeWindowMessageFilterEx(hwnd, g_taskbarCreatedMessage, 1 /*MSGFLT_ALLOW*/, nullptr);
    } else {
        using ChangeWindowMessageFilter_t = BOOL(WINAPI*)(UINT, DWORD);
        static auto pChangeWindowMessageFilter = reinterpret_cast<ChangeWindowMessageFilter_t>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilter"));
        if (pChangeWindowMessageFilter) {
            if (g_shellHookMessage) pChangeWindowMessageFilter(g_shellHookMessage, 1 /*MSGFLT_ADD*/);
            if (g_taskbarCreatedMessage) pChangeWindowMessageFilter(g_taskbarCreatedMessage, 1 /*MSGFLT_ADD*/);
        }
    }
    EnableBlurBehind(hwnd);
    ApplyBackdropMaterial(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ApplyHideShowHotkey();

    if (g_settings.autoHideIdleSeconds == 0) {
        g_manuallyHidden = false;
        host::set_int_value(L"ManuallyHidden", 0);
    } else {
        g_manuallyHidden = host::int_value(L"ManuallyHidden", 0) != 0;
    }
    if (g_manuallyHidden.load()) {
        ShowWindow(hwnd, SW_HIDE);
    }

    Renderer renderer;
    if (!renderer.Initialize(hwnd)) {
        DestroyWindow(hwnd);
        g_hwnd = nullptr;
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    // Restore the File Tray from disk. Done here because the shell icons need the
    // COM apartment this thread already initialised.
    LoadFileTray();

    // A 1ms timer resolution costs power, so it is requested only while the island
    // is actually animating. It used to be requested once here and released at
    // shutdown, so a parked or idle island held it for the mod's whole lifetime.
    //
    // Since Windows 10 2004 this affects only the calling process's timers rather
    // than the system clock globally, but the power cost is the reason to scope it
    // either way.
    //
    // The flag is only set when timeBeginPeriod actually succeeded, so the
    // begin/end pairs stay balanced -- these calls are reference counted per
    // process, and an unmatched timeEndPeriod would decrement someone else's
    // request.
    bool highResTimer = false;

    // How long the resolution is held after animation stops. Long enough to ride
    // out the gaps between 60Hz content frames and brief pauses between spring
    // animations without flapping, short enough that a settled island gives it back
    // promptly. A plain local rather than a static, so an unload/reload cycle cannot
    // carry a stale timestamp across.
    constexpr double kHighResTimerHoldSec = 0.5;
    double lastAnimatingAt = -1.0;

    auto setHighResTimer = [&](bool want) {
        if (want == highResTimer) {
            return;
        }
        if (want) {
            if (timeBeginPeriod(1) == TIMERR_NOERROR) {
                highResTimer = true;
            }
        } else {
            timeEndPeriod(1);
            highResTimer = false;
        }
    };

    SpringValue widthSpring;
    SpringValue heightSpring;
    SpringValue nudgeSpring;
    SpringValue volumeBarSpring;
    widthSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 120.0f) * g_settings.sizeScale);
    heightSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 36.0f) * g_settings.sizeScale);
    nudgeSpring.Reset(0.0f);

    IslandKind previousPrimary = IslandKind::Idle;
    auto previousFrame = std::chrono::steady_clock::now();
    auto nextFrameTarget = previousFrame;
    double nextBatteryPoll = 0.0;
    double nextProgressPoll = 0.0;
    double nextSystemPoll = 0.0;
    double nextBrightnessPoll = 0.0;
    double nextPrivacyPoll = 0.0;
    bool wasManuallyHidden = false;
    bool wasAutoHiddenParked = false;
    bool parkedForFullscreen = false;

    static double lastInteractionTime = NowSeconds();
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_APP_NEW_EVENT) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
                lastInteractionTime = NowSeconds();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        BindVolumeEndpoint();

        bool justUnhidden = false;

        if (g_manuallyHidden.load()) {
            // Manual hide always takes precedence over — and invalidates —
            // any auto-park bookkeeping, since the window's shown/hidden
            // state is now fully owned by the manual toggle. Without this,
            // toggling manual-hide off after having been auto-parked would
            // leave g_autoHiddenParked stale-true and the window stuck
            // hidden.
            g_autoHiddenParked = false;
            wasAutoHiddenParked = false;

            if (!wasManuallyHidden) {
                // Just hid: drop to zero-CPU parking immediately, no
                // lingering render/animation work this frame.
                ShowWindow(hwnd, SW_HIDE);
                DwmBackdrop::HideAll();
                g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
                wasManuallyHidden = true;
            }
            previousFrame = std::chrono::steady_clock::now();
            // Fully parked: no polling, no timer wakeups — only the stop
            // event or a posted/queued message (hotkey, settings change,
            // clipboard update, etc.) wakes this thread while hidden. Nothing
            // is being paced, so give the system timer resolution back.
            setHighResTimer(false);
            MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
            nextFrameTarget = std::chrono::steady_clock::now();
            continue;
        }

        // Sits ahead of the parked branch below on purpose: that branch can block
        // for up to 1.5s, and the mouse thread needs to hear about a transition
        // before that, not after.
        {
            static bool prevParkedForHook = false;
            const bool parkedNow = g_autoHiddenParked.load(std::memory_order_relaxed);
            if (parkedNow != prevParkedForHook) {
                prevParkedForHook = parkedNow;
                NotifyMouseThreadParkedChanged();
            }
        }

        if (g_autoHiddenParked.load()) {
            if (g_settings.autoHideIdleSeconds == 0 && !parkedForFullscreen) {
                g_autoHiddenParked = false;
            } else {
                previousFrame = std::chrono::steady_clock::now();
                // Auto-parked, so nothing is animating; same reasoning as above.
                setHighResTimer(false);
                if (parkedForFullscreen) {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, 1500, QS_ALLINPUT);
                    const bool stillFullscreen =
                        g_settings.autoHideFullscreen && IsForegroundFullscreen(hwnd);
                    g_isFullscreen.store(stillFullscreen, std::memory_order_relaxed);
                    if (!stillFullscreen) {
                        g_fullscreenOverrideVisible = false;
                        g_autoHiddenParked = false;
                    } else if (g_fullscreenOverrideVisible.load()) {
                        g_autoHiddenParked = false;
                    }
                } else {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
                }
                nextFrameTarget = std::chrono::steady_clock::now();
                continue;
            }
        }

        if (wasManuallyHidden) {
            // Just un-hid. Show now; springs get snapped straight to their
            // freshly-computed targets further down this same iteration so
            // there's no stale pop-in animation from wherever they were
            // left off before hiding.
            wasManuallyHidden = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        if (wasAutoHiddenParked && !g_autoHiddenParked.load()) {
            // Something woke us (mouse near the island, a transient alert,
            // the hotkey, fullscreen ending, etc.) — reveal and let this
            // frame's normal logic decide whether to actually stay visible
            // or immediately re-collapse and re-park.
            wasAutoHiddenParked = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        const double now = NowSeconds();
        // True while a tab transition is genuinely being painted. The registry /
        // device polls below run on this thread and can take several ms, which
        // shows up as a hitch mid-animation, so they wait for the spring to settle.
        const bool tabBusy = g_tabAnimating.load(std::memory_order_relaxed) &&
                             (now - g_tabLastPaintAt.load(std::memory_order_relaxed)) < 0.25;
        // The island itself is resizing (spring state from the previous frame).
        const bool springBusy =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f;
        const bool pollBusy = tabBusy || springBusy;
        if (now >= nextBatteryPoll) {
            UpdateBatterySnapshot();
            nextBatteryPoll = now + 15.0;
        }
        if (now >= nextProgressPoll) {
            UpdateProgressSnapshot();
            nextProgressPoll = now + 0.25;
        }
        if (!pollBusy && now >= nextPrivacyPoll) {
            UpdatePrivacyIndicators();
            nextPrivacyPoll = now + 2.0;  // poll every 2 s
        }

        bool timerJustCompleted = false;
        SharedState snapshot;
        {
            std::lock_guard lock(g_stateMutex);
            snapshot = g_state;
            if (g_state.timer.active && g_state.timer.running && now >= g_state.timer.endsAt) {
                g_state.timer.active = false;
                g_state.timer.running = false;
                g_state.timer.justFinished = true;
                g_state.timer.finishedExpiresAt = now + 6.0;
                timerJustCompleted = true;
            }
            if (g_state.timer.justFinished && now >= g_state.timer.finishedExpiresAt) {
                g_state.timer.justFinished = false;
            }
            snapshot.timer = g_state.timer;
            if (g_state.clipboard.active && now >= g_state.clipboard.expiresAt) {
                g_state.clipboard.active = false;
                snapshot.clipboard.active = false;
            }
            if (g_state.notification.active && now >= g_state.notification.expiresAt) {
                g_state.notification.active = false;
                snapshot.notification.active = false;
            }
            if (g_state.volume.active && now >= g_state.volume.expiresAt) {
                g_state.volume.active = false;
                snapshot.volume.active = false;
            }
            if (g_state.brightness.active && now >= g_state.brightness.expiresAt) {
                g_state.brightness.active = false;
                snapshot.brightness.active = false;
            }
            if (g_state.capsLock.active && now >= g_state.capsLock.expiresAt) {
                g_state.capsLock.active = false;
                snapshot.capsLock.active = false;
            }
            if (g_state.battery.active && now >= g_state.battery.expiresAt) {
                g_state.battery.active = false;
                snapshot.battery.active = false;
            }
            if (g_state.device.active && now >= g_state.device.expiresAt) {
                g_state.device.active = false;
                snapshot.device.active = false;
            }
        }
            if (timerJustCompleted) {
                TriggerNudge();
        }

        // Quick Lookup: hand keyboard focus back once the panel has timed out or the
        // module was switched off in settings.
        if (g_lookupUi.open &&
            (!g_settings.quickLookup || !snapshot.lookup.active || now >= snapshot.lookup.expiresAt)) {
            CloseLookupPanel(hwnd);
        }

        const std::vector<IslandKind> kinds = ChooseActivities(snapshot, g_settings, now);
        Activity primary = ActivityForKind(kinds[0], g_settings, snapshot);
        std::optional<Activity> secondary;
        if (kinds.size() >= 2) {
            secondary = ActivityForKind(kinds[1], g_settings, snapshot);
        }

        const bool pinned = g_pinnedExpanded.load();

        if (primary.kind != previousPrimary) {
            if (primary.kind != IslandKind::Idle) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
            }
            lastInteractionTime = now;
        }
        previousPrimary = primary.kind;

        POINT cursor = {};
        GetCursorPos(&cursor);

        // Pill rectangle in screen space, from the same layout Render() uses. The window
        // is a big fixed stage now, so its own rect says nothing about where the pill is.
        const bool flushTopNow = g_settings.notchStyle || g_settings.borderMergedMode;
        const StageLayout hoverStage =
            ComputeStageLayout(g_settings, widthSpring.value, heightSpring.value);
        const float hoverTop = hoverStage.pillTop +
            (flushTopNow ? std::max(0.0f, nudgeSpring.value) : nudgeSpring.value);
        RECT pillRect = {
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop)),
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft + widthSpring.value)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop + heightSpring.value))
        };

        bool hover = false;
        if (widthSpring.value > 1.0f && heightSpring.value > 1.0f) {
            hover = PtInRect(&pillRect, cursor) != FALSE;
        } else if (g_settings.unhideOnHover) {
            RECT dockRect = GetIslandDockRect();
            hover = PtInRect(&dockRect, cursor) != FALSE;
        }

        // A file drag hovering over the island counts as hover, so it expands onto
        // the File Tray even with "Expand on hover" turned off.
        if (g_trayDragOver.load(std::memory_order_relaxed)) {
            hover = true;
        }

        bool needsRender = false;

        if (!hover && g_clickExpanded.load()) {
            g_clickExpanded = false;
            needsRender = true;
        }
        if (!hover && g_notificationCenterDetailOpen.load(std::memory_order_relaxed)) {
            CloseNotificationCenterDetail();
            needsRender = true;
        }
        if (!hover && g_hoveredMediaButton.load() != -1) {
            g_hoveredMediaButton = -1;
            needsRender = true;
        }
        // Fixes windhawk-mods#4738: active playback used to count as a continuous
        // event, so the island stayed visible for as long as anything was playing
        // and kept resetting the auto-hide timer. Only the 5s window after a
        // *title* change is transient now, so the pill behaves like the clipboard
        // and battery alerts -- it surfaces, then hides again while playback
        // continues in the background.
        const bool recentTrackChange = g_settings.mediaAutoExpand &&
                                       !MediaExpandBlocked(snapshot.media) &&
                                       primary.kind == IslandKind::Media &&
                                       snapshot.media.playing &&
                                       !snapshot.media.title.empty() &&
                                       (now - snapshot.media.titleChangedAt < 5.0);

        bool isTransientAlert = (primary.kind == IslandKind::Clipboard ||
                                 primary.kind == IslandKind::Notification ||
                                 primary.kind == IslandKind::Volume ||
                                 primary.kind == IslandKind::Brightness ||
                                 primary.kind == IslandKind::BatteryLow ||
                                 primary.kind == IslandKind::CapsLock ||
                                 primary.kind == IslandKind::Device ||
                                 primary.kind == IslandKind::Bluetooth ||
                                 primary.kind == IslandKind::DoNotDisturb ||
                                 primary.kind == IslandKind::Lookup ||
                                 recentTrackChange);

        const bool unhideGraceActive = (now < g_hotkeyUnhideUntil.load());
        const bool hoverUnhides = g_settings.unhideOnHover && hover;

        bool currentlyHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !pinned) {
                currentlyHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                currentlyHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
            }
        }

        bool isHoverExpanded = g_settings.expandOnHover ? hover : (hover && g_clickExpanded.load());
        const bool gameMetricsPresent = primary.kind == IslandKind::Idle &&
            (g_settings.gameOverlay || g_gameOverlayPinned.load());
        if (gameMetricsPresent) {
            isHoverExpanded = false;
        }

        if (currentlyHidden && !g_settings.unhideOnHover) {
            isHoverExpanded = false;
        } else if (isHoverExpanded || hoverUnhides || pinned || isTransientAlert || unhideGraceActive) {
            lastInteractionTime = now;
        }

        bool isHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !isHoverExpanded && !hoverUnhides && !pinned) {
                isHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                if (hoverUnhides) {
                    isHidden = false;
                } else {
                    isHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
                }
            }
        }

        static bool isFullscreen = false;
        static double lastFullscreenCheck = -1.0;  // -1.0 guarantees the very first iteration checks
        if (now - lastFullscreenCheck > 0.5) {
            const bool newFullscreen = g_settings.autoHideFullscreen && IsForegroundFullscreen(hwnd);
            if (isFullscreen && !newFullscreen) {
                // Fullscreen ended — re-arm so the next fullscreen session
                // hides again even if the hotkey was used to reveal the
                // island this time.
                g_fullscreenOverrideVisible = false;
            }
            isFullscreen = newFullscreen;
            g_isFullscreen.store(isFullscreen, std::memory_order_relaxed);
            lastFullscreenCheck = now;
        }

        // Reclaim the top of the z-order if another always-on-top window has
        // been raised over the island. Cheap and rate-limited, and skipped
        // while the island is hidden or suppressed so it cannot un-hide itself.
        static double lastTopmostCheck = 0.0;
        if (!isFullscreen && !g_manuallyHidden.load() && !g_autoHiddenParked.load() &&
            now - lastTopmostCheck > 1.0) {
            EnsureTopmost(hwnd, &pillRect);
            lastTopmostCheck = now;
        }

        const bool micIndicatorActive = snapshot.system.micActive && g_settings.privacyDots && g_settings.privacyDotsMic;
        const bool camIndicatorActive = snapshot.system.cameraActive && g_settings.privacyDots && g_settings.privacyDotsCam;
        const bool privacyActive = micIndicatorActive || camIndicatorActive;

        // Whether any surface that actually draws CPU / RAM / disk / GPU / network
        // figures is on screen: the in-game overlay, or the idle dashboard's
        // Hardware Monitor tab while expanded (hovered or pinned) and scrolled into
        // view.
        //
        // Hoisted out of the poll block below because two decisions need it: whether
        // to sample the expensive GPU and network counters at all, and whether a
        // change in those numbers is worth repainting for.
        const int metricTabIdx = NormalizedTabIndex(g_settings);
        const bool onHardwareMonitorTab = (metricTabIdx == HardwareMonitorTabIndex(g_settings));
        const bool hwMonitorVisible = (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) &&
            !isFullscreen && !gameMetricsPresent && (pinned || isHoverExpanded) && onHardwareMonitorTab;
        const bool gameOverlayVisible = gameMetricsPresent && !isFullscreen;
        const bool systemMetricsVisible = hwMonitorVisible || gameOverlayVisible;

        if (g_settings.brightness && !pollBusy && now >= nextBrightnessPoll) {
            UpdateBrightnessSnapshot();
            nextBrightnessPoll = now + 0.25;
        }

        if (!pollBusy && now >= nextSystemPoll) {
            const bool needGpuStats = gameOverlayVisible || hwMonitorVisible;
            const bool needNetStats = hwMonitorVisible;  // net is only ever drawn in the HW dashboard

            UpdateSystemSnapshot(needGpuStats, needNetStats);
            if (!g_endpointVolume) {
                g_volumeEndpointStale = true;  // no audio device yet, or binding failed
            }
            nextSystemPoll = now + 1.0;
        }

        if (primary.kind == IslandKind::Idle) {
            if (!isFullscreen && (pinned || isHoverExpanded)) {
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
            } else if (primary.width > 0.0f) {
                // Collapsed: size the strip to the text it will actually render
                // (windhawk-mods#5086). ActivityForKind's 96/170px was wrong both
                // ways -- dead air around a short "9:41", and clipping on
                // "10:41:32 PM" at larger Text size.
                // The > 0 guard preserves ActivityForKind's fully-hidden case.
                primary.width =
                    renderer.MeasureIdleStrip(snapshot, g_settings, now).totalWidth *
                    g_settings.sizeScale;
            }
        }
        if (!isFullscreen && primary.kind == IslandKind::Idle &&
            (g_settings.gameOverlay || g_gameOverlayPinned.load())) {
            // Width follows the metrics actually enabled (#25) so switching some
            // off shrinks the strip instead of leaving empty space, and compact
            // mode narrows it enough to sit neatly in the taskbar area. Both the
            // size and the painting come from GameOverlayLayout, so the strip
            // cannot end up sized for a card width DrawGameOverlay no longer uses.
            const int metricCount = (g_settings.gameOverlayShowCpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowRam ? 1 : 0) +
                                    (g_settings.gameOverlayShowGpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowDisk ? 1 : 0);

            const bool compact = g_settings.gameOverlayCompact;
            primary.width = GameOverlayLayout::Width(compact, g_settings.gameOverlayShowFps,
                                                     metricCount) * g_settings.sizeScale;
            primary.height = GameOverlayLayout::For(compact).height * g_settings.sizeScale;
        }
        if (primary.kind == IslandKind::Media) {
            if (!isFullscreen && (isHoverExpanded || pinned || recentTrackChange)) {
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
                // (Immersive lyrics: the island keeps its normal 380px width.)
            } else {
                // Optional clock section on the left of the collapsed pill.
                const float clockSection = g_settings.mediaPillClock
                    ? renderer.MeasureMediaPill(snapshot, g_settings).sectionWidth
                    : 0.0f;
                if (CollapsedLyricsActive(snapshot, g_settings)) {
                    // Collapsed but carrying lyrics: wider pill, same height.
                    primary.width = (kCollapsedLyricsWidth + clockSection) * g_settings.sizeScale;
                } else if (g_settings.mediaPillClock) {
                    const float privacyExtra = snapshot.system.micActive || snapshot.system.cameraActive
                        ? MediaPillLayout::kPrivacyExtraWidth : 0.0f;
                    primary.width = (MediaPillLayout::kBaseWidth + privacyExtra + clockSection) * g_settings.sizeScale;
                }
            }
        }

        if (primary.kind == IslandKind::Lookup) {
            secondary.reset();  // the panel stands alone (see HandleLookupClick)
            if (primary.width > 1.0f) {
                // The Lookup card is sized to its (wrapped, line-capped) content.
                const Settings lookupSettings = GetSettingsCopy();
                primary.height = renderer.MeasureLookupCard(snapshot, lookupSettings) *
                                 lookupSettings.sizeScale;
            }
        }

        const bool fullscreenSuppressed =
            isFullscreen && !g_fullscreenOverrideVisible.load(std::memory_order_relaxed);
        if ((isHidden || fullscreenSuppressed) && !privacyActive && !pinned && !isHoverExpanded && !isTransientAlert) {
            primary.width = 0.0f;
            primary.height = 0.0f;
            secondary.reset();
        }

        const bool mediaWaveformVisible =
            g_settings.media && snapshot.media.playing && g_settings.spectrumStyle != SpectrumStyle::Bars &&
            ((primary.kind == IslandKind::Media && primary.width > 1.0f && primary.height > 1.0f) ||
             (secondary && secondary->kind == IslandKind::Media && secondary->width > 1.0f && secondary->height > 1.0f));
        g_audioCaptureNeeded.store(mediaWaveformVisible, std::memory_order_relaxed);

        float targetWidth = primary.width;
        float targetHeight = primary.height;
        if (secondary) {
            targetWidth = primary.width + secondary->width + 12.0f * g_settings.sizeScale;
            targetHeight = std::max(primary.height, secondary->height);
        }

        widthSpring.target = targetWidth;
        heightSpring.target = targetHeight;

        if (justUnhidden || (unhideGraceActive && widthSpring.value < 0.5f && targetWidth > 1.0f)) {
            widthSpring.Reset(targetWidth);
            heightSpring.Reset(targetHeight);
            nudgeSpring.Reset(0.0f);
        }

        const auto currentFrame = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(currentFrame - previousFrame).count();
        previousFrame = currentFrame;
        dt = Clamp(dt, 0.001f, 0.050f);

        float styleStiffnessMult = 1.0f;
        float styleDampingMult = 1.0f;
        if (g_settings.animationStyle == AnimationStyle::Smooth) {
            styleStiffnessMult = 1.0f;
            styleDampingMult = 1.35f; // Critically damped, no bounciness
        } else if (g_settings.animationStyle == AnimationStyle::Bouncy) {
            styleStiffnessMult = 1.1f;
            styleDampingMult = 0.70f; // Underdamped, lively elasticity
        } else if (g_settings.animationStyle == AnimationStyle::Snappy) {
            styleStiffnessMult = 1.5f;
            styleDampingMult = 1.25f; // High stiffness and quick settle
        }

        const float speed = g_settings.animationSpeed;
        float widthStiffness = 280.0f * styleStiffnessMult;
        float widthDamping = 24.0f * styleDampingMult;
        if (targetWidth > widthSpring.value) {
            widthStiffness = 380.0f * styleStiffnessMult;
            widthDamping = 26.0f * styleDampingMult;
        } else if (targetWidth < widthSpring.value) {
            widthStiffness = 200.0f * styleStiffnessMult;
            widthDamping = 28.0f * styleDampingMult;
        }

        float heightStiffness = 280.0f * styleStiffnessMult;
        float heightDamping = 24.0f * styleDampingMult;
        if (targetHeight > heightSpring.value) {
            heightStiffness = 380.0f * styleStiffnessMult;
            heightDamping = 26.0f * styleDampingMult;
        } else if (targetHeight < heightSpring.value) {
            heightStiffness = 200.0f * styleStiffnessMult;
            heightDamping = 28.0f * styleDampingMult;
        }

        widthSpring.Step(dt * speed, widthStiffness, widthDamping);
        if (widthSpring.value < 0.0f) {
            widthSpring.value = 0.0f;
            widthSpring.velocity = 0.0f;
        }

        heightSpring.Step(dt * speed, heightStiffness, heightDamping);
        if (heightSpring.value < 0.0f) {
            heightSpring.value = 0.0f;
            heightSpring.velocity = 0.0f;
        }

        nudgeSpring.Step(dt * speed, 280.0f * styleStiffnessMult, 24.0f * styleDampingMult);

        // Tracks the live level while the flyout is closed, so opening it doesn't sweep from a stale value.
        if (snapshot.volume.active) {
            volumeBarSpring.target = static_cast<float>(snapshot.volume.percent);
        } else {
            volumeBarSpring.Reset(static_cast<float>(snapshot.system.volumePercent));
        }
        // Fixed, overdamped constants: the animation style must not make the bar overshoot.
        volumeBarSpring.Step(dt * speed, 520.0f, 46.0f);
        snapshot.volume.displayPercent = volumeBarSpring.value;

        {
            std::lock_guard lock(g_stateMutex);
            g_state.system.renderFps = ClampInt(static_cast<int>(1.0f / std::max(dt, 0.001f) + 0.5f), 0, 1000);
        }

        const bool draggingOrHover = hover || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && PtInRect(&pillRect, cursor));

        // Ctrl+hover see-through: holding Ctrl while hovering makes the island
        // transparent and click-through in ANY state, so clicks pass through
        // to windows underneath. Releasing Ctrl or moving away restores it.
        const bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool ctrlHoverCT = hover && ctrlHeld && !g_lookupUi.open;

        SetClickThrough(hwnd, (primary.kind == IslandKind::Idle && !draggingOrHover && !pinned) || ctrlHoverCT);

        // Check if animating structurally
        if (std::abs(widthSpring.velocity) > 0.01f || std::abs(widthSpring.target - widthSpring.value) > 0.01f ||
            std::abs(heightSpring.velocity) > 0.01f || std::abs(heightSpring.target - heightSpring.value) > 0.01f ||
            std::abs(nudgeSpring.velocity) > 0.01f || std::abs(nudgeSpring.target - nudgeSpring.value) > 0.01f ||
            std::abs(volumeBarSpring.velocity) > 0.01f || std::abs(volumeBarSpring.target - volumeBarSpring.value) > 0.01f) {
            needsRender = true;
        }

        // Active Monitor Tracking & dynamic DPI scaling
        {
            HMONITOR currentAnchor = nullptr;
            GetAnchorWorkRect(&currentAnchor);
            const float currentDpi = GetMonitorDpiScale(currentAnchor);
            static HMONITOR s_lastAnchorMon = nullptr;
            static float s_lastDpi = 0.0f;
            if (currentAnchor != s_lastAnchorMon || currentDpi != s_lastDpi) {
                s_lastAnchorMon = currentAnchor;
                s_lastDpi = currentDpi;
                std::lock_guard lock(g_settingsMutex);
                if (g_settings.autoDpiScale) {
                    g_settings.sizeScale = g_settings.baseSizeScale * currentDpi;
                } else {
                    g_settings.sizeScale = g_settings.baseSizeScale;
                }
                g_layoutDirty = true;
            }
        }

        // Quick Lookup panel: caret blink, and keeps the published hit-test geometry fresh.
        if (g_lookupUi.open && primary.kind == IslandKind::Lookup) {
            static double s_lastLookupPaint = 0.0;
            if (now - s_lastLookupPaint >= 0.2) {
                s_lastLookupPaint = now;
                needsRender = true;
            }
        }

        // A tab transition keeps painting until its spring settles. It used to
        // request that through g_layoutDirty, which also made Render() run
        // SetWindowPos / backdrop-region upkeep on every frame of the animation.
        if (g_tabAnimating.load(std::memory_order_relaxed)) {
            if (tabBusy) {
                needsRender = true;
            } else {
                // The tab view stopped drawing mid-motion (island collapsed or an
                // alert took over); don't keep the loop spinning for it.
                g_tabAnimating.store(false, std::memory_order_relaxed);
            }
        }

        // Check if layout was explicitly invalidated
        if (g_layoutDirty.load()) {
            needsRender = true;
        }

        // Hover or pinned state changes visual elements slightly
        static bool prevHover = false;
        static bool prevPinned = false;
        if (hover != prevHover || pinned != prevPinned) {
            needsRender = true;
            prevHover = hover;
            prevPinned = pinned;
        }

        // Activities that animate continuously: the waveform bars, the marquee
        // scroll and the battery pulse. These still only want ~60Hz -- they look no
        // different above it, and they should not piggyback on whatever high Target
        // FPS the user picked for structural resize animation.
        //
        // The rate limit lives in the pacer at the bottom of the loop, not here.
        // This used to gate needsRender behind a 1/60s (16.667ms) timer and then
        // fall through to the flat 16ms idle wait, which cannot satisfy it: 16ms is
        // shorter than the gate, so the next pass failed the check and waited a
        // second time. The result was a paint roughly every 32ms -- about 31fps
        // instead of 60, which is what made playing media look choppy.
        // Media only counts as continuous while something on it is actually moving.
        //
        // ChooseActivities selects the Media pill for any *available* SMTC session,
        // playing or not, so keying off the kind alone meant a paused Spotify or a
        // browser tab with a paused video -- which can sit there for hours, since
        // browsers keep the session alive -- held the island at a 60fps render loop
        // and 1ms timer resolution indefinitely. Nothing on it moves in that state:
        // the collapsed pill draws the album art plus a row of flat bars, taking the
        // !playing branch that skips DrawWaveform entirely.
        //
        // Paused media now falls back to the ordinary change detection above (title
        // and art generation, springs, hover), so the timer is released 0.5s after
        // the last paint.
        //
        // The marquees are expanded-only, hence the hover/pinned terms. No need to
        // include recentTrackChange: it already requires snapshot.media.playing, so
        // it cannot be true while paused.
        // Checks the secondary pill too. The island can show two pills side by side,
        // so a playing media pill sitting next to a running timer or a progress ring
        // is the secondary one -- and looking only at primary left its waveform
        // frozen.
        const bool mediaShown = primary.kind == IslandKind::Media ||
                                (secondary && secondary->kind == IslandKind::Media);
        const bool mediaAnimating =
            mediaShown && (snapshot.media.playing || isHoverExpanded || pinned);

        // Transient alert pills. All expire within a few seconds, so treating them as
        // continuous cannot run away.
        //
        // Device, Bluetooth and Do Not Disturb are here only when the countdown bar
        // is switched on, since that bar is the one thing on them that moves.
        const auto isTransientPill = [](IslandKind kind) {
            return kind == IslandKind::BatteryLow || kind == IslandKind::Clipboard ||
                   kind == IslandKind::Notification;
        };
        const auto hasCountdownBar = [](IslandKind kind) {
            return kind == IslandKind::Device || kind == IslandKind::Bluetooth ||
                   kind == IslandKind::DoNotDisturb;
        };
        const bool countdownBarsOn = g_settings.statusCountdownProgress;

        const bool continuousAnimation =
            mediaAnimating || isTransientPill(primary.kind) ||
            (secondary && isTransientPill(secondary->kind)) ||
            (countdownBarsOn && (hasCountdownBar(primary.kind) ||
                                 (secondary && hasCountdownBar(secondary->kind))));
        if (continuousAnimation) {
            needsRender = true;
        }

        // Privacy dots
        if (snapshot.system.micActive || snapshot.system.cameraActive) {
            needsRender = true;
        }

        // Idle dashboard clock changes once a minute
        static SYSTEMTIME prevTime = {};
        const bool clockOnScreen =
            primary.kind == IslandKind::Idle ||
            (primary.kind == IslandKind::Media && g_settings.mediaPillClock);
        if (clockOnScreen && !isHidden) {
            SYSTEMTIME local = {};
            GetLocalTime(&local);
            if (local.wMinute != prevTime.wMinute) {
                needsRender = true;
                prevTime = local;
            }
        }

        // Text that ticks once a second: the focus timer countdown and, when Show
        // seconds is on, the clock.
        //
        // Both used to ride on a side effect. The system poll refreshed CPU load
        // every second and the change detection compared it unconditionally, so the
        // whole island repainted about once a second whether anything visible had
        // changed or not. Gating those metric comparisons on visibility removed that,
        // which left DrawTimer recomputing its m:ss from NowSeconds() on a surface
        // nothing marked dirty -- a 25 minute session sat at 25:00 until some
        // unrelated event forced a paint -- and left the seconds clock updating once
        // a minute despite its own setting promising every second.
        //
        // Keyed to the displayed value rather than to elapsed time, so each visible
        // change paints exactly once. Deliberately not folded into
        // continuousAnimation: these need one frame per second, not the 1ms timer
        // resolution that continuous animation asks for.
        int shownSecond = -1;
        const bool timerShown = primary.kind == IslandKind::Timer ||
                               (secondary && secondary->kind == IslandKind::Timer);
        if (timerShown && snapshot.timer.running) {
            shownSecond = static_cast<int>(std::ceil(snapshot.timer.endsAt - now));
        } else if (clockOnScreen && g_settings.showSeconds && !isHidden) {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            shownSecond = st.wSecond;
        }
        static int s_prevShownSecond = -1;
        if (shownSecond != s_prevShownSecond) {
            s_prevShownSecond = shownSecond;
            needsRender = true;
        }

        // Compare data snapshot to detect changes
        static uint64_t prevArtGen = 0;
        static uint64_t prevSrcIconGen = 0;
        static uint64_t prevNotifIconGen = 0;
        static uint64_t prevClipIconGen = 0;
        static int prevCpu = -1;
        static int prevRam = -1;
        static int prevDisk = -1;
        static int prevVol = -1;
        static int prevBright = -1;
        static bool prevMuted = false;
        static int prevBat = -1;
        static bool prevCharging = false;
        static int prevProg = -1;
        static std::wstring prevMediaTitle;
        static bool prevPlaying = false;

        // CPU / RAM / disk are compared only while a surface that draws them is on
        // screen. UpdateSystemSnapshot refreshes them every second and CPU load
        // essentially always differs between samples, so comparing them
        // unconditionally repainted the whole island once a second for numbers that
        // appear nowhere on the collapsed pill.
        const bool systemMetricsChanged =
            systemMetricsVisible && (snapshot.system.cpuPercent != prevCpu ||
                                     snapshot.system.memoryPercent != prevRam ||
                                     snapshot.system.diskFreePercent != prevDisk);

        if (snapshot.media.artGeneration != prevArtGen ||
            snapshot.media.sourceIconGeneration != prevSrcIconGen ||
            snapshot.media.title != prevMediaTitle ||
            // Play/pause has to be in here now that the metric tick no longer
            // repaints every second as a side effect. Pausing while collapsed swaps
            // the live waveform for the static bars, and without this the frozen
            // last playing frame stayed up until some unrelated repaint came along.
            snapshot.media.playing != prevPlaying ||
            snapshot.notification.icon->generation != prevNotifIconGen ||
            snapshot.clipboard.appIcon->generation != prevClipIconGen ||
            systemMetricsChanged ||
            snapshot.system.volumePercent != prevVol ||
            snapshot.system.volumeMuted != prevMuted ||
            snapshot.brightness.percent != prevBright ||
            snapshot.battery.percent != prevBat ||
            snapshot.battery.charging != prevCharging ||
            snapshot.progress.percent != prevProg) {
            needsRender = true;
            prevArtGen = snapshot.media.artGeneration;
            prevSrcIconGen = snapshot.media.sourceIconGeneration;
            prevMediaTitle = snapshot.media.title;
            prevPlaying = snapshot.media.playing;
            prevNotifIconGen = snapshot.notification.icon->generation;
            prevClipIconGen = snapshot.clipboard.appIcon->generation;
            prevCpu = snapshot.system.cpuPercent;
            prevRam = snapshot.system.memoryPercent;
            prevDisk = snapshot.system.diskFreePercent;
            prevVol = snapshot.system.volumePercent;
            prevMuted = snapshot.system.volumeMuted;
            prevBright = snapshot.brightness.percent;
            prevBat = snapshot.battery.percent;
            prevCharging = snapshot.battery.charging;
            prevProg = snapshot.progress.percent;
        }

        // Track whether Ctrl+hover click-through state changed so we re-render
        static bool prevCtrlHoverCT = false;
        if (ctrlHoverCT != prevCtrlHoverCT) {
            needsRender = true;
            prevCtrlHoverCT = ctrlHoverCT;
        }

        if (needsRender) {
            // When Ctrl+hover click-through is active, reduce pill opacity so the
            // island becomes visually see-through to match the pass-through behavior.
            Settings renderSettings = GetSettingsCopy();
            if (ctrlHoverCT) {
                renderSettings.pillOpacity = Clamp(renderSettings.pillOpacity * 0.35f, 0.15f, 0.45f);
            } else if (renderSettings.themePreset == ThemePreset::Graphite && g_pillOpacityOverride.load() < 0) {
                const bool isExpanded = isHoverExpanded || pinned || isTransientAlert || (widthSpring.value > 260.0f);
                renderSettings.pillOpacity = isExpanded ? 0.98f : 0.88f;
            }
            // No usable backdrop (old Windows / effects off): keep the solid background.
            if (renderSettings.backdropMaterial != BackdropMaterial::None &&
                !DwmBackdrop::Available()) {
                renderSettings.backdropMaterial = BackdropMaterial::None;
            }
            renderer.Render(snapshot, renderSettings, primary, secondary,
                            widthSpring.value, heightSpring.value, nudgeSpring.value,
                            hover, pinned, now);
        }

        // --- Zero-CPU parking for auto-hidden states (idle timeout / fullscreen) ---
        // Only parks once the collapse animation has actually settled at 0,
        // so the shrink still animates before we cut over to OS-hidden.
        const bool wantsAutoHiddenPark =
            !g_manuallyHidden.load() && !pinned && !isHoverExpanded && !isTransientAlert &&
            !privacyActive && (isHidden || fullscreenSuppressed) &&
            widthSpring.value < 0.5f && heightSpring.value < 0.5f &&
            std::fabs(widthSpring.velocity) < 0.5f && std::fabs(heightSpring.velocity) < 0.5f;

        if (wantsAutoHiddenPark) {
            parkedForFullscreen = fullscreenSuppressed;
            wasAutoHiddenParked = true;
            g_autoHiddenParked = true;
            ShowWindow(hwnd, SW_HIDE);
            DwmBackdrop::HideAll();
            g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
            setHighResTimer(false);
            continue;
        }

        int targetFps = g_settings.targetFps;
        if (targetFps <= 0) {
            static int s_cachedRate = 60;
            static double s_rateCheckedAt = -1e9;
            if (now - s_rateCheckedAt > 2.0) {
                s_cachedRate = GetMonitorRefreshRate(hwnd);
                s_rateCheckedAt = now;
            }
            targetFps = s_cachedRate;
        }
        targetFps = ClampInt(targetFps, 30, 1000);
        double targetFrameMs = 1000.0 / static_cast<double>(targetFps);

        // Structural animation -- the springs resizing or nudging the island -- is
        // what benefits from a high Target FPS. Continuous content does not, so once
        // the springs have settled the interval is relaxed to 60Hz and the precise
        // pacer below hits it accurately.
        //
        // std::max, not a plain assignment: a user who deliberately set Target FPS
        // to 40 should keep 40 rather than being pushed up to 60.
        const bool springsAnimating =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f ||
            std::fabs(nudgeSpring.value) > 0.5f ||
            std::fabs(nudgeSpring.velocity) > 0.5f ||
            std::fabs(volumeBarSpring.value - volumeBarSpring.target) > 0.5f ||
            std::fabs(volumeBarSpring.velocity) > 0.5f ||
            g_tabAnimating.load(std::memory_order_relaxed);

        if (continuousAnimation && !springsAnimating) {
            constexpr double kContinuousFrameMs = 1000.0 / 60.0;
            targetFrameMs = std::max(targetFrameMs, kContinuousFrameMs);
        }

        // Keyed to actual animation, not to repainting.
        //
        // A single repaint does not need 1ms pacing -- it needs one frame, which the
        // pacer delivers fine at the default resolution. Only things that draw a
        // *sequence* of frames care: continuous content and spring motion.
        //
        // Keying it to needsRender was wrong twice over. Per frame it thrashed,
        // because needsRender alternates when 60Hz content runs under a higher
        // Target FPS. And with the hold added, any one-off repaint still took the
        // resolution for the full hold -- including the metric tick, which fired
        // every second, so a plain idle island requested and released it once a
        // second and held it roughly half the time with nothing moving at all.
        //
        // continuousAnimation and springsAnimating are exactly the two cases that
        // want precise frame spacing, so they drive it directly.
        if (continuousAnimation || springsAnimating) {
            lastAnimatingAt = now;
        }
        setHighResTimer(lastAnimatingAt >= 0.0 && now - lastAnimatingAt < kHighResTimerHoldSec);

        if (!needsRender) {
            // Nothing changed on screen, so poll at ~60Hz rather than the target
            // frame rate. Accuracy does not matter here -- this is a poll interval,
            // not frame pacing -- so it runs at whatever resolution is in effect.
            WaitForSingleObject(g_stopEvent, 16);
            nextFrameTarget = std::chrono::steady_clock::now();
        } else if (springsAnimating && g_settings.targetFps <= 0 && SUCCEEDED(DwmFlush())) {
            // Auto FPS while the island is resizing: block until DWM's next
            // composition so every frame lands exactly on a vsync. A timer drifts
            // against the display's refresh and produces judder even at high fps.
            nextFrameTarget = std::chrono::steady_clock::now();
        } else {
            // When animating, achieve ultra-smooth target refresh rate (e.g. 144Hz, 240Hz, 360Hz+).
            nextFrameTarget += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(targetFrameMs));

            auto nowTime = std::chrono::steady_clock::now();
            if (nowTime < nextFrameTarget) {
                double remainingMs = std::chrono::duration<double, std::milli>(nextFrameTarget - nowTime).count();
                if (remainingMs >= 1.5) {
                    // Sleep for the bulk of the remaining time using OS event wait (zero CPU usage)
                    WaitForSingleObject(g_stopEvent, static_cast<DWORD>(remainingMs - 0.5));
                }
                // Spinning only pays off for fast spring motion on high refresh rates.
                // For 60Hz content a frame landing under 1ms late is invisible, so sleep only.
                if (springsAnimating && targetFrameMs < 10.0) {
                    while (std::chrono::steady_clock::now() < nextFrameTarget &&
                           WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                        std::this_thread::yield();
                    }
                }
            } else {
                // If we fell behind, reset target to avoid speed-up catch-up loop
                nextFrameTarget = nowTime;
            }
        }
    }

    // Balances whichever state the loop exited in; a no-op if already released.
    setHighResTimer(false);

    UnbindVolumeEndpoint();
    DwmBackdrop::DestroyAll();
    renderer.Shutdown();
    DestroyWindow(hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(kWindowClass, wc.hInstance);

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return 0;
}


}
