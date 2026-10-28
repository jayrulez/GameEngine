// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :absolute_layout partition
//
// Positions children at explicit X/Y coordinates. Ported from Sedulous.UI/src/Layout/AbsoluteLayout.bf.
// The offset comes from the child's LayoutStyle::Left/Top.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:absolute_layout;

import foundation.core; // Max, RefPtr, kFloatMax
import :view;
import :layout_style;
import :box_constraints;
import :size_spec;
import :thickness;

using namespace foundation::core;

export namespace foundation::ui
{
    class AbsoluteLayout : public ViewGroup
    {
        RTTI_OBJECT(AbsoluteLayout, ViewGroup)
    public:
        AbsoluteLayout() = default;

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            f32 maxR = 0, maxB = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }

                // Fixed + margin are base-handled here. Wrap children measure unbounded (absolute
                // placement has no natural box), Match fills the content area.
                const f32 availW = Max(0.0f, constraints.MaxWidth - chrome.TotalHorizontal());
                const f32 availH = Max(0.0f, constraints.MaxHeight - chrome.TotalVertical());
                const LayoutStyle& ls = child->Layout();
                const bool fillW = ls.Width->kind == SizeSpec::Kind::Match;
                const bool fillH = ls.Height->kind == SizeSpec::Kind::Match;
                child->Measure(BoxConstraints{fillW ? availW : 0.0f, fillW ? availW : kFloatMax,
                                              fillH ? availH : 0.0f, fillH ? availH : kFloatMax});

                const f32 x = ls.Left;
                const f32 y = ls.Top;
                const Float2 mb = child->MarginBoxSize();
                maxR = Max(maxR, x + mb.x);
                maxB = Max(maxB, y + mb.y);
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(maxR + chrome.TotalHorizontal()),
                                  constraints.ConstrainHeight(maxB + chrome.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            (void)left;
            (void)top;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }

                const LayoutStyle& ls = child->Layout();
                const f32 ax = ls.Left;
                const f32 ay = ls.Top;
                const f32 x = chrome.Left + ax;
                const f32 y = chrome.Top + ay;

                // Margin-box rects (base insets by margin).
                const Float2 mb = child->MarginBoxSize();
                f32 w = mb.x;
                f32 h = mb.y;
                if (ls.Width->kind == SizeSpec::Kind::Match)
                {
                    w = Max(0.0f, width - chrome.TotalHorizontal() - ax);
                }
                if (ls.Height->kind == SizeSpec::Kind::Match)
                {
                    h = Max(0.0f, height - chrome.TotalVertical() - ay);
                }
                child->Layout(x, y, w, h);
            }
        }

    };

    RTTI_DEFINE_OBJECT(AbsoluteLayout, "rtti::ui")
}
