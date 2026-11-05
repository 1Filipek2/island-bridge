#include "services/geo.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <vector>

namespace island::geo
{
namespace
{

constexpr double earth_radius_km = 6371.0;
constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
constexpr std::size_t max_language_chars = 16;
constexpr std::size_t max_json_bytes = 64 * 1024;
constexpr std::array<std::string_view, 6> place_keys = {"city", "town", "village", "municipality", "suburb", "county"};

struct Field
{
    std::string_view key;
    std::string_view raw;
};

bool is_hidden(wchar_t ch)
{
    return ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) || (ch >= 0x200B && ch <= 0x200F) ||
           (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2066 && ch <= 0x2069) || ch == 0x061C || ch == 0xFEFF;
}

void append_utf8(std::string& out, std::uint32_t code)
{
    if (code < 0x80)
    {
        out.push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else if (code < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::optional<std::uint32_t> read_hex4(std::string_view text, std::size_t at)
{
    if (at + 4 > text.size())
    {
        return std::nullopt;
    }

    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data() + at, text.data() + at + 4, value, 16);

    if (result.ec != std::errc{} || result.ptr != text.data() + at + 4)
    {
        return std::nullopt;
    }

    return value;
}

std::optional<std::string> unescape(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());

    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        const char ch = raw[i];

        if (ch != '\\')
        {
            out.push_back(ch);
            continue;
        }

        if (++i >= raw.size())
        {
            return std::nullopt;
        }

        switch (raw[i])
        {
        case '"':
        case '\\':
        case '/':
            out.push_back(raw[i]);
            break;
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            out.push_back(' ');
            break;
        case 'u':
        {
            auto code = read_hex4(raw, i + 1);

            if (!code)
            {
                return std::nullopt;
            }

            i += 4;

            if (*code >= 0xD800 && *code <= 0xDBFF)
            {
                if (i + 2 >= raw.size() || raw[i + 1] != '\\' || raw[i + 2] != 'u')
                {
                    return std::nullopt;
                }

                const auto low = read_hex4(raw, i + 3);

                if (!low || *low < 0xDC00 || *low > 0xDFFF)
                {
                    return std::nullopt;
                }

                code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                i += 6;
            }
            else if (*code >= 0xDC00 && *code <= 0xDFFF)
            {
                return std::nullopt;
            }

            append_utf8(out, *code);
            break;
        }
        default:
            return std::nullopt;
        }
    }

    return out;
}

std::optional<std::wstring> to_wide(const std::string& utf8)
{
    if (utf8.empty())
    {
        return std::wstring{};
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);

    if (length <= 0)
    {
        return std::nullopt;
    }

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

std::wstring clean(const std::wstring& text)
{
    std::wstring out;

    for (const wchar_t ch : text)
    {
        if (!is_hidden(ch))
        {
            out.push_back(ch);
        }
    }

    const auto first = out.find_first_not_of(L' ');

    if (first == std::wstring::npos)
    {
        return {};
    }

    out = out.substr(first, out.find_last_not_of(L' ') - first + 1);

    if (out.size() > max_place_chars)
    {
        out.resize(max_place_chars);

        if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF)
        {
            out.pop_back();
        }
    }

    return out;
}

std::optional<std::size_t> string_end(std::string_view json, std::size_t open)
{
    for (std::size_t i = open + 1; i < json.size(); ++i)
    {
        if (json[i] == '\\')
        {
            ++i;
        }
        else if (json[i] == '"')
        {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> find_address_object(std::string_view json)
{
    int depth = 0;

    for (std::size_t i = 0; i < json.size(); ++i)
    {
        const char ch = json[i];

        if (ch == '"')
        {
            const auto end = string_end(json, i);

            if (!end)
            {
                return std::nullopt;
            }

            const std::string_view key = json.substr(i + 1, *end - i - 1);
            i = *end;

            if (depth != 1 || key != "address")
            {
                continue;
            }

            std::size_t j = i + 1;

            while (j < json.size() && (json[j] == ' ' || json[j] == ':' || json[j] == '\n' || json[j] == '\r' || json[j] == '\t'))
            {
                ++j;
            }

            if (j < json.size() && json[j] == '{')
            {
                return j;
            }
        }
        else if (ch == '{' || ch == '[')
        {
            ++depth;
        }
        else if (ch == '}' || ch == ']')
        {
            --depth;
        }
    }

    return std::nullopt;
}

std::optional<std::vector<Field>> address_fields(std::string_view json, std::size_t open)
{
    std::vector<Field> fields;
    std::string_view pending_key;
    bool expecting_value = false;
    int depth = 0;

    for (std::size_t i = open; i < json.size(); ++i)
    {
        const char ch = json[i];

        if (ch == '"')
        {
            const auto end = string_end(json, i);

            if (!end)
            {
                return std::nullopt;
            }

            const std::string_view text = json.substr(i + 1, *end - i - 1);
            i = *end;

            if (depth != 1)
            {
                continue;
            }

            if (expecting_value)
            {
                fields.push_back(Field{pending_key, text});
                expecting_value = false;
            }
            else
            {
                pending_key = text;
            }
        }
        else if (ch == ':' && depth == 1)
        {
            expecting_value = true;
        }
        else if (ch == ',' && depth == 1)
        {
            expecting_value = false;
        }
        else if (ch == '{' || ch == '[')
        {
            ++depth;
            expecting_value = false;
        }
        else if (ch == '}' || ch == ']')
        {
            if (--depth == 0)
            {
                return fields;
            }
        }
    }

    return std::nullopt;
}

}

bool valid(Point point)
{
    return std::isfinite(point.latitude) && std::isfinite(point.longitude) && point.latitude >= -90.0 &&
           point.latitude <= 90.0 && point.longitude >= -180.0 && point.longitude <= 180.0 &&
           !(point.latitude == 0.0 && point.longitude == 0.0);
}

Point rounded(Point point)
{
    return Point{std::round(point.latitude * 100.0) / 100.0, std::round(point.longitude * 100.0) / 100.0};
}

double distance_km(Point from, Point to)
{
    const double d_lat = (to.latitude - from.latitude) * degrees_to_radians;
    const double d_lon = (to.longitude - from.longitude) * degrees_to_radians;
    const double a = std::sin(d_lat / 2) * std::sin(d_lat / 2) + std::cos(from.latitude * degrees_to_radians) *
                     std::cos(to.latitude * degrees_to_radians) * std::sin(d_lon / 2) * std::sin(d_lon / 2);
    return 2.0 * earth_radius_km * std::asin(std::sqrt(std::min(1.0, a)));
}

std::wstring format_coordinate(double value)
{
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, 2);
    return std::wstring(buffer.data(), result.ptr);
}

std::wstring reverse_path(Point point, std::wstring_view language)
{
    std::wstring path = L"/reverse?format=jsonv2&zoom=10&addressdetails=1&lat=" + format_coordinate(point.latitude) +
                        L"&lon=" + format_coordinate(point.longitude);
    std::wstring tag;

    for (const wchar_t ch : language)
    {
        if (tag.size() >= max_language_chars)
        {
            break;
        }

        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'-')
        {
            tag.push_back(ch);
        }
        else
        {
            break;
        }
    }

    if (!tag.empty())
    {
        path += L"&accept-language=" + tag;
    }

    return path;
}

std::optional<std::wstring> parse_place_name(std::string_view json)
{
    if (json.empty() || json.size() > max_json_bytes)
    {
        return std::nullopt;
    }

    const auto open = find_address_object(json);

    if (!open)
    {
        return std::nullopt;
    }

    const auto fields = address_fields(json, *open);

    if (!fields)
    {
        return std::nullopt;
    }

    for (const std::string_view wanted : place_keys)
    {
        for (const Field& field : *fields)
        {
            if (field.key != wanted)
            {
                continue;
            }

            const auto utf8 = unescape(field.raw);

            if (!utf8)
            {
                continue;
            }

            const auto wide = to_wide(*utf8);

            if (!wide)
            {
                continue;
            }

            std::wstring name = clean(*wide);

            if (!name.empty())
            {
                return name;
            }
        }
    }

    return std::nullopt;
}

}
