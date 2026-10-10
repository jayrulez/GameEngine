// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :scroll_bar partition
//
// Standalone scrollbar (used by ScrollView internally). Ported from Sedulous.UI/src/Controls/ScrollBar.bf.
// Beef get/set properties (Value/MaxValue/ViewportSize/IsHorizontal) -> methods; Math.Clamp/Max ->
// core::Clamp/core::Max (qualified where a getter would shadow); Context.InputManager.MouseX/Y ->
// Context->GetInputManager()->MouseX()/MouseY(); RectangleF -> core::Rectangle.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:scroll_bar;

import foundation.core;
import :view;
import :event;
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
    class ScrollBar : public View
    {
        RTTI_OBJECT(ScrollBar, View)
    public:
        /// Scrollbar thickness in pixels.
        f32 BarThickness = 10.0f;

        Event<void(ScrollBar*, f32)> OnValueChanged;

        explicit ScrollBar(bool horizontal = false)
        {
            m_isHorizontal = horizontal;
            // Always the arrow pointer - a bar inside a text control must not inherit the
            // parent's IBeam through the EffectiveCursor parent walk.
            Cursor = CursorType::Arrow;
        }

        /// A press on the bar drags its thumb or pages: never a drag of a source above it.
        [[nodiscard]] bool OwnsPressDrag() const override { return true; }

        // === Properties (Beef get/set -> methods) ===
        [[nodiscard]] f32 Value() const noexcept { return m_value; }
        void SetValue(f32 value)
        {
            const f32 clamped = core::Clamp(value, 0.0f, m_maxValue);
            if (m_value == clamped)
            {
                return;
            }
            m_value = clamped;
            Invalidate();
            OnValueChanged.Invoke(this, m_value);
        }
        [[nodiscard]] f32 MaxValue() const noexcept { return m_maxValue; }
        void SetMaxValue(f32 value)
        {
            m_maxValue = core::Max(0.0f, value);
            SetValue(m_value);
        }
        [[nodiscard]] f32 ViewportSize() const noexcept { return m_viewportSize; }
        void SetViewportSize(f32 value)
        {
            m_viewportSize = core::Max(1.0f, value);
            Invalidate();
        }
        [[nodiscard]] bool IsHorizontal() const noexcept { return m_isHorizontal; }
        void SetIsHorizontal(bool value)
        {
            m_isHorizontal = value;
            Invalidate();
        }

        /// Thumb rectangle in local coordinates.
        [[nodiscard]] Rectangle GetThumbRect() const
        {
            const f32 ratio = ThumbRatio();
            const f32 norm = NormalizedValue();
            if (m_isHorizontal)
            {
                const f32 thumbW = Width() * ratio;
                const f32 thumbX = (Width() - thumbW) * norm;
                return Rectangle{thumbX, 0, thumbW, Height()};
            }
            const f32 thumbH = Height() * ratio;
            const f32 thumbY = (Height() - thumbH) * norm;
            return Rectangle{0, thumbY, Width(), thumbH};
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button != MouseButton::Left)
            {
                return;
            }

            // e.X/e.Y may be in another view's space (bubbling) - use screen coords via the input manager.
            const f32 screenX =
                Context && Context->GetInputManager() ? Context->GetInputManager()->MouseX() : 0.0f;
            const f32 screenY =
                Context && Context->GetInputManager() ? Context->GetInputManager()->MouseY() : 0.0f;
            const Float2 local = ScreenToLocal(Float2{screenX, screenY});
            const f32 localPos = m_isHorizontal ? local.x : local.y;
            const f32 screenPos = m_isHorizontal ? screenX : screenY;

            const Rectangle thumbRect = GetThumbRect();
            const f32 thumbStart = m_isHorizontal ? thumbRect.x : thumbRect.y;
            const f32 thumbEnd = thumbStart + (m_isHorizontal ? thumbRect.width : thumbRect.height);

            if (localPos >= thumbStart && localPos <= thumbEnd)
            {
                m_dragging = true;
                m_dragStartValue = m_value;
                m_dragStartMouse = screenPos;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
            }
            else
            {
                const f32 trackSize = m_isHorizontal ? Width() : Height();
                const f32 thumbSize = trackSize * ThumbRatio();
                const f32 clickNorm = (localPos - thumbSize * 0.5f) / (trackSize - thumbSize);
                SetValue(core::Clamp(clickNorm * m_maxValue, 0.0f, m_maxValue));
            }
            e.Handled = true;
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (!m_dragging)
            {
                return;
            }
            const f32 screenX =
                Context && Context->GetInputManager() ? Context->GetInputManager()->MouseX() : 0.0f;
            const f32 screenY =
                Context && Context->GetInputManager() ? Context->GetInputManager()->MouseY() : 0.0f;
            const f32 screenPos = m_isHorizontal ? screenX : screenY;

            const f32 trackSize = m_isHorizontal ? Width() : Height();
            const f32 thumbSize = trackSize * ThumbRatio();
            const f32 trackRange = trackSize - thumbSize;
            if (trackRange > 0)
            {
                const f32 delta = screenPos - m_dragStartMouse;
                const f32 valueDelta = (delta / trackRange) * m_maxValue;
                SetValue(m_dragStartValue + valueDelta);
            }
            e.Handled = true;
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (m_dragging)
            {
                m_dragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // Bounded fill (P2c): default length under an unbounded parent.
            if (m_isHorizontal)
                MeasuredSize = Float2{constraints.ConstrainWidth(constraints.BoundedMaxWidth(100.0f)),
                                      constraints.ConstrainHeight(BarThickness)};
            else
                MeasuredSize = Float2{constraints.ConstrainWidth(BarThickness),
                                      constraints.ConstrainHeight(constraints.BoundedMaxHeight(100.0f))};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const ControlState state = GetControlState();
            Drawable* trackDrawable =
                ResolvePartDrawable(u8"track", StyleProperty::Background, state);
            Drawable* thumbDrawable =
                ResolvePartDrawable(u8"thumb", StyleProperty::Background, state);
            const Rectangle bounds{0, 0, Width(), Height()};

            if (trackDrawable != nullptr)
            {
                trackDrawable->Draw(ctx, bounds);
            }
            else
            {
                ctx.VG().FillRect(
                    bounds, Color{40.0f / 255.0f, 42.0f / 255.0f, 50.0f / 255.0f, 150.0f / 255.0f});
            }

            const Rectangle thumbRect = GetThumbRect();
            if (thumbDrawable != nullptr)
            {
                thumbDrawable->Draw(ctx, thumbRect);
            }
            else
            {
                ctx.VG().FillRect(thumbRect, Color{100.0f / 255.0f, 110.0f / 255.0f,
                                                   130.0f / 255.0f, 200.0f / 255.0f});
            }
        }

    private:
        [[nodiscard]] f32 ThumbRatio() const
        {
            return core::Clamp(m_viewportSize / (m_maxValue + m_viewportSize), 0.05f, 1.0f);
        }
        [[nodiscard]] f32 NormalizedValue() const
        {
            return (m_maxValue > 0) ? m_value / m_maxValue : 0.0f;
        }

        f32 m_value = 0.0f;
        f32 m_maxValue = 100.0f;
        f32 m_viewportSize = 50.0f;
        bool m_isHorizontal = false;
        bool m_dragging = false;
        f32 m_dragStartValue = 0.0f;
        f32 m_dragStartMouse = 0.0f;
    };

    RTTI_DEFINE_OBJECT(ScrollBar, "rtti::ui")
}
