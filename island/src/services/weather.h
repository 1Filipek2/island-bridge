#pragma once

#include "core/common.h"
namespace island
{


// ── Open-Meteo weather (fork: replaces the original wttr.in thread) ─────────
// Cities live in the mod setting Modules.WeatherCities as
// "Name|latitude|longitude;Name|latitude|longitude". The selected city index is
// kept in the mod's local storage (WeatherCityIndex).
static constexpr UINT kWeatherCityMenuIdBase = 80;
static constexpr UINT kWeatherCityMenuMax = 16;  // 80..95; media sources start at 100.

struct WeatherCityEntry {
    std::wstring name;
    std::wstring latitude;
    std::wstring longitude;
};
std::vector<WeatherCityEntry> LoadWeatherCities();

// WeatherCityIndex == -1 (the default) means "Auto": the city nearest to the
// PC's IP-based location is used.
static constexpr int kWeatherCityAuto = -1;
static constexpr UINT kWeatherCityAutoMenuId = 96;
static constexpr UINT kWeatherLocationFixMenuId = 97;  // free slot between cities (80..95) and media (100..)
inline std::atomic<int> g_autoWeatherCityIndex{-1};        // last nearest city found, -1 = unknown
bool IsWeatherCityAuto();
int GetActiveWeatherCityIndex(size_t cityCount);
DWORD WINAPI WeatherThreadProc(void*);

}
