#include "ui/render/renderer_impl.h"

namespace island
{

void Renderer::get_weather_icon_and_text(int code, std::wstring& icon, std::wstring& text)
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

int Renderer::get_days_in_month(int year, int month)
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

int Renderer::get_day_of_week(int year, int month, int day)
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

void Renderer::draw_calendar_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
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
        calendar_cached_date_.wDay != local.wDay || calendar_cached_first_day_setting_ != settings.first_day_of_week)
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
        target_->DrawTextW(calendar_cached_month_name_.c_str(), static_cast<UINT32>(calendar_cached_month_name_.size()),
                           bold_text_format_.Get(),
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
        target_->DrawTextW(year_str, static_cast<UINT32>(std::wcslen(year_str)), small_text_format_.Get(),
                           D2D1::RectF(hero.left, hero.top + 21.0f * scale, hero.right, hero.top + 36.0f * scale),
                           muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    wchar_t day_str[16] = {};
    swprintf_s(day_str, L"%d", local.wDay);
    text_brush_->SetOpacity(0.97f);
    target_->DrawTextW(day_str, static_cast<UINT32>(std::wcslen(day_str)),
                       cal_day_large_format_ ? cal_day_large_format_.Get() : huge_text_format_.Get(),
                       D2D1::RectF(hero.left, hero.top + 34.0f * scale, hero.right, hero.bottom - 26.0f * scale),
                       text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

    muted_brush_->SetOpacity(0.70f);
    if (bold_text_format_)
    {
        target_->DrawTextW(calendar_cached_weekday_name_.c_str(),
                           static_cast<UINT32>(calendar_cached_weekday_name_.size()), bold_text_format_.Get(),
                           D2D1::RectF(hero.left + 4.0f * scale, hero.bottom - 24.0f * scale, hero.right - 4.0f * scale,
                                       hero.bottom - 6.0f * scale),
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
                          D2D1::Point2F(grid_start + 7.0f * col_w - 2.0f * scale, hr_y), hr_brush.Get(), 1.0f * scale);
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
std::wstring Renderer::wind_dir_to_arrow(const std::wstring& dir)
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
void Renderer::draw_cloud_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush)
{
    const D2D1_RECT_F base = D2D1::RectF(c.x - 0.44f * s, c.y + 0.02f * s, c.x + 0.44f * s, c.y + 0.21f * s);
    target_->FillRoundedRectangle(D2D1::RoundedRect(base, 0.10f * s, 0.10f * s), brush);
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.02f * s), 0.18f * s, 0.18f * s), brush);
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.01f * s, c.y - 0.10f * s), 0.24f * s, 0.24f * s), brush);
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + 0.25f * s, c.y + 0.03f * s), 0.17f * s, 0.17f * s), brush);
}

void Renderer::draw_sun_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush, float core_radius)
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

void Renderer::draw_lightning_shape(D2D1_POINT_2F c, float s, ID2D1Brush* brush)
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

void Renderer::draw_weather_icon(D2D1_POINT_2F center, float size, WeatherVisual visual, ID2D1Brush* strong,
                                 ID2D1Brush* soft)
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
                target_->DrawLine(D2D1::Point2F(center.x - half, y), D2D1::Point2F(center.x + half, y), soft, stroke);
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
                target_->DrawLine(D2D1::Point2F(x + 0.05f * s, y), D2D1::Point2F(x - 0.03f * s, y + 0.26f * s), soft,
                                  stroke);
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
void Renderer::draw_metric_glyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush)
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
        target_->DrawLine(D2D1::Point2F(c.x, c.y - 0.38f * s * dir), D2D1::Point2F(c.x, c.y + 0.34f * s * dir), brush,
                          stroke);
        target_->DrawLine(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.10f * s * dir),
                          D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
        target_->DrawLine(D2D1::Point2F(c.x + 0.24f * s, c.y + 0.10f * s * dir),
                          D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
    };

    switch (kind)
    {
        case 3:
        { // cpu: chip body with pins on all four sides
            const D2D1_RECT_F body = D2D1::RectF(c.x - 0.28f * s, c.y - 0.28f * s, c.x + 0.28f * s, c.y + 0.28f * s);
            target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.07f * s, 0.07f * s), brush, stroke);
            const D2D1_RECT_F core = D2D1::RectF(c.x - 0.11f * s, c.y - 0.11f * s, c.x + 0.11f * s, c.y + 0.11f * s);
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
            const D2D1_RECT_F body = D2D1::RectF(c.x - 0.42f * s, c.y - 0.26f * s, c.x + 0.42f * s, c.y + 0.22f * s);
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
            const D2D1_RECT_F body = D2D1::RectF(c.x - 0.44f * s, c.y - 0.24f * s, c.x + 0.44f * s, c.y + 0.26f * s);
            target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
            target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.15f * s, 0.15f * s),
                                 brush, stroke * 0.9f);
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.045f * s, 0.045f * s),
                                 brush);
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
                target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, y), 0.36f * s, 0.12f * s), brush, stroke * 0.9f);
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

void Renderer::draw_weather_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                                      float scale, bool has_weather, const std::wstring& w_icon,
                                      const std::wstring& w_text)
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
        target_->DrawTextW(w_temp, static_cast<UINT32>(std::wcslen(w_temp)), huge_text_format_.Get(),
                           D2D1::RectF(hero_left + 52.0f * scale, reading_center_y - 32.0f * scale, hero_right,
                                       reading_center_y + 32.0f * scale),
                           text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        huge_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    const float desc_top = rect.top + 120.0f * scale;
    const float desc_bottom = rect.top + 162.0f * scale;

    const std::size_t desc_length = desc.length();
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
    target_->CreateSolidColorBrush(with_alpha(material_.hairline, material_.hairline.a * settings_opacity_), &divider);
    target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(divider_x, rect.top + 34.0f * scale,
                                                                divider_x + 1.0f * scale, rect.bottom - 34.0f * scale),
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
            target_->DrawTextW(
                m.label, static_cast<UINT32>(std::wcslen(m.label)), small_text_format_.Get(),
                D2D1::RectF(text_left, card.top + 4.0f * scale, card.right - 8.0f * scale, card.top + 20.0f * scale),
                muted_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            small_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        if (text_format_)
        {
            text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text_brush_->SetOpacity(0.95f);
            target_->DrawTextW(
                m.value.c_str(), static_cast<UINT32>(m.value.size()), text_format_.Get(),
                D2D1::RectF(text_left, card.top + 19.0f * scale, card.right - 8.0f * scale, card.bottom - 3.0f * scale),
                text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        card_top += card_h + card_gap;
    }

    text_brush_->SetOpacity(0.96f);
    muted_brush_->SetOpacity(0.75f);
}

// Load color is semantic: accent when comfortable, amber under pressure, red when saturated.
D2D1_COLOR_F Renderer::load_state_color(float fraction) const
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

void Renderer::draw_git_hub_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale)
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
            target_->DrawTextW(
                loading, static_cast<UINT32>(std::wcslen(loading)), small_text_format_.Get(),
                D2D1::RectF(rect.left + pad_x, rect.top + 80.0f * scale, rect.right - pad_x, rect.top + 104.0f * scale),
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
            const D2D1_ROUNDED_RECT square = D2D1::RoundedRect(D2D1::RectF(x, y, x + cell, y + cell), radius, radius);
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
            target_->DrawTextW(stat.text, static_cast<UINT32>(std::wcslen(stat.text)), small_text_format_.Get(),
                               D2D1::RectF(grid_left, stats_top, grid_left + grid_width, stats_bottom),
                               text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        small_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    text_brush_->SetOpacity(1.0f);
}

void Renderer::draw_hardware_monitor_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
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
            hw_title, static_cast<UINT32>(std::wcslen(hw_title)), bold_text_format_.Get(),
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
            target_->DrawTextW(m.label, static_cast<UINT32>(std::wcslen(m.label)), small_text_format_.Get(),
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
            target_->DrawTextW(m.value, static_cast<UINT32>(std::wcslen(m.value)), text_format_.Get(),
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

void Renderer::draw_time_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
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
            target_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(glow_center, D2D1::Point2F(0, 0), 150.0f * scale, 70.0f * scale),
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
                        D2D1::RoundedRect(D2D1::RectF(pill_x, pill_y, pill_x + pill_w, pill_y + pill_h), pill_h * 0.5f,
                                          pill_h * 0.5f),
                        badge_bg.Get());
                }

                ComPtr<ID2D1SolidColorBrush> dot_brush;
                dot_color.a = settings_opacity_;
                target_->CreateSolidColorBrush(dot_color, &dot_brush);
                if (dot_brush)
                {
                    const float badge_dot_r = 3.5f * scale;
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pill_x + 10.0f * scale, pill_y + pill_h * 0.5f),
                                                       badge_dot_r, badge_dot_r),
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

void Renderer::draw_idle_dashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now)
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
        target_->PushLayer(D2D1::LayerParameters(rect, dash_mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), dash_layer);
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
        const float inner_left = rect.left + idle_strip_layout::pad_x * scale;
        const float inner_right = rect.right - idle_strip_layout::pad_x * scale - privacy_reserve;

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
        target_->DrawTextW(time_buf, static_cast<UINT32>(std::wcslen(time_buf)), idle_fmt, time_rect, text_brush_.Get(),
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

            const D2D1_RECT_F w_rect = D2D1::RectF(slot_x, rect.top, slot_x + idle_metrics.weather_width, rect.bottom);
            target_->DrawTextW(weather_label, static_cast<UINT32>(std::wcslen(weather_label)), idle_fmt, w_rect,
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
                                    draw_weather_dashboard(state, full_rect, settings, now, scale, has_weather, w_icon,
                                                           w_text);
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

void Renderer::draw_game_overlay(const SharedState& state, D2D1_RECT_F rect, float unused_scale)
{
    (void)unused_scale;
    const float scale = 1.0f;
    const bool compact = g_settings.game_overlay_compact;
    const game_overlay_layout::Metrics m = game_overlay_layout::metrics_for(compact);

    const float card_top = rect.top + m.pad_y;
    const float card_bottom = rect.bottom - m.pad_y;
    float cursor_x = rect.left + m.pad_x;

    // FPS gets a wider card and a larger figure; it has no 0-100 ceiling, so no load bar.
    if (g_settings.game_overlay_show_fps)
    {
        const D2D1_RECT_F fps_card = D2D1::RectF(cursor_x, card_top, cursor_x + m.fps_w, card_bottom);
        draw_card(fps_card, m.radius);

        ComPtr<ID2D1SolidColorBrush> fps_icon;
        if (SUCCEEDED(
                target_->CreateSolidColorBrush(with_alpha(material_.accent, 0.90f * settings_opacity_), &fps_icon)) &&
            fps_icon)
        {
            draw_metric_glyph(D2D1::Point2F(fps_card.left + 15.0f * scale, fps_card.top + 15.0f * scale), 15.0f * scale,
                              9, fps_icon.Get());
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
            target_->DrawTextW(fps_value, static_cast<UINT32>(std::wcslen(fps_value)), fps_fmt,
                               D2D1::RectF(text_left, top, text_right, card_bottom - 3.0f * scale), text_brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

void Renderer::draw_game_metric_card(D2D1_RECT_F rect, const wchar_t* label, int percent, int glyph, float radius)
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
        target_->DrawTextW(label, static_cast<UINT32>(std::wcslen(label)), small_text_format_.Get(),
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
        target_->DrawTextW(value, static_cast<UINT32>(std::wcslen(value)), text_format_.Get(),
                           D2D1::RectF(text_left, value_top, text_right, rect.bottom - 8.0f * scale), text_brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
                D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom), 1.25f * scale,
                                  1.25f * scale),
                fill_brush.Get());
        }
    }

    text_brush_->SetOpacity(0.90f);
    muted_brush_->SetOpacity(0.58f);
}

}
