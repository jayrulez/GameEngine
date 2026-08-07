// Draconic UI - :absolute_layout partition
//
// Positions children at explicit X/Y coordinates. Ported from Sedulous.UI/src/Layout/AbsoluteLayout.bf.
// (Beef nested `AbsoluteLayout.LayoutParams` -> top-level `AbsoluteLayoutParams`; the class's own
// child-constraint helper is renamed to avoid hiding ViewGroup::MakeChildConstraints.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:absolute_layout;

import draconic.foundation; // Max, RefPtr, kFloatMax
import :view;
import :layout_params;
import :box_constraints;
import :size_spec;
import :thickness;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// LayoutParams for an AbsoluteLayout child: explicit X/Y.
    class AbsoluteLayoutParams : public LayoutParams
    {
        DRACONIC_OBJECT(AbsoluteLayoutParams, LayoutParams)
    public:
        f32 X = 0.0f;
        f32 Y = 0.0f;
        AbsoluteLayoutParams() = default;
    };

    class AbsoluteLayout : public ViewGroup
    {
        DRACONIC_OBJECT(AbsoluteLayout, ViewGroup)
    public:
        AbsoluteLayout() = default;

    protected:
        LayoutParamsPtr CreateDefaultLayoutParams() override
        {
            return MakeRef<AbsoluteLayoutParams>(DefaultAllocator());
        }

        void OnMeasure(BoxConstraints constraints) override
        {
            f32 maxR = 0, maxB = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }

                child->Measure(MakeAbsoluteChildConstraints(constraints, child));

                AbsoluteLayoutParams* alp = Cast<AbsoluteLayoutParams>(child->LayoutParams.Get());
                const f32 x = alp != nullptr ? alp->X : 0.0f;
                const f32 y = alp != nullptr ? alp->Y : 0.0f;
                maxR = Max(maxR, x + child->MeasuredSize.x);
                maxB = Max(maxB, y + child->MeasuredSize.y);
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(maxR + Padding.TotalHorizontal()),
                                  constraints.ConstrainHeight(maxB + Padding.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }

                const LayoutParamsPtr& lp = child->LayoutParams;
                AbsoluteLayoutParams* alp = Cast<AbsoluteLayoutParams>(lp.Get());
                const f32 ax = alp != nullptr ? alp->X : 0.0f;
                const f32 ay = alp != nullptr ? alp->Y : 0.0f;
                const f32 x = Padding.Left + ax;
                const f32 y = Padding.Top + ay;

                f32 w = child->MeasuredSize.x;
                f32 h = child->MeasuredSize.y;
                if (lp)
                {
                    if (lp->Width.kind == SizeSpec::Kind::Match)
                        w = Max(0.0f, width - Padding.TotalHorizontal() - ax);
                    if (lp->Height.kind == SizeSpec::Kind::Match)
                        h = Max(0.0f, height - Padding.TotalVertical() - ay);
                }
                child->Layout(x, y, w, h);
            }
        }

    private:
        BoxConstraints MakeAbsoluteChildConstraints(BoxConstraints parentConstraints,
                                                    View* child) const
        {
            const LayoutParamsPtr& lp = child->LayoutParams;
            f32 minW = 0, maxW = kFloatMax, minH = 0, maxH = kFloatMax;
            if (lp)
            {
                switch (lp->Width.kind)
                {
                case SizeSpec::Kind::Fixed:
                {
                    const f32 v = lp->Width.ResolveFixed(1.0f);
                    minW = v;
                    maxW = v;
                    break;
                }
                case SizeSpec::Kind::Match:
                {
                    const f32 avail =
                        Max(0.0f, parentConstraints.MaxWidth - Padding.TotalHorizontal());
                    minW = avail;
                    maxW = avail;
                    break;
                }
                case SizeSpec::Kind::Wrap:
                    break;
                }
                switch (lp->Height.kind)
                {
                case SizeSpec::Kind::Fixed:
                {
                    const f32 v = lp->Height.ResolveFixed(1.0f);
                    minH = v;
                    maxH = v;
                    break;
                }
                case SizeSpec::Kind::Match:
                {
                    const f32 avail =
                        Max(0.0f, parentConstraints.MaxHeight - Padding.TotalVertical());
                    minH = avail;
                    maxH = avail;
                    break;
                }
                case SizeSpec::Kind::Wrap:
                    break;
                }
            }
            return BoxConstraints{minW, maxW, minH, maxH};
        }
    };

    DRACONIC_DEFINE_OBJECT(AbsoluteLayoutParams, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(AbsoluteLayout, "draconic::ui")
}
