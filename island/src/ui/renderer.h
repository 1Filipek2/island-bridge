#pragma once

#include "core/common.h"
#include "core/model.h"
namespace island
{

std::vector<IslandKind> choose_activities(const SharedState& state, const Settings& settings, double now);
DWORD WINAPI render_thread_proc(void*);

}
