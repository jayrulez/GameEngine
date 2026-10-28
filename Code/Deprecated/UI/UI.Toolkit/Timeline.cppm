// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::UI.Toolkit - the `foundation.ui.toolkit:timeline` partition.
//
// Timeline: a time RULER + draggable PLAYHEAD scrubber over a SECONDS axis.
// Domain-agnostic - it knows a duration, a
// playhead time, and the shared time<->pixel transform (pixelsPerSecond / scrollSeconds /
// labelColumnWidth); the ruler itself knows NOTHING about clips or keyframes (lanes + keys +
// selection layer on as this widget grows, so consumers stay decoupled and it is headlessly
// testable). SECONDS are the only currency; the frame display is a label format only. Scrubbing
// fires OnPlayheadMoved. All chrome resolves from the theme with fallbacks; no hand-picked hex in
// draw code.

module;
#include <cmath>
#include <cstdio>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:timeline;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    namespace core = foundation::core;
    namespace fonts = foundation::fonts;

    // One dopesheet row: a fixed height + the key TIMES on it (seconds, host-sorted). The widget draws
    // a marker per key and handles selection/drag; the host owns what a key MEANS (which track/channel).
    struct DopesheetLane
    {
        f32 height = 22.0f;
        String label; // shown in the left gutter (LabelColumnWidth > 0); e.g. "Transform.position"
        Array<f32> keyTimes;
    };

    // A key address within the lane model: (lane, index-in-lane). Selection + the drag event speak
    // these; the host maps them back to clip tracks/keys (no id on the key - Fable D3 ruling).
    struct DopesheetKeyRef
    {
        u32 lane = 0;
        u32 index = 0;
    };

    class Timeline : public View
    {
        RTTI_OBJECT(Timeline, View)
    public:
        // The clip length (seconds): the ruler extent + the playhead clamp bound.
        [[nodiscard]] f32 Duration() const noexcept { return m_duration; }
        void SetDuration(f32 seconds)
        {
            m_duration = Max(seconds, 0.0f);
            SetPlayheadTime(m_playhead); // re-clamp to the new length
            m_scrollSeconds = ClampScroll(m_scrollSeconds); // a shorter clip cannot strand the view
            Invalidate();
        }

        // Playhead time (seconds), clamped to [0, Duration]. Fires OnPlayheadMoved iff it changed.
        [[nodiscard]] f32 PlayheadTime() const noexcept { return m_playhead; }
        void SetPlayheadTime(f32 seconds)
        {
            const f32 clamped = Clamp(seconds, 0.0f, Max(m_duration, 0.0f));
            if (clamped == m_playhead)
            {
                return;
            }
            m_playhead = clamped;
            OnPlayheadMoved.Invoke(m_playhead);
            Invalidate();
        }

        // The shared time<->pixel transform. Lanes + the curve view READ these; none caches
        // its own copy (a per-row cache here would be a bug).
        [[nodiscard]] f32 PixelsPerSecond() const noexcept { return m_pixelsPerSecond; }
        void SetPixelsPerSecond(f32 pps)
        {
            m_pixelsPerSecond = Clamp(pps, kMinPps, kMaxPps);
            m_scrollSeconds = ClampScroll(m_scrollSeconds); // zooming out shrinks the max scroll
            OnViewChanged.Invoke(); // a consumer (the curve canvas) shares this transform (D1)
            Invalidate();
        }
        [[nodiscard]] f32 ScrollSeconds() const noexcept { return m_scrollSeconds; }
        void SetScrollSeconds(f32 seconds)
        {
            m_scrollSeconds = ClampScroll(seconds);
            OnViewChanged.Invoke();
            Invalidate();
        }

        // Seconds visible in the ruler area at the current zoom (0 before layout).
        [[nodiscard]] f32 VisibleSeconds() const noexcept
        {
            const f32 span = Width() - LabelColumnWidth;
            return (span > 0.0f && m_pixelsPerSecond > 0.0f) ? span / m_pixelsPerSecond : 0.0f;
        }

        f32 LabelColumnWidth = 0.0f; // left gutter reserved for track labels (0 when unused)
        i32 DisplayFps = 0;          // 0 = seconds labels; >0 = frame-number labels (display only, A3)

        [[nodiscard]] f32 TimeToX(f32 t) const noexcept
        {
            return LabelColumnWidth + (t - m_scrollSeconds) * m_pixelsPerSecond;
        }
        [[nodiscard]] f32 XToTime(f32 x) const noexcept
        {
            return (m_pixelsPerSecond > 0.0f)
                       ? m_scrollSeconds + (x - LabelColumnWidth) / m_pixelsPerSecond
                       : m_scrollSeconds;
        }

        // Pick the ruler label step (seconds) from the {1,2,5}x10^n sequence: the smallest such step
        // whose on-screen width is at least `minLabelPx`, so labels never collide at any zoom (Godot's
        // ruler algorithm). Pure + headlessly testable (A8).
        [[nodiscard]] static f32 PickTickStep(f32 pixelsPerSecond, f32 minLabelPx) noexcept
        {
            if (pixelsPerSecond <= 0.0f || minLabelPx <= 0.0f)
            {
                return 1.0f;
            }
            const f32 minStep = minLabelPx / pixelsPerSecond; // min seconds between labels
            const f32 mag = std::pow(10.0f, std::floor(std::log10(minStep)));
            const f32 candidates[] = {1.0f * mag, 2.0f * mag, 5.0f * mag, 10.0f * mag};
            for (const f32 c : candidates)
            {
                if (c >= minStep)
                {
                    return c;
                }
            }
            return 10.0f * mag;
        }

        Event<void(f32)> OnPlayheadMoved;

        // === dopesheet lanes ===
        // The host sets the lane model (rebuilt from the clip); the widget renders keys + handles
        // selection + drag. It NEVER mutates the model - a key drag is purely visual and, on release,
        // emits OnKeysMoved(deltaSeconds) for the host to apply + re-sort + re-select (D4/D5).
        void SetLanes(Array<DopesheetLane> lanes)
        {
            m_lanes = Move(lanes);
            m_selection.Clear();
            if (m_selectedLane >= static_cast<i32>(m_lanes.Size()))
            {
                m_selectedLane = -1; // fewer lanes than before: the pick no longer exists
            }
            Invalidate();
        }

        // The SELECTED LANE (track): set by a label-column click or a key pick; -1 = none. The
        // Sedulous shape - the dopesheet IS the track list, and the host shows the selected
        // track's strip/canvas from this.
        [[nodiscard]] i32 SelectedLane() const noexcept { return m_selectedLane; }
        /// Programmatic set (host-driven): clamps, no OnLaneSelected (no feedback loops).
        void SetSelectedLane(i32 lane)
        {
            m_selectedLane =
                (lane >= 0 && lane < static_cast<i32>(m_lanes.Size())) ? lane : -1;
            Invalidate();
        }
        [[nodiscard]] usize LaneCount() const noexcept { return m_lanes.Size(); }
        [[nodiscard]] usize SelectedCount() const noexcept { return m_selection.Size(); }
        [[nodiscard]] bool IsKeySelected(u32 lane, u32 index) const noexcept
        {
            return SelectionContains(Pack(lane, index));
        }
        void ClearSelection()
        {
            if (!m_selection.IsEmpty())
            {
                m_selection.Clear();
                OnSelectionChanged.Invoke();
                Invalidate();
            }
        }
        // Replace the selection wholesale (the host re-selects by time after a commit-remap - D3/D4).
        void SetSelection(const Array<DopesheetKeyRef>& keys)
        {
            m_selection.Clear();
            for (const DopesheetKeyRef& k : keys)
            {
                m_selection.PushBack(Pack(k.lane, k.index));
            }
            Invalidate();
        }
        [[nodiscard]] Array<DopesheetKeyRef> Selection() const
        {
            Array<DopesheetKeyRef> out;
            for (const u64 packed : m_selection)
            {
                out.PushBack(DopesheetKeyRef{static_cast<u32>(packed >> 32),
                                             static_cast<u32>(packed & 0xffffffffu)});
            }
            return out;
        }

        Event<void()> OnSelectionChanged;
        Event<void(f32)> OnKeysMoved; // a key drag committed: move all selected keys by this delta (s)
        Event<void(i32)> OnLaneSelected; // the USER picked a lane (label click or key pick); -1 never fires
        Event<void()> OnViewChanged; // the time transform (zoom/scroll) changed - shared consumers re-sync (D1)

        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(200.0f),
                                  constraints.ConstrainHeight(kRulerHeight + LanesHeight())};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 w = Width();
            const f32 h = Height();
            // Everything clips to the widget: tick labels near the right edge and long lane
            // labels must never paint outside the timeline (UAT: label overflow).
            ctx.VG().PushClipRect(Rectangle{0.0f, 0.0f, w, h});
            // Resolve the ruler band ONCE; the gutter is derived from it (F2 - a single themed
            // Background rule, not a second token with its own hand-picked fallback).
            const core::Color band =
                ResolveStyleColor(StyleProperty::Background, core::Color::Rgb(24, 25, 30, 255));
            ctx.VG().FillRect(Rectangle{0.0f, 0.0f, w, h}, band);

            const core::Color tickColor =
                ResolveStyleColor(StyleProperty::BorderColor, core::Color::Rgb(62, 64, 74, 255));
            const core::Color labelColor =
                ResolveStyleColor(StyleProperty::TextDimColor, core::Color::Rgb(150, 152, 162, 255));
            fonts::CachedFont* font =
                (ctx.FontService() != nullptr) ? ctx.FontService()->GetFont(9.0f) : nullptr;

            const f32 step = PickTickStep(m_pixelsPerSecond, kMinLabelPx);
            const f32 startTime = Max(XToTime(Max(LabelColumnWidth, 0.0f)), 0.0f);
            const f32 endTime = XToTime(w);
            // Index-based ticks: accumulating `t += step` drifts in f32 and the drift prints
            // ("0.0005322" instead of "0.0005"); i * step stays exact per tick.
            const i64 firstTick = static_cast<i64>(std::floor(startTime / step));
            const i64 lastTick = static_cast<i64>(std::ceil(endTime / step));
            for (i64 tick = firstTick; tick <= lastTick; ++tick)
            {
                const f32 t = static_cast<f32>(tick) * step;
                if (t < -1e-4f)
                {
                    continue;
                }
                const f32 x = TimeToX(t);
                if (x >= LabelColumnWidth && x <= w)
                {
                    ctx.VG().FillRect(Rectangle{x, h * 0.45f, 1.0f, h * 0.55f}, tickColor);
                    if (font != nullptr)
                    {
                        const String label = FormatTime(t);
                        ctx.VG().DrawText(label, font, Rectangle{x + 3.0f, 1.0f, 46.0f, 12.0f},
                                          fonts::TextAlignment::Left, fonts::VerticalAlignment::Top,
                                          labelColor);
                    }
                }
                // A minor tick halfway to the next label.
                const f32 xm = TimeToX(t + step * 0.5f);
                if (xm >= LabelColumnWidth && xm <= w)
                {
                    ctx.VG().FillRect(Rectangle{xm, h * 0.72f, 1.0f, h * 0.28f}, tickColor);
                }
            }

            // Left label gutter: the band darkened, so a themed Background keeps the gutter distinct
            // without a second token (F2).
            if (LabelColumnWidth > 0.0f)
            {
                const core::Color gutter{band.r * 0.82f, band.g * 0.82f, band.b * 0.82f, band.a};
                ctx.VG().FillRect(Rectangle{0.0f, 0.0f, LabelColumnWidth, h}, gutter);
                // Track labels, one per lane (the dopesheet IS the track list - Sedulous
                // shape). Larger than the ruler digits (9pt read as fine print) and clipped
                // to the gutter so a long path never bleeds into the key grid.
                const core::Color laneLabelSel = ResolveStyleColor(
                    StyleProperty::AccentColor, core::Color::Rgb(90, 150, 235, 255));
                fonts::CachedFont* laneFont =
                    (ctx.FontService() != nullptr) ? ctx.FontService()->GetFont(12.0f) : nullptr;
                ctx.VG().PushClipRect(Rectangle{0.0f, 0.0f, LabelColumnWidth, h});
                f32 gy = kRulerHeight;
                for (usize li = 0; li < m_lanes.Size(); ++li)
                {
                    const DopesheetLane& lane = m_lanes[li];
                    if (static_cast<i32>(li) == m_selectedLane)
                    {
                        ctx.VG().FillRect(
                            Rectangle{0.0f, gy, LabelColumnWidth, lane.height},
                            core::Color{laneLabelSel.r, laneLabelSel.g, laneLabelSel.b, 0.28f});
                    }
                    if (laneFont != nullptr && !lane.label.IsEmpty())
                    {
                        ctx.VG().DrawText(lane.label, laneFont,
                                          Rectangle{6.0f, gy, LabelColumnWidth - 10.0f,
                                                    lane.height},
                                          fonts::TextAlignment::Left,
                                          fonts::VerticalAlignment::Middle, labelColor);
                    }
                    gy += lane.height;
                }
                ctx.VG().PopClip();
            }

            // Dopesheet lanes below the ruler: an alternating row per lane + a marker per key. Selected
            // keys use the accent; a live key-drag offsets the selected markers visually (D4).
            const core::Color rowEven{band.r * 1.12f, band.g * 1.12f, band.b * 1.12f, band.a};
            const core::Color rowOdd{band.r * 1.24f, band.g * 1.24f, band.b * 1.24f, band.a};
            const core::Color keyColor =
                ResolveStyleColor(StyleProperty::TextDimColor, core::Color::Rgb(170, 174, 186, 255));
            const core::Color keySel =
                ResolveStyleColor(StyleProperty::AccentColor, core::Color::Rgb(90, 150, 235, 255));
            const core::Color laneSel =
                ResolveStyleColor(StyleProperty::AccentColor, core::Color::Rgb(90, 150, 235, 255));
            f32 laneY = kRulerHeight;
            for (usize li = 0; li < m_lanes.Size(); ++li)
            {
                const DopesheetLane& lane = m_lanes[li];
                const bool laneIsSelected = (static_cast<i32>(li) == m_selectedLane);
                ctx.VG().FillRect(Rectangle{LabelColumnWidth, laneY, w - LabelColumnWidth, lane.height},
                                  laneIsSelected
                                      ? core::Color{laneSel.r, laneSel.g, laneSel.b, 0.18f}
                                      : ((li & 1u) ? rowOdd : rowEven));
                const f32 cy = laneY + lane.height * 0.5f;
                for (usize ki = 0; ki < lane.keyTimes.Size(); ++ki)
                {
                    const bool sel = SelectionContains(Pack(static_cast<u32>(li), static_cast<u32>(ki)));
                    f32 kx = TimeToX(lane.keyTimes[ki]);
                    if (sel && m_keyDragging)
                    {
                        kx += m_dragDeltaX;
                    }
                    if (kx < LabelColumnWidth - kKeyRadius || kx > w + kKeyRadius)
                    {
                        continue;
                    }
                    ctx.VG().FillCircle(Float2{kx, cy}, kKeyRadius, sel ? keySel : keyColor);
                }
                laneY += lane.height;
            }

            // Playhead: a vertical line + a small head flag at the top (theme error-red). Snap x to a
            // whole pixel + draw a fixed 2px width so a fractional scrub/advance stays crisp and does
            // not shimmer between 1 and 2 px each frame (a sub-pixel anti-aliasing artifact).
            const f32 px = TimeToX(m_playhead);
            if (px >= LabelColumnWidth - 0.5f && px <= w)
            {
                const f32 lx = std::round(px);
                const core::Color playhead =
                    ResolveStyleColor(StyleProperty::ErrorColor, core::Color::Rgb(232, 84, 84, 255));
                ctx.VG().FillRect(Rectangle{lx - 1.0f, 0.0f, 2.0f, h}, playhead);
                ctx.VG().FillRect(Rectangle{lx - 4.0f, 0.0f, 8.0f, 5.0f}, playhead);
            }

            // Box-select overlay (drawn last, over the lanes + playhead).
            if (m_boxSelecting)
            {
                const f32 bx = Min(m_boxStart.x, m_boxEnd.x);
                const f32 by = Min(m_boxStart.y, m_boxEnd.y);
                const f32 bw = Abs(m_boxEnd.x - m_boxStart.x);
                const f32 bh = Abs(m_boxEnd.y - m_boxStart.y);
                const core::Color accent =
                    ResolveStyleColor(StyleProperty::AccentColor, core::Color::Rgb(90, 150, 235, 255));
                ctx.VG().FillRect(Rectangle{bx, by, bw, bh}, core::Color{accent.r, accent.g, accent.b, 0.18f});
            }
            ctx.VG().PopClip();
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            // Middle button pans (the DCC standard - drag left/right to reach any section).
            if (e.Button == MouseButton::Middle)
            {
                m_panning = true;
                m_panStartX = e.X;
                m_panStartScroll = m_scrollSeconds;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
                return;
            }
            if (e.Button != MouseButton::Left)
            {
                return;
            }
            // Label-gutter click: select that lane (track). The gutter above the lanes (the
            // ruler-height corner) stays inert.
            if (e.X < LabelColumnWidth)
            {
                if (e.Y > kRulerHeight)
                {
                    f32 laneY = kRulerHeight;
                    for (usize li = 0; li < m_lanes.Size(); ++li)
                    {
                        if (e.Y >= laneY && e.Y < laneY + m_lanes[li].height)
                        {
                            SelectLaneFromUser(static_cast<i32>(li));
                            e.Handled = true;
                            return;
                        }
                        laneY += m_lanes[li].height;
                    }
                }
                return;
            }
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetCapture(this);
            }
            e.Handled = true;

            // The ruler band (or a lane-less timeline) scrubs the playhead.
            if (e.Y <= kRulerHeight || m_lanes.IsEmpty())
            {
                m_dragging = true;
                SetPlayheadTime(XToTime(e.X));
                return;
            }

            // Lane area: hit a key -> select + begin a (visual) drag; empty -> box-select.
            const bool ctrl = HasFlag(e.Modifiers, KeyModifiers::Ctrl);
            u32 lane = 0, index = 0;
            if (HitTestKey(e.X, e.Y, lane, index))
            {
                SelectLaneFromUser(static_cast<i32>(lane)); // a key pick selects its track too
                const u64 key = Pack(lane, index);
                if (ctrl)
                {
                    ToggleSelection(key);
                    OnSelectionChanged.Invoke();
                }
                else if (!SelectionContains(key))
                {
                    m_selection.Clear();
                    m_selection.PushBack(key);
                    OnSelectionChanged.Invoke();
                }
                m_keyDragging = true;
                m_dragStartX = e.X;
                m_dragDeltaX = 0.0f;
            }
            else if (Abs(e.X - TimeToX(m_playhead)) <= kPlayheadGrabPx)
            {
                // Clicked ON the playhead line (down in the lanes): grab + scrub it, not box-select.
                m_dragging = true;
                SetPlayheadTime(XToTime(e.X));
            }
            else
            {
                if (!ctrl && !m_selection.IsEmpty())
                {
                    m_selection.Clear();
                    OnSelectionChanged.Invoke();
                }
                m_boxSelecting = true;
                m_boxStart = Float2{e.X, e.Y};
                m_boxEnd = m_boxStart;
            }
            Invalidate();
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_panning)
            {
                if (m_pixelsPerSecond > 0.0f)
                {
                    SetScrollSeconds(m_panStartScroll - (e.X - m_panStartX) / m_pixelsPerSecond);
                }
                e.Handled = true;
                Invalidate();
                return;
            }
            if (m_dragging)
            {
                SetPlayheadTime(XToTime(e.X));
                e.Handled = true;
            }
            else if (m_keyDragging)
            {
                m_dragDeltaX = e.X - m_dragStartX;
                e.Handled = true;
                Invalidate();
            }
            else if (m_boxSelecting)
            {
                m_boxEnd = Float2{e.X, e.Y};
                e.Handled = true;
                Invalidate();
            }
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Middle && m_panning)
            {
                m_panning = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
                return;
            }
            if (e.Button != MouseButton::Left)
            {
                return;
            }
            const bool wasActive = m_dragging || m_keyDragging || m_boxSelecting;
            if (m_keyDragging)
            {
                const f32 dx = m_dragDeltaX;
                m_keyDragging = false;
                m_dragDeltaX = 0.0f;
                // Epsilon gate: a small move is a click (select only), not a drag (D4).
                if (Abs(dx) >= kDragEpsilonPx && m_pixelsPerSecond > 0.0f)
                {
                    OnKeysMoved.Invoke(dx / m_pixelsPerSecond); // host applies + re-sorts + re-selects
                }
                Invalidate();
            }
            else if (m_boxSelecting)
            {
                m_boxSelecting = false;
                ApplyBoxSelection();
                OnSelectionChanged.Invoke();
                Invalidate();
            }
            m_dragging = false;
            if (wasActive && Context != nullptr)
            {
                Context->GetFocusManager()->ReleaseCapture();
            }
            e.Handled = true;
        }

        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            // Horizontal wheel (trackpads) or Shift+wheel PANS; plain vertical wheel zooms.
            const bool shift = HasFlag(e.Modifiers, KeyModifiers::Shift);
            const f32 panDelta = (e.DeltaX != 0.0f) ? e.DeltaX : (shift ? e.DeltaY : 0.0f);
            if (panDelta != 0.0f)
            {
                if (m_pixelsPerSecond > 0.0f)
                {
                    SetScrollSeconds(m_scrollSeconds - panDelta * kWheelPanPx / m_pixelsPerSecond);
                }
                e.Handled = true;
                Invalidate();
                return;
            }
            if (e.DeltaY == 0.0f)
            {
                return;
            }
            // Zoom anchored at the cursor: keep the time under the cursor fixed (Godot).
            const f32 timeAtCursor = XToTime(e.X);
            SetPixelsPerSecond(m_pixelsPerSecond * (e.DeltaY > 0.0f ? kZoomStep : 1.0f / kZoomStep));
            SetScrollSeconds(timeAtCursor - (e.X - LabelColumnWidth) / m_pixelsPerSecond);
            e.Handled = true;
            Invalidate();
        }

    private:
        void SelectLaneFromUser(i32 lane)
        {
            if (lane == m_selectedLane)
            {
                return;
            }
            m_selectedLane = lane;
            OnLaneSelected.Invoke(lane);
            Invalidate();
        }

        // Clamp a scroll offset to the content extent: the view never scrolls before 0, and
        // never past the point where the clip END sits at the right edge (with a small tail so
        // end keys stay grabbable). When the whole clip fits (or before layout), scroll is 0 -
        // zooming out cannot push both ends away from the content.
        [[nodiscard]] f32 ClampScroll(f32 seconds) const noexcept
        {
            const f32 visible = VisibleSeconds();
            if (visible <= 0.0f)
            {
                return Max(seconds, 0.0f); // pre-layout: only the lower bound is known
            }
            const f32 tail = visible * kEndTailFraction;
            const f32 maxScroll = Max(m_duration + tail - visible, 0.0f);
            return Clamp(seconds, 0.0f, maxScroll);
        }

        [[nodiscard]] String FormatTime(f32 t) const
        {
            String s;
            if (DisplayFps > 0)
            {
                AppendValue(s, static_cast<i32>(std::lround(t * static_cast<f32>(DisplayFps))));
                s += u8"f";
            }
            else
            {
                // 4 significant digits: every {1,2,5}x10^n tick step prints exactly, and f32
                // noise ("0.0005322") never reaches the ruler.
                char buffer[32];
                const int n = std::snprintf(buffer, sizeof(buffer), "%.4g",
                                            static_cast<double>(t));
                if (n > 0)
                {
                    s += StringView(reinterpret_cast<const utf8char*>(buffer),
                                    static_cast<usize>(n));
                }
                s += u8"s";
            }
            return s;
        }

        // --- dopesheet helpers ---
        [[nodiscard]] static u64 Pack(u32 lane, u32 index) noexcept
        {
            return (static_cast<u64>(lane) << 32) | static_cast<u64>(index);
        }
        [[nodiscard]] bool SelectionContains(u64 key) const noexcept
        {
            for (const u64 k : m_selection)
            {
                if (k == key)
                {
                    return true;
                }
            }
            return false;
        }
        void ToggleSelection(u64 key)
        {
            for (usize i = 0; i < m_selection.Size(); ++i)
            {
                if (m_selection[i] == key)
                {
                    m_selection.RemoveAt(i);
                    return;
                }
            }
            m_selection.PushBack(key);
        }
        [[nodiscard]] f32 LanesHeight() const noexcept
        {
            f32 total = 0.0f;
            for (const DopesheetLane& lane : m_lanes)
            {
                total += lane.height;
            }
            return total;
        }
        // Which lane row contains y, and the nearest key on it within the hit radius (in x).
        [[nodiscard]] bool HitTestKey(f32 x, f32 y, u32& outLane, u32& outIndex) const
        {
            f32 laneY = kRulerHeight;
            for (usize li = 0; li < m_lanes.Size(); ++li)
            {
                const DopesheetLane& lane = m_lanes[li];
                if (y >= laneY && y < laneY + lane.height)
                {
                    f32 best = kKeyHitPx;
                    bool found = false;
                    for (usize ki = 0; ki < lane.keyTimes.Size(); ++ki)
                    {
                        const f32 dx = Abs(TimeToX(lane.keyTimes[ki]) - x);
                        if (dx <= best)
                        {
                            best = dx;
                            outLane = static_cast<u32>(li);
                            outIndex = static_cast<u32>(ki);
                            found = true;
                        }
                    }
                    return found;
                }
                laneY += lane.height;
            }
            return false;
        }
        // Add every key whose marker falls inside the current box to the selection.
        void ApplyBoxSelection()
        {
            const f32 bx = Min(m_boxStart.x, m_boxEnd.x);
            const f32 by = Min(m_boxStart.y, m_boxEnd.y);
            const f32 ex = Max(m_boxStart.x, m_boxEnd.x);
            const f32 ey = Max(m_boxStart.y, m_boxEnd.y);
            f32 laneY = kRulerHeight;
            for (usize li = 0; li < m_lanes.Size(); ++li)
            {
                const DopesheetLane& lane = m_lanes[li];
                const f32 cy = laneY + lane.height * 0.5f;
                if (cy >= by && cy <= ey)
                {
                    for (usize ki = 0; ki < lane.keyTimes.Size(); ++ki)
                    {
                        const f32 kx = TimeToX(lane.keyTimes[ki]);
                        if (kx >= bx && kx <= ex)
                        {
                            const u64 key = Pack(static_cast<u32>(li), static_cast<u32>(ki));
                            if (!SelectionContains(key))
                            {
                                m_selection.PushBack(key);
                            }
                        }
                    }
                }
                laneY += lane.height;
            }
        }

        static constexpr f32 kRulerHeight = 24.0f;
        static constexpr f32 kMinPps = 4.0f;
        static constexpr f32 kMaxPps = 4000.0f;
        static constexpr f32 kMinLabelPx = 48.0f;
        static constexpr f32 kZoomStep = 1.15f;
        static constexpr f32 kKeyRadius = 4.0f;
        static constexpr f32 kKeyHitPx = 6.0f;
        static constexpr f32 kDragEpsilonPx = 3.0f;
        static constexpr f32 kPlayheadGrabPx = 5.0f;
        static constexpr f32 kWheelPanPx = 40.0f;      // px scrolled per wheel notch
        static constexpr f32 kEndTailFraction = 0.15f; // visible-width tail past the clip end

        f32 m_duration = 1.0f;
        f32 m_playhead = 0.0f;
        f32 m_pixelsPerSecond = 100.0f;
        f32 m_scrollSeconds = 0.0f;
        bool m_dragging = false;
        bool m_panning = false;
        f32 m_panStartX = 0.0f;
        f32 m_panStartScroll = 0.0f;

        // Dopesheet state.
        Array<DopesheetLane> m_lanes;
        Array<u64> m_selection; // packed (lane<<32 | index)
        bool m_keyDragging = false;
        f32 m_dragStartX = 0.0f;
        f32 m_dragDeltaX = 0.0f;
        bool m_boxSelecting = false;
        Float2 m_boxStart{};
        Float2 m_boxEnd{};
        i32 m_selectedLane = -1;
    };

    RTTI_DEFINE_OBJECT(Timeline, "rtti::ui::toolkit")
}
