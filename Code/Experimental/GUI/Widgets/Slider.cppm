// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :slider partition
//
// Slider: a draggable value in [0,1]. A lean native control modeled on eepp's
// UISlider (role only). Pressing/dragging sets the value from the cursor x; because the
// EventDispatcher captures the pointer to the pressed node, the drag keeps tracking even
// when the cursor leaves the slider. OnDraw renders a track, a fill up to the handle, and a
// round handle. Horizontal only (vertical is a follow-up).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:slider;

import foundation.core; // Color, Function, Move, Max, Min, Float2, Rectangle
import foundation.vg;   // CornerRadii
import :reactive; // Observable (what bindings follow)
import :rect;
import :event;
import :draw_context;
import :ui_widget;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    class Slider : public UIWidget
    {
        RTTI_OBJECT(Slider, UIWidget)
    public:
        Slider()
        {
            SetTag(core::StringView(u8"slider"));
            SetTabFocusable(true);
        }

        /// The value (a read a binding records).
        [[nodiscard]] f32 GetValue() const
        {
            m_valueChanged.Track();
            return m_value;
        }
        void SetValue(f32 value)
        {
            value = core::Max(0.0f, core::Min(1.0f, value));
            if (value == m_value)
                return;
            m_value = value;
            Invalidate();
            m_valueChanged.Notify();
            if (m_onChanged)
                m_onChanged(m_value);
            SendEvent(Event(EventType::Changed, this));
        }

        void SetOnValueChanged(core::Function<void(f32)> callback)
        {
            m_onChanged = core::Move(callback);
        }

        void SetTrackColor(Color color)
        {
            m_trackColor = color;
            Invalidate();
        }
        void SetFillColor(Color color)
        {
            m_fillColor = color;
            Invalidate();
        }
        void SetHandleColor(Color color)
        {
            m_handleColor = color;
            Invalidate();
        }

        /// The ring and halo around the thumb while it has keyboard or pad focus.
        void SetFocusColor(Color color)
        {
            m_focusColor = color;
            Invalidate();
        }

        // Theming parts: slider::track / ::fill / ::thumb / ::focus (the thumb's focus ring).
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"track"));
            out.PushBack(core::StringView(u8"fill"));
            out.PushBack(core::StringView(u8"thumb"));
            out.PushBack(core::StringView(u8"focus"));
        }

        /// A track's length and a thumb's height when nothing sizes it (E2).
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{160.0f, 24.0f}; }

    protected:
        // Drag state is the slider's own m_dragging: pointer capture brings the moves and the
        // release here even after the cursor leaves it.
        // Left and right step the value, and stay the slider's at its ends: holding the d-pad to
        // reach 0 or 100 must not throw focus off it (user ruling); up and down navigate. Home and
        // End jump.
        void OnKeyDown(const KeyEvent& event) override
        {
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (key == KeyCode::Left)
                SetValue(m_value - m_keyStep);
            else if (key == KeyCode::Right)
                SetValue(m_value + m_keyStep);
            else if (key == KeyCode::Home)
                SetValue(0.0f);
            else if (key == KeyCode::End)
                SetValue(1.0f);
            else
                return;
            event.Handled = true;
        }

        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button != MouseButton::Left)
                return;
            m_dragging = true;
            UpdateFromEvent(event);
            event.Handled = true;
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            if (m_dragging)
                UpdateFromEvent(event);
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            (void)event;
            m_dragging = false;
        }
        void OnPointerCancel(const MouseEvent&) override { m_dragging = false; }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            const f32 handleR = b.height * 0.5f;
            const f32 cy = b.y + b.height * 0.5f;
            const f32 trackH = 4.0f;
            const f32 trackTop = cy - trackH * 0.5f;
            const f32 x0 = b.x + handleR;
            const f32 x1 = b.x + b.width - handleR;
            const f32 handleX = x0 + (x1 - x0) * m_value;

            // Each part its own look with the sheet's over it (slider::track, ::fill, ::thumb, ::focus).
            GetPartStyle(u8"track").Draw(ctx, Rect{b.x, trackTop, b.width, trackH}, FilledBox(m_trackColor, vg::CornerRadii(trackH * 0.5f)));
            GetPartStyle(u8"fill").Draw(ctx, Rect{b.x, trackTop, handleX - b.x, trackH}, FilledBox(m_fillColor, vg::CornerRadii(trackH * 0.5f)));
            // Focused by the keyboard or a pad, the thumb (the part that moves) is marked: a ring
            // round it with a glow, not a ring around the whole track.
            if (IsFocusVisible())
            {
                // Its glow takes the ring's colour, the sheet's (slider::focus { border-color }) or its own.
                const PartStyle& focus = GetPartStyle(u8"focus");
                const Color ringColor = focus.HasBorderColor || focus.HasBorderWidth ? focus.Box.BorderColors.top : m_focusColor;
                const f32 ringR = handleR + 3.0f;
                NodeBox ring = OutlinedBox(ringColor, 2.0f, vg::CornerRadii(ringR));
                ring.Shadows.PushBack(BoxShadow{core::Float2{0.0f, 0.0f}, 6.0f, 1.0f,
                                                Color{ringColor.r, ringColor.g, ringColor.b, ringColor.a * 0.45f}, false});
                focus.Draw(ctx, Rect{handleX - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f}, ring);
            }
            GetPartStyle(u8"thumb").Draw(ctx, Rect{handleX - handleR, cy - handleR, handleR * 2.0f, handleR * 2.0f},
                                         FilledBox(m_handleColor, vg::CornerRadii(handleR)));
        }

    private:
        void UpdateFromEvent(const MouseEvent& event)
        {
            const Rect b = GetContentBounds();
            const f32 handleR = b.height * 0.5f;
            const core::Float2 local = ConvertToNodeSpace(event.Position);
            const f32 x0 = b.x + handleR;
            const f32 x1 = b.x + b.width - handleR;
            SetValue((x1 > x0) ? (local.x - x0) / (x1 - x0) : 0.0f);
        }

        f32 m_value = 0.0f;
        f32 m_keyStep = 0.05f; // an arrow's step
        Observable m_valueChanged; // what bindings reading the value follow
        bool m_dragging = false;
        Color m_trackColor{0.28f, 0.30f, 0.35f, 1.0f};
        Color m_fillColor{0.31f, 0.63f, 0.85f, 1.0f};
        Color m_handleColor{0.86f, 0.89f, 0.93f, 1.0f};
        Color m_focusColor{0.31f, 0.61f, 1.0f, 1.0f};
        core::Function<void(f32)> m_onChanged;
    };
}
