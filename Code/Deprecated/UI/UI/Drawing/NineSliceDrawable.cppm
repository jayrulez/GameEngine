// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :nine_slice_drawable partition
//
// 9-slice image drawable with optional Expand (shadow/glow extending beyond the logical bounds).
// Ported from Sedulous.UI/src/Drawing/NineSliceDrawable.bf. Slices is image::NineSlice (lowercase
// fields); Expand is a ui::Thickness (PascalCase).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:nine_slice_drawable;

import foundation.core;  // Color, Rectangle, Float2, Optional, Max
import foundation.image; // ImageData, NineSlice
import :thickness;
import :drawable;
import :draw_context;

using namespace foundation::core;
namespace image = foundation::image;

export namespace foundation::ui
{
    class NineSliceDrawable : public Drawable
    {
        RTTI_OBJECT(NineSliceDrawable, Drawable)
    public:
        const image::ImageData* Image = nullptr;
        image::NineSlice Slices{};
        Thickness Expand{};
        Color Tint = Color::White;

        NineSliceDrawable() = default;
        NineSliceDrawable(const image::ImageData* image, image::NineSlice slices,
                          Color tint = Color::White)
            : Image(image), Slices(slices), Tint(tint)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (Image == nullptr)
            {
                return;
            }
            const Rectangle drawBounds{bounds.x - Expand.Left, bounds.y - Expand.Top,
                                       bounds.width + Expand.TotalHorizontal(),
                                       bounds.height + Expand.TotalVertical()};
            const Rectangle srcRect{0.0f, 0.0f, static_cast<f32>(Image->Width()),
                                    static_cast<f32>(Image->Height())};
            ctx.VG().DrawNineSlice(Image, drawBounds, srcRect, Slices, Tint);
        }

        [[nodiscard]] Thickness DrawablePadding() const override
        {
            return Thickness{
                Max(0.0f, Slices.left - Expand.Left), Max(0.0f, Slices.top - Expand.Top),
                Max(0.0f, Slices.right - Expand.Right), Max(0.0f, Slices.bottom - Expand.Bottom)};
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            if (Image != nullptr)
            {
                return Float2{static_cast<f32>(Image->Width()) - Expand.TotalHorizontal(),
                              static_cast<f32>(Image->Height()) - Expand.TotalVertical()};
            }
            return {};
        }
    };

    RTTI_DEFINE_OBJECT(NineSliceDrawable, "rtti::ui")
}
