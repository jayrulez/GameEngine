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

        // Theming parts: slider::track / ::fill / ::thumb.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"track"));
            out.PushBack(core::StringView(u8"fill"));
            out.PushBack(core::StringView(u8"thumb"));
        }
        void SetThemePartColor(core::StringView part, Color color) override
        {
            if (part == core::StringView(u8"track"))
                SetTrackColor(color);
            else if (part == core::StringView(u8"fill"))
                SetFillColor(color);
            else if (part == core::StringView(u8"thumb"))
                SetHandleColor(color);
        }

    protected:
        // Drag state is tracked with our own m_dragging flag rather than IsPressed(): a
        // captured drag that leaves the slider fires OnMouseLeave (which clears m_pressed)
        // BEFORE the routed OnMouseMove, so gating the move on IsPressed() would stop the
        // drag the instant the cursor left. Pointer capture guarantees the release reaches
        // us, so OnMouseUp reliably clears m_dragging.
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
            UINode::OnMouseDown(event);
            if (event.Button != MouseButton::Left)
                return;
            m_dragging = true;
            UpdateFromEvent(event);
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            if (m_dragging)
                UpdateFromEvent(event);
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            m_dragging = false;
            UINode::OnMouseUp(event);
        }

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

            ctx.VG().FillRoundedRect(core::Rectangle{b.x, trackTop, b.width, trackH},
                                     vg::CornerRadii(trackH * 0.5f), m_trackColor);
            ctx.VG().FillRoundedRect(core::Rectangle{b.x, trackTop, handleX - b.x, trackH},
                                     vg::CornerRadii(trackH * 0.5f), m_fillColor);
            ctx.VG().FillCircle(core::Float2{handleX, cy}, handleR, m_handleColor);
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
        core::Function<void(f32)> m_onChanged;
    };
}
