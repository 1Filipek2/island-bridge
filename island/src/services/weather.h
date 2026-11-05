#pragma once

#include "core/common.h"

namespace island
{

// ── Open-Meteo weather (fork: replaces the original wttr.in thread) ─────────
// Cities live in the mod setting Modules.WeatherCities as
// "Name|latitude|longitude;Name|latitude|longitude". The selected city index is
// kept in the mod's local storage (WeatherCityIndex).
static constexpr UINT weather_city_menu_id_base = 80;
static constexpr UINT weather_city_menu_max = 16; // 80..95; media sources start at 100.

struct WeatherCityEntry
{
    std::wstring name;
    std::wstring latitude;
    std::wstring longitude;
};

std::vector<WeatherCityEntry> load_weather_cities();

// WeatherCityIndex == -1 (the default) means "Auto": the city nearest to the
// PC's IP-based location is used.
static constexpr int weather_city_auto = -1;
static constexpr UINT weather_city_auto_menu_id = 96;
static constexpr UINT weather_location_fix_menu_id = 97; // free slot between cities (80..95) and media (100..)
inline std::atomic<int> g_auto_weather_city_index{-1};   // last nearest city found, -1 = unknown
bool is_weather_city_auto();
int get_active_weather_city_index(size_t city_count);
DWORD WINAPI weather_thread_proc(void*);

}
