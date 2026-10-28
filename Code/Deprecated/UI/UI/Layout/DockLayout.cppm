// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :dock_layout partition
//
// Docks children to edges (Left/Top/Right/Bottom/Fill); each docked child claims space from its edge,
// shrinking the remaining area for subsequent children. Ported from Sedulous.UI/src/Layout/DockLayout.bf.
// The child's side comes from its LayoutStyle::Dock.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:dock_layout;

import foundation.core; // Max
import :view;
import :layout_style; // Dock, LayoutStyle
import :box_constraints;
import :thickness;

using namespace foundation::core;

export namespace foundation::ui
{
    class DockLayout : public ViewGroup
    {
        RTTI_OBJECT(DockLayout, ViewGroup)
    public:
        /// When true, the last child fills all remaining space regardless of its Dock.
        bool LastChildFill = false;

        DockLayout() = default;

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            f32 usedLeft = 0, usedTop = 0, usedRight = 0, usedBottom = 0, maxW = 0, maxH = 0;
            const usize count = ChildCount();

            for (usize i = 0; i < count; ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }

                const foundation::ui::Dock dock = child->Layout().Dock;

                // Margin is base-handled: pass the remaining space as the
                // child's margin-box availability; aggregate margin-box sizes.
                const f32 remainW = Max(0.0f, constraints.MaxWidth - chrome.TotalHorizontal() -
                                                  usedLeft - usedRight);
                const f32 remainH = Max(0.0f, constraints.MaxHeight - chrome.TotalVertical() -
                                                  usedTop - usedBottom);

                const bool isFill =
                    (LastChildFill && i == count - 1) || dock == foundation::ui::Dock::Fill;
                child->Measure(isFill ? BoxConstraints::Tight(remainW, remainH)
                                      : BoxConstraints{0, remainW, 0, remainH});
                const Float2 mb = child->MarginBoxSize();

                switch (dock)
                {
                case foundation::ui::Dock::Left:
                    usedLeft += mb.x;
                    break;
                case foundation::ui::Dock::Right:
                    usedRight += mb.x;
                    break;
                case foundation::ui::Dock::Top:
                    usedTop += mb.y;
                    break;
                case foundation::ui::Dock::Bottom:
                    usedBottom += mb.y;
                    break;
                case foundation::ui::Dock::Fill:
                    break;
                }
                maxW = Max(maxW, usedLeft + usedRight);
                maxH = Max(maxH, usedTop + usedBottom);
            }

            // The chrome (padding AND border) was deflated above, so it is the chrome that
            // comes back - adding only the padding measured a bordered dock short by the border.
            MeasuredSize = Float2{constraints.ConstrainWidth(maxW + chrome.TotalHorizontal()),
                                  constraints.ConstrainHeight(maxH + chrome.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            f32 dockLeft = chrome.Left;
            f32 dockTop = chrome.Top;
            f32 dockRight = width - chrome.Right;
            f32 dockBottom = height - chrome.Bottom;
            const usize count = ChildCount();

            for (usize i = 0; i < count; ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }

                const foundation::ui::Dock dock = child->Layout().Dock;
                const bool isFill =
                    (LastChildFill && i == count - 1) || dock == foundation::ui::Dock::Fill;

                // Rects below are MARGIN boxes - the base Layout insets by margin once.
                const Float2 mb = child->MarginBoxSize();
                if (isFill)
                {
                    child->Layout(dockLeft, dockTop, Max(0.0f, dockRight - dockLeft),
                                  Max(0.0f, dockBottom - dockTop));
                    continue;
                }

                switch (dock)
                {
                case foundation::ui::Dock::Left:
                    child->Layout(dockLeft, dockTop, mb.x, Max(0.0f, dockBottom - dockTop));
                    dockLeft += mb.x;
                    break;
                case foundation::ui::Dock::Right:
                    child->Layout(dockRight - mb.x, dockTop, mb.x,
                                  Max(0.0f, dockBottom - dockTop));
                    dockRight -= mb.x;
                    break;
                case foundation::ui::Dock::Top:
                    child->Layout(dockLeft, dockTop, Max(0.0f, dockRight - dockLeft), mb.y);
                    dockTop += mb.y;
                    break;
                case foundation::ui::Dock::Bottom:
                    child->Layout(dockLeft, dockBottom - mb.y,
                                  Max(0.0f, dockRight - dockLeft), mb.y);
                    dockBottom -= mb.y;
                    break;
                case foundation::ui::Dock::Fill:
                    break;
                }
            }
        }
    };

    RTTI_DEFINE_OBJECT(DockLayout, "rtti::ui")
}
