// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :image_drawable partition
//
// Draws an image stretched to fill bounds. Ported from Sedulous.UI/src/Drawing/ImageDrawable.bf.
// Sedulous IImageData -> foundation::image::ImageData (non-owning pointer; the image is owned by the
// theme/atlas, not the drawable).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:image_drawable;

import foundation.core;  // Color, Rectangle, Float2, Optional
import foundation.image; // ImageData
import foundation.vg;    // CornerRadii
import :drawable;
import :draw_context;

using namespace foundation::core;
namespace image = foundation::image;

export namespace foundation::ui
{
    class ImageDrawable : public Drawable
    {
        RTTI_OBJECT(ImageDrawable, Drawable)
    public:
        const image::ImageData* Image = nullptr;
        Color Tint = Color::White;
        /// Rounds the image's corners within the bounds it is drawn to; zero: square.
        vg::CornerRadii Radii{};

        ImageDrawable() = default;
        explicit ImageDrawable(const image::ImageData* image, Color tint = Color::White)
            : Image(image), Tint(tint)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (Image != nullptr)
            {
                ctx.VG().DrawImageRounded(Image, bounds,
                                          Rectangle{0.0f, 0.0f, static_cast<f32>(Image->Width()),
                                                    static_cast<f32>(Image->Height())},
                                          Radii, Tint);
            }
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            if (Image != nullptr)
            {
                return Float2{static_cast<f32>(Image->Width()), static_cast<f32>(Image->Height())};
            }
            return {};
        }
    };

    RTTI_DEFINE_OBJECT(ImageDrawable, "rtti::ui")
}
