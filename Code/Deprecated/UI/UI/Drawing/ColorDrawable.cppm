// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :color_drawable partition
//
// Fills bounds with a solid color. Ported from Sedulous.UI/src/Drawing/ColorDrawable.bf.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:color_drawable;

import foundation.core; // Color, Rectangle
import :drawable;
import :draw_context;

using namespace foundation::core;
namespace core = foundation::core; // to name core::Color where the field shadows the type

export namespace foundation::ui
{
    class ColorDrawable : public Drawable
    {
        RTTI_OBJECT(ColorDrawable, Drawable)
    public:
        core::Color Color{};
        /// A custom property (`--name`) the colour comes from where the drawn view has one; Color is
        /// its fallback.
        String ColorVariable;

        ColorDrawable() = default;
        explicit ColorDrawable(core::Color color) : Color(color) {}

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            const core::Color color = ctx.ColorOf(Color, ColorVariable.AsView());
            if (color.a > 0.0f)
            {
                ctx.VG().FillRect(bounds, color);
            }
        }
    };

    RTTI_DEFINE_OBJECT(ColorDrawable, "rtti::ui")
}
