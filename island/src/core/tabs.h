#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{

int ActiveTabCount(const Settings& settings);
int NormalizedTabIndex(const Settings& settings);
int MediaTabIndex(const Settings& settings);
int HardwareMonitorTabIndex(const Settings& settings);
int FileTrayTabIndex(const Settings& settings);
int NotificationCenterTabIndex(const Settings& settings);
void StepIdleTab(int direction);

}
