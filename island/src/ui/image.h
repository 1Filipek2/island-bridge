#pragma once

#include "core/common.h"
#include "core/pixels.h"
namespace island
{

bool DecodeImageBytesToPixels(const std::vector<uint8_t>& bytes, BitmapPixels* outPixels);

}
