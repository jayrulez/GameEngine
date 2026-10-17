// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :drawable partition
//
// Drawable: base for composable visual primitives, rendered into a destination Rect via
// DrawContext. Derived from eepp's Drawable (include/eepp/graphics/drawable.hpp): keeps
// the color/alpha tint state and the stateful flag, but replaces eepp's global-draw with
// the DrawContext seam. Object-derived (our RTTI everywhere; -fno-rtti) so concrete
// drawables get Cast<T> for downcasts; not reflection-registered/scripted.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:drawable;

import foundation.core; // Object, Optional, Float2, Color
import :rect;
import :draw_context;
import :control_state;

using namespace foundation::core;

export namespace experimental::gui
{
    class Drawable : public Object
    {
        RTTI_OBJECT(Drawable, Object)
    public:
        // State-unaware draw into a destination rect.
        virtual void Draw(DrawContext& ctx, const Rect& dest) = 0;

        // State-aware draw - defaults to the state-unaware overload.
        virtual void Draw(DrawContext& ctx, const Rect& dest, ControlState state)
        {
            (void)state;
            Draw(ctx, dest);
        }

        // True if drawing varies with ControlState (StateListDrawable, skins).
        [[nodiscard]] virtual bool IsStateful() const { return false; }
        /// Draws it multiplied by `tint` (an Image's image-color); one that cannot tint draws as it is.
        virtual void DrawTinted(DrawContext& ctx, const Rect& dest, Color tint)
        {
            (void)tint;
            Draw(ctx, dest);
        }

        // Natural size (icons/images); empty = no intrinsic size.
        [[nodiscard]] virtual Optional<Float2> IntrinsicSize() const { return {}; }

        // Tint / opacity (eepp Drawable color state).
        [[nodiscard]] Color GetColor() const noexcept { return m_color; }
        void SetColor(Color color) noexcept { m_color = color; }
        [[nodiscard]] f32 GetAlpha() const noexcept { return m_color.a; }
        void SetAlpha(f32 alpha) noexcept { m_color.a = alpha; }

    protected:
        Color m_color = Color::White;
    };

    RTTI_DEFINE_OBJECT(Drawable, "rtti::gui")
}
