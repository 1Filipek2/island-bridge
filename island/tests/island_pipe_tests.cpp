#include "pipe/pipe_protocol.h"
#include <cstdio>
#include <random>

namespace
{

std::vector<std::uint8_t> encode(std::uint8_t category, const std::vector<std::string>& fields)
{
    std::vector<std::uint8_t> out = {1, 1, category, 0};

    for (const auto& field : fields)
    {
        out.push_back(static_cast<std::uint8_t>(field.size() & 0xFF));
        out.push_back(static_cast<std::uint8_t>(field.size() >> 8));
        out.insert(out.end(), field.begin(), field.end());
    }

    return out;
}

int failures = 0;

void check(bool condition, const char* name)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

}

int main()
{
    using namespace island::island_pipe;

    const auto valid = encode(4, {"WhatsApp", "net.whatsapp.WhatsApp", "Alice", "", "Ahoj \xC4\x8D"});
    const auto parsed = parse_message(valid.data(), valid.size());
    check(parsed.has_value(), "valid parses");
    check(parsed && parsed->app == L"WhatsApp", "app field");
    check(parsed && parsed->body == L"Ahoj č", "utf8 body");
    check(parsed && parsed->category == 4, "category");

    for (std::size_t cut = 0; cut < valid.size(); ++cut)
    {
        check(!parse_message(valid.data(), cut).has_value(), "every prefix rejected");
    }

    auto trailing = valid;
    trailing.push_back(0);
    check(!parse_message(trailing.data(), trailing.size()).has_value(), "trailing byte rejected");

    auto bad_version = valid;
    bad_version[0] = 3;
    check(!parse_message(bad_version.data(), bad_version.size()).has_value(), "unknown version rejected");

    auto bad_kind = valid;
    bad_kind[1] = 7;
    check(!parse_message(bad_kind.data(), bad_kind.size()).has_value(), "unknown kind rejected");

    const auto oversized_app = encode(0, {std::string(257, 'a'), "", "", "", ""});
    check(!parse_message(oversized_app.data(), oversized_app.size()).has_value(), "field cap enforced");

    std::vector<std::uint8_t> too_big(max_message_bytes + 1, 0);
    check(!parse_message(too_big.data(), too_big.size()).has_value(), "size cap enforced");

    auto with_icon = valid;
    with_icon[0] = 2;
    const std::vector<std::uint8_t> icon_bytes = {8, 8, 1, 2, 3};
    with_icon.push_back(static_cast<std::uint8_t>(icon_bytes.size()));
    with_icon.push_back(0);
    with_icon.insert(with_icon.end(), icon_bytes.begin(), icon_bytes.end());
    const auto parsed_icon = parse_message(with_icon.data(), with_icon.size());
    check(parsed_icon.has_value(), "v2 with icon parses");
    check(parsed_icon && parsed_icon->icon == icon_bytes, "v2 icon bytes");
    check(parsed_icon && parsed_icon->app == L"WhatsApp", "v2 keeps fields");
    check(parsed && parsed->icon.empty(), "v1 has no icon");

    for (std::size_t cut = 0; cut < with_icon.size(); ++cut)
    {
        check(!parse_message(with_icon.data(), cut).has_value(), "every v2 prefix rejected");
    }

    auto v2_empty_icon = valid;
    v2_empty_icon[0] = 2;
    v2_empty_icon.push_back(0);
    v2_empty_icon.push_back(0);
    const auto parsed_empty_icon = parse_message(v2_empty_icon.data(), v2_empty_icon.size());
    check(parsed_empty_icon && parsed_empty_icon->icon.empty(), "v2 with empty icon parses");

    auto v2_without_icon_field = valid;
    v2_without_icon_field[0] = 2;
    check(!parse_message(v2_without_icon_field.data(), v2_without_icon_field.size()).has_value(),
          "v2 needs the icon field");

    auto oversized_icon = valid;
    oversized_icon[0] = 2;
    const std::size_t big_icon = max_icon_bytes + 1;
    oversized_icon.push_back(static_cast<std::uint8_t>(big_icon & 0xFF));
    oversized_icon.push_back(static_cast<std::uint8_t>(big_icon >> 8));
    oversized_icon.insert(oversized_icon.end(), big_icon, 0);
    check(!parse_message(oversized_icon.data(), oversized_icon.size()).has_value(), "icon cap enforced");

    check(static_cast<bool>(decode_icon({16, 16})), "small icon decodes");
    check(!decode_icon({}), "empty icon rejected");
    check(!decode_icon({0, 16}), "zero width rejected");
    check(!decode_icon(std::vector<std::uint8_t>(max_icon_bytes + 1, 1)), "oversized icon bytes rejected");

    check(sanitize(L"\u202Eevil\r\nline\x01", 100) == L"evil line", "sanitize controls and bidi");
    check(sanitize(L"ab\U0001F600", 3) == L"ab", "no split surrogate");
    check(sanitize(L"  a   b  ", 100) == L"a b", "collapse whitespace");
    check(sanitize(std::wstring(1, static_cast<wchar_t>(0xD800)) + L"x", 10) == L"x", "lone surrogate dropped");

    std::mt19937 random(12345);
    std::size_t accepted = 0;

    for (int i = 0; i < 2000000; ++i)
    {
        std::vector<std::uint8_t> data(random() % 600);

        for (auto& value : data)
        {
            value = static_cast<std::uint8_t>(random());
        }

        if (data.size() >= 2 && (random() & 1))
        {
            data[0] = static_cast<std::uint8_t>(1 + (random() & 1));
            data[1] = 1;
        }

        if (const auto message = parse_message(data.data(), data.size()))
        {
            ++accepted;
            sanitize(message->title, 120);
            sanitize(message->body, 512);
        }
    }

    for (int i = 0; i < 200000; ++i)
    {
        auto data = (i & 1) ? with_icon : valid;
        const int mutations = 1 + static_cast<int>(random() % 4);

        for (int m = 0; m < mutations; ++m)
        {
            data[random() % data.size()] = static_cast<std::uint8_t>(random());
        }

        if (const auto message = parse_message(data.data(), data.size()))
        {
            check(message->app.size() <= 256 && message->body.size() <= 3072 && message->icon.size() <= max_icon_bytes,
                  "mutated fields bounded");
            decode_icon(message->icon);
        }
    }

    std::printf("fuzz accepted %zu random inputs, failures %d\n", accepted, failures);
    return failures == 0 ? 0 : 1;
}
