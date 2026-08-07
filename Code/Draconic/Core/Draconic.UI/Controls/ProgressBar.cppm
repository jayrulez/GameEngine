// Draconic UI - :progress_bar partition
//
// Progress indicator showing a filled bar from 0 to 1. Ported from Sedulous.UI/src/Controls/ProgressBar.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:progress_bar;

import draconic.foundation;
import draconic.vg;
import :view;
import :event;
import :property;
import :box_constraints;
import :style_property;
import :control_state;
import :draw_context;
import :drawable;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class ProgressBar : public View
    {
        DRACONIC_OBJECT(ProgressBar, View)
    public:
        Property<f32> Value{0.0f}; ///< Progress value (0..1).
        Property<bool> IsIndeterminate{false};

        ProgressBar()
        {
            Value.SetOwner(this, InvalidationKind::Visual);
            IsIndeterminate.SetOwner(this, InvalidationKind::Visual);
            ProgressBar* self = this;
            Value.Changed.Add(Event<void(f32)>::Handler{[self](f32 val)
                                                        {
                                                            const f32 clamped =
                                                                Max(0.0f, Min(val, 1.0f));
                                                            if (clamped != val)
                                                            {
                                                                self->Value.SetSilent(clamped);
                                                            }
                                                        }});
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(16.0f)};
        }
        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const ControlState state = GetControlState();

            if (Drawable* track = ResolvePartDrawable(u8"track", StyleProperty::Background, state))
            {
                track->Draw(ctx, bounds);
            }
            else
            {
                ctx.VG().FillRect(bounds,
                                  Color{50.0f / 255.0f, 52.0f / 255.0f, 62.0f / 255.0f, 1.0f});
            }

            if (Value.Value() > 0)
            {
                const f32 fillW = Width() * Value.Value();
                ctx.VG().PushClipRect(Rectangle{0, 0, fillW, Height()});
                if (Drawable* fill =
                        ResolvePartDrawable(u8"fill", StyleProperty::Background, state))
                {
                    fill->Draw(ctx, bounds);
                }
                else
                {
                    ctx.VG().FillRect(
                        bounds, Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});
                }
                ctx.VG().PopClip();
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(ProgressBar, "draconic::ui")
}
