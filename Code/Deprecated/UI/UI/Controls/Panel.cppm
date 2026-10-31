// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :panel partition
//
// Container with an optional background drawable; children fill the panel minus padding.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:panel;

import foundation.core;
import :view;
import :box_constraints;
import :thickness;
import :style_property;
import :control_state;
import :draw_context;
import :drawable;

using namespace foundation::core;

export namespace foundation::ui
{
    class Panel : public ViewGroup
    {
        RTTI_OBJECT(Panel, ViewGroup)
    public:
        Panel() = default;

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            if (Drawable* bg = ResolveStyleDrawable(StyleProperty::Background))
            {
                bg->Draw(ctx, bounds, GetControlState());
            }
            DrawChildren(ctx);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // The base class handles margins: children get the loose content box, and the panel
            // sizes to their margin boxes.
            const Thickness pad = EffectivePadding();
            const BoxConstraints inner = constraints.Deflate(pad).Loosen();
            f32 maxW = 0, maxH = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                child->Measure(inner);
                const Float2 mb = child->MarginBoxSize();
                maxW = Max(maxW, mb.x);
                maxH = Max(maxH, mb.y);
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(maxW + pad.TotalHorizontal()),
                                  constraints.ConstrainHeight(maxH + pad.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const Thickness pad = EffectivePadding();
            const f32 contentW = width - pad.TotalHorizontal();
            const f32 contentH = height - pad.TotalVertical();
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                // Panel stretches every child across the content box: pass the whole content
                // box as the child's margin box, which the base class insets by the margin once.
                child->Layout(pad.Left, pad.Top, Max(0.0f, contentW), Max(0.0f, contentH));
            }
        }

    private:
        /// The resolved chrome from View::ResolveBoxMetrics: the padding field, the drawable's
        /// padding and the stylesheet `padding:` merged by taking the larger of each side, plus
        /// space for the border (border-box).
        [[nodiscard]] Thickness EffectivePadding() { return ResolveBoxMetrics().Chrome(); }
    };

    RTTI_DEFINE_OBJECT(Panel, "rtti::ui")
}
