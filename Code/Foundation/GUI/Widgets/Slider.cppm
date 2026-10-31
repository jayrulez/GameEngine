// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :slider partition
//
// Slider: a draggable value over a range, horizontal or vertical (a vertical slider's value grows
// upward). It is a RangeWidget, so min, max, step, page, exponential and the rest come from there.
// Pressing or dragging sets the value from the pointer; the dispatcher's pointer capture keeps a
// drag going when the pointer leaves the slider. A drag sends `input` as it goes and `change` on
// release. The arrow keys along the slider step it, and the slider keeps handling them at its
// ends, so holding the d-pad to reach an end does not move focus away. Page Up and Page Down step
// by a page, Home and End go to the ends, and the wheel steps the value while the slider is
// hovered or focused, unless turned off (in a scrolling list the wheel should scroll the list).
// Escape during a drag restores the value from before the drag and reports the drag cancelled.
// Ticks, when enabled, mark the track at even steps.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:slider;

import foundation.core; // Color, Max, Min, Float2, Rectangle, String
import foundation.vg;   // CornerRadii
import :rect;
import :event;
import :draw_context;
import :ui_widget;
import :layout_style; // Orientation
import :range_value;
import :range_widget;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    class Slider : public RangeWidget
    {
        RTTI_OBJECT(Slider, RangeWidget)
    public:
        Slider()
        {
            SetTag(core::StringView(u8"slider"));
            SetTabFocusable(true);
        }

        /// Sets whether the wheel steps the value (the default). When off, the wheel event passes on
        /// to the slider's ancestors.
        void SetStepsOnWheel(bool steps) noexcept { m_stepsOnWheel = steps; }
        [[nodiscard]] bool StepsOnWheel() const noexcept { return m_stepsOnWheel; }
        /// Sets the callback for a drag cancelled with Escape, after the value is restored.
        void SetOnValueCancelled(core::Function<void()> callback) { m_onCancelled = core::Move(callback); }
        [[nodiscard]] bool IsDragging() const noexcept { return m_dragging; }

        /// Returns the page key step; 0 means a tenth of the range.
        [[nodiscard]] f64 GetPage() const noexcept { return GetPageSize(); }
        void SetPage(f64 page) { SetPageSize(page); }

        void SetOrientation(Orientation orientation)
        {
            if (orientation == m_orientation)
                return;
            m_orientation = orientation;
            RequestLayout();
            Invalidate();
        }
        [[nodiscard]] Orientation GetOrientation() const noexcept { return m_orientation; }
        /// Sets the orientation from "horizontal" or "vertical" (the `orientation` property of
        /// markup and style sheets).
        void SetOrientationName(const core::String& name)
        {
            SetOrientation(name.AsView() == core::StringView(u8"vertical") ? Orientation::Vertical : Orientation::Horizontal);
        }
        [[nodiscard]] core::String GetOrientationName() const
        {
            return core::String(m_orientation == Orientation::Vertical ? core::StringView(u8"vertical") : core::StringView(u8"horizontal"));
        }
        /// Sets how many tick marks the track shows, ends included; 0 or 1 means none.
        void SetTicks(i32 ticks)
        {
            m_ticks = core::Max(0, ticks);
            Invalidate();
        }
        [[nodiscard]] i32 GetTicks() const noexcept { return m_ticks; }

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
        /// Sets the colour of the ring and halo around the thumb while it has keyboard or pad focus.
        void SetFocusColor(Color color)
        {
            m_focusColor = color;
            Invalidate();
        }

        // Style parts: slider::track, ::fill, ::thumb, ::focus (the thumb's ring) and ::tick.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"track"));
            out.PushBack(core::StringView(u8"fill"));
            out.PushBack(core::StringView(u8"thumb"));
            out.PushBack(core::StringView(u8"focus"));
            out.PushBack(core::StringView(u8"tick"));
        }

        /// Returns the track's length and the thumb's size when nothing else sizes the slider.
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            return m_orientation == Orientation::Vertical ? core::Float2{24.0f, 160.0f} : core::Float2{160.0f, 24.0f};
        }

    protected:
        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Modifiers != 0)
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (m_dragging)
            {
                if (key == KeyCode::Escape)
                {
                    m_dragging = false;
                    SetValue(m_dragStart); // restore the value; there is nothing to commit
                    if (m_onCancelled)
                        m_onCancelled();
                    event.Handled = true;
                }
                return; // other keys do nothing during a drag
            }
            const bool vertical = m_orientation == Orientation::Vertical;
            const f64 step = GetRange().KeyStep();
            const f64 v = GetRange().Value;
            // A horizontal slider runs from its inline start, so in right to left Left raises it.
            const KeyCode lower = Reversed() ? KeyCode::Right : KeyCode::Left;
            const KeyCode raise = Reversed() ? KeyCode::Left : KeyCode::Right;
            if ((!vertical && key == lower) || (vertical && key == KeyCode::Down))
                ChangeByUser(v - step, true);
            else if ((!vertical && key == raise) || (vertical && key == KeyCode::Up))
                ChangeByUser(v + step, true);
            else if (key == KeyCode::PageDown)
                ChangeByUser(v - GetRange().PageStep(), true);
            else if (key == KeyCode::PageUp)
                ChangeByUser(v + GetRange().PageStep(), true);
            else if (key == KeyCode::Home)
                ChangeByUser(GetRange().Min, true);
            else if (key == KeyCode::End)
                ChangeByUser(GetRange().Upper(), true);
            else
                return;
            event.Handled = true;
        }
        // The wheel steps the value while the slider is hovered or focused. At an end the wheel
        // event is left unhandled, so it passes on to the slider's ancestors.
        void OnMouseWheel(const WheelEvent& event) override
        {
            const f32 notches = event.Delta.y != 0.0f ? event.Delta.y : event.Delta.x;
            if (notches == 0.0f || !m_stepsOnWheel)
                return;
            const f64 before = GetRange().Value;
            ChangeByUser(before + GetRange().KeyStep() * static_cast<f64>(notches), true);
            event.Handled = GetRange().Value != before;
        }

        // The slider keeps its presses: a drag is never offered to the nodes that hold it.
        [[nodiscard]] bool OwnsPressDrag(core::Float2) const override { return true; }
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button != MouseButton::Left)
                return;
            m_dragging = true;
            m_dragStart = GetValue();
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
            if (m_dragging)
                Commit();
            m_dragging = false;
        }
        void OnPointerCancel(const MouseEvent&) override
        {
            if (m_dragging)
                Commit();
            m_dragging = false;
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            const bool vertical = m_orientation == Orientation::Vertical;
            const f32 handleR = HandleRadius(b);
            // The track's thickness comes from the sheet (slider::track { height }, or its width for a
            // vertical slider), or defaults to 4.
            const PartStyle& trackStyle = GetPartStyle(u8"track");
            const bool sized = vertical ? trackStyle.HasWidth : trackStyle.HasHeight;
            const f32 trackW = sized ? (vertical ? trackStyle.Width : trackStyle.Height) : 4.0f;
            const f32 ratio = static_cast<f32>(GetRatio());
            const f32 lo = (vertical ? b.y : b.x) + handleR;
            const f32 hi = (vertical ? b.y + b.height : b.x + b.width) - handleR;
            // The value runs along the track from its start (left, or right in right to left), or
            // from bottom to top.
            const bool reversed = Reversed();
            const f32 along = vertical || reversed ? hi - (hi - lo) * ratio : lo + (hi - lo) * ratio;
            const f32 mid = vertical ? b.x + b.width * 0.5f : b.y + b.height * 0.5f;
            const core::Float2 handle = vertical ? core::Float2{mid, along} : core::Float2{along, mid};
            const vg::CornerRadii round(trackW * 0.5f);

            // Each part has its own default look, which the sheet's part style overrides
            // (slider::track, ::fill, ::tick, ::thumb, ::focus).
            const Rect track = vertical ? Rect{mid - trackW * 0.5f, b.y, trackW, b.height} : Rect{b.x, mid - trackW * 0.5f, b.width, trackW};
            trackStyle.Draw(ctx, track, FilledBox(m_trackColor, round));
            const Rect fill = vertical   ? Rect{track.x, along, trackW, b.y + b.height - along}
                              : reversed ? Rect{along, track.y, b.x + b.width - along, trackW}
                                         : Rect{b.x, track.y, along - b.x, trackW};
            GetPartStyle(u8"fill").Draw(ctx, fill, FilledBox(m_fillColor, round));
            if (m_ticks >= 2)
            {
                const PartStyle& tick = GetPartStyle(u8"tick");
                for (i32 i = 0; i < m_ticks; ++i)
                {
                    const f32 t = static_cast<f32>(i) / static_cast<f32>(m_ticks - 1);
                    const f32 at = vertical ? hi - (hi - lo) * t : lo + (hi - lo) * t;
                    const Rect mark = vertical ? Rect{mid + trackW, at - 0.5f, 4.0f, 1.0f} : Rect{at - 0.5f, mid + trackW, 1.0f, 4.0f};
                    tick.Draw(ctx, mark, FilledBox(m_trackColor));
                }
            }
            // With keyboard or pad focus, the thumb (the part that moves) is marked with a ring and a
            // glow, rather than a ring around the whole track.
            if (IsFocusVisible())
            {
                // The glow takes the ring's colour: the sheet's (slider::focus { border-color }) or
                // the default.
                const PartStyle& focus = GetPartStyle(u8"focus");
                const Color ringColor = focus.HasBorderColor || focus.HasBorderWidth ? focus.Box.BorderColors.top : m_focusColor;
                const f32 ringR = handleR + 3.0f;
                NodeBox ring = OutlinedBox(ringColor, 2.0f, vg::CornerRadii(ringR));
                ring.Shadows.PushBack(BoxShadow{core::Float2{0.0f, 0.0f}, 6.0f, 1.0f,
                                                Color{ringColor.r, ringColor.g, ringColor.b, ringColor.a * 0.45f}, false});
                focus.Draw(ctx, Rect{handle.x - ringR, handle.y - ringR, ringR * 2.0f, ringR * 2.0f}, ring);
            }
            GetPartStyle(u8"thumb").Draw(ctx, Rect{handle.x - handleR, handle.y - handleR, handleR * 2.0f, handleR * 2.0f},
                                         FilledBox(m_handleColor, vg::CornerRadii(handleR)));
        }

    private:
        /// A horizontal slider in right to left runs from the right.
        [[nodiscard]] bool Reversed() const { return m_orientation == Orientation::Horizontal && IsRightToLeft(); }

        /// Returns the thumb's radius. Its diameter is the sheet's size (slider::thumb { width }), or
        /// the slider's thickness when the sheet sets none, and never more than that thickness.
        [[nodiscard]] f32 HandleRadius(const Rect& b) const
        {
            const bool vertical = m_orientation == Orientation::Vertical;
            const f32 across = vertical ? b.width : b.height;
            const PartStyle& thumb = GetPartStyle(u8"thumb");
            const f32 sized = thumb.HasWidth ? thumb.Width : thumb.HasHeight ? thumb.Height : across;
            return Min(sized, across) * 0.5f;
        }

        void UpdateFromEvent(const MouseEvent& event)
        {
            const Rect b = GetContentBounds();
            const bool vertical = m_orientation == Orientation::Vertical;
            const f32 handleR = HandleRadius(b);
            const core::Float2 local = ConvertToNodeSpace(event.Position);
            const f32 lo = (vertical ? b.y : b.x) + handleR;
            const f32 hi = (vertical ? b.y + b.height : b.x + b.width) - handleR;
            const f32 at = vertical ? local.y : local.x;
            f32 ratio = hi > lo ? (at - lo) / (hi - lo) : 0.0f;
            if (vertical || Reversed())
                ratio = 1.0f - ratio;
            ChangeByUser(GetRange().FromRatio(static_cast<f64>(ratio)), false);
        }

        Orientation m_orientation = Orientation::Horizontal;
        i32 m_ticks = 0;
        bool m_dragging = false;
        f64 m_dragStart = 0.0; // the value before the drag, which Escape restores
        bool m_stepsOnWheel = true;
        core::Function<void()> m_onCancelled;
        Color m_trackColor{0.28f, 0.30f, 0.35f, 1.0f};
        Color m_fillColor{0.31f, 0.63f, 0.85f, 1.0f};
        Color m_handleColor{0.86f, 0.89f, 0.93f, 1.0f};
        Color m_focusColor{0.31f, 0.61f, 1.0f, 1.0f};
    };
}
