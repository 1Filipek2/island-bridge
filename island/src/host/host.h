#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <string>

namespace host
{

constexpr wchar_t registry_root[] = L"Software\\IslandBridge\\Island";
constexpr wchar_t settings_subkey[] = L"Software\\IslandBridge\\Island\\Settings";
constexpr wchar_t state_subkey[] = L"Software\\IslandBridge\\Island\\State";
constexpr wchar_t logging_setting[] = L"Diagnostics.Logging";

bool start();
void stop();

int int_setting(const wchar_t* name);
std::wstring string_setting(const wchar_t* name);
int int_value(const wchar_t* name, int fallback);
bool set_int_value(const wchar_t* name, int value);
bool storage_path(wchar_t* buffer, std::size_t size);

HANDLE settings_changed_event();
void rearm_settings_watch();
void refresh_logging();

void log(const wchar_t* format, ...);

}
