// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :progress_bar partition
//
// Progress indicator showing a filled bar from 0 to 1, or, indeterminate, a segment sliding across
// the track while it shows. Ported from Sedulous.UI/src/Controls/ProgressBar.bf.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:progress_bar;

import foundation.core;
import foundation.vg;
import :view;
import :event;
import :property;
import :box_constraints;
import :style_property;
import :control_state;
import :draw_context;
import :drawable;

using namespace foundation::core;

export namespace foundation::ui
{
    class ProgressBar : public View
    {
        RTTI_OBJECT(ProgressBar, View)
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
            // Bounded fill (P2c): default length under an unbounded parent.
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.BoundedMaxWidth(200.0f)),
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

            if (IsIndeterminate.Value())
            {
                // No known amount: a segment a third of the track slides across it, entering at
                // the left and leaving at the right, once every kIndeterminatePeriod seconds.
                const f32 t = Context != nullptr ? Context->TotalTime() : 0.0f;
                const f32 phase = t / kIndeterminatePeriod - Floor(t / kIndeterminatePeriod);
                const f32 segment = Width() / 3.0f;
                const f32 left = Max(0.0f, phase * (Width() + segment) - segment);
                const f32 right = Min(Width(), phase * (Width() + segment));
                ctx.VG().PushClipRect(Rectangle{left, 0, Max(0.0f, right - left), Height()});
                DrawFill(ctx, bounds, state);
                ctx.VG().PopClip();
                if (Context != nullptr)
                {
                    Context->RequestNextFrame(); // it moves while it shows
                }
            }
            else if (Value.Value() > 0)
            {
                const f32 fillW = Width() * Value.Value();
                ctx.VG().PushClipRect(Rectangle{0, 0, fillW, Height()});
                DrawFill(ctx, bounds, state);
                ctx.VG().PopClip();
            }
        }

    public:
        /// Seconds the indeterminate segment takes to cross the track.
        static constexpr f32 kIndeterminatePeriod = 1.5f;

    private:
        void DrawFill(UIDrawContext& ctx, const Rectangle& bounds, ControlState state)
        {
            if (Drawable* fill = ResolvePartDrawable(u8"fill", StyleProperty::Background, state))
            {
                fill->Draw(ctx, bounds);
            }
            else
            {
                ctx.VG().FillRect(bounds, Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});
            }
        }
    };

    RTTI_DEFINE_OBJECT(ProgressBar, "rtti::ui")
}
