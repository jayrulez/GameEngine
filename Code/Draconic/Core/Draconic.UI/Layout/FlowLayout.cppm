// Draconic UI - :flow_layout partition
//
// Arranges children left-to-right (horizontal) or top-to-bottom (vertical), wrapping to the next
// line/column when space runs out. Ported from Sedulous.UI/src/Layout/FlowLayout.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:flow_layout;

import draconic.foundation; // Max, kFloatMax
import :view;
import :box_constraints;
import :enums; // Orientation

using namespace draconic::foundation;

export namespace draconic::ui
{
    using OrientationValue = Orientation;

    class FlowLayout : public ViewGroup
    {
        DRACONIC_OBJECT(FlowLayout, ViewGroup)
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
            const f32 maxWidth = (constraints.MaxWidth < kFloatMax)
                                     ? constraints.MaxWidth - Padding.TotalHorizontal()
                                     : 100000.0f;
            f32 lineW = 0, lineH = 0, totalW = 0, totalH = 0;
            bool firstInLine = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                child->Measure(BoxConstraints::Expand());
                const f32 cw = child->MeasuredSize.x;
                const f32 ch = child->MeasuredSize.y;

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

            MeasuredSize = Float2{constraints.ConstrainWidth(totalW + Padding.TotalHorizontal()),
                                  constraints.ConstrainHeight(totalH + Padding.TotalVertical())};
        }

        void MeasureVertical(BoxConstraints constraints)
        {
            const f32 maxHeight = (constraints.MaxHeight < kFloatMax)
                                      ? constraints.MaxHeight - Padding.TotalVertical()
                                      : 100000.0f;
            f32 colW = 0, colH = 0, totalW = 0, totalH = 0;
            bool firstInCol = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                child->Measure(BoxConstraints::Expand());
                const f32 cw = child->MeasuredSize.x;
                const f32 ch = child->MeasuredSize.y;

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

            MeasuredSize = Float2{constraints.ConstrainWidth(totalW + Padding.TotalHorizontal()),
                                  constraints.ConstrainHeight(totalH + Padding.TotalVertical())};
        }

        void LayoutHorizontal(f32 width, f32 height)
        {
            (void)height;
            const f32 maxWidth = width - Padding.TotalHorizontal();
            f32 xPos = Padding.Left;
            f32 yPos = Padding.Top;
            f32 lineH = 0;
            bool firstInLine = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                const f32 cw = child->MeasuredSize.x;
                const f32 ch = child->MeasuredSize.y;

                if (!firstInLine && xPos - Padding.Left + HSpacing + cw > maxWidth)
                {
                    yPos += lineH + VSpacing;
                    xPos = Padding.Left;
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
            (void)width;
            const f32 maxHeight = height - Padding.TotalVertical();
            f32 xPos = Padding.Left;
            f32 yPos = Padding.Top;
            f32 colW = 0;
            bool firstInCol = true;

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                const f32 cw = child->MeasuredSize.x;
                const f32 ch = child->MeasuredSize.y;

                if (!firstInCol && yPos - Padding.Top + VSpacing + ch > maxHeight)
                {
                    xPos += colW + HSpacing;
                    yPos = Padding.Top;
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

    DRACONIC_DEFINE_OBJECT(FlowLayout, "draconic::ui")
}
