#pragma once

#include "core/common.h"
namespace island
{

void UpdateBatterySnapshot();
inline ComPtr<IAudioEndpointVolume> g_endpointVolume;
void BindVolumeEndpoint();
void UnbindVolumeEndpoint();
void UpdateSystemSnapshot(bool includeGpuStats, bool includeNetStats);
void UpdateBrightnessSnapshot();

}
