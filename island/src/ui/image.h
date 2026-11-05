#pragma once

#include "core/common.h"
#include "core/pixels.h"

namespace island
{

bool decode_image_bytes_to_pixels(const std::vector<uint8_t>& bytes, BitmapPixels* out_pixels);

}
