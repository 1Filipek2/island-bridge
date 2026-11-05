#pragma once

#include "core/common.h"

namespace island
{
struct BitmapPixels
{
    std::vector<std::uint8_t> bgra;
    UINT width = 0;
    UINT height = 0;
    std::uint64_t generation = 0;
    D2D1_COLOR_F sampled_accent = D2D1::ColorF(0x4cc9f0);
};

// Immutable shared pixels: copying is a refcount bump, an empty ref reads as an empty bitmap.
class PixelsRef
{
public:
    PixelsRef() = default;

    explicit PixelsRef(BitmapPixels&& p) : p_(std::make_shared<const BitmapPixels>(std::move(p)))
    {
    }

    explicit operator bool() const
    {
        return p_ && !p_->bgra.empty();
    }

    const BitmapPixels* operator->() const
    {
        return p_ ? p_.get() : &empty();
    }

    const BitmapPixels& operator*() const
    {
        return *operator->();
    }

private:
    static const BitmapPixels& empty()
    {
        static const BitmapPixels e{};
        return e;
    }

    std::shared_ptr<const BitmapPixels> p_;
};

}
