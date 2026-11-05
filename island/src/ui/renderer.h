#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{

std::vector<IslandKind> ChooseActivities(const SharedState& state, const Settings& settings, double now);
DWORD WINAPI RenderThreadProc(void*);

}
