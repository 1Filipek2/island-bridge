#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace island::geo
{

constexpr double rename_distance_km = 2.0;
constexpr std::size_t max_place_chars = 48;

struct Point
{
    double latitude = 0.0;
    double longitude = 0.0;
};

bool valid(Point point);
Point rounded(Point point);
double distance_km(Point from, Point to);
std::wstring format_coordinate(double value);
std::wstring reverse_path(Point point, std::wstring_view language);
std::optional<std::wstring> parse_place_name(std::string_view json);

}
