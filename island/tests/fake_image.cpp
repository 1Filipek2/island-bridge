#include "ui/image.h"

namespace island
{

bool decode_image_bytes_to_pixels(const std::vector<std::uint8_t>& bytes, BitmapPixels* pixels)
{
    if (bytes.size() < 2)
    {
        return false;
    }

    pixels->width = bytes[0];
    pixels->height = bytes[1];
    pixels->bgra.assign(static_cast<std::size_t>(pixels->width) * pixels->height * 4, 0);
    return true;
}

}
