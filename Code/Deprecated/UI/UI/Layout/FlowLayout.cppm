// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :flow_layout partition
//
// Arranges children left-to-right (horizontal) or top-to-bottom (vertical), wrapping to the next
// line/column when space runs out.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:flow_layout;

import foundation.core; // Max, kFloatMax
import :view;
import :box_constraints;
import :enums; // Orientation

using namespace foundation::core;

export namespace foundation::ui
{
    using OrientationValue = Orientation;

    class FlowLayout : public ViewGroup
    {
        RTTI_OBJECT(FlowLayout, ViewGroup)
    public:
        OrientationValue Orientation = OrientationValue::Horizontal;
        f32 HSpacing = 0.0f;
        f32 VSpacing = 0.0f;

        FlowLayout() = default;

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            if (Orientation == OrientationValue::Horizontal)
            {
                MeasureHorizontal(constraints);
            }
            else
            {
                MeasureVertical(constraints);
            }
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (Orientation == OrientationValue::Horizontal)
            {
                LayoutHorizontal(width, height);
            }
            else
            {
                LayoutVertical(width, height);
            }
        }

    private:
        void MeasureHorizontal(BoxConstraints constraints)
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const f32 maxWidth = (constraints.MaxWidth < kFloatMax)
                                     ? constraints.MaxWidth - chrome.TotalHorizontal()
                                     : kFloatMax;
            // Children get bounded loose constraints (an unbounded fill-style leaf would measure
            // to kFloatMax and break the flow); the base class handles margins, and rows use
            // margin boxes.
            const f32 crossMax = (constraints.MaxHeight < kFloatMax)
                                     ? Max(0.0f, constraints.MaxHeight - chrome.TotalVertical())
                                     : kFloatMax;
            f32 lineW = 0, lineH = 0, totalW = 0, totalH = 0;
            bool firstInLine = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                child->Measure(BoxConstraints{0, maxWidth, 0, crossMax});
                const Float2 mb = child->MarginBoxSize();
                const f32 cw = mb.x;
                const f32 ch = mb.y;

                if (!firstInLine && lineW + HSpacing + cw > maxWidth)
                {
                    totalW = Max(totalW, lineW);
                    totalH += lineH + VSpacing;
                    lineW = 0;
                    lineH = 0;
                    firstInLine = true;
                }
                if (!firstInLine)
                {
                    lineW += HSpacing;
                }
                lineW += cw;
                lineH = Max(lineH, ch);
                firstInLine = false;
            }
            totalW = Max(totalW, lineW);
            totalH += lineH;

            MeasuredSize = Float2{constraints.ConstrainWidth(totalW + chrome.TotalHorizontal()),
                                  constraints.ConstrainHeight(totalH + chrome.TotalVertical())};
        }

        void MeasureVertical(BoxConstraints constraints)
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const f32 maxHeight = (constraints.MaxHeight < kFloatMax)
                                      ? constraints.MaxHeight - chrome.TotalVertical()
                                      : kFloatMax;
            const f32 crossMax = (constraints.MaxWidth < kFloatMax)
                                     ? Max(0.0f, constraints.MaxWidth - chrome.TotalHorizontal())
                                     : kFloatMax;
            f32 colW = 0, colH = 0, totalW = 0, totalH = 0;
            bool firstInCol = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                child->Measure(BoxConstraints{0, crossMax, 0, maxHeight});
                const Float2 mb = child->MarginBoxSize();
                const f32 cw = mb.x;
                const f32 ch = mb.y;

                if (!firstInCol && colH + VSpacing + ch > maxHeight)
                {
                    totalH = Max(totalH, colH);
                    totalW += colW + HSpacing;
                    colW = 0;
                    colH = 0;
                    firstInCol = true;
                }
                if (!firstInCol)
                {
                    colH += VSpacing;
                }
                colH += ch;
                colW = Max(colW, cw);
                firstInCol = false;
            }
            totalH = Max(totalH, colH);
            totalW += colW;

            MeasuredSize = Float2{constraints.ConstrainWidth(totalW + chrome.TotalHorizontal()),
                                  constraints.ConstrainHeight(totalH + chrome.TotalVertical())};
        }

        void LayoutHorizontal(f32 width, f32 height)
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            (void)height;
            const f32 maxWidth = width - chrome.TotalHorizontal();
            f32 xPos = chrome.Left;
            f32 yPos = chrome.Top;
            f32 lineH = 0;
            bool firstInLine = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                const Float2 mb = child->MarginBoxSize(); // placed as margin boxes
                const f32 cw = mb.x;
                const f32 ch = mb.y;

                if (!firstInLine && xPos - chrome.Left + HSpacing + cw > maxWidth)
                {
                    yPos += lineH + VSpacing;
                    xPos = chrome.Left;
                    lineH = 0;
                    firstInLine = true;
                }
                if (!firstInLine)
                {
                    xPos += HSpacing;
                }
                child->Layout(xPos, yPos, cw, ch);
                xPos += cw;
                lineH = Max(lineH, ch);
                firstInLine = false;
            }
        }

        void LayoutVertical(f32 width, f32 height)
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            (void)width;
            const f32 maxHeight = height - chrome.TotalVertical();
            f32 xPos = chrome.Left;
            f32 yPos = chrome.Top;
            f32 colW = 0;
            bool firstInCol = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                const Float2 mb = child->MarginBoxSize(); // placed as margin boxes
                const f32 cw = mb.x;
                const f32 ch = mb.y;

                if (!firstInCol && yPos - chrome.Top + VSpacing + ch > maxHeight)
                {
                    xPos += colW + HSpacing;
                    yPos = chrome.Top;
                    colW = 0;
                    firstInCol = true;
                }
                if (!firstInCol)
                {
                    yPos += VSpacing;
                }
                child->Layout(xPos, yPos, cw, ch);
                yPos += ch;
                colW = Max(colW, cw);
                firstInCol = false;
            }
        }
    };

    RTTI_DEFINE_OBJECT(FlowLayout, "rtti::ui")
}
