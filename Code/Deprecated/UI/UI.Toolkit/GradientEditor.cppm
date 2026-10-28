// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :gradient_editor partition
//
// Interactive color-ramp editor. Stops along a normalized [0,1] time axis carry RGBA (HDR-allowed Float4);
// the widget renders a live linearly-interpolated gradient strip and lets the user add / move / delete /
// re-color stops via direct manipulation. Ported from Sedulous.UI.Toolkit/src/Controls/GradientEditor.bf
// (a View).
//
// Beef `List<Stop>` -> Array<Stop>; `Span<Stop>` -> Span<const Stop>; nested `struct Stop` -> a public
// nested value struct; byte `Color(r,g,b,a)` -> private static Rgb(); float `Color(r,g,b,a)` -> Color{...}.
// The immediate path API (BeginPath/MoveTo/LineTo/ClosePath/Fill/Stroke) ports 1:1 to foundation.vg.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:gradient_editor;

import foundation.core;
import foundation.vg;
import foundation.ui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui::toolkit
{
    /// Interactive color-ramp editor with direct-manipulation stops.
    class GradientEditor : public View
    {
        RTTI_OBJECT(GradientEditor, View)
    public:
        /// One stop in the gradient. Time in [0,1]; Color in HDR-allowed Float4 (R,G,B,A).
        struct Stop
        {
            f32 Time = 0.0f;
            Float4 Color{0, 0, 0, 0};

            Stop() = default;
            Stop(f32 time, Float4 color) : Time(time), Color(color) {}
        };

        /// Cap on the number of stops. Defaults to the particle limit.
        i32 MaxStops = 8;

        /// Color used to fill the gradient strip when there are no stops.
        core::Color EmptyFill = Rgb(40, 40, 46, 255);

        Event<void()> OnEditBegin;
        Event<void()> OnEditEnd;
        Event<void(i32)> OnStopAdded;
        Event<void(i32)> OnStopChanged;
        Event<void(i32)> OnStopRemoved;
        Event<void(i32)> OnStopColorRequested;

        [[nodiscard]] i32 StopCount() const { return static_cast<i32>(m_stops.Size()); }
        [[nodiscard]] i32 SelectedIndex() const { return m_selectedIdx; }
        [[nodiscard]] Stop GetStop(i32 i) const { return m_stops[static_cast<usize>(i)]; }

        /// Replace all stops. Does not fire events.
        void SetStops(Span<const Stop> stops)
        {
            m_stops.Clear();
            for (usize i = 0; i < stops.Size(); ++i)
            {
                m_stops.PushBack(stops[i]);
            }
            m_selectedIdx = -1;
            m_draggingIdx = -1;
            Invalidate();
        }

        /// Update a single stop's color (typically from the color-picker callback). Fires OnStopChanged.
        void UpdateStopColor(i32 idx, Float4 color)
        {
            if (idx < 0 || static_cast<usize>(idx) >= m_stops.Size())
            {
                return;
            }
            m_stops[static_cast<usize>(idx)].Color = color;
            OnStopChanged.Invoke(idx);
            Invalidate();
        }

        // === Mouse ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left)
            {
                const i32 markerHit = MarkerAt(e.X, e.Y);
                if (markerHit >= 0)
                {
                    m_selectedIdx = markerHit;
                    if (e.ClickCount >= 2)
                    {
                        OnStopColorRequested.Invoke(markerHit);
                        e.Handled = true;
                        Invalidate();
                        return;
                    }
                    m_draggingIdx = markerHit;
                    BeginGesture();
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->SetCapture(this);
                    }
                    e.Handled = true;
                    Invalidate();
                    return;
                }

                // Click on gradient strip - add a stop at the clicked time; initial color sampled from the
                // existing gradient so the new stop "blends in".
                if (IsOverStrip(e.Y) && static_cast<i32>(m_stops.Size()) < MaxStops)
                {
                    const f32 t = XToTime(e.X);
                    const Float4 initialColor = m_stops.Size() > 0 ? Sample(t) : Float4{1, 1, 1, 1};
                    BeginGesture();
                    const i32 idx = InsertSorted(Stop{t, initialColor});
                    m_selectedIdx = idx;
                    m_draggingIdx = idx;
                    OnStopAdded.Invoke(idx);
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->SetCapture(this);
                    }
                    e.Handled = true;
                    Invalidate();
                }
            }
            else if (e.Button == MouseButton::Right)
            {
                const i32 markerHit = MarkerAt(e.X, e.Y);
                if (markerHit >= 0)
                {
                    const i32 oldIdx = markerHit;
                    BeginGesture();
                    m_stops.RemoveAt(static_cast<usize>(markerHit));
                    if (m_selectedIdx == markerHit)
                    {
                        m_selectedIdx = -1;
                    }
                    else if (m_selectedIdx > markerHit)
                    {
                        m_selectedIdx--;
                    }
                    OnStopRemoved.Invoke(oldIdx);
                    EndGesture();
                    e.Handled = true;
                    Invalidate();
                }
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_draggingIdx < 0 || static_cast<usize>(m_draggingIdx) >= m_stops.Size())
            {
                return;
            }
            const f32 newTime = XToTime(e.X);

            // Re-sort by time; track moved index.
            Stop moved = m_stops[static_cast<usize>(m_draggingIdx)];
            moved.Time = newTime;
            m_stops.RemoveAt(static_cast<usize>(m_draggingIdx));
            const i32 newIdx = InsertSorted(moved);
            m_draggingIdx = newIdx;
            m_selectedIdx = newIdx;

            OnStopChanged.Invoke(newIdx);
            e.Handled = true;
            Invalidate();
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (m_draggingIdx >= 0)
            {
                m_draggingIdx = -1;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                EndGesture();
                e.Handled = true;
            }
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            // Outer background from the theme (background-color = raw-color resolve).
            const core::Color canvasBg =
                ResolveStyleColor(StyleProperty::Background, Rgb(28, 28, 33, 255));
            ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, canvasBg);

            const f32 stripH = StripBottom();
            if (m_stops.Size() == 0)
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), stripH}, EmptyFill);
            }
            else
            {
                // Sample the gradient at 1-px columns.
                const i32 cols = static_cast<i32>(core::Max(Width(), 1.0f));
                for (i32 i = 0; i < cols; ++i)
                {
                    const f32 t = i / static_cast<f32>(cols - 1);
                    const core::Color c = Vector4ToColor(Sample(t));
                    ctx.VG().FillRect(Rectangle{static_cast<f32>(i), 0, 1, stripH}, c);
                }
            }

            // Strip border.
            const core::Color stripBorder =
                ResolveStyleColor(StyleProperty::BorderColor, Rgb(60, 60, 68, 255));
            ctx.VG().FillRect(Rectangle{0, 0, Width(), 1}, stripBorder);
            ctx.VG().FillRect(Rectangle{0, stripH - 1, Width(), 1}, stripBorder);

            // Marker strip background - a step up from the canvas.
            ctx.VG().FillRect(Rectangle{0, stripH, Width(), kMarkerStripHeight},
                              Palette::Lighten(canvasBg, 0.05f));

            // Markers - triangle pointing up.
            for (i32 i = 0; i < static_cast<i32>(m_stops.Size()); ++i)
            {
                const f32 mx = TimeToX(m_stops[static_cast<usize>(i)].Time);
                const bool isSel = (i == m_selectedIdx);
                const core::Color body = Vector4ToColor(m_stops[static_cast<usize>(i)].Color);
                const core::Color stroke =
                    isSel ? Rgb(255, 220, 100, 255)
                          : ResolveStyleColor(StyleProperty::TextDimColor, Rgb(200, 200, 210, 255));

                // Filled triangle.
                ctx.VG().BeginPath();
                ctx.VG().MoveTo(mx, stripH + 2);
                ctx.VG().LineTo(mx - kMarkerHalfWidth, stripH + 2 + kMarkerHeight);
                ctx.VG().LineTo(mx + kMarkerHalfWidth, stripH + 2 + kMarkerHeight);
                ctx.VG().ClosePath();
                ctx.VG().Fill(body);

                // Outline.
                ctx.VG().BeginPath();
                ctx.VG().MoveTo(mx, stripH + 2);
                ctx.VG().LineTo(mx - kMarkerHalfWidth, stripH + 2 + kMarkerHeight);
                ctx.VG().LineTo(mx + kMarkerHalfWidth, stripH + 2 + kMarkerHeight);
                ctx.VG().ClosePath();
                ctx.VG().Stroke(stroke, isSel ? 2.0f : 1.0f);
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(200.0f), constraints.ConstrainHeight(60.0f)};
        }

    private:
        [[nodiscard]] static core::Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return core::Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] static core::Color Vector4ToColor(Float4 c)
        {
            return core::Color{core::Clamp(c.x, 0.0f, 1.0f), core::Clamp(c.y, 0.0f, 1.0f),
                               core::Clamp(c.z, 0.0f, 1.0f), core::Clamp(c.w, 0.0f, 1.0f)};
        }

        // === Sampling (linear interp between stops) ===

        [[nodiscard]] Float4 Sample(f32 t) const
        {
            const usize n = m_stops.Size();
            if (n == 0)
            {
                return Float4{0, 0, 0, 0};
            }
            if (n == 1)
            {
                return m_stops[0].Color;
            }
            if (t <= m_stops[0].Time)
            {
                return m_stops[0].Color;
            }
            if (t >= m_stops[n - 1].Time)
            {
                return m_stops[n - 1].Color;
            }

            for (usize i = 0; i < n - 1; ++i)
            {
                const Stop& a = m_stops[i];
                const Stop& b = m_stops[i + 1];
                if (t >= a.Time && t <= b.Time)
                {
                    const f32 seg = b.Time - a.Time;
                    if (seg < 0.0001f)
                    {
                        return a.Color;
                    }
                    const f32 lt = (t - a.Time) / seg;
                    return Float4{a.Color.x + (b.Color.x - a.Color.x) * lt,
                                  a.Color.y + (b.Color.y - a.Color.y) * lt,
                                  a.Color.z + (b.Color.z - a.Color.z) * lt,
                                  a.Color.w + (b.Color.w - a.Color.w) * lt};
                }
            }
            return m_stops[n - 1].Color;
        }

        // === Coordinate / layout ===

        static constexpr f32 kMarkerStripHeight = 18.0f;
        static constexpr f32 kMarkerHalfWidth = 6.0f;
        static constexpr f32 kMarkerHeight = 10.0f;

        [[nodiscard]] f32 StripBottom() const { return Height() - kMarkerStripHeight; }
        [[nodiscard]] f32 TimeToX(f32 t) const { return t * Width(); }
        [[nodiscard]] f32 XToTime(f32 x) const { return core::Clamp(x / Width(), 0.0f, 1.0f); }

        [[nodiscard]] bool IsOverStrip(f32 y) const { return y < StripBottom(); }
        [[nodiscard]] bool IsOverMarkers(f32 y) const { return y >= StripBottom(); }

        [[nodiscard]] i32 MarkerAt(f32 x, f32 y) const
        {
            if (!IsOverMarkers(y))
            {
                return -1;
            }
            for (i32 i = 0; i < static_cast<i32>(m_stops.Size()); ++i)
            {
                const f32 mx = TimeToX(m_stops[static_cast<usize>(i)].Time);
                if (Abs(x - mx) <= kMarkerHalfWidth + 2)
                {
                    return i;
                }
            }
            return -1;
        }

        void BeginGesture()
        {
            if (!m_inGesture)
            {
                m_inGesture = true;
                OnEditBegin.Invoke();
            }
        }
        void EndGesture()
        {
            if (m_inGesture)
            {
                m_inGesture = false;
                OnEditEnd.Invoke();
            }
        }

        i32 InsertSorted(Stop s)
        {
            i32 idx = static_cast<i32>(m_stops.Size());
            for (i32 i = 0; i < static_cast<i32>(m_stops.Size()); ++i)
            {
                if (m_stops[static_cast<usize>(i)].Time > s.Time)
                {
                    idx = i;
                    break;
                }
            }
            m_stops.Insert(static_cast<usize>(idx), s);
            return idx;
        }

        Array<Stop> m_stops;
        i32 m_selectedIdx = -1;
        i32 m_draggingIdx = -1;
        bool m_inGesture = false;
    };

    RTTI_DEFINE_OBJECT(GradientEditor, "rtti::ui::toolkit")
}
