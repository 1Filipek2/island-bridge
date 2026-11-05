#pragma once

#include <span>
#include <string_view>

namespace host
{

enum class SettingKind
{
    Number,
    Text,
};

struct SettingDefault
{
    std::wstring_view name;
    SettingKind kind;
    int number;
    std::wstring_view text;
};

std::span<const SettingDefault> setting_defaults();

const SettingDefault* find_setting_default(std::wstring_view name);

}
