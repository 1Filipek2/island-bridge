#include "ui/image.h"
#include "core/color.h"
#include "core/model.h"
#include "core/state.h"

namespace island
{
bool decode_image_bytes_to_pixels(const std::vector<std::uint8_t>& bytes, BitmapPixels* out_pixels)
{
    if (!out_pixels || bytes.empty())
    {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        return false;
    }

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem)
    {
        return false;
    }

    void* locked = GlobalLock(mem);
    std::memcpy(locked, bytes.data(), bytes.size());
    GlobalUnlock(mem);

    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(mem, TRUE, &stream);
    if (FAILED(hr))
    {
        GlobalFree(mem);
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        return false;
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    converter->GetSize(&width, &height);
    if (!width || !height || width > 2048 || height > 2048)
    {
        return false;
    }

    BitmapPixels pixels;
    pixels.width = width;
    pixels.height = height;
    pixels.bgra.resize(static_cast<std::size_t>(width) * height * 4);

    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.bgra.size()), pixels.bgra.data());
    if (FAILED(hr))
    {
        return false;
    }

    struct Bucket
    {
        std::uint32_t count = 0;
        std::uint32_t r = 0;
        std::uint32_t g = 0;
        std::uint32_t b = 0;
        std::uint32_t sat_sum = 0;
    };

    std::array<Bucket, 16 * 16 * 16> buckets{};
    for (std::size_t i = 0; i + 3 < pixels.bgra.size(); i += 4)
    {
        const std::uint8_t alpha = pixels.bgra[i + 3];
        const std::uint8_t blue = pixels.bgra[i + 0];
        const std::uint8_t green = pixels.bgra[i + 1];
        const std::uint8_t red = pixels.bgra[i + 2];
        if (alpha < 32)
        {
            continue;
        }

        const int maxc = std::max({red, green, blue});
        const int minc = std::min({red, green, blue});
        const int luminance = (54 * red + 183 * green + 19 * blue) / 256;
        const int saturation = maxc - minc;
        if (luminance < 36 || luminance > 220 || saturation < 28)
        {
            continue;
        }

        const std::size_t bucket_index = ((red >> 4) << 8) | ((green >> 4) << 4) | (blue >> 4);
        Bucket& bucket = buckets[bucket_index];
        const std::uint32_t weight = 1 + static_cast<std::uint32_t>(saturation / 32);
        bucket.count += weight;
        bucket.r += red * weight;
        bucket.g += green * weight;
        bucket.b += blue * weight;
        bucket.sat_sum += static_cast<std::uint32_t>(saturation) * weight;
    }

    // Vibrancy-weighted winner: of the top 3 buckets by count, the most saturated wins, so a large muted
    // area does not beat a smaller vivid accent.
    struct Candidate
    {
        const Bucket* b = nullptr;
        std::size_t idx = 0;
    };

    Candidate top[3];
    for (std::size_t i = 0; i < buckets.size(); ++i)
    {
        const Bucket& bk = buckets[i];
        if (bk.count == 0)
        {
            continue;
        }
        for (int s = 0; s < 3; ++s)
        {
            if (!top[s].b || bk.count > top[s].b->count)
            {
                for (int t = 2; t > s; --t)
                {
                    top[t] = top[t - 1];
                }
                top[s] = {&bk, i};
                break;
            }
        }
    }

    const Bucket* best = nullptr;
    std::size_t best_idx = 0;
    float best_vibrancy = -1.0f;
    for (int s = 0; s < 3; ++s)
    {
        if (!top[s].b)
        {
            break;
        }
        const float vibrancy = static_cast<float>(top[s].b->sat_sum) / static_cast<float>(top[s].b->count);
        if (vibrancy > best_vibrancy)
        {
            best_vibrancy = vibrancy;
            best = top[s].b;
            best_idx = top[s].idx;
        }
    }

    if (best && best->count > 0)
    {
        // Pool the winning bucket with its 26 neighbours so a color on a bucket boundary stays stable.
        std::uint64_t pool_count = 0;
        double pool_r = 0, pool_g = 0, pool_b = 0;

        const int bi = static_cast<int>((best_idx >> 8) & 0xF);
        const int gi = static_cast<int>((best_idx >> 4) & 0xF);
        const int bli = static_cast<int>(best_idx & 0xF);

        for (int dr = -1; dr <= 1; ++dr)
        {
            for (int dg = -1; dg <= 1; ++dg)
            {
                for (int db = -1; db <= 1; ++db)
                {
                    const int ni = bi + dr, nj = gi + dg, nk = bli + db;
                    if (ni < 0 || ni > 15 || nj < 0 || nj > 15 || nk < 0 || nk > 15)
                    {
                        continue;
                    }
                    const std::size_t n_idx =
                        (static_cast<std::size_t>(ni) << 8) | (static_cast<std::size_t>(nj) << 4) | static_cast<std::size_t>(nk);
                    const Bucket& nb = buckets[n_idx];
                    if (nb.count == 0)
                    {
                        continue;
                    }
                    pool_count += nb.count;
                    pool_r += nb.r;
                    pool_g += nb.g;
                    pool_b += nb.b;
                }
            }
        }

        if (pool_count > 0)
        {
            const float inv_c = 1.0f / static_cast<float>(pool_count);
            const float raw_r = static_cast<float>(pool_r * inv_c) / 255.0f;
            const float raw_g = static_cast<float>(pool_g * inv_c) / 255.0f;
            const float raw_b = static_cast<float>(pool_b * inv_c) / 255.0f;

            D2D1_COLOR_F bg_color = g_settings.pill_bg_color;
            if (bg_color.a <= 0.0f)
            {
                bg_color = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f);
            }

            pixels.sampled_accent =
                ensure_contrast_against_background(D2D1::ColorF(raw_r, raw_g, raw_b, 1.0f), bg_color);
        }
    }

    pixels.generation = ++g_art_generation_counter;
    *out_pixels = std::move(pixels);
    return true;
}

}
