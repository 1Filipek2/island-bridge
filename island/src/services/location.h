#pragma once

#include "services/geo.h"

#include <optional>
#include <string>

namespace island::location
{

std::optional<geo::Point> current_point();
std::optional<std::wstring> place_name(geo::Point point);

}
