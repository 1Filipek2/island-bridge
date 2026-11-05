#pragma once

#include "core/common.h"

namespace island
{
// Weather (Open-Meteo). Cities come from Modules.WeatherCities as "Name|lat|lon;...", the selected
// index is stored as WeatherCityIndex (-1 = Auto: Windows location, then IP).
static constexpr UINT weather_city_menu_id_base = 80;
static constexpr UINT weather_city_menu_max = 16; // 80..95; media sources start at 100.

struct WeatherCityEntry
{
    std::wstring name;
    std::wstring latitude;
    std::wstring longitude;
};

std::vector<WeatherCityEntry> load_weather_cities();

static constexpr int weather_city_auto = -1;
static constexpr UINT weather_city_auto_menu_id = 96;
static constexpr UINT weather_location_fix_menu_id = 97; // free slot between the cities (80..95) and media (100..)
inline std::atomic<int> g_auto_weather_city_index{-1};   // last nearest city found, -1 = unknown
bool is_weather_city_auto();
int get_active_weather_city_index(std::size_t city_count);
DWORD WINAPI weather_thread_proc(void*);

}
