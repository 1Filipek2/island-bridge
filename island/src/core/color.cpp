#include "core/color.h"
#include "core/util.h"

namespace island
{
bool equals_no_case(std::wstring_view a, std::wstring_view b)
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
}

D2D1_COLOR_F color_from_hex(std::wstring text, D2D1_COLOR_F fallback)
{
    const size_t first_char = text.find_first_not_of(L" \t\r\n");
    if (first_char == std::wstring::npos)
    {
        return fallback;
    }
    text = text.substr(first_char, text.find_last_not_of(L" \t\r\n") - first_char + 1);

    if (!text.empty() && text[0] == L'#')
    {
        text.erase(text.begin());
    }

    // Accepted forms: RGB, RGBA, RRGGBB, RRGGBBAA.
    const size_t digits = text.size();
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8)
    {
        return fallback;
    }
    if (text.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos)
    {
        return fallback;
    }

    auto nibble = [](wchar_t c) -> int
    {
        if (c >= L'0' && c <= L'9')
        {
            return c - L'0';
        }
        if (c >= L'a' && c <= L'f')
        {
            return c - L'a' + 10;
        }
        return c - L'A' + 10;
    };

    int r = 0, g = 0, b = 0, a = 255;
    if (digits == 3 || digits == 4)
    {
        r = nibble(text[0]) * 17;
        g = nibble(text[1]) * 17;
        b = nibble(text[2]) * 17;
        if (digits == 4)
        {
            a = nibble(text[3]) * 17;
        }
    }
    else
    {
        r = nibble(text[0]) * 16 + nibble(text[1]);
        g = nibble(text[2]) * 16 + nibble(text[3]);
        b = nibble(text[4]) * 16 + nibble(text[5]);
        if (digits == 8)
        {
            a = nibble(text[6]) * 16 + nibble(text[7]);
        }
    }

    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

namespace
{
static float hue_to_rgb(float p, float q, float t)
{
    if (t < 0.0f)
    {
        t += 1.0f;
    }
    if (t > 1.0f)
    {
        t -= 1.0f;
    }
    if (t < 1.0f / 6.0f)
    {
        return p + (q - p) * 6.0f * t;
    }
    if (t < 1.0f / 2.0f)
    {
        return q;
    }
    if (t < 2.0f / 3.0f)
    {
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    }
    return p;
}

D2D1_COLOR_F hsl_to_rgb(float h, float s, float l, float a = 1.0f)
{
    h = std::fmod(h, 360.0f);
    if (h < 0.0f)
    {
        h += 360.0f;
    }
    s = clamp(s, 0.0f, 1.0f);
    l = clamp(l, 0.0f, 1.0f);

    if (s <= 1e-5f)
    {
        return D2D1::ColorF(l, l, l, a);
    }

    const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    const float p = 2.0f * l - q;
    const float h_norm = h / 360.0f;

    const float r = clamp(hue_to_rgb(p, q, h_norm + 1.0f / 3.0f), 0.0f, 1.0f);
    const float g = clamp(hue_to_rgb(p, q, h_norm), 0.0f, 1.0f);
    const float b = clamp(hue_to_rgb(p, q, h_norm - 1.0f / 3.0f), 0.0f, 1.0f);

    return D2D1::ColorF(r, g, b, a);
}

void rgb_to_hsl(float r, float g, float b, float& h, float& s, float& l)
{
    r = clamp(r, 0.0f, 1.0f);
    g = clamp(g, 0.0f, 1.0f);
    b = clamp(b, 0.0f, 1.0f);

    const float max_val = std::max({r, g, b});
    const float min_val = std::min({r, g, b});
    const float delta = max_val - min_val;

    l = (max_val + min_val) * 0.5f;

    if (delta <= 1e-5f)
    {
        h = 0.0f;
        s = 0.0f;
        return;
    }

    s = (l > 0.5f) ? (delta / (2.0f - max_val - min_val)) : (delta / (max_val + min_val));

    if (max_val == r)
    {
        h = ((g - b) / delta) + (g < b ? 6.0f : 0.0f);
    }
    else if (max_val == g)
    {
        h = ((b - r) / delta) + 2.0f;
    }
    else
    {
        h = ((r - g) / delta) + 4.0f;
    }
    h *= 60.0f;
    if (h < 0.0f)
    {
        h += 360.0f;
    }
    if (h >= 360.0f)
    {
        h -= 360.0f;
    }
}

}

double relative_luminance(D2D1_COLOR_F c)
{
    auto to_linear = [](float channel) -> double
    {
        const double v = clamp(channel, 0.0f, 1.0f);
        return (v <= 0.04045) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * to_linear(c.r) + 0.7152 * to_linear(c.g) + 0.0722 * to_linear(c.b);
}

// Moves the accent away from the background (lighter on dark, darker on light) until it reaches 3:1 contrast.
D2D1_COLOR_F ensure_contrast_against_background(D2D1_COLOR_F candidate, D2D1_COLOR_F bg_color)
{
    float h = 0.0f, s = 0.0f, l = 0.0f;
    rgb_to_hsl(candidate.r, candidate.g, candidate.b, h, s, l);

    if (s > 0.01f)
    {
        s = std::max(s, 0.35f);
    }

    const double bg_lum = relative_luminance(bg_color);
    const bool on_dark = bg_lum < 0.45;

    l = on_dark ? std::min(l, 0.85f) : std::max(l, 0.15f);

    const float step = on_dark ? 0.02f : -0.02f;
    const float limit = on_dark ? 0.95f : 0.06f;

    candidate = hsl_to_rgb(h, s, l, candidate.a);
    auto contrast_of = [&](const D2D1_COLOR_F& c)
    {
        const double lum = relative_luminance(c);
        return (std::max(bg_lum, lum) + 0.05) / (std::min(bg_lum, lum) + 0.05);
    };

    double contrast = contrast_of(candidate);
    while (contrast < 3.0 && (on_dark ? (l < limit) : (l > limit)))
    {
        l += step;
        candidate = hsl_to_rgb(h, s, l, candidate.a);
        contrast = contrast_of(candidate);
    }

    return candidate;
}

}
