// Draconic UI - :panel partition
//
// Container with an optional background drawable; children fill the panel minus padding. Ported from
// Sedulous.UI/src/Controls/Panel.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:panel;

import draconic.foundation;
import :view;
import :box_constraints;
import :thickness;
import :style_property;
import :control_state;
import :draw_context;
import :drawable;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class Panel : public ViewGroup
    {
        DRACONIC_OBJECT(Panel, ViewGroup)
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
                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};
                child->Measure(inner.Deflate(margin));
                maxW = Max(maxW, child->MeasuredSize.x + margin.TotalHorizontal());
                maxH = Max(maxH, child->MeasuredSize.y + margin.TotalVertical());
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
                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};
                child->Layout(pad.Left + margin.Left, pad.Top + margin.Top,
                              Max(0.0f, contentW - margin.TotalHorizontal()),
                              Max(0.0f, contentH - margin.TotalVertical()));
            }
        }

    private:
        /// Max of explicit Padding and the background drawable's DrawablePadding.
        [[nodiscard]] Thickness EffectivePadding()
        {
            Thickness dp{};
            if (Drawable* bg = ResolveStyleDrawable(StyleProperty::Background))
            {
                dp = bg->DrawablePadding();
            }
            return Thickness{Max(Padding.Left, dp.Left), Max(Padding.Top, dp.Top),
                             Max(Padding.Right, dp.Right), Max(Padding.Bottom, dp.Bottom)};
        }
    };

    DRACONIC_DEFINE_OBJECT(Panel, "draconic::ui")
}
