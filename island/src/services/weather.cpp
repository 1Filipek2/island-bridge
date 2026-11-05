#include "services/weather.h"
#include "services/http.h"
#include "core/model.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/localization.h"
#include "core/util.h"
#include "services/location.h"

namespace island
{
namespace
{
std::vector<WeatherCityEntry> parse_weather_cities(const std::wstring& spec)
{
    std::vector<WeatherCityEntry> cities;
    size_t start = 0;
    while (start <= spec.size() && cities.size() < weather_city_menu_max)
    {
        size_t end = spec.find(L';', start);
        if (end == std::wstring::npos)
        {
            end = spec.size();
        }
        const std::wstring item = spec.substr(start, end - start);
        start = end + 1;

        const size_t first = item.find(L'|');
        const size_t second = first == std::wstring::npos ? first : item.find(L'|', first + 1);
        if (second == std::wstring::npos)
        {
            continue;
        }

        WeatherCityEntry city{item.substr(0, first), item.substr(first + 1, second - first - 1),
                              item.substr(second + 1)};
        wchar_t* lat_end = nullptr;
        wchar_t* lon_end = nullptr;
        const double lat = wcstod(city.latitude.c_str(), &lat_end);
        const double lon = wcstod(city.longitude.c_str(), &lon_end);
        if (city.name.empty() || city.latitude.empty() || city.longitude.empty() || *lat_end != L'\0' ||
            *lon_end != L'\0' || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        {
            host::log(L"Weather: ignoring invalid city entry: %s", item.c_str());
            continue;
        }
        cities.push_back(std::move(city));
    }
    return cities;
}

}

std::vector<WeatherCityEntry> load_weather_cities()
{
    return parse_weather_cities(get_string_setting_copy(L"Modules.WeatherCities"));
}

bool is_weather_city_auto()
{
    return host::int_value(L"WeatherCityIndex", weather_city_auto) == weather_city_auto;
}

int get_active_weather_city_index(size_t city_count)
{
    int index = host::int_value(L"WeatherCityIndex", weather_city_auto);
    if (index == weather_city_auto)
    {
        index = g_auto_weather_city_index.load();
    }
    return (index >= 0 && static_cast<size_t>(index) < city_count) ? index : 0;
}

namespace
{
static bool read_json_double(const std::string& json, const char* key, double& out)
{
    const char* field = strstr(json.c_str(), key);
    if (!field)
    {
        return false;
    }
    field += strlen(key);
    while (*field == ' ' || *field == '"')
    {
        ++field;
    }
    char* end = nullptr;
    out = strtod(field, &end);
    return end != field;
}

// IP geolocation -> index of the nearest configured city, or -1.
int find_nearest_weather_city(const std::vector<WeatherCityEntry>& cities)
{
    struct Provider
    {
        const wchar_t* host;
        const wchar_t* path;
        const char* lat_key;
        const char* lon_key;
    };

    static const Provider providers[] = {
        {L"ipwho.is", L"/?fields=success,latitude,longitude", "\"latitude\":", "\"longitude\":"},
        {L"ipapi.co", L"/json/", "\"latitude\":", "\"longitude\":"},
    };
    for (const Provider& provider : providers)
    {
        const std::string response = http_get(provider.host, provider.path, true);
        double lat = 0.0, lon = 0.0;
        if (response.empty() || !read_json_double(response, provider.lat_key, lat) ||
            !read_json_double(response, provider.lon_key, lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 ||
            lon > 180.0 || (lat == 0.0 && lon == 0.0))
        {
            host::log(L"Weather: location lookup via %s failed", provider.host);
            continue;
        }
        int best = -1;
        double best_distance = 0.0;
        const double to_rad = 3.14159265358979323846 / 180.0;
        for (size_t i = 0; i < cities.size(); ++i)
        {
            const double c_lat = wcstod(cities[i].latitude.c_str(), nullptr);
            const double c_lon = wcstod(cities[i].longitude.c_str(), nullptr);
            const double d_lat = (c_lat - lat) * to_rad;
            const double d_lon = (c_lon - lon) * to_rad;
            const double a = sin(d_lat / 2) * sin(d_lat / 2) +
                             cos(lat * to_rad) * cos(c_lat * to_rad) * sin(d_lon / 2) * sin(d_lon / 2);
            if (best < 0 || a < best_distance)
            {
                best = static_cast<int>(i);
                best_distance = a;
            }
        }
        host::log(L"Weather: IP location resolved to a configured city");
        return best;
    }
    return -1;
}

// Open-Meteo WMO code -> the WWO code the icons are keyed on, plus an English description.
int wwo_code_from_wmo(int wmo, std::wstring& desc)
{
    switch (wmo)
    {
        case 0:
            desc = L"Clear";
            return 113;
        case 1:
            desc = L"Mainly clear";
            return 113;
        case 2:
            desc = L"Partly cloudy";
            return 116;
        case 3:
            desc = L"Overcast";
            return 122;
        case 45:
            desc = L"Fog";
            return 248;
        case 48:
            desc = L"Freezing fog";
            return 260;
        case 51:
            desc = L"Light drizzle";
            return 263;
        case 53:
            desc = L"Drizzle";
            return 266;
        case 55:
            desc = L"Heavy drizzle";
            return 266;
        case 56:
            desc = L"Freezing drizzle";
            return 281;
        case 57:
            desc = L"Heavy freezing drizzle";
            return 284;
        case 61:
            desc = L"Light rain";
            return 296;
        case 63:
            desc = L"Rain";
            return 302;
        case 65:
            desc = L"Heavy rain";
            return 308;
        case 66:
            desc = L"Freezing rain";
            return 311;
        case 67:
            desc = L"Heavy freezing rain";
            return 314;
        case 71:
            desc = L"Light snow";
            return 326;
        case 73:
            desc = L"Snow";
            return 332;
        case 75:
            desc = L"Heavy snow";
            return 338;
        case 77:
            desc = L"Snow grains";
            return 326;
        case 80:
            desc = L"Light showers";
            return 353;
        case 81:
            desc = L"Showers";
            return 356;
        case 82:
            desc = L"Heavy showers";
            return 359;
        case 85:
            desc = L"Snow showers";
            return 368;
        case 86:
            desc = L"Heavy snow showers";
            return 371;
        case 95:
            desc = L"Thunderstorm";
            return 389;
        case 96:
            desc = L"Thunderstorm with hail";
            return 392;
        case 99:
            desc = L"Severe thunderstorm with hail";
            return 395;
        default:
            desc = L"Unknown";
            return 0;
    }
}

struct LocatedPlace
{
    geo::Point point;
    std::wstring name;
    bool named = false;
};

std::wstring nearest_configured_name(const std::vector<WeatherCityEntry>& cities, geo::Point point)
{
    std::wstring best;
    double best_km = 0.0;
    for (const WeatherCityEntry& city : cities)
    {
        const geo::Point candidate{wcstod(city.latitude.c_str(), nullptr), wcstod(city.longitude.c_str(), nullptr)};
        const double km = geo::distance_km(point, candidate);
        if (best.empty() || km < best_km)
        {
            best = city.name;
            best_km = km;
        }
    }
    return best;
}

std::optional<WeatherCityEntry> locate_city(const std::vector<WeatherCityEntry>& cities,
                                            std::optional<LocatedPlace>& place)
{
    const std::optional<geo::Point> point = location::current_point();
    if (!point)
    {
        host::log(L"Weather: Windows location unavailable");
        return std::nullopt;
    }

    const bool moved = !place || geo::distance_km(place->point, *point) > geo::rename_distance_km;
    if (moved || !place->named)
    {
        const std::optional<std::wstring> name = location::place_name(*point);
        if (name)
        {
            place = LocatedPlace{*point, *name, true};
        }
        else if (moved)
        {
            const std::wstring fallback = nearest_configured_name(cities, *point);
            place = LocatedPlace{*point, fallback.empty() ? std::wstring(loc(L"Current location")) : fallback, false};
        }
        host::log(L"Weather: place name %s", name ? L"resolved" : L"unavailable");
    }
    else
    {
        place->point = *point;
    }

    return WeatherCityEntry{place->name, geo::format_coordinate(point->latitude),
                            geo::format_coordinate(point->longitude)};
}

std::wstring wind_degrees_to_compass(float degrees)
{
    static const wchar_t* const k_points[] = {L"N", L"NNE", L"NE", L"ENE", L"E", L"ESE", L"SE", L"SSE",
                                              L"S", L"SSW", L"SW", L"WSW", L"W", L"WNW", L"NW", L"NNW"};
    const int sector = static_cast<int>((fmod(degrees, 360.0f) + 360.0f + 11.25f) / 22.5f) % 16;
    return k_points[sector];
}

// Reads only from the "current" object, so "current_units" is never mistaken for a reading.
bool read_current_number(const char* current, const char* key, float& out)
{
    const char* field = strstr(current, key);
    if (!field)
    {
        return false;
    }
    field += strlen(key);
    return sscanf(field, "%f", &out) == 1;
}

}

DWORD WINAPI weather_thread_proc(void*)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    WaitForSingleObject(g_stop_event, 3000);

    // Failures back off; a good reading refreshes every 15 min (Open-Meteo's update rate).
    static const DWORD backoff_ms[] = {60 * 1000, 2 * 60 * 1000, 5 * 60 * 1000, 15 * 60 * 1000};
    size_t backoff_index = 0;
    std::wstring previous_key;
    std::optional<LocatedPlace> located_place;

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        bool is_fahrenheit = false;
        bool weather_enabled = true;
        {
            std::lock_guard lock(g_settings_mutex);
            weather_enabled = g_settings.weather;
            is_fahrenheit = g_settings.weather_fahrenheit;
        }

        if (!weather_enabled)
        {
            WaitForSingleObject(g_stop_event, 2000);
            continue;
        }

        const std::vector<WeatherCityEntry> cities = load_weather_cities();
        bool weather_committed = false;
        std::optional<WeatherCityEntry> located;
        if (is_weather_city_auto())
        {
            located = locate_city(cities, located_place);
        }

        if (cities.empty() && !located)
        {
            host::log(L"Weather: no valid city in Modules.WeatherCities.");
        }
        else
        {
            if (!located && is_weather_city_auto())
            {
                const int nearest = find_nearest_weather_city(cities);
                if (nearest >= 0)
                {
                    g_auto_weather_city_index = nearest;
                }
            }
            const WeatherCityEntry& city = located ? *located : cities[get_active_weather_city_index(cities.size())];
            const std::wstring key =
                located ? L"located|" + city.name : city.name + L"|" + city.latitude + L"|" + city.longitude;
            if (key != previous_key)
            {
                // Never show one city's reading under another city's name.
                std::lock_guard lock(g_state_mutex);
                g_state.weather.has_data = false;
                g_state.weather.city = city.name;
                previous_key = key;
            }

            std::wstring path = L"/v1/forecast?latitude=" + city.latitude + L"&longitude=" + city.longitude +
                                L"&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
                                L"weather_code,wind_speed_10m,wind_direction_10m&timezone=auto";
            if (is_fahrenheit)
            {
                path += L"&temperature_unit=fahrenheit&wind_speed_unit=mph";
            }

            host::log(L"Weather: requesting Open-Meteo");
            const std::string response = http_get(L"api.open-meteo.com", path.c_str(), true);
            const char* current = response.empty() ? nullptr : strstr(response.c_str(), "\"current\":{");

            float temp = 0.0f, feels = 0.0f, humidity = 0.0f, wind = 0.0f, wind_deg = 0.0f, wmo = 0.0f;
            if (current && read_current_number(current, "\"temperature_2m\":", temp) &&
                read_current_number(current, "\"weather_code\":", wmo))
            {
                const bool has_feels = read_current_number(current, "\"apparent_temperature\":", feels);
                const bool has_humidity = read_current_number(current, "\"relative_humidity_2m\":", humidity);
                const bool has_wind = read_current_number(current, "\"wind_speed_10m\":", wind);
                const bool has_wind_dir = read_current_number(current, "\"wind_direction_10m\":", wind_deg);

                // Whole degrees; + 0.0f turns -0 into 0.
                temp = roundf(temp) + 0.0f;
                feels = roundf(feels) + 0.0f;

                std::wstring desc;
                const int code = wwo_code_from_wmo(static_cast<int>(wmo), desc);
                wchar_t buffer[32];

                std::lock_guard lock(g_state_mutex);
                g_state.weather.has_data = true;
                g_state.weather.temperature = temp;
                g_state.weather.weather_code = code;
                g_state.weather.city = city.name;
                g_state.weather.weather_desc = desc;
                swprintf_s(buffer, L"%.0f", wind);
                g_state.weather.wind_speed = has_wind ? buffer : L"--";
                g_state.weather.wind_dir = has_wind_dir ? wind_degrees_to_compass(wind_deg) : L"";
                swprintf_s(buffer, L"%.0f", humidity);
                g_state.weather.humidity = has_humidity ? buffer : L"--";
                swprintf_s(buffer, L"%.0f", feels);
                g_state.weather.feels_like = has_feels ? buffer : L"--";
                g_state.weather.last_updated = now_seconds();
                weather_committed = true;
                host::log(L"Weather: reading updated");
            }
            else
            {
                host::log(L"Weather: no usable reading from Open-Meteo, keeping previous data.");
            }
        }

        DWORD wait_ms;
        if (weather_committed)
        {
            backoff_index = 0;
            wait_ms = 15 * 60 * 1000;
        }
        else
        {
            wait_ms = backoff_ms[backoff_index];
            if (backoff_index + 1 < ARRAYSIZE(backoff_ms))
            {
                ++backoff_index;
            }
        }

        HANDLE events[] = {g_stop_event, g_settings_changed_event};
        DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, wait_ms);
        if (wait_result == WAIT_OBJECT_0)
        {
            break;
        }
        if (wait_result == WAIT_OBJECT_0 + 1)
        {
            backoff_index = 0;
        }
    }
    winrt::uninit_apartment();
    return 0;
}

}
