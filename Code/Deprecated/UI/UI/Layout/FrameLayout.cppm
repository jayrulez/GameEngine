// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :frame_layout partition
//
// Stacks children on top of each other, each positioned independently by Gravity. Simplest ViewGroup.
// Ported from Sedulous.UI/src/Layout/FrameLayout.bf.
//
// The child's anchor comes from its LayoutStyle::Gravity.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:frame_layout;

import foundation.core; // Max, RefPtr, Rectangle
import :view;         // View, ViewGroup
import :layout_style;
import :box_constraints;
import :thickness;
import :gravity;
import :gravity_helper;

using namespace foundation::core;

export namespace foundation::ui
{
    using GravityValue = Gravity;

    class FrameLayout : public ViewGroup
    {
        RTTI_OBJECT(FrameLayout, ViewGroup)
    public:
        FrameLayout() = default;

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // Margin + Fixed live in the base Measure; the parent's
            // remaining decision is loose-vs-fill (AvailForChild). Chrome is the merged
            // metrics, so stylesheet padding/borders count.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const BoxConstraints inner = constraints.Deflate(chrome);
            f32 maxW = 0, maxH = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                child->Measure(AvailForChild(inner.MaxWidth, inner.MaxHeight, child));
                const Float2 mb = child->MarginBoxSize();
                maxW = Max(maxW, mb.x);
                maxH = Max(maxH, mb.y);
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(maxW + chrome.TotalHorizontal()),
                                  constraints.ConstrainHeight(maxH + chrome.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const f32 contentW = width - chrome.TotalHorizontal();
            const f32 contentH = height - chrome.TotalVertical();

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }

                const GravityValue gravity = child->Layout().Gravity;

                // Gravity positions the MARGIN box; the base Layout insets to the border box.
                const Float2 mb = child->MarginBoxSize();
                Rectangle rect = GravityHelper::Apply(gravity, contentW, contentH, mb.x, mb.y);
                rect.x += chrome.Left;
                rect.y += chrome.Top;
                child->Layout(rect.x, rect.y, rect.width, rect.height);
            }
        }
    };

    RTTI_DEFINE_OBJECT(FrameLayout, "rtti::ui")
}
