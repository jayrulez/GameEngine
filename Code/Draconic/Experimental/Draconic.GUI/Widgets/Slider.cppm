// Draconic GUI - :slider partition
//
// Slider: a draggable value in [0,1]. A lean Draconic-native control modeled on eepp's
// UISlider (role only). Pressing/dragging sets the value from the cursor x; because the
// EventDispatcher captures the pointer to the pressed node, the drag keeps tracking even
// when the cursor leaves the slider. OnDraw renders a track, a fill up to the handle, and a
// round handle. Horizontal only (vertical is a follow-up).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:slider;

import draconic.foundation; // Color, Function, Move, Max, Min, Float2, Rectangle
import draconic.vg;   // CornerRadii
import :rect;
import :event;
import :draw_context;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class Slider : public UIWidget
    {
        DRACONIC_OBJECT(Slider, UIWidget)
    public:
        Slider()
        {
            SetTag(foundation::StringView(u8"slider"));
            SetTabFocusable(true);
        }

        [[nodiscard]] f32 GetValue() const noexcept { return m_value; }
        void SetValue(f32 value)
        {
            value = foundation::Max(0.0f, foundation::Min(1.0f, value));
            if (value == m_value)
                return;
            m_value = value;
            Invalidate();
            if (m_onChanged)
                m_onChanged(m_value);
        }

        void SetOnValueChanged(foundation::Function<void(f32)> callback)
        {
            m_onChanged = foundation::Move(callback);
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
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"track"));
            out.PushBack(foundation::StringView(u8"fill"));
            out.PushBack(foundation::StringView(u8"thumb"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"track"))
                SetTrackColor(color);
            else if (part == foundation::StringView(u8"fill"))
                SetFillColor(color);
            else if (part == foundation::StringView(u8"thumb"))
                SetHandleColor(color);
        }

    protected:
        // Drag state is tracked with our own m_dragging flag rather than IsPressed(): a
        // captured drag that leaves the slider fires OnMouseLeave (which clears m_pressed)
        // BEFORE the routed OnMouseMove, so gating the move on IsPressed() would stop the
        // drag the instant the cursor left. Pointer capture guarantees the release reaches
        // us, so OnMouseUp reliably clears m_dragging.
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

            ctx.VG().FillRoundedRect(foundation::Rectangle{b.x, trackTop, b.width, trackH},
                                     vg::CornerRadii(trackH * 0.5f), m_trackColor);
            ctx.VG().FillRoundedRect(foundation::Rectangle{b.x, trackTop, handleX - b.x, trackH},
                                     vg::CornerRadii(trackH * 0.5f), m_fillColor);
            ctx.VG().FillCircle(foundation::Float2{handleX, cy}, handleR, m_handleColor);
        }

    private:
        void UpdateFromEvent(const MouseEvent& event)
        {
            const Rect b = GetContentBounds();
            const f32 handleR = b.height * 0.5f;
            const foundation::Float2 local = ConvertToNodeSpace(event.Position);
            const f32 x0 = b.x + handleR;
            const f32 x1 = b.x + b.width - handleR;
            SetValue((x1 > x0) ? (local.x - x0) / (x1 - x0) : 0.0f);
        }

        f32 m_value = 0.0f;
        bool m_dragging = false;
        Color m_trackColor{0.28f, 0.30f, 0.35f, 1.0f};
        Color m_fillColor{0.31f, 0.63f, 0.85f, 1.0f};
        Color m_handleColor{0.86f, 0.89f, 0.93f, 1.0f};
        foundation::Function<void(f32)> m_onChanged;
    };

    DRACONIC_DEFINE_OBJECT(Slider, "draconic::gui")
}
