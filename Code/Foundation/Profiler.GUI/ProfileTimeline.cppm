// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :timeline partition
//
// The frames chosen on one time axis (X7 of Specs/editor-gui.md): a lane per thread with its
// scopes stacked by depth, the GPU's passes in a lane of their own drawn as the scopes are; the
// ruler (the time from the first frame's start) and the frames' row pinned on top; the lanes'
// names in a column at the start, as wide as the frame strip's gutter. A scope keeps its name's
// colour in every lane. Scopes too narrow to draw are kept, side by side as one hatched block
// with their count, never dropped.
//
// Mouse first: the wheel zooms about the pointer (Shift and the wheel scroll the lanes), a drag
// pans (a middle drag too), a double click zooms to what is under the pointer (a scope; nothing:
// the frames again; a lane's name: the lane's span), Fit in the corner fits the frames. A click on
// a scope chooses its name (on nothing, none): its every instance lit and the rest dimmed, the
// page's other views told. A drag on the ruler marks a span (its length read out), a click there
// lets it go. A click on a lane's arrow folds it to one row. The hover outlines the name's other
// instances and names the scope with its duration.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.profiler.gui:timeline;

import foundation.core;
import foundation.vg;
import foundation.gui;
import foundation.profiler;
import :focus;
import :widget;

using namespace foundation::core;
using namespace foundation::gui;

export namespace foundation::profiler::gui
{
    /// ProfileTimeline::kRowHeight, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr f32 kProfileTimelineRowHeight = 18.0f;

    class ProfileTimeline final : public ProfilerWidget
    {
        RTTI_OBJECT(ProfileTimeline, ProfilerWidget)
    public:
        static constexpr f32 kHeaderWidth = 128.0f; // the lanes' names; the frame strip's gutter
        static constexpr f32 kTickHeight = 20.0f;   // the ruler's ticks and times
        static constexpr f32 kFramesHeight = 16.0f; // the frames' row under it
        static constexpr f32 kRulerHeight = kTickHeight + kFramesHeight;
        static constexpr f32 kRowHeight = kProfileTimelineRowHeight; // a depth of scopes
        static constexpr f32 kLaneGap = 6.0f;
        static constexpr f32 kMinBarWidth = 3.0f; // narrower scopes join a hatched block
        static constexpr f32 kFoldWidth = 18.0f;  // a lane's arrow, at its name's start
        static constexpr f32 kMinLabelSpacing = 72.0f;
        static constexpr f32 kDragThreshold = 3.0f;
        static constexpr u32 kGpuLane = ~0u; // the GPU's lane, among the threads' indices

        struct Lane
        {
            String name;
            bool gpu = false;
            u32 thread = 0; // the thread's index; kGpuLane for the GPU's
            u32 depths = 1;
            bool folded = false;
            f32 y = 0.0f; // its top in the view, after the lanes' scroll
            f32 height = 0.0f;
        };

        struct Bar
        {
            u32 lane = 0;
            u32 depth = 0;
            NameId name = 0;
            bool gpu = false;
            u64 beginTick = 0;
            u64 endTick = 0;
            u32 merged = 0; // 0: one scope; else the scopes a hatched block holds
            Rect rect{};
        };

        ProfileTimeline()
        {
            SetTag(StringView(u8"profiletimeline"));
            SetTabFocusable(false);
        }

        // === The view ===
        /// The time shown at the plot's start and end, in the source's ticks.
        [[nodiscard]] f64 ViewBegin() const noexcept { return m_viewBegin; }
        [[nodiscard]] f64 ViewEnd() const noexcept { return m_viewEnd; }
        /// The view back to the frames chosen: the first's start to the last's end or its last GPU
        /// pass's, whichever is later.
        void FitView()
        {
            m_scrollY = 0.0f;
            u64 begin = 0;
            u64 end = 0;
            if (FramesSpan(begin, end))
                ZoomTo(static_cast<f64>(begin), static_cast<f64>(end));
            else
            {
                m_viewBegin = 0.0;
                m_viewEnd = 1.0;
                Invalidate();
            }
        }
        /// Shows [begin, end] (ticks), with a sliver of room either side.
        void ZoomTo(f64 begin, f64 end)
        {
            const f64 span = Max(end - begin, MinSpan());
            const f64 room = span * 0.01;
            m_viewBegin = begin - room;
            m_viewEnd = begin + span + room;
            Invalidate();
        }
        /// Zooms by `factor` (above 1 in) about `x` (its own space): the time under it stays. No
        /// closer than a microsecond across the plot.
        void ZoomAt(f32 x, f32 factor)
        {
            if (factor <= 0.0f)
                return;
            const f64 anchor = TickAt(x);
            const f64 span = Max((m_viewEnd - m_viewBegin) / static_cast<f64>(factor), MinSpan());
            const Rect plot = PlotBounds();
            const f64 along = static_cast<f64>((x - plot.x) / Max(plot.width, 1.0f));
            m_viewBegin = anchor - along * span;
            m_viewEnd = m_viewBegin + span;
            Invalidate();
        }
        /// Moves the view by `dx` pixels (positive shows earlier time) and the lanes by `dy`.
        void PanBy(f32 dx, f32 dy)
        {
            const f64 shift = static_cast<f64>(dx / Max(PlotBounds().width, 1.0f)) * (m_viewEnd - m_viewBegin);
            m_viewBegin -= shift;
            m_viewEnd -= shift;
            ScrollLanes(-dy);
            Invalidate();
        }
        /// Scrolls the lanes by `dy` pixels (positive shows later lanes), kept over them.
        void ScrollLanes(f32 dy)
        {
            m_scrollY = Clamp(m_scrollY + dy, 0.0f, Max(LanesHeight() - (PlotBounds().height - kRulerHeight), 0.0f));
            Invalidate();
        }
        [[nodiscard]] f32 LaneScroll() const noexcept { return m_scrollY; }

        /// The plot (its own space): the content less the names' column.
        [[nodiscard]] Rect PlotBounds() const
        {
            const Rect box = GetContentBounds();
            const f32 header = Min(kHeaderWidth, box.width);
            return Rect{box.x + header, box.y, Max(box.width - header, 0.0f), box.height};
        }
        /// The time at `x` (its own space) and where a time is drawn.
        [[nodiscard]] f64 TickAt(f32 x) const noexcept
        {
            const Rect plot = PlotBounds();
            return m_viewBegin + static_cast<f64>((x - plot.x) / Max(plot.width, 1.0f)) * (m_viewEnd - m_viewBegin);
        }
        [[nodiscard]] f32 XAt(f64 tick) const noexcept
        {
            const Rect plot = PlotBounds();
            return plot.x + static_cast<f32>((tick - m_viewBegin) / (m_viewEnd - m_viewBegin)) * plot.width;
        }

        // === Lanes ===
        /// Folds a lane to one row (its top scopes as plain activity), or opens it.
        void SetFolded(u32 thread, bool folded)
        {
            for (usize i = 0; i < m_folded.Size(); ++i)
                if (m_folded[i] == thread)
                {
                    if (!folded)
                        m_folded.RemoveAt(i);
                    m_dirty = true;
                    Invalidate();
                    return;
                }
            if (folded)
                m_folded.PushBack(thread);
            m_dirty = true;
            Invalidate();
        }
        [[nodiscard]] bool IsFolded(u32 thread) const noexcept
        {
            for (const u32 at : m_folded)
                if (at == thread)
                    return true;
            return false;
        }

        // === The model (the draw and the tests read it) ===
        [[nodiscard]] Span<const Lane> Lanes() const
        {
            Compute();
            return m_lanes.AsSpan();
        }
        [[nodiscard]] Span<const Bar> Bars() const
        {
            Compute();
            return m_bars.AsSpan();
        }
        /// The bar at a point (its own space), or null.
        [[nodiscard]] const Bar* BarAt(core::Float2 point) const
        {
            Compute();
            if (point.y < PlotBounds().y + kRulerHeight)
                return nullptr;
            for (usize i = m_bars.Size(); i > 0; --i)
            {
                const Rect& r = m_bars[i - 1].rect;
                if (point.x >= r.x - 1.0f && point.x <= r.x + Max(r.width, 1.0f) + 1.0f && point.y >= r.y && point.y < r.y + r.height)
                    return &m_bars[i - 1];
            }
            return nullptr;
        }
        /// The lane whose name is at a point (its own space), or -1.
        [[nodiscard]] i32 LaneAt(core::Float2 point) const
        {
            Compute();
            for (usize i = 0; i < m_lanes.Size(); ++i)
                if (point.y >= m_lanes[i].y && point.y < m_lanes[i].y + m_lanes[i].height + kLaneGap)
                    return static_cast<i32>(i);
            return -1;
        }
        /// The scope under the pointer (null: none).
        [[nodiscard]] const Bar* Hovered() const
        {
            Compute();
            return m_hovered >= 0 && static_cast<usize>(m_hovered) < m_bars.Size() ? &m_bars[static_cast<usize>(m_hovered)] : nullptr;
        }
        /// Whether a bar is the name chosen's.
        [[nodiscard]] bool IsChosen(const Bar& bar) const noexcept
        {
            return bar.merged == 0 && m_focus && m_focus->Name().Is(bar.name, bar.gpu);
        }

        /// The scope under the pointer named with its duration, else its own tooltip.
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            m_tip.Clear();
            const IProfileSource* source = Source();
            if (const Bar* bar = Hovered(); bar != nullptr && source != nullptr)
            {
                const f32 ms = TicksToMs(bar->endTick - bar->beginTick, source->TickFrequency());
                if (bar->merged > 0)
                    m_tip = Format(u8"{} scopes too short to draw, {} ms in all: zoom in", bar->merged, FormatFixed(ms, 3).AsView());
                else
                    m_tip = Format(u8"{}  {} ms", source->NameOf(bar->name), FormatFixed(ms, 3).AsView());
                return m_tip.AsView();
            }
            return UIWidget::GetTooltipText();
        }

        /// The parts it draws, which the sheet styles as `tag::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            ProfilerWidget::CollectStyleParts(out);
            out.PushBack(StringView(u8"separator"));
            out.PushBack(StringView(u8"frame-edge"));
            out.PushBack(StringView(u8"ruler"));
            out.PushBack(StringView(u8"tick"));
            out.PushBack(StringView(u8"frame"));
            out.PushBack(StringView(u8"header"));
            out.PushBack(StringView(u8"fit"));
            out.PushBack(StringView(u8"fold"));
            out.PushBack(StringView(u8"lane-name"));
            out.PushBack(StringView(u8"gpu-name"));
            out.PushBack(StringView(u8"bar"));
            out.PushBack(StringView(u8"hover"));
            out.PushBack(StringView(u8"chosen"));
            out.PushBack(StringView(u8"hatch"));
            out.PushBack(StringView(u8"activity"));
            out.PushBack(StringView(u8"span"));
            out.PushBack(StringView(u8"span-edge"));
            out.PushBack(StringView(u8"span-head"));
        }

        void OnProfileSourceChanged() override
        {
            m_dirty = true;
            Invalidate();
        }
        void OnProfileFramesChanged() override
        {
            m_dirty = true;
            FitView();
        }
        void OnProfileChoiceChanged() override { Invalidate(); }

    protected:
        void OnMouseDown(const MouseEvent& event) override
        {
            const core::Float2 at = Local(event.Position);
            const Rect plot = PlotBounds();
            if (event.Button == MouseButton::Middle)
            {
                event.Handled = true;
                BeginDrag(Drag::Pan, at);
                return;
            }
            if (event.Button != MouseButton::Left)
                return;
            event.Handled = true;
            if (at.x < plot.x)
            {
                // The corner's Fit, a lane's arrow, a lane's name.
                if (at.y < plot.y + kRulerHeight)
                {
                    FitView();
                    return;
                }
                const i32 lane = LaneAt(at);
                if (lane < 0)
                    return;
                const Lane& shown = m_lanes[static_cast<usize>(lane)];
                if (event.ClickCount == 2)
                    ZoomToLane(shown);
                else if (at.x < plot.x - kHeaderWidth + kFoldWidth)
                    SetFolded(shown.thread, !shown.folded);
                return;
            }
            if (at.y < plot.y + kRulerHeight)
            {
                BeginDrag(Drag::PendingSpan, at);
                return;
            }
            if (event.ClickCount == 2)
            {
                if (const Bar* bar = BarAt(at))
                    ZoomTo(static_cast<f64>(bar->beginTick), static_cast<f64>(bar->endTick));
                else
                    FitView();
                return;
            }
            BeginDrag(Drag::PendingPan, at);
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            const core::Float2 at = Local(event.Position);
            m_pointer = at;
            m_pointerIn = true;
            const bool past = Abs(at.x - m_pressAt.x) >= kDragThreshold || Abs(at.y - m_pressAt.y) >= kDragThreshold;
            if (m_drag == Drag::PendingPan && past)
                m_drag = Drag::Pan;
            if (m_drag == Drag::PendingSpan && past)
                m_drag = Drag::Span;
            if (m_drag == Drag::Pan)
            {
                PanBy(at.x - m_lastAt.x, at.y - m_lastAt.y);
                m_lastAt = at;
            }
            else if (m_drag == Drag::Span && m_focus)
            {
                // To the nearest tick on either side.
                const f64 from = Max(TickAt(m_pressAt.x), 0.0) + 0.5;
                const f64 to = Max(TickAt(at.x), 0.0) + 0.5;
                m_focus->SetSpan(static_cast<u64>(from), static_cast<u64>(to));
            }
            Hover();
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            if (event.Button == MouseButton::Left)
            {
                if (m_drag == Drag::PendingPan && m_focus)
                {
                    // A click: the name of the scope under it chosen, or none.
                    const Bar* bar = BarAt(m_pressAt);
                    if (bar != nullptr && bar->merged == 0)
                        m_focus->ChooseName(bar->name, bar->gpu);
                    else if (bar == nullptr)
                        m_focus->ClearName();
                }
                else if (m_drag == Drag::PendingSpan && m_focus)
                    m_focus->ClearSpan();
            }
            if (event.Button == MouseButton::Left || (event.Button == MouseButton::Middle && m_drag == Drag::Pan))
                m_drag = Drag::None;
        }
        void OnPointerCancel(const MouseEvent&) override { m_drag = Drag::None; }
        void OnMouseLeave(const MouseEvent& event) override
        {
            UIWidget::OnMouseLeave(event);
            m_pointerIn = false;
            Hover();
        }
        void OnMouseWheel(const WheelEvent& event) override
        {
            event.Handled = true;
            if ((event.Modifiers & KeyModShift) != 0)
            {
                ScrollLanes(-(event.Delta.y != 0.0f ? event.Delta.y : event.Delta.x) * kRowHeight * 3.0f);
                return;
            }
            if (event.Delta.y != 0.0f)
                ZoomAt(Local(event.Position).x, Pow(1.25f, event.Delta.y));
        }
        [[nodiscard]] Cursor GetCursorNow() const override { return m_drag == Drag::Pan ? Cursor::Grabbing : UIWidget::GetCursorNow(); }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            UIWidget::OnDraw(ctx, localBounds);
            Compute();
            const Rect box = GetContentBounds();
            const Rect plot = PlotBounds();
            const IProfileSource* source = Source();
            const Color separator = PartColor(u8"separator", Color{0.5f, 0.5f, 0.5f, 0.25f});
            const Color edgeColor = PartColor(u8"frame-edge", Color{0.6f, 0.6f, 0.6f, 0.8f});
            const Color dimText = PartTextColor(u8"ruler");
            const Color barText = PartTextColor(u8"bar");
            const Color hoverEdge = PartColor(u8"hover", Color{1.0f, 1.0f, 1.0f, 0.7f});
            const Color chosenEdge = PartColor(u8"chosen", Color{1.0f, 1.0f, 1.0f, 1.0f});
            const Color hatch = PartColor(u8"hatch", Color{0.55f, 0.57f, 0.62f, 0.35f});
            const Color activity = PartColor(u8"activity", Color{0.44f, 0.53f, 0.72f, 0.4f});
            const bool choosing = m_focus && m_focus->Name().any;
            const Bar* hovered = Hovered();

            // The lanes: their separators, then their scopes.
            const Rect lanesClip{plot.x, plot.y + kRulerHeight, plot.width, Max(plot.height - kRulerHeight, 0.0f)};
            ctx.PushClip(lanesClip);
            for (const Lane& lane : m_lanes)
                ctx.VG().FillRect(Rectangle{plot.x, lane.y + lane.height + kLaneGap * 0.5f, plot.width, 1.0f}, separator);
            for (const Bar& bar : m_bars)
            {
                const Lane& lane = m_lanes[bar.lane];
                if (bar.merged > 0)
                {
                    DrawHatch(ctx, bar.rect, hatch);
                    if (bar.rect.width > 28.0f)
                        DrawLabel(ctx, Format(u8"×{}", bar.merged).AsView(), bar.rect, barText, TextHAlign::Center);
                    continue;
                }
                const StringView name = source != nullptr ? source->NameOf(bar.name) : StringView();
                Color color = lane.folded ? activity : NameColor(name);
                if (choosing && !IsChosen(bar))
                    color.a *= 0.45f;
                ctx.VG().FillRect(bar.rect.ToRectangle(), color);
                if (IsChosen(bar))
                    ctx.VG().StrokeRect(bar.rect.ToRectangle(), chosenEdge, 1.0f);
                else if (hovered != nullptr && hovered->merged == 0 && hovered->name == bar.name && hovered->gpu == bar.gpu)
                    ctx.VG().StrokeRect(bar.rect.ToRectangle(), hoverEdge, 1.0f);
                if (!lane.folded && bar.rect.width > 24.0f)
                {
                    const f32 ms = TicksToMs(bar.endTick - bar.beginTick, source->TickFrequency());
                    const String withTime = Format(u8"{}  {} ms", name, FormatFixed(ms, 2).AsView());
                    const bool roomy = LabelWidth(withTime.AsView()) + 8.0f < bar.rect.width;
                    Color text = barText;
                    if (choosing && !IsChosen(bar))
                        text.a *= 0.6f;
                    DrawLabel(ctx, roomy ? withTime.AsView() : name, Rect{bar.rect.x + 4.0f, bar.rect.y, bar.rect.width - 8.0f, bar.rect.height},
                              text, TextHAlign::Center);
                }
            }
            ctx.PopClip();

            // The frames' edges, down the ruler's foot and the lanes; the span marked.
            ctx.PushClip(Rect{plot.x, plot.y + kTickHeight, plot.width, Max(plot.height - kTickHeight, 0.0f)});
            for (const FrameEdge& edge : m_edges)
                ctx.VG().FillRect(Rectangle{Floor(edge.x), plot.y + kTickHeight, 1.0f, plot.height - kTickHeight}, edgeColor);
            ctx.PopClip();
            ctx.PushClip(plot);
            if (m_focus && m_focus->Span().any)
            {
                const MarkedSpan span = m_focus->Span();
                const f32 x0 = XAt(static_cast<f64>(span.beginTick));
                const f32 x1 = XAt(static_cast<f64>(span.endTick));
                ctx.VG().FillRect(Rectangle{x0, plot.y, x1 - x0, plot.height}, PartColor(u8"span", Color{0.18f, 0.43f, 0.87f, 0.12f}));
                const Color spanEdge = PartColor(u8"span-edge", Color{0.18f, 0.43f, 0.87f, 1.0f});
                ctx.VG().FillRect(Rectangle{Floor(x0), plot.y, 1.0f, plot.height}, spanEdge);
                ctx.VG().FillRect(Rectangle{Floor(x1), plot.y, 1.0f, plot.height}, spanEdge);
            }
            ctx.PopClip();

            // The ruler: its ticks and times, the span's length, the frames' numbers.
            ctx.VG().FillRect(Rectangle{plot.x, plot.y, plot.width, kRulerHeight}, PartColor(u8"ruler", Color{0.0f, 0.0f, 0.0f, 0.15f}));
            ctx.PushClip(Rect{plot.x, plot.y, plot.width, kRulerHeight});
            DrawTicks(ctx, plot, dimText);
            if (m_focus && m_focus->Span().any && source != nullptr)
            {
                const MarkedSpan span = m_focus->Span();
                const f32 x0 = XAt(static_cast<f64>(span.beginTick));
                const f32 x1 = XAt(static_cast<f64>(span.endTick));
                ctx.VG().FillRect(Rectangle{x0, plot.y, x1 - x0, kTickHeight}, PartColor(u8"span-head", Color{0.18f, 0.43f, 0.87f, 0.35f}));
                const String length = Format(u8"{} ms", FormatFixed(TicksToMs(span.endTick - span.beginTick, source->TickFrequency()), 2).AsView());
                DrawLabel(ctx, length.AsView(), Rect{x0, plot.y, Max(x1 - x0, LabelWidth(length.AsView()) + 8.0f), kTickHeight},
                          PartTextColor(u8"span-head"), TextHAlign::Center);
            }
            for (const FrameEdge& edge : m_edges)
            {
                const f32 room = edge.next - edge.x;
                if (room < 30.0f || source == nullptr)
                    continue;
                const String label = room > 150.0f ? Format(u8"Frame {}  {} ms", edge.frame, FormatFixed(edge.ms, 2).AsView())
                                                    : Format(u8"{}", edge.frame);
                const f32 x = Max(edge.x, plot.x) + 3.0f;
                DrawLabel(ctx, label.AsView(), Rect{x, plot.y + kTickHeight, Min(edge.next, plot.Right()) - x - 3.0f, kFramesHeight},
                          PartTextColor(u8"frame"));
            }
            ctx.PopClip();

            // The names' column over everything: the corner's Fit, then each lane's arrow and name.
            const Rect header{box.x, box.y, plot.x - box.x, box.height};
            ctx.VG().FillRect(header.ToRectangle(), PartColor(u8"header", Color{0.0f, 0.0f, 0.0f, 0.1f}));
            ctx.VG().FillRect(Rectangle{plot.x - 1.0f, box.y, 1.0f, box.height}, separator);
            DrawLabel(ctx, StringView(u8"Fit"), Rect{header.x, header.y, header.width, kRulerHeight}, PartTextColor(u8"fit"), TextHAlign::Center);
            ctx.VG().FillRect(Rectangle{header.x, plot.y + kRulerHeight - 1.0f, header.width, 1.0f}, separator);
            ctx.PushClip(Rect{header.x, plot.y + kRulerHeight, header.width, Max(header.height - kRulerHeight, 0.0f)});
            const Color fold = PartColor(u8"fold", dimText);
            for (const Lane& lane : m_lanes)
            {
                DrawArrow(ctx, core::Float2{header.x + 9.0f, lane.y + kRowHeight * 0.5f}, lane.folded, fold);
                DrawLabel(ctx, lane.name.AsView(), Rect{header.x + kFoldWidth, lane.y, header.width - kFoldWidth - 4.0f, kRowHeight},
                          PartTextColor(lane.gpu ? StringView(u8"gpu-name") : StringView(u8"lane-name")));
            }
            ctx.PopClip();
        }

    private:
        enum class Drag : u8
        {
            None,
            PendingPan,  // pressed in the lanes, not yet past the threshold: a click
            Pan,
            PendingSpan, // pressed on the ruler: a click lets the span go
            Span,
        };

        struct FrameEdge
        {
            u64 frame = 0;
            f32 x = 0.0f;    // its start
            f32 next = 0.0f; // its end
            f32 ms = 0.0f;
        };

        [[nodiscard]] f64 MinSpan() const
        {
            const IProfileSource* source = Source();
            return Max(source != nullptr ? static_cast<f64>(source->TickFrequency()) / 1.0e6 : 1.0, 1.0e-9);
        }

        // The frames chosen's span in ticks; false when none has a record.
        bool FramesSpan(u64& begin, u64& end) const
        {
            const IProfileSource* source = Source();
            const FrameRange frames = m_focus ? m_focus->Frames() : FrameRange{};
            bool any = false;
            for (u64 n = frames.first; source != nullptr && !frames.empty && n <= frames.last; ++n)
                if (const FrameRecord* frame = source->Frame(n))
                {
                    u64 last = frame->endTick;
                    for (const GpuPassEvent& pass : frame->gpu)
                        last = Max(last, pass.endTick);
                    begin = any ? Min(begin, frame->beginTick) : frame->beginTick;
                    end = any ? Max(end, last) : last;
                    any = true;
                }
            return any;
        }

        void ZoomToLane(const Lane& lane)
        {
            bool any = false;
            f64 begin = 0.0;
            f64 end = 0.0;
            for (const Bar& bar : AllBarsOf(lane))
            {
                begin = any ? Min(begin, static_cast<f64>(bar.beginTick)) : static_cast<f64>(bar.beginTick);
                end = any ? Max(end, static_cast<f64>(bar.endTick)) : static_cast<f64>(bar.endTick);
                any = true;
            }
            if (any)
                ZoomTo(begin, end);
        }
        // Every scope of a lane in the frames chosen, in view or not.
        [[nodiscard]] Array<Bar> AllBarsOf(const Lane& lane) const
        {
            Array<Bar> out;
            const IProfileSource* source = Source();
            const FrameRange frames = m_focus ? m_focus->Frames() : FrameRange{};
            for (u64 n = frames.first; source != nullptr && !frames.empty && n <= frames.last; ++n)
                if (const FrameRecord* frame = source->Frame(n))
                {
                    if (lane.gpu)
                        for (const GpuPassEvent& pass : frame->gpu)
                            out.PushBack(Bar{0, pass.depth, pass.name, true, pass.beginTick, pass.endTick});
                    else
                        for (const ThreadFrame& thread : frame->threads)
                            if (thread.thread == lane.thread)
                                for (const ScopeEvent& scope : thread.scopes)
                                    out.PushBack(Bar{0, scope.depth, scope.name, false, scope.beginTick, scope.endTick});
                }
            return out;
        }

        void BeginDrag(Drag drag, core::Float2 at)
        {
            m_drag = drag;
            m_pressAt = at;
            m_lastAt = at;
        }

        void Hover()
        {
            const Bar* bar = m_pointerIn ? BarAt(m_pointer) : nullptr;
            const i32 hovered = bar != nullptr ? static_cast<i32>(bar - m_bars.Data()) : -1;
            if (hovered != m_hovered)
            {
                m_hovered = hovered;
                Invalidate();
            }
        }

        [[nodiscard]] f32 LanesHeight() const
        {
            Compute();
            f32 height = kLaneGap;
            for (const Lane& lane : m_lanes)
                height += lane.height + kLaneGap;
            return height;
        }

        // The lanes, their scopes in view and the frames' edges, from the frames chosen.
        void Compute() const
        {
            const Rect plot = PlotBounds();
            const Rect bounds = GetContentBounds();
            const bool same = !m_dirty && m_computedBegin == m_viewBegin && m_computedEnd == m_viewEnd && m_computedScroll == m_scrollY &&
                              m_computedWidth == bounds.width && m_computedHeight == bounds.height;
            if (same)
                return;
            m_dirty = false;
            m_computedBegin = m_viewBegin;
            m_computedEnd = m_viewEnd;
            m_computedScroll = m_scrollY;
            m_computedWidth = bounds.width;
            m_computedHeight = bounds.height;
            m_lanes.Clear();
            m_bars.Clear();
            m_edges.Clear();
            const IProfileSource* source = Source();
            const FrameRange frames = m_focus ? m_focus->Frames() : FrameRange{};
            if (source == nullptr || frames.empty || plot.width <= 0.0f)
            {
                m_hovered = -1;
                return;
            }
            // The lanes: each thread with a scope in the frames, in the threads' order, then the GPU.
            u32 gpuDepths = 0;
            bool anyGpu = false;
            for (u64 n = frames.first; n <= frames.last; ++n)
                if (const FrameRecord* frame = source->Frame(n))
                {
                    for (const ThreadFrame& thread : frame->threads)
                        if (!thread.scopes.IsEmpty())
                        {
                            Lane& lane = LaneFor(thread.thread, *source);
                            for (const ScopeEvent& scope : thread.scopes)
                                lane.depths = Max(lane.depths, scope.depth + 1);
                        }
                    for (const GpuPassEvent& pass : frame->gpu)
                    {
                        anyGpu = true;
                        gpuDepths = Max(gpuDepths, pass.depth + 1);
                    }
                }
            if (anyGpu)
            {
                Lane gpu;
                gpu.name = String(u8"GPU");
                gpu.gpu = true;
                gpu.thread = kGpuLane;
                gpu.depths = Max<u32>(gpuDepths, 1);
                m_lanes.PushBack(core::Move(gpu));
            }
            f32 y = plot.y + kRulerHeight + kLaneGap - m_scrollY;
            for (Lane& lane : m_lanes)
            {
                lane.folded = IsFolded(lane.thread);
                lane.y = y;
                lane.height = static_cast<f32>(lane.folded ? 1 : lane.depths) * kRowHeight;
                y += lane.height + kLaneGap;
            }
            // The scopes in view, a lane and a depth at a time so narrow ones join their neighbours.
            Pending pending;
            for (u32 l = 0; l < m_lanes.Size(); ++l)
            {
                const Lane& lane = m_lanes[l];
                for (u32 depth = 0; depth < (lane.folded ? 1u : lane.depths); ++depth)
                {
                    pending = Pending{};
                    for (u64 n = frames.first; n <= frames.last; ++n)
                    {
                        const FrameRecord* frame = source->Frame(n);
                        if (frame == nullptr)
                            continue;
                        if (lane.gpu)
                        {
                            for (const GpuPassEvent& pass : frame->gpu)
                                if (pass.depth == depth)
                                    Place(pending, l, depth, pass.name, true, pass.beginTick, pass.endTick);
                        }
                        else
                            for (const ThreadFrame& thread : frame->threads)
                                if (thread.thread == lane.thread)
                                    for (const ScopeEvent& scope : thread.scopes)
                                        if (scope.depth == depth)
                                            Place(pending, l, depth, scope.name, false, scope.beginTick, scope.endTick);
                    }
                    Flush(pending);
                }
            }
            for (u64 n = frames.first; n <= frames.last; ++n)
                if (const FrameRecord* frame = source->Frame(n))
                {
                    FrameEdge edge;
                    edge.frame = n;
                    edge.x = XAt(static_cast<f64>(frame->beginTick));
                    edge.next = XAt(static_cast<f64>(frame->endTick));
                    edge.ms = TicksToMs(frame->endTick - frame->beginTick, source->TickFrequency());
                    if (edge.next >= plot.x && edge.x <= plot.Right())
                        m_edges.PushBack(edge);
                }
            // The bars are new: the one under the pointer is found again.
            m_hovered = -1;
            if (m_pointerIn)
            {
                const Bar* bar = nullptr;
                for (usize i = m_bars.Size(); i > 0 && bar == nullptr; --i)
                {
                    const Rect& r = m_bars[i - 1].rect;
                    if (m_pointer.x >= r.x - 1.0f && m_pointer.x <= r.x + Max(r.width, 1.0f) + 1.0f && m_pointer.y >= r.y &&
                        m_pointer.y < r.y + r.height && m_pointer.y >= plot.y + kRulerHeight)
                        bar = &m_bars[i - 1];
                }
                m_hovered = bar != nullptr ? static_cast<i32>(bar - m_bars.Data()) : -1;
            }
        }

        // Narrow scopes side by side, gathered into one block.
        struct Pending
        {
            Bar block;
            bool open = false;
        };

        void Place(Pending& pending, u32 lane, u32 depth, NameId name, bool gpu, u64 begin, u64 end) const
        {
            const Rect plot = PlotBounds();
            const f32 x0 = XAt(static_cast<f64>(begin));
            const f32 x1 = XAt(static_cast<f64>(end));
            if (x1 < plot.x || x0 > plot.Right())
                return;
            const f32 top = m_lanes[lane].y + static_cast<f32>(depth) * kRowHeight + 1.0f;
            if (x1 - x0 >= kMinBarWidth)
            {
                Flush(pending);
                Bar bar{lane, depth, name, gpu, begin, end, 0, Rect{Max(x0, plot.x - 2.0f), top, Min(x1, plot.Right() + 2.0f) - Max(x0, plot.x - 2.0f), kRowHeight - 2.0f}};
                m_bars.PushBack(bar);
                return;
            }
            if (pending.open && x0 - pending.block.rect.Right() <= 2.0f)
            {
                pending.block.endTick = Max(pending.block.endTick, end);
                pending.block.rect.width = Max(pending.block.rect.width, x1 - pending.block.rect.x);
                ++pending.block.merged;
                return;
            }
            Flush(pending);
            pending.open = true;
            pending.block = Bar{lane, depth, name, gpu, begin, end, 1, Rect{x0, top, x1 - x0, kRowHeight - 2.0f}};
        }
        void Flush(Pending& pending) const
        {
            if (!pending.open)
                return;
            pending.open = false;
            Bar block = pending.block;
            block.rect.width = Max(block.rect.width, 1.0f);
            if (block.merged == 1)
                block.merged = 0; // one narrow scope alone: a thin bar of its own
            m_bars.PushBack(block);
        }

        Lane& LaneFor(u32 thread, const IProfileSource& source) const
        {
            usize at = 0;
            while (at < m_lanes.Size() && m_lanes[at].thread < thread)
                ++at;
            if (at < m_lanes.Size() && m_lanes[at].thread == thread)
                return m_lanes[at];
            Lane lane;
            lane.thread = thread;
            for (const ThreadInfo& info : source.Threads())
                if (info.index == thread)
                    lane.name = String(info.name.AsView());
            if (lane.name.IsEmpty())
                lane.name = Format(u8"Thread {}", thread);
            return m_lanes.Insert(at, core::Move(lane));
        }

        // The ruler's ticks: a step of 1, 2 or 5 times a power of ten of ms, at least the label
        // spacing apart, labelled from the first frame's start.
        void DrawTicks(DrawContext& ctx, const Rect& plot, Color text) const
        {
            const IProfileSource* source = Source();
            u64 origin = 0;
            u64 unused = 0;
            if (source == nullptr || !FramesSpan(origin, unused))
                return;
            const f64 msPerPixel = (m_viewEnd - m_viewBegin) / static_cast<f64>(source->TickFrequency()) * 1000.0 / static_cast<f64>(Max(plot.width, 1.0f));
            const f32 step = PickStep(static_cast<f32>(msPerPixel * kMinLabelSpacing));
            const f64 ticksPerMs = static_cast<f64>(source->TickFrequency()) / 1000.0;
            const f64 firstMs = (m_viewBegin - static_cast<f64>(origin)) / ticksPerMs;
            const f64 lastMs = (m_viewEnd - static_cast<f64>(origin)) / ticksPerMs;
            const Color tick = PartColor(u8"tick", text);
            const i64 first = static_cast<i64>(firstMs / static_cast<f64>(step)) - 1;
            for (i64 i = first; static_cast<f64>(i) * static_cast<f64>(step) <= lastMs; ++i)
            {
                const f64 ms = static_cast<f64>(i) * static_cast<f64>(step);
                const f32 x = XAt(static_cast<f64>(origin) + ms * ticksPerMs);
                ctx.VG().FillRect(Rectangle{Floor(x), plot.y + kTickHeight * 0.5f, 1.0f, kTickHeight * 0.5f}, tick);
                DrawLabel(ctx, Format(u8"{} ms", FormatStep(static_cast<f32>(ms), step)).AsView(), Rect{x + 3.0f, plot.y, kMinLabelSpacing, kTickHeight * 0.6f},
                          text);
            }
        }
        [[nodiscard]] static f32 PickStep(f32 least) noexcept
        {
            if (least <= 0.0f)
                return 1.0f;
            const f32 magnitude = Pow(10.0f, Floor(Log(least) / Log(10.0f)));
            const f32 multiples[] = {1.0f, 2.0f, 5.0f, 10.0f};
            for (const f32 m : multiples)
                if (m * magnitude >= least * 0.9999f)
                    return m * magnitude;
            return 10.0f * magnitude;
        }
        // `ms` to the decimals of `step`.
        [[nodiscard]] static String FormatStep(f32 ms, f32 step)
        {
            i32 decimals = 0;
            for (f32 s = step; s < 0.999f && decimals < 6; s *= 10.0f)
                ++decimals;
            return FormatFixed(ms, decimals);
        }

        // A block of scopes too narrow to draw: diagonal strokes over a faint fill.
        static void DrawHatch(DrawContext& ctx, const Rect& r, Color color)
        {
            ctx.VG().FillRect(r.ToRectangle(), color);
            ctx.PushClip(r);
            Color stroke = color;
            stroke.a = Min(color.a * 2.0f, 1.0f);
            for (f32 x = r.x - r.height; x < r.Right(); x += 5.0f)
                ctx.VG().DrawLine(core::Float2{x, r.Bottom()}, core::Float2{x + r.height, r.y}, stroke, 1.0f);
            ctx.PopClip();
        }
        // A lane's arrow: down when open, toward the name when folded.
        static void DrawArrow(DrawContext& ctx, core::Float2 at, bool folded, Color color)
        {
            vg::PathBuilder arrow;
            if (folded)
            {
                arrow.MoveTo(at.x - 2.0f, at.y - 4.0f);
                arrow.LineTo(at.x + 3.0f, at.y);
                arrow.LineTo(at.x - 2.0f, at.y + 4.0f);
            }
            else
            {
                arrow.MoveTo(at.x - 4.0f, at.y - 2.0f);
                arrow.LineTo(at.x + 4.0f, at.y - 2.0f);
                arrow.LineTo(at.x, at.y + 3.0f);
            }
            arrow.Close();
            ctx.VG().FillPath(arrow.ToPath(), color);
        }

        f64 m_viewBegin = 0.0;
        f64 m_viewEnd = 1.0;
        f32 m_scrollY = 0.0f;
        Array<u32> m_folded;
        Drag m_drag = Drag::None;
        core::Float2 m_pressAt{0.0f, 0.0f};
        core::Float2 m_lastAt{0.0f, 0.0f};
        core::Float2 m_pointer{0.0f, 0.0f};
        bool m_pointerIn = false;
        mutable String m_tip;

        // What Compute made, and what it made it from.
        mutable Array<Lane> m_lanes;
        mutable Array<Bar> m_bars;
        mutable Array<FrameEdge> m_edges;
        mutable i32 m_hovered = -1;
        mutable bool m_dirty = true;
        mutable f64 m_computedBegin = 0.0;
        mutable f64 m_computedEnd = 0.0;
        mutable f32 m_computedScroll = 0.0f;
        mutable f32 m_computedWidth = -1.0f;
        mutable f32 m_computedHeight = -1.0f;
    };
}
