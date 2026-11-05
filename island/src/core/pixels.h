#pragma once

#include "core/common.h"
namespace island
{


struct BitmapPixels {
    std::vector<uint8_t> bgra;
    UINT width = 0;
    UINT height = 0;
    uint64_t generation = 0;
    D2D1_COLOR_F sampled_accent = D2D1::ColorF(0x4cc9f0);
};

// Immutable, shared pixel data. Copying one is a refcount bump, so copying
// SharedState every frame no longer copies album art / icons.
// Null-safe: operator-> on an empty ref points at a static empty BitmapPixels.
class PixelsRef {
   public:
    PixelsRef() = default;
    explicit PixelsRef(BitmapPixels&& p)
        : p_(std::make_shared<const BitmapPixels>(std::move(p))) {}

    explicit operator bool() const { return p_ && !p_->bgra.empty(); }
    const BitmapPixels* operator->() const { return p_ ? p_.get() : &empty(); }
    const BitmapPixels& operator*() const { return *operator->(); }

   private:
    static const BitmapPixels& empty() {
        static const BitmapPixels e{};
        return e;
    }
    std::shared_ptr<const BitmapPixels> p_;
};

}
