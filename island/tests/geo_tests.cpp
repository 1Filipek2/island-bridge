#include "services/geo.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace
{

namespace geo = island::geo;

int failures = 0;

void check(bool condition, const char* name)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

void check_parsing()
{
    const auto city = geo::parse_place_name(R"({"place_id":1,"name":"x","address":{"city":"Trnava","county":"okres Trnava","country":"Slovensko"}})");
    check(city && *city == L"Trnava", "city wins");

    const auto town = geo::parse_place_name(R"({"address":{"county":"okres","town":"\u017dilina"}})");
    check(town && *town == L"\u017Dilina", "town with unicode escape");

    const auto utf8 = geo::parse_place_name("{\"address\":{\"village\":\"Ko\xC5\xA1ice\"}}");
    check(utf8 && *utf8 == L"Ko\u0161ice", "raw utf-8 village");

    const auto nested = geo::parse_place_name(R"({"extratags":{"city":"Wrong"},"address":{"town":"Right"}})");
    check(nested && *nested == L"Right", "only the address object counts");

    const auto surrogate = geo::parse_place_name(R"({"address":{"city":"A\ud83d\ude00B"}})");
    check(surrogate && *surrogate == L"A\U0001F600B", "surrogate pair");

    const auto hidden = geo::parse_place_name(R"({"address":{"city":"\u202eEvil\n\u200bName"}})");
    check(hidden && *hidden == L"Evil Name", "bidi and control chars removed");

    const auto blank = geo::parse_place_name(R"({"address":{"city":"   ","town":"Backup"}})");
    check(blank && *blank == L"Backup", "blank city falls through");

    const std::string long_name(200, 'a');
    const auto capped = geo::parse_place_name("{\"address\":{\"city\":\"" + long_name + "\"}}");
    check(capped && capped->size() == geo::max_place_chars, "name capped");

    check(!geo::parse_place_name(R"({"error":"Unable to geocode"})"), "error response");
    check(!geo::parse_place_name(R"({"address":{"road":"Main"}})"), "no place key");
    check(!geo::parse_place_name(R"({"address":{"city":"bad\x"}})"), "bad escape");
    check(!geo::parse_place_name(R"({"address":{"city":"\ud83d"}})"), "lone surrogate");
    check(!geo::parse_place_name("{\"address\":{\"city\":\"\xC3\x28\"}}"), "invalid utf-8");
    check(!geo::parse_place_name(R"({"address":{"city":"open)"), "truncated");
    check(!geo::parse_place_name(""), "empty");
}

void check_points()
{
    check(geo::valid({48.37, 17.58}), "valid point");
    check(!geo::valid({0.0, 0.0}), "null island");
    check(!geo::valid({91.0, 0.0}), "latitude range");
    check(!geo::valid({0.0, -181.0}), "longitude range");
    check(!geo::valid({std::nan(""), 1.0}), "nan");

    const geo::Point r = geo::rounded({48.37773, 17.58603});
    check(std::fabs(r.latitude - 48.38) < 1e-9 && std::fabs(r.longitude - 17.59) < 1e-9, "rounded");

    const double km = geo::distance_km({48.37773, 17.58603}, {49.06651, 18.92399});
    check(km > 124.0 && km < 125.0, "trnava to martin");
    check(geo::distance_km({48.0, 17.0}, {48.0, 17.0}) == 0.0, "zero distance");

    check(geo::format_coordinate(48.38) == L"48.38", "format");
    check(geo::format_coordinate(-0.5) == L"-0.50", "format negative");

    check(geo::reverse_path({48.38, 17.59}, L"sk") ==
              L"/reverse?format=jsonv2&zoom=10&addressdetails=1&lat=48.38&lon=17.59&accept-language=sk",
          "reverse path");
    check(geo::reverse_path({48.38, 17.59}, L"sk&x=1").ends_with(L"&accept-language=sk"), "language sanitized");
    check(geo::reverse_path({48.38, 17.59}, L"").ends_with(L"&lon=17.59"), "no language");
}

void fuzz()
{
    std::mt19937 random(1234);
    const std::string seed = R"({"address":{"city":"Trnava","town":"x"},"a":[1,{"b":"c"}]})";

    for (int i = 0; i < 200000; ++i)
    {
        std::string input = seed;
        const int edits = 1 + static_cast<int>(random() % 6);

        for (int e = 0; e < edits; ++e)
        {
            const std::size_t at = random() % (input.size() + 1);

            switch (random() % 3)
            {
            case 0:
                input.insert(input.begin() + static_cast<std::ptrdiff_t>(at), static_cast<char>(random() % 256));
                break;
            case 1:
                if (at < input.size())
                {
                    input.erase(at, 1);
                }
                break;
            default:
                if (at < input.size())
                {
                    input[at] = static_cast<char>(random() % 256);
                }
                break;
            }
        }

        const auto name = geo::parse_place_name(input);

        if (name)
        {
            check(!name->empty() && name->size() <= geo::max_place_chars, "fuzz bounds");
        }
    }
}

}

int main()
{
    check_parsing();
    check_points();
    fuzz();

    if (failures == 0)
    {
        std::printf("geo tests passed\n");
    }

    return failures == 0 ? 0 : 1;
}
