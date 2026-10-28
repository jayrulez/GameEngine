// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :shape_drawable partition
//
// Delegate-based custom drawing without subclassing. Ported from
// Sedulous.UI/src/Drawing/ShapeDrawable.bf (Beef delegate -> core::Function).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:shape_drawable;

import foundation.core; // Function, Rectangle
import :drawable;
import :draw_context;

using namespace foundation::core;

export namespace foundation::ui
{
    class ShapeDrawable : public Drawable
    {
        RTTI_OBJECT(ShapeDrawable, Drawable)
    public:
        using DrawFn = Function<void(UIDrawContext&, const Rectangle&)>;

        explicit ShapeDrawable(DrawFn drawFn) : m_drawFn(Move(drawFn)) {}

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (m_drawFn)
            {
                m_drawFn(ctx, bounds);
            }
        }

    private:
        DrawFn m_drawFn;
    };

    RTTI_DEFINE_OBJECT(ShapeDrawable, "rtti::ui")
}
