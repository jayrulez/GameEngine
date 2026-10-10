// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :slider partition
//
// Value slider with track, fill, and draggable thumb. Ported from Sedulous.UI/src/Controls/Slider.bf.
// (The Min/Max/Orientation properties shadow core::Min/Max and the Orientation type, so the core
// functions are called qualified as core::Min/core::Max and the enum is fully qualified. Round -> std::round.)

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:slider;

import foundation.core;
import foundation.vg;
import :view;
import :event;
import :property;
import :control_state;
import :style_property;
import :box_constraints;
import :draw_context;
import :drawable;
import :event_args;
import :input_enums;
import :enums;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui
{
    class Slider : public View
    {
        RTTI_OBJECT(Slider, View)
    public:
        Property<f32> Value{0.0f};
        Property<f32> Min{0.0f};
        Property<f32> Max{1.0f};
        Property<f32> Step{0.0f};
        Property<::foundation::ui::Orientation> Orientation{::foundation::ui::Orientation::Horizontal};

        Event<void(Slider*, f32)> OnValueChanged;
        Event<void(Slider*)> OnDragStarted;
        Event<void(Slider*)> OnDragEnded;

        Slider() { Init(); }
        Slider(f32 minV, f32 maxV, f32 value = 0.0f)
        {
            Init();
            Min.SetSilent(minV);
            Max.SetSilent(maxV);
            Value.SetSilent(core::Max(minV, core::Min(value, maxV)));
        }

        /// A press drags the thumb: never a drag of a source above it (a slider in a draggable row).
        [[nodiscard]] bool OwnsPressDrag() const override { return true; }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (e.Button == MouseButton::Left)
            {
                m_dragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                UpdateValueFromMouse(e.X, e.Y);
                OnDragStarted.Invoke(this);
                e.Handled = true;
            }
        }
        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left && m_dragging)
            {
                m_dragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                OnDragEnded.Invoke(this);
                e.Handled = true;
            }
        }
        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_dragging)
            {
                UpdateValueFromMouse(e.X, e.Y);
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            const f32 range = Max.Value() - Min.Value();
            const f32 smallStep = Step.Value() > 0 ? Step.Value() : range * 0.05f;
            // Only the arrows along the slider's own axis: the other two are left unhandled, so in a
            // menu of sliders and buttons they still move between them, keyboard and pad alike,
            // instead of a horizontal slider swallowing up and down.
            const bool horizontal = Orientation.Value() == ::foundation::ui::Orientation::Horizontal;
            const KeyCode increase = horizontal ? KeyCode::Right : KeyCode::Up;
            const KeyCode decrease = horizontal ? KeyCode::Left : KeyCode::Down;
            if (e.Key == increase)
            {
                Value.SetValue(Value.Value() + smallStep);
                e.Handled = true;
                return;
            }
            if (e.Key == decrease)
            {
                Value.SetValue(Value.Value() - smallStep);
                e.Handled = true;
                return;
            }
            switch (e.Key)
            {
            case KeyCode::Home:
                Value.SetValue(Min.Value());
                e.Handled = true;
                break;
            case KeyCode::End:
                Value.SetValue(Max.Value());
                e.Handled = true;
                break;
            default:
                break;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // Bounded fill (P2c): default length under an unbounded parent.
            if (Orientation.Value() == ::foundation::ui::Orientation::Horizontal)
                MeasuredSize = Float2{constraints.ConstrainWidth(constraints.BoundedMaxWidth(160.0f)),
                                      constraints.ConstrainHeight(20.0f)};
            else
                MeasuredSize = Float2{constraints.ConstrainWidth(20.0f),
                                      constraints.ConstrainHeight(constraints.BoundedMaxHeight(160.0f))};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const ControlState state = GetControlState();
            const f32 trackHeight = ResolvePartFloat(u8"track", StyleProperty::Height, state, 4.0f);
            const f32 thumbSize = ResolvePartFloat(u8"thumb", StyleProperty::Width, state, 16.0f);
            const f32 thumbHalf = thumbSize * 0.5f;
            Drawable* trackDrawable =
                ResolvePartDrawable(u8"track", StyleProperty::Background, state);
            Drawable* fillDrawable =
                ResolvePartDrawable(u8"fill", StyleProperty::Background, state);
            Drawable* thumbDrawable =
                ResolvePartDrawable(u8"thumb", StyleProperty::Background, state);
            const f32 progress = (Max.Value() > Min.Value())
                                     ? (Value.Value() - Min.Value()) / (Max.Value() - Min.Value())
                                     : 0.0f;
            const Color trackCol{50.0f / 255.0f, 52.0f / 255.0f, 62.0f / 255.0f, 1.0f};
            const Color fillCol{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f};
            const Color thumbCol{220.0f / 255.0f, 220.0f / 255.0f, 230.0f / 255.0f, 1.0f};

            if (Orientation.Value() == ::foundation::ui::Orientation::Horizontal)
            {
                const f32 trackY = (Height() - trackHeight) * 0.5f;
                const f32 trackLeft = thumbHalf;
                const f32 trackW = (Width() - thumbHalf) - trackLeft;
                DrawPart(ctx, trackDrawable, Rectangle{trackLeft, trackY, trackW, trackHeight}, trackCol, state);
                const f32 fillW = trackW * progress;
                if (fillW > 0)
                {
                    DrawPart(ctx, fillDrawable, Rectangle{trackLeft, trackY, fillW, trackHeight}, fillCol, state);
                }
                const f32 thumbX = trackLeft + trackW * progress;
                const Rectangle thumbRect{thumbX - thumbHalf, Height() * 0.5f - thumbHalf,
                                          thumbSize, thumbSize};
                if (thumbDrawable)
                {
                    thumbDrawable->Draw(ctx, thumbRect, state);
                }
                else
                {
                    ctx.VG().FillCircle(Float2{thumbX, Height() * 0.5f}, thumbHalf, thumbCol);
                }
            }
            else
            {
                const f32 trackX = (Width() - trackHeight) * 0.5f;
                const f32 trackTop = thumbHalf;
                const f32 trackH = (Height() - thumbHalf) - trackTop;
                DrawPart(ctx, trackDrawable, Rectangle{trackX, trackTop, trackHeight, trackH}, trackCol, state);
                // Vertical runs bottom to top: the fill grows UP from the low end, where a vertical
                // slider's minimum sits.
                const f32 fillH = trackH * progress;
                if (fillH > 0)
                {
                    DrawPart(ctx, fillDrawable, Rectangle{trackX, (trackTop + trackH) - fillH, trackHeight, fillH},
                             fillCol, state);
                }
                const f32 thumbY = (trackTop + trackH) - trackH * progress;
                const Rectangle thumbRect{Width() * 0.5f - thumbHalf, thumbY - thumbHalf, thumbSize,
                                          thumbSize};
                if (thumbDrawable)
                {
                    thumbDrawable->Draw(ctx, thumbRect, state);
                }
                else
                {
                    ctx.VG().FillCircle(Float2{Width() * 0.5f, thumbY}, thumbHalf, thumbCol);
                }
            }
        }

    private:
        // With the control's state, so a state-list part (a thumb that lights up when focused)
        // draws the entry for it; without, a state list only ever draws its normal entry.
        static void DrawPart(UIDrawContext& ctx, Drawable* drawable, const Rectangle& rect, const Color& fallback,
                             ControlState state)
        {
            if (drawable != nullptr)
            {
                drawable->Draw(ctx, rect, state);
            }
            else
            {
                ctx.VG().FillRect(rect, fallback);
            }
        }

        void Init()
        {
            IsFocusable = true;
            IsTabStop = true;
            WantsArrowKeys = true;
            Cursor = CursorType::Hand;
            Value.SetOwner(this, InvalidationKind::Visual);
            Min.SetOwner(this, InvalidationKind::Visual);
            Max.SetOwner(this, InvalidationKind::Visual);
            Step.SetOwner(this, InvalidationKind::Visual);
            Orientation.SetOwner(this);
            Slider* self = this;
            Value.Changed.Add(Event<void(f32)>::Handler{
                [self](f32 val)
                {
                    const f32 clamped = self->SnapToStep(
                        core::Max(self->Min.Value(), core::Min(val, self->Max.Value())));
                    if (clamped != val)
                    {
                        self->Value.SetSilent(clamped);
                    }
                    self->OnValueChanged.Invoke(self, self->Value.Value());
                }});
            Min.Changed.Add(Event<void(f32)>::Handler{[self](f32) { self->ReclampValue(); }});
            Max.Changed.Add(Event<void(f32)>::Handler{[self](f32) { self->ReclampValue(); }});
            Step.Changed.Add(Event<void(f32)>::Handler{[self](f32 val)
                                                       {
                                                           self->Step.SetSilent(
                                                               core::Max(0.0f, val));
                                                           self->ReclampValue();
                                                       }});
        }

        void ReclampValue()
        {
            const f32 clamped =
                SnapToStep(core::Max(Min.Value(), core::Min(Value.Value(), Max.Value())));
            if (clamped != Value.Value())
            {
                Value.SetValue(clamped);
            }
        }

        void UpdateValueFromMouse(f32 localX, f32 localY)
        {
            const f32 thumbSize =
                ResolvePartFloat(u8"thumb", StyleProperty::Width, GetControlState(), 16.0f);
            const f32 thumbHalf = thumbSize * 0.5f;
            f32 progress;
            if (Orientation.Value() == ::foundation::ui::Orientation::Horizontal)
            {
                const f32 trackW = Width() - thumbSize;
                progress = trackW > 0 ? (localX - thumbHalf) / trackW : 0.0f;
            }
            else
            {
                const f32 trackH = Height() - thumbSize;
                progress = trackH > 0 ? 1.0f - (localY - thumbHalf) / trackH : 0.0f;
            }
            Value.SetValue(Min.Value() + (Max.Value() - Min.Value()) *
                                             core::Max(0.0f, core::Min(progress, 1.0f)));
        }

        [[nodiscard]] f32 SnapToStep(f32 value) const
        {
            if (Step.Value() <= 0)
            {
                return value;
            }
            return Min.Value() + core::Round((value - Min.Value()) / Step.Value()) * Step.Value();
        }

        bool m_dragging = false;
    };

    RTTI_DEFINE_OBJECT(Slider, "rtti::ui")
}
