// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :atlas_image_drawable partition
//
// Draws a sub-region of a shared atlas image (single-texture batching for themed UI).
// Ported from Sedulous.UI/src/Drawing/AtlasImageDrawable.bf.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:atlas_image_drawable;

import foundation.core;  // Color, Rectangle, Float2, Optional
import foundation.image; // ImageData
import :drawable;
import :draw_context;

using namespace foundation::core;
namespace image = foundation::image;

export namespace foundation::ui
{
    class AtlasImageDrawable : public Drawable
    {
        RTTI_OBJECT(AtlasImageDrawable, Drawable)
    public:
        const image::ImageData* AtlasImage = nullptr;
        Rectangle SourceRect{};
        Color Tint = Color::White;

        AtlasImageDrawable() = default;
        AtlasImageDrawable(const image::ImageData* atlas, Rectangle sourceRect,
                           Color tint = Color::White)
            : AtlasImage(atlas), SourceRect(sourceRect), Tint(tint)
        {
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (AtlasImage != nullptr)
            {
                ctx.VG().DrawImage(AtlasImage, bounds, SourceRect, Tint);
            }
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            return Float2{SourceRect.width, SourceRect.height};
        }
    };

    RTTI_DEFINE_OBJECT(AtlasImageDrawable, "rtti::ui")
}
