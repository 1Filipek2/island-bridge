#pragma once

#include "core/common.h"
#include "core/model.h"

namespace island
{

int active_tab_count(const Settings& settings);
int normalized_tab_index(const Settings& settings);
int hardware_monitor_tab_index(const Settings& settings);
int file_tray_tab_index(const Settings& settings);
int notification_center_tab_index(const Settings& settings);
void step_idle_tab(int direction);

}
