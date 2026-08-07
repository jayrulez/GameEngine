// Draconic UI - :dock_layout partition
//
// Docks children to edges (Left/Top/Right/Bottom/Fill); each docked child claims space from its edge,
// shrinking the remaining area for subsequent children. Ported from Sedulous.UI/src/Layout/DockLayout.bf.
// (Beef nested `DockLayout.LayoutParams` -> top-level `DockLayoutParams`.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:dock_layout;

import draconic.foundation; // Max
import :view;
import :layout_params;
import :box_constraints;
import :thickness;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Dock position for a DockLayout child.
    enum class Dock
    {
        Left,
        Top,
        Right,
        Bottom,
        Fill
    };

    /// LayoutParams for a DockLayout child.
    class DockLayoutParams : public LayoutParams
    {
        DRACONIC_OBJECT(DockLayoutParams, LayoutParams)
    public:
        ::draconic::ui::Dock Dock = ::draconic::ui::Dock::Left;
        DockLayoutParams() = default;
        explicit DockLayoutParams(::draconic::ui::Dock dock) : Dock(dock) {}
    };

    class DockLayout : public ViewGroup
    {
        DRACONIC_OBJECT(DockLayout, ViewGroup)
    public:
        /// When true, the last child fills all remaining space regardless of its Dock.
        bool LastChildFill = false;

        DockLayout() = default;

    protected:
        LayoutParamsPtr CreateDefaultLayoutParams() override
        {
            return MakeRef<DockLayoutParams>(DefaultAllocator());
        }

        void OnMeasure(BoxConstraints constraints) override
        {
            f32 usedLeft = 0, usedTop = 0, usedRight = 0, usedBottom = 0, maxW = 0, maxH = 0;
            const usize count = ChildCount();

            for (usize i = 0; i < count; ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }

                DockLayoutParams* lp = Cast<DockLayoutParams>(child->LayoutParams.Get());
                const draconic::ui::Dock dock = lp != nullptr ? lp->Dock : draconic::ui::Dock::Left;
                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};

                const f32 remainW = Max(0.0f, constraints.MaxWidth - Padding.TotalHorizontal() -
                                                  usedLeft - usedRight);
                const f32 remainH = Max(0.0f, constraints.MaxHeight - Padding.TotalVertical() -
                                                  usedTop - usedBottom);

                const bool isFill =
                    (LastChildFill && i == count - 1) || dock == draconic::ui::Dock::Fill;
                BoxConstraints childConstraints =
                    isFill ? BoxConstraints::Tight(Max(0.0f, remainW - margin.TotalHorizontal()),
                                                   Max(0.0f, remainH - margin.TotalVertical()))
                           : BoxConstraints{0, Max(0.0f, remainW - margin.TotalHorizontal()), 0,
                                            Max(0.0f, remainH - margin.TotalVertical())};
                child->Measure(childConstraints);

                switch (dock)
                {
                case draconic::ui::Dock::Left:
                    usedLeft += child->MeasuredSize.x + margin.TotalHorizontal();
                    break;
                case draconic::ui::Dock::Right:
                    usedRight += child->MeasuredSize.x + margin.TotalHorizontal();
                    break;
                case draconic::ui::Dock::Top:
                    usedTop += child->MeasuredSize.y + margin.TotalVertical();
                    break;
                case draconic::ui::Dock::Bottom:
                    usedBottom += child->MeasuredSize.y + margin.TotalVertical();
                    break;
                case draconic::ui::Dock::Fill:
                    break;
                }
                maxW = Max(maxW, usedLeft + usedRight);
                maxH = Max(maxH, usedTop + usedBottom);
            }

            MeasuredSize = Float2{constraints.ConstrainWidth(maxW + Padding.TotalHorizontal()),
                                  constraints.ConstrainHeight(maxH + Padding.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            f32 dockLeft = Padding.Left;
            f32 dockTop = Padding.Top;
            f32 dockRight = width - Padding.Right;
            f32 dockBottom = height - Padding.Bottom;
            const usize count = ChildCount();

            for (usize i = 0; i < count; ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }

                DockLayoutParams* lp = Cast<DockLayoutParams>(child->LayoutParams.Get());
                const draconic::ui::Dock dock = lp != nullptr ? lp->Dock : draconic::ui::Dock::Left;
                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};
                const bool isFill =
                    (LastChildFill && i == count - 1) || dock == draconic::ui::Dock::Fill;

                if (isFill)
                {
                    child->Layout(dockLeft + margin.Left, dockTop + margin.Top,
                                  Max(0.0f, dockRight - dockLeft - margin.TotalHorizontal()),
                                  Max(0.0f, dockBottom - dockTop - margin.TotalVertical()));
                    continue;
                }

                switch (dock)
                {
                case draconic::ui::Dock::Left:
                    child->Layout(dockLeft + margin.Left, dockTop + margin.Top,
                                  child->MeasuredSize.x,
                                  Max(0.0f, dockBottom - dockTop - margin.TotalVertical()));
                    dockLeft += child->MeasuredSize.x + margin.TotalHorizontal();
                    break;
                case draconic::ui::Dock::Right:
                    child->Layout(dockRight - child->MeasuredSize.x - margin.Right,
                                  dockTop + margin.Top, child->MeasuredSize.x,
                                  Max(0.0f, dockBottom - dockTop - margin.TotalVertical()));
                    dockRight -= child->MeasuredSize.x + margin.TotalHorizontal();
                    break;
                case draconic::ui::Dock::Top:
                    child->Layout(dockLeft + margin.Left, dockTop + margin.Top,
                                  Max(0.0f, dockRight - dockLeft - margin.TotalHorizontal()),
                                  child->MeasuredSize.y);
                    dockTop += child->MeasuredSize.y + margin.TotalVertical();
                    break;
                case draconic::ui::Dock::Bottom:
                    child->Layout(dockLeft + margin.Left,
                                  dockBottom - child->MeasuredSize.y - margin.Bottom,
                                  Max(0.0f, dockRight - dockLeft - margin.TotalHorizontal()),
                                  child->MeasuredSize.y);
                    dockBottom -= child->MeasuredSize.y + margin.TotalVertical();
                    break;
                case draconic::ui::Dock::Fill:
                    break;
                }
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(DockLayoutParams, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(DockLayout, "draconic::ui")
}
