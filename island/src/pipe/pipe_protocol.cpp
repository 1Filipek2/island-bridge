#include "pipe/pipe_protocol.h"
#include "ui/image.h"

namespace island
{
namespace island_pipe
{
namespace
{

constexpr std::uint8_t protocol_version = 1;
constexpr std::uint8_t protocol_version_icon = 2;
constexpr std::uint8_t kind_notification = 1;
constexpr std::size_t header_bytes = 4;
constexpr std::size_t field_count = 5;

constexpr std::array<std::size_t, field_count> max_field_bytes = {256, 256, 1024, 1024, 3072};

bool is_bidi_control(wchar_t ch)
{
    return (ch >= 0x202A && ch <= 0x202E) ||
           (ch >= 0x2066 && ch <= 0x2069) ||
           ch == 0x200E ||
           ch == 0x200F ||
           ch == 0x061C;
}

bool is_line_break(wchar_t ch)
{
    return ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == 0x0085 || ch == 0x2028 || ch == 0x2029;
}

bool is_control(wchar_t ch)
{
    return ch < 0x20 || (ch >= 0x7F && ch <= 0x9F);
}

}
}

namespace island_pipe
{


std::wstring sanitize(const std::wstring& text, std::size_t max_chars)
{
    std::wstring result;
    result.reserve(std::min(text.size(), max_chars));
    bool pending_space = false;

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const wchar_t ch = text[i];

        if (is_line_break(ch) || ch == L' ')
        {
            pending_space = !result.empty();
            continue;
        }

        if (is_control(ch) || is_bidi_control(ch))
        {
            continue;
        }

        const bool high_surrogate = ch >= 0xD800 && ch <= 0xDBFF;
        const bool has_low = high_surrogate && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF;

        if ((ch >= 0xDC00 && ch <= 0xDFFF) || (high_surrogate && !has_low))
        {
            continue;
        }

        const std::size_t needed = (has_low ? 2 : 1) + (pending_space ? 1 : 0);

        if (result.size() + needed > max_chars)
        {
            break;
        }

        if (pending_space)
        {
            result.push_back(L' ');
            pending_space = false;
        }

        result.push_back(ch);

        if (has_low)
        {
            result.push_back(text[++i]);
        }
    }

    return result;
}

}

namespace island_pipe
{
namespace
{


std::wstring utf8_to_wide(const std::uint8_t* data, std::size_t size)
{
    if (size == 0 || size > static_cast<std::size_t>(INT_MAX))
    {
        return std::wstring{};
    }

    const char* source = reinterpret_cast<const char*>(data);
    const int length = static_cast<int>(size);
    const int needed = MultiByteToWideChar(CP_UTF8, 0, source, length, nullptr, 0);

    if (needed <= 0)
    {
        return std::wstring{};
    }

    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, source, length, result.data(), needed);
    result.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return result;
}

std::uint16_t read_u16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

}
}

namespace island_pipe
{


std::optional<PipeMessage> parse_message(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size < header_bytes + field_count * 2 || size > max_message_bytes)
    {
        return std::nullopt;
    }

    const bool has_icon = data[0] == protocol_version_icon;

    if ((data[0] != protocol_version && !has_icon) || data[1] != kind_notification)
    {
        return std::nullopt;
    }

    PipeMessage message;
    message.category = data[2];
    message.flags = data[3];

    std::array<std::wstring*, field_count> fields = {&message.app, &message.bundle, &message.title, &message.subtitle, &message.body};
    std::size_t offset = header_bytes;

    for (std::size_t index = 0; index < field_count; ++index)
    {
        if (size - offset < 2)
        {
            return std::nullopt;
        }

        const std::size_t length = read_u16(data + offset);
        offset += 2;

        if (length > max_field_bytes[index] || length > size - offset)
        {
            return std::nullopt;
        }

        *fields[index] = utf8_to_wide(data + offset, length);
        offset += length;
    }

    if (has_icon)
    {
        if (size - offset < 2)
        {
            return std::nullopt;
        }

        const std::size_t length = read_u16(data + offset);
        offset += 2;

        if (length > max_icon_bytes || length > size - offset)
        {
            return std::nullopt;
        }

        message.icon.assign(data + offset, data + offset + length);
        offset += length;
    }

    if (offset != size)
    {
        return std::nullopt;
    }

    return message;
}

PixelsRef decode_icon(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty() || bytes.size() > max_icon_bytes)
    {
        return PixelsRef{};
    }

    BitmapPixels decoded;

    if (!decode_image_bytes_to_pixels(bytes, &decoded))
    {
        return PixelsRef{};
    }

    const std::size_t expected = static_cast<std::size_t>(decoded.width) * decoded.height * 4;

    if (decoded.width == 0 || decoded.height == 0 || decoded.width > max_icon_dimension || decoded.height > max_icon_dimension || decoded.bgra.size() != expected)
    {
        return PixelsRef{};
    }

    return PixelsRef(std::move(decoded));
}

}

}
