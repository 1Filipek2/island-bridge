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


std::vector<WeatherCityEntry> ParseWeatherCities(const std::wstring& spec) {
    std::vector<WeatherCityEntry> cities;
    size_t start = 0;
    while (start <= spec.size() && cities.size() < kWeatherCityMenuMax) {
        size_t end = spec.find(L';', start);
        if (end == std::wstring::npos) end = spec.size();
        const std::wstring item = spec.substr(start, end - start);
        start = end + 1;

        const size_t first = item.find(L'|');
        const size_t second = first == std::wstring::npos ? first : item.find(L'|', first + 1);
        if (second == std::wstring::npos) continue;

        WeatherCityEntry city{item.substr(0, first), item.substr(first + 1, second - first - 1),
                              item.substr(second + 1)};
        wchar_t* latEnd = nullptr;
        wchar_t* lonEnd = nullptr;
        const double lat = wcstod(city.latitude.c_str(), &latEnd);
        const double lon = wcstod(city.longitude.c_str(), &lonEnd);
        if (city.name.empty() || city.latitude.empty() || city.longitude.empty() || *latEnd != L'\0' ||
            *lonEnd != L'\0' || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            host::log(L"Weather: ignoring invalid city entry: %s", item.c_str());
            continue;
        }
        cities.push_back(std::move(city));
    }
    return cities;
}

}



std::vector<WeatherCityEntry> LoadWeatherCities() {
    return ParseWeatherCities(GetStringSettingCopy(L"Modules.WeatherCities"));
}

bool IsWeatherCityAuto() {
    return host::int_value(L"WeatherCityIndex", kWeatherCityAuto) == kWeatherCityAuto;
}

int GetActiveWeatherCityIndex(size_t cityCount) {
    int index = host::int_value(L"WeatherCityIndex", kWeatherCityAuto);
    if (index == kWeatherCityAuto) {
        index = g_autoWeatherCityIndex.load();
    }
    return (index >= 0 && static_cast<size_t>(index) < cityCount) ? index : 0;
}


namespace
{


static bool ReadJsonDouble(const std::string& json, const char* key, double& out) {
    const char* field = strstr(json.c_str(), key);
    if (!field) return false;
    field += strlen(key);
    while (*field == ' ' || *field == '"') ++field;
    char* end = nullptr;
    out = strtod(field, &end);
    return end != field;
}

// Looks up the approximate location of this PC from its public IP and returns
// the index of the nearest configured city, or -1 when the lookup fails.
int FindNearestWeatherCity(const std::vector<WeatherCityEntry>& cities) {
    struct Provider { const wchar_t* host; const wchar_t* path; const char* latKey; const char* lonKey; };
    static const Provider kProviders[] = {
        {L"ipwho.is", L"/?fields=success,latitude,longitude", "\"latitude\":", "\"longitude\":"},
        {L"ipapi.co", L"/json/", "\"latitude\":", "\"longitude\":"},
    };
    for (const Provider& provider : kProviders) {
        const std::string response = HttpGet(provider.host, provider.path, true);
        double lat = 0.0, lon = 0.0;
        if (response.empty() || !ReadJsonDouble(response, provider.latKey, lat) ||
            !ReadJsonDouble(response, provider.lonKey, lon) || lat < -90.0 || lat > 90.0 ||
            lon < -180.0 || lon > 180.0 || (lat == 0.0 && lon == 0.0)) {
            host::log(L"Weather: location lookup via %s failed", provider.host);
            continue;
        }
        int best = -1;
        double bestDistance = 0.0;
        const double toRad = 3.14159265358979323846 / 180.0;
        for (size_t i = 0; i < cities.size(); ++i) {
            const double cLat = wcstod(cities[i].latitude.c_str(), nullptr);
            const double cLon = wcstod(cities[i].longitude.c_str(), nullptr);
            // Haversine; only the ordering matters, so skip the final radius factor.
            const double dLat = (cLat - lat) * toRad;
            const double dLon = (cLon - lon) * toRad;
            const double a = sin(dLat / 2) * sin(dLat / 2) +
                             cos(lat * toRad) * cos(cLat * toRad) * sin(dLon / 2) * sin(dLon / 2);
            if (best < 0 || a < bestDistance) {
                best = static_cast<int>(i);
                bestDistance = a;
            }
        }
        host::log(L"Weather: IP location resolved to a configured city");
        return best;
    }
    return -1;
}

// Maps a WMO code from Open-Meteo to the WWO code the existing icons and visuals
// are keyed on, and to the English description the wttr.in feed used to supply.
int WwoCodeFromWmo(int wmo, std::wstring& desc) {
    switch (wmo) {
        case 0: desc = L"Clear"; return 113;
        case 1: desc = L"Mainly clear"; return 113;
        case 2: desc = L"Partly cloudy"; return 116;
        case 3: desc = L"Overcast"; return 122;
        case 45: desc = L"Fog"; return 248;
        case 48: desc = L"Freezing fog"; return 260;
        case 51: desc = L"Light drizzle"; return 263;
        case 53: desc = L"Drizzle"; return 266;
        case 55: desc = L"Heavy drizzle"; return 266;
        case 56: desc = L"Freezing drizzle"; return 281;
        case 57: desc = L"Heavy freezing drizzle"; return 284;
        case 61: desc = L"Light rain"; return 296;
        case 63: desc = L"Rain"; return 302;
        case 65: desc = L"Heavy rain"; return 308;
        case 66: desc = L"Freezing rain"; return 311;
        case 67: desc = L"Heavy freezing rain"; return 314;
        case 71: desc = L"Light snow"; return 326;
        case 73: desc = L"Snow"; return 332;
        case 75: desc = L"Heavy snow"; return 338;
        case 77: desc = L"Snow grains"; return 326;
        case 80: desc = L"Light showers"; return 353;
        case 81: desc = L"Showers"; return 356;
        case 82: desc = L"Heavy showers"; return 359;
        case 85: desc = L"Snow showers"; return 368;
        case 86: desc = L"Heavy snow showers"; return 371;
        case 95: desc = L"Thunderstorm"; return 389;
        case 96: desc = L"Thunderstorm with hail"; return 392;
        case 99: desc = L"Severe thunderstorm with hail"; return 395;
        default: desc = L"Unknown"; return 0;
    }
}

struct LocatedPlace {
    geo::Point point;
    std::wstring name;
    bool named = false;
};

std::wstring nearest_configured_name(const std::vector<WeatherCityEntry>& cities, geo::Point point) {
    std::wstring best;
    double best_km = 0.0;
    for (const WeatherCityEntry& city : cities) {
        const geo::Point candidate{wcstod(city.latitude.c_str(), nullptr), wcstod(city.longitude.c_str(), nullptr)};
        const double km = geo::distance_km(point, candidate);
        if (best.empty() || km < best_km) {
            best = city.name;
            best_km = km;
        }
    }
    return best;
}

std::optional<WeatherCityEntry> locate_city(const std::vector<WeatherCityEntry>& cities,
                                            std::optional<LocatedPlace>& place) {
    const std::optional<geo::Point> point = location::current_point();
    if (!point) {
        host::log(L"Weather: Windows location unavailable");
        return std::nullopt;
    }

    const bool moved = !place || geo::distance_km(place->point, *point) > geo::rename_distance_km;
    if (moved || !place->named) {
        const std::optional<std::wstring> name = location::place_name(*point);
        if (name) {
            place = LocatedPlace{*point, *name, true};
        } else if (moved) {
            const std::wstring fallback = nearest_configured_name(cities, *point);
            place = LocatedPlace{*point, fallback.empty() ? std::wstring(Loc(L"Current location")) : fallback, false};
        }
        host::log(L"Weather: place name %s", name ? L"resolved" : L"unavailable");
    } else {
        place->point = *point;
    }

    return WeatherCityEntry{place->name, geo::format_coordinate(point->latitude),
                            geo::format_coordinate(point->longitude)};
}

std::wstring WindDegreesToCompass(float degrees) {
    static const wchar_t* const kPoints[] = {L"N", L"NNE", L"NE", L"ENE", L"E", L"ESE", L"SE", L"SSE",
                                             L"S", L"SSW", L"SW", L"WSW", L"W", L"WNW", L"NW", L"NNW"};
    const int sector = static_cast<int>((fmod(degrees, 360.0f) + 360.0f + 11.25f) / 22.5f) % 16;
    return kPoints[sector];
}

// Reads a numeric field from the "current" object only, so "current_units"
// (whose values are strings) can never be mistaken for a reading.
bool ReadCurrentNumber(const char* current, const char* key, float& out) {
    const char* field = strstr(current, key);
    if (!field) return false;
    field += strlen(key);
    return sscanf(field, "%f", &out) == 1;
}

}



DWORD WINAPI WeatherThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    WaitForSingleObject(g_stopEvent, 3000);

    // Failures back off; a good reading is refreshed every 15 minutes, which
    // matches how often Open-Meteo updates its "current" values.
    static const DWORD kBackoffMs[] = {60 * 1000, 2 * 60 * 1000, 5 * 60 * 1000, 15 * 60 * 1000};
    size_t backoffIndex = 0;
    std::wstring previousKey;
    std::optional<LocatedPlace> located_place;

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        bool isFahrenheit = false;
        bool weatherEnabled = true;
        {
            std::lock_guard lock(g_settingsMutex);
            weatherEnabled = g_settings.weather;
            isFahrenheit = g_settings.weatherFahrenheit;
        }

        if (!weatherEnabled) {
            WaitForSingleObject(g_stopEvent, 2000);
            continue;
        }

        const std::vector<WeatherCityEntry> cities = LoadWeatherCities();
        bool weatherCommitted = false;
        std::optional<WeatherCityEntry> located;
        if (IsWeatherCityAuto()) {
            located = locate_city(cities, located_place);
        }

        if (cities.empty() && !located) {
            host::log(L"Weather: no valid city in Modules.WeatherCities.");
        } else {
            if (!located && IsWeatherCityAuto()) {
                // Re-check where the PC is on every refresh (and after resume);
                // on failure the previously found city stays in use.
                const int nearest = FindNearestWeatherCity(cities);
                if (nearest >= 0) {
                    g_autoWeatherCityIndex = nearest;
                }
            }
            const WeatherCityEntry& city = located ? *located : cities[GetActiveWeatherCityIndex(cities.size())];
            const std::wstring key = located ? L"located|" + city.name
                                             : city.name + L"|" + city.latitude + L"|" + city.longitude;
            if (key != previousKey) {
                // Never show one city's reading under another city's name.
                std::lock_guard lock(g_stateMutex);
                g_state.weather.hasData = false;
                g_state.weather.city = city.name;
                previousKey = key;
            }

            std::wstring path = L"/v1/forecast?latitude=" + city.latitude + L"&longitude=" + city.longitude +
                                L"&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
                                L"weather_code,wind_speed_10m,wind_direction_10m&timezone=auto";
            if (isFahrenheit) {
                path += L"&temperature_unit=fahrenheit&wind_speed_unit=mph";
            }

            host::log(L"Weather: requesting Open-Meteo");
            const std::string response = HttpGet(L"api.open-meteo.com", path.c_str(), true);
            const char* current = response.empty() ? nullptr : strstr(response.c_str(), "\"current\":{");

            float temp = 0.0f, feels = 0.0f, humidity = 0.0f, wind = 0.0f, windDeg = 0.0f, wmo = 0.0f;
            // Open-Meteo reports errors as {"error":true,"reason":...} with no
            // "current" object, so any failure lands here as "no reading".
            if (current && ReadCurrentNumber(current, "\"temperature_2m\":", temp) &&
                ReadCurrentNumber(current, "\"weather_code\":", wmo)) {
                const bool hasFeels = ReadCurrentNumber(current, "\"apparent_temperature\":", feels);
                const bool hasHumidity = ReadCurrentNumber(current, "\"relative_humidity_2m\":", humidity);
                const bool hasWind = ReadCurrentNumber(current, "\"wind_speed_10m\":", wind);
                const bool hasWindDir = ReadCurrentNumber(current, "\"wind_direction_10m\":", windDeg);

                // Whole degrees are all the UI shows; rounding here (and adding
                // 0.0f, which turns -0 into 0) keeps "-0°" off the island.
                temp = roundf(temp) + 0.0f;
                feels = roundf(feels) + 0.0f;

                std::wstring desc;
                const int code = WwoCodeFromWmo(static_cast<int>(wmo), desc);
                wchar_t buffer[32];

                std::lock_guard lock(g_stateMutex);
                g_state.weather.hasData = true;
                g_state.weather.temperature = temp;
                g_state.weather.weatherCode = code;
                g_state.weather.city = city.name;
                g_state.weather.weatherDesc = desc;
                swprintf_s(buffer, L"%.0f", wind);
                g_state.weather.windSpeed = hasWind ? buffer : L"--";
                g_state.weather.windDir = hasWindDir ? WindDegreesToCompass(windDeg) : L"";
                swprintf_s(buffer, L"%.0f", humidity);
                g_state.weather.humidity = hasHumidity ? buffer : L"--";
                swprintf_s(buffer, L"%.0f", feels);
                g_state.weather.feelsLike = hasFeels ? buffer : L"--";
                g_state.weather.lastUpdated = NowSeconds();
                weatherCommitted = true;
                host::log(L"Weather: reading updated");
            } else {
                host::log(L"Weather: no usable reading from Open-Meteo, keeping previous data.");
            }
        }

        DWORD waitMs;
        if (weatherCommitted) {
            backoffIndex = 0;
            waitMs = 15 * 60 * 1000;
        } else {
            waitMs = kBackoffMs[backoffIndex];
            if (backoffIndex + 1 < ARRAYSIZE(kBackoffMs)) {
                ++backoffIndex;
            }
        }

        HANDLE events[] = {g_stopEvent, g_settingsChangedEvent};
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, waitMs);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            // City changed or the PC resumed, so retry immediately.
            backoffIndex = 0;
        }
    }
    winrt::uninit_apartment();
    return 0;
}


}
