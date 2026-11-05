#pragma once

#include "services/geo.h"

#include <optional>
#include <string>

namespace island::location
{

enum class Status
{
    Unknown,
    Ready,
    ServiceDisabled,
    Denied,
    Unavailable
};

std::optional<geo::Point> current_point();
std::optional<std::wstring> place_name(geo::Point point);
Status status();
bool can_fix(Status value);
void fix_access(void* refresh_event);

}
