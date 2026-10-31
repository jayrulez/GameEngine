// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :drawable partition
//
// Drawable: image content drawn into a destination Rect via DrawContext (an Image widget's
// picture, a menu item's or a button's icon); a node's look is its box, not a drawable.
// Derived from eepp's Drawable (include/eepp/graphics/drawable.hpp): keeps its colour and alpha
// tint state, but draws through DrawContext rather than a global renderer. It derives from Object
// (the engine builds with -fno-rtti) so concrete drawables can be downcast with Cast<T>. It is not
// reflection-registered or scripted.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:drawable;

import foundation.core;
import :rect;
import :draw_context;

using namespace foundation::core;

export namespace foundation::gui
{
    class Drawable : public Object
    {
        RTTI_OBJECT(Drawable, Object)
    public:
        // Draws into a destination rect.
        virtual void Draw(DrawContext& ctx, const Rect& dest) = 0;
        /// Draws the drawable multiplied by `tint` (an Image's image-color). A drawable that cannot
        /// tint draws unchanged.
        virtual void DrawTinted(DrawContext& ctx, const Rect& dest, Color tint)
        {
            (void)tint;
            Draw(ctx, dest);
        }

        // Natural size (icons/images); empty = no intrinsic size.
        [[nodiscard]] virtual Optional<Float2> IntrinsicSize() const { return {}; }

        // Tint and opacity.
        [[nodiscard]] Color GetColor() const noexcept { return m_color; }
        void SetColor(Color color) noexcept { m_color = color; }
        [[nodiscard]] f32 GetAlpha() const noexcept { return m_color.a; }
        void SetAlpha(f32 alpha) noexcept { m_color.a = alpha; }

    protected:
        Color m_color = Color::White;
    };

    RTTI_DEFINE_OBJECT(Drawable, "rtti::gui")
}
