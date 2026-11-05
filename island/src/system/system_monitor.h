#pragma once

#include "core/common.h"
namespace island
{

void update_battery_snapshot();
inline ComPtr<IAudioEndpointVolume> g_endpoint_volume;
void bind_volume_endpoint();
void unbind_volume_endpoint();
void update_system_snapshot(bool include_gpu_stats, bool include_net_stats);
void update_brightness_snapshot();

}
