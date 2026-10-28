// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :inset_drawable partition
//
// Wraps a drawable and insets its draw bounds; advertises the inset as DrawablePadding
// so layout can query it. Ported from Sedulous.UI/src/Drawing/InsetDrawable.bf.
// Beef "consumes the caller's ref" -> RefPtr<Drawable> held by value (auto-released).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:inset_drawable;

import foundation.core; // RefPtr, Rectangle, Max
import :thickness;
import :control_state;
import :drawable;
import :draw_context;

using namespace foundation::core;

export namespace foundation::ui
{
    class InsetDrawable : public Drawable
    {
        RTTI_OBJECT(InsetDrawable, Drawable)
    public:
        Thickness Inset{};

        InsetDrawable(RefPtr<Drawable> inner, Thickness inset) : Inset(inset), m_inner(Move(inner))
        {
        }

        [[nodiscard]] Drawable* Inner() const noexcept { return m_inner.Get(); }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (m_inner)
            {
                m_inner->Draw(ctx, InsetBounds(bounds));
            }
        }
        void DrawState(UIDrawContext& ctx, const Rectangle& bounds, ControlState state) override
        {
            if (m_inner)
            {
                m_inner->Draw(ctx, InsetBounds(bounds), state);
            }
        }

        [[nodiscard]] Thickness DrawablePadding() const override { return Inset; }

    private:
        [[nodiscard]] Rectangle InsetBounds(const Rectangle& b) const noexcept
        {
            return Rectangle{b.x + Inset.Left, b.y + Inset.Top,
                             Max(0.0f, b.width - Inset.TotalHorizontal()),
                             Max(0.0f, b.height - Inset.TotalVertical())};
        }

        RefPtr<Drawable> m_inner;
    };

    RTTI_DEFINE_OBJECT(InsetDrawable, "rtti::ui")
}
