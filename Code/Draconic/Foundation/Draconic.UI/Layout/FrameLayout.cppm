// Draconic UI - :frame_layout partition
//
// Stacks children on top of each other, each positioned independently by Gravity. Simplest ViewGroup.
// Ported from Sedulous.UI/src/Layout/FrameLayout.bf.
//
// Divergence (language): Beef's nested `FrameLayout.LayoutParams` becomes a distinct top-level
// `FrameLayoutParams` (a nested `LayoutParams` would shadow View's inherited `LayoutParams` field).
// The `Gravity` field's type is referenced via the `GravityValue` alias inside the class (the field
// name shadows the enum type).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:frame_layout;

import draconic.foundation; // Max, RefPtr, Rectangle
import :view;         // View, ViewGroup, LayoutParamsPtr
import :layout_params;
import :box_constraints;
import :thickness;
import :gravity;
import :gravity_helper;

using namespace draconic::foundation;

export namespace draconic::ui
{
    using GravityValue = Gravity;

    /// LayoutParams for a FrameLayout child: a Gravity anchor.
    class FrameLayoutParams : public LayoutParams
    {
        DRACONIC_OBJECT(FrameLayoutParams, LayoutParams)
    public:
        GravityValue Gravity = GravityValue::None;
        FrameLayoutParams() = default;
    };

    class FrameLayout : public ViewGroup
    {
        DRACONIC_OBJECT(FrameLayout, ViewGroup)
    public:
        FrameLayout() = default;

    protected:
        LayoutParamsPtr CreateDefaultLayoutParams() override
        {
            return MakeRef<FrameLayoutParams>(DefaultAllocator());
        }

        void OnMeasure(BoxConstraints constraints) override
        {
            f32 maxW = 0, maxH = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }

                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};
                const BoxConstraints inner =
                    MakeChildConstraints(constraints.Deflate(Padding), child);
                child->Measure(inner);

                maxW = Max(maxW, child->MeasuredSize.x + margin.TotalHorizontal());
                maxH = Max(maxH, child->MeasuredSize.y + margin.TotalVertical());
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(maxW + Padding.TotalHorizontal()),
                                  constraints.ConstrainHeight(maxH + Padding.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const f32 contentW = width - Padding.TotalHorizontal();
            const f32 contentH = height - Padding.TotalVertical();

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }

                FrameLayoutParams* flp = Cast<FrameLayoutParams>(child->LayoutParams.Get());
                const GravityValue gravity = flp != nullptr ? flp->Gravity : GravityValue::None;
                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};

                Rectangle rect =
                    GravityHelper::Apply(gravity, contentW, contentH, child->MeasuredSize.x,
                                         child->MeasuredSize.y, margin);
                rect.x += Padding.Left;
                rect.y += Padding.Top;
                child->Layout(rect.x, rect.y, rect.width, rect.height);
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(FrameLayoutParams, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(FrameLayout, "draconic::ui")
}
