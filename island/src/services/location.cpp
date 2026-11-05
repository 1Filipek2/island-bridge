#include "services/location.h"
#include "services/http.h"
#include "host/host.h"

#include <winrt/Windows.Devices.Geolocation.h>

namespace island::location
{
namespace
{

constexpr wchar_t nominatim_host[] = L"nominatim.openstreetmap.org";
constexpr wchar_t user_agent[] = L"IslandBridge.Island/" ISLAND_VERSION;
constexpr auto maximum_age = std::chrono::minutes(10);
constexpr auto locate_timeout = std::chrono::seconds(15);

std::wstring user_language()
{
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};

    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) <= 0)
    {
        return {};
    }

    const std::wstring name = locale;
    return name.substr(0, name.find(L'-'));
}

}

std::optional<geo::Point> current_point()
{
    using namespace winrt::Windows::Devices::Geolocation;

    try
    {
        const GeolocationAccessStatus access = Geolocator::RequestAccessAsync().get();

        if (access != GeolocationAccessStatus::Allowed)
        {
            host::log(L"Location: access status %d", static_cast<int>(access));
            return std::nullopt;
        }

        Geolocator locator;
        locator.DesiredAccuracy(PositionAccuracy::Default);
        const Geoposition position = locator.GetGeopositionAsync(maximum_age, locate_timeout).get();
        const BasicGeoposition coordinate = position.Coordinate().Point().Position();
        const geo::Point point{coordinate.Latitude, coordinate.Longitude};

        if (!geo::valid(point))
        {
            return std::nullopt;
        }

        return geo::rounded(point);
    }
    catch (const winrt::hresult_error& error)
    {
        host::log(L"Location: failed with 0x%08X", static_cast<unsigned>(error.code().value));
        return std::nullopt;
    }
}

std::optional<std::wstring> place_name(geo::Point point)
{
    const std::wstring path = geo::reverse_path(point, user_language());
    const HttpResult result = HttpGetEx(nominatim_host, path.c_str(), true, user_agent);

    if (!result.ok || result.status != 200)
    {
        return std::nullopt;
    }

    return geo::parse_place_name(result.body);
}

}
