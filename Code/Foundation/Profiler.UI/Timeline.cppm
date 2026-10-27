// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.UI - :timeline partition.
///
/// The selected frames on one time axis (Specs/profiler.md, P5): a lane per thread with its
/// scopes stacked by depth, a GPU lane with its passes, the frames' edges as lines. The wheel zooms
/// about the pointer (the time under it stays put), a drag pans, Shift and the wheel scroll the
/// lanes, and hovering a bar names it with its duration. A name the scope table selects is
/// highlighted wherever it ran. What it lays out (the lanes, the bars) is its model, which the
/// draw and the tests read.

module;
#include "Core/Prelude.h"

export module foundation.profiler.ui:timeline;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;
import foundation.profiler;
import :view;

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::profiler::ui
{
    class Timeline final : public ProfilerView
    {
    public:
        static constexpr f32 kHeaderWidth = 96.0f; // the lanes' names
        static constexpr f32 kRulerHeight = 18.0f; // the frames' numbers
        static constexpr f32 kRowHeight = 16.0f;   // a depth of scopes
        static constexpr f32 kLaneGap = 6.0f;
        static constexpr f32 kMinBarWidth = 0.25f; // narrower bars are not drawn

        struct Lane
        {
            String name;
            bool gpu = false;
            u32 thread = 0; // the thread's index (CPU lanes)
            u32 depths = 1; // rows of scopes
            f32 y = 0.0f;   // top, in the view (after the lanes' scroll)
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
            Rectangle rect{}; // in the view
        };

        explicit Timeline(IAllocator& allocator)
            : ProfilerView(allocator), m_lanes(allocator), m_bars(allocator), m_frameEdges(allocator)
        {
            IsHitTestVisible = true; // it zooms, pans and hovers
        }

        /// Show these frames, the whole span in view.
        void SetFrames(FrameRange frames)
        {
            m_frames = frames;
            FitView();
            Refresh();
        }
        [[nodiscard]] FrameRange Frames() const noexcept { return m_frames; }

        /// The view back to the selected frames' whole span: from the first frame's start to the
        /// last frame's end or its last GPU pass's, whichever is later.
        void FitView()
        {
            const IProfileSource& source = SourceOrEmpty();
            m_viewBegin = 0.0;
            m_viewEnd = 1.0;
            m_scrollY = 0.0f;
            if (m_frames.empty)
            {
                return;
            }
            bool any = false;
            for (u64 n = m_frames.first; n <= m_frames.last; ++n)
            {
                const FrameRecord* frame = source.Frame(n);
                if (frame == nullptr)
                {
                    continue;
                }
                const f64 begin = static_cast<f64>(frame->beginTick);
                f64 end = static_cast<f64>(frame->endTick);
                for (const GpuPassEvent& pass : frame->gpu)
                {
                    end = Max(end, static_cast<f64>(pass.endTick));
                }
                m_viewBegin = any ? Min(m_viewBegin, begin) : begin;
                m_viewEnd = any ? Max(m_viewEnd, end) : end;
                any = true;
            }
            if (m_viewEnd <= m_viewBegin)
            {
                m_viewEnd = m_viewBegin + 1.0;
            }
        }

        /// The time (in the source's ticks) shown at `x`, and where a time shows.
        [[nodiscard]] f64 TickAt(f32 x) const noexcept
        {
            return m_viewBegin + static_cast<f64>((x - kHeaderWidth) / PlotWidth()) * (m_viewEnd - m_viewBegin);
        }
        [[nodiscard]] f32 XAt(f64 tick) const noexcept
        {
            return kHeaderWidth + static_cast<f32>((tick - m_viewBegin) / (m_viewEnd - m_viewBegin)) * PlotWidth();
        }
        [[nodiscard]] f64 ViewBegin() const noexcept { return m_viewBegin; }
        [[nodiscard]] f64 ViewEnd() const noexcept { return m_viewEnd; }

        /// Zoom by `factor` (above 1 in) about `x`: the time under it stays under it. No closer
        /// than a microsecond across the plot.
        void ZoomAt(f32 x, f32 factor)
        {
            if (factor <= 0.0f)
            {
                return;
            }
            const f64 anchor = TickAt(x);
            const f64 minimum = static_cast<f64>(SourceOrEmpty().TickFrequency()) / 1.0e6;
            const f64 span = Max((m_viewEnd - m_viewBegin) / static_cast<f64>(factor), Max(minimum, 1.0e-9));
            const f64 along = static_cast<f64>((x - kHeaderWidth) / PlotWidth());
            m_viewBegin = anchor - along * span;
            m_viewEnd = m_viewBegin + span;
            Refresh();
        }

        /// Move the view by `dx` view units (positive shows earlier time).
        void PanBy(f32 dx)
        {
            const f64 shift = static_cast<f64>(dx / PlotWidth()) * (m_viewEnd - m_viewBegin);
            m_viewBegin -= shift;
            m_viewEnd -= shift;
            Refresh();
        }

        /// Highlight every bar of `name` in the CPU scopes or the GPU passes; none with ClearHighlight.
        void SetHighlight(NameId name, bool gpu)
        {
            m_highlight = name;
            m_highlightGpu = gpu;
            m_highlighting = true;
            InvalidateVisual();
        }
        void ClearHighlight()
        {
            m_highlighting = false;
            InvalidateVisual();
        }
        [[nodiscard]] bool IsHighlighted(const Bar& bar) const noexcept
        {
            return m_highlighting && bar.name == m_highlight && bar.gpu == m_highlightGpu;
        }

        /// The model: the lanes and the bars in view, the frames' edges, the bar under the pointer.
        [[nodiscard]] Span<const Lane> Lanes() const noexcept { return m_lanes.AsSpan(); }
        [[nodiscard]] Span<const Bar> Bars() const noexcept { return m_bars.AsSpan(); }
        [[nodiscard]] Span<const f32> FrameEdges() const noexcept { return m_frameEdges.AsSpan(); }
        [[nodiscard]] const Bar* Hovered() const noexcept
        {
            return m_hovered >= 0 && static_cast<usize>(m_hovered) < m_bars.Size() ? &m_bars[static_cast<usize>(m_hovered)]
                                                                                 : nullptr;
        }

        /// The bar at a point in the view, or null.
        [[nodiscard]] const Bar* BarAt(Float2 point) const noexcept
        {
            for (usize i = m_bars.Size(); i > 0; --i)
            {
                const Rectangle& r = m_bars[i - 1].rect;
                if (point.x >= r.x && point.x <= r.x + Max(r.width, 2.0f) && point.y >= r.y && point.y < r.y + r.height)
                {
                    return &m_bars[i - 1];
                }
            }
            return nullptr;
        }

        void OnMouseWheel(foundation::ui::MouseWheelEventArgs& e) override
        {
            if (HasFlag(e.Modifiers, foundation::ui::KeyModifiers::Shift))
            {
                m_scrollY = Max(m_scrollY - e.DeltaY * kRowHeight * 3.0f, 0.0f);
                Refresh();
            }
            else if (e.DeltaY != 0.0f)
            {
                ZoomAt(e.X, e.DeltaY > 0.0f ? 1.25f : 0.8f);
            }
            e.Handled = true;
        }

        void OnMouseDown(foundation::ui::MouseEventArgs& e) override
        {
            if (e.Button != foundation::ui::MouseButton::Left)
            {
                return;
            }
            if (e.ClickCount >= 2)
            {
                FitView(); // a double click: the whole selection again
                Refresh();
                e.Handled = true;
                return;
            }
            m_panning = true;
            m_lastX = e.X;
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetCapture(this);
            }
            e.Handled = true;
        }

        void OnMouseMove(foundation::ui::MouseEventArgs& e) override
        {
            if (m_panning)
            {
                PanBy(e.X - m_lastX);
                m_lastX = e.X;
                e.Handled = true;
                return;
            }
            m_pointer = Float2{e.X, e.Y};
            m_pointerIn = true;
            const i32 hovered = HoveredAt(m_pointer);
            if (hovered != m_hovered)
            {
                m_hovered = hovered;
                InvalidateVisual();
            }
        }

        void OnMouseUp(foundation::ui::MouseEventArgs& e) override
        {
            if (!m_panning)
            {
                return;
            }
            m_panning = false;
            if (Context != nullptr)
            {
                Context->GetFocusManager()->ReleaseCapture();
            }
            e.Handled = true;
        }

        void OnMouseLeave() override
        {
            m_pointerIn = false;
            if (m_hovered >= 0)
            {
                m_hovered = -1;
                InvalidateVisual();
            }
        }

        void OnDraw(foundation::ui::UIDrawContext& ctx) override
        {
            Compute(); // the size may have changed since the last refresh
            const f32 w = Width();
            const f32 h = Height();
            ctx.VG().FillRect(Rectangle{0, 0, w, h}, palette.plot);
            ctx.VG().PushClipRect(Rectangle{0, 0, w, h});
            for (const Lane& lane : m_lanes)
            {
                ctx.VG().DrawLine(Float2{0, lane.y - kLaneGap * 0.5f}, Float2{w, lane.y - kLaneGap * 0.5f}, palette.grid, 1.0f);
                DrawLabel(ctx, lane.name.AsView(), Rectangle{4, lane.y, kHeaderWidth - 8, kRowHeight},
                          lane.gpu ? palette.gpu : palette.dim);
            }
            ctx.VG().PushClipRect(Rectangle{kHeaderWidth, 0, PlotWidth(), h});
            for (const f32 x : m_frameEdges)
            {
                ctx.VG().DrawLine(Float2{x, 0}, Float2{x, h}, palette.budget, 1.0f);
            }
            const IProfileSource& source = SourceOrEmpty();
            for (const Bar& bar : m_bars)
            {
                Color color = BarColor(bar);
                if (m_highlighting && !IsHighlighted(bar))
                {
                    color.a *= 0.35f;
                }
                ctx.VG().FillRect(bar.rect, color);
                if (IsHighlighted(bar))
                {
                    ctx.VG().StrokeRect(bar.rect, palette.highlight, 1.0f);
                }
                if (bar.rect.width > 40.0f)
                {
                    DrawLabel(ctx, source.NameOf(bar.name),
                              Rectangle{bar.rect.x + 3, bar.rect.y, bar.rect.width - 6, bar.rect.height}, palette.text);
                }
            }
            ctx.VG().PopClip();
            ctx.VG().FillRect(Rectangle{kHeaderWidth, 0, PlotWidth(), kRulerHeight}, palette.panel);
            DrawRuler(ctx);
            if (const Bar* bar = Hovered())
            {
                String info(Allocator());
                AppendFormat(info, u8"{}  {} ms  {}", source.NameOf(bar->name),
                             FormatFixed(TicksToMs(bar->endTick - bar->beginTick, source.TickFrequency()), 3,
                                         Allocator())
                                 .AsView(),
                             bar->gpu ? StringView(u8"GPU") : StringView(u8"CPU"));
                const Rectangle box{Max(w - 320.0f, kHeaderWidth), kRulerHeight + 2, 316, kRowHeight + 4};
                ctx.VG().FillRoundedRect(box, 3.0f, palette.panel);
                DrawLabel(ctx, info.AsView(), Rectangle{box.x + 6, box.y + 2, box.width - 12, kRowHeight}, palette.text);
            }
            ctx.VG().PopClip();
        }

    protected:
        void OnMeasure(foundation::ui::BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(600.0f), constraints.ConstrainHeight(240.0f)};
        }

        void Compute() override
        {
            m_lanes.Clear();
            m_bars.Clear();
            m_frameEdges.Clear();
            m_hovered = -1;
            ComputeBars();
            // The bars are new: the one under the pointer is found again, so a recompute (every
            // draw does one) keeps the hover and its box.
            m_hovered = m_pointerIn ? HoveredAt(m_pointer) : -1;
        }

        // The index of the bar under `at`, or -1.
        [[nodiscard]] i32 HoveredAt(Float2 at) const
        {
            const Bar* bar = BarAt(at);
            return bar != nullptr ? static_cast<i32>(bar - m_bars.Data()) : -1;
        }

        void ComputeBars()
        {
            const IProfileSource& source = SourceOrEmpty();
            if (m_frames.empty || PlotWidth() <= 0.0f)
            {
                return;
            }
            // The lanes: each thread with a scope in the frames, in thread order, then the GPU.
            u32 gpuDepths = 0;
            bool anyGpu = false;
            for (u64 n = m_frames.first; n <= m_frames.last; ++n)
            {
                const FrameRecord* frame = source.Frame(n);
                if (frame == nullptr)
                {
                    continue;
                }
                for (const ThreadFrame& thread : frame->threads)
                {
                    if (thread.scopes.IsEmpty())
                    {
                        continue;
                    }
                    Lane& lane = LaneFor(thread.thread, source);
                    for (const ScopeEvent& scope : thread.scopes)
                    {
                        lane.depths = Max(lane.depths, scope.depth + 1);
                    }
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
                gpu.name = String(u8"GPU", Allocator());
                gpu.gpu = true;
                gpu.depths = Max<u32>(gpuDepths, 1);
                m_lanes.PushBack(Move(gpu));
            }
            f32 y = kRulerHeight + kLaneGap - m_scrollY;
            for (Lane& lane : m_lanes)
            {
                lane.y = y;
                lane.height = static_cast<f32>(lane.depths) * kRowHeight;
                y += lane.height + kLaneGap;
            }
            // The bars in view, and the frames' edges.
            for (u64 n = m_frames.first; n <= m_frames.last; ++n)
            {
                const FrameRecord* frame = source.Frame(n);
                if (frame == nullptr)
                {
                    continue;
                }
                const f32 edge = XAt(static_cast<f64>(frame->beginTick));
                if (edge >= kHeaderWidth && edge <= Width())
                {
                    m_frameEdges.PushBack(edge);
                }
                for (const ThreadFrame& thread : frame->threads)
                {
                    const u32 lane = LaneIndexOf(thread.thread);
                    for (const ScopeEvent& scope : thread.scopes)
                    {
                        AddBar(lane, scope.depth, scope.name, false, scope.beginTick, scope.endTick);
                    }
                }
                if (anyGpu)
                {
                    for (const GpuPassEvent& pass : frame->gpu)
                    {
                        AddBar(static_cast<u32>(m_lanes.Size() - 1), pass.depth, pass.name, true, pass.beginTick,
                               pass.endTick);
                    }
                }
            }
        }

    private:
        [[nodiscard]] f32 PlotWidth() const noexcept { return Max(Width() - kHeaderWidth, 1.0f); }

        Lane& LaneFor(u32 thread, const IProfileSource& source)
        {
            usize at = 0;
            while (at < m_lanes.Size() && m_lanes[at].thread < thread)
            {
                ++at;
            }
            if (at < m_lanes.Size() && m_lanes[at].thread == thread)
            {
                return m_lanes[at];
            }
            Lane lane;
            lane.thread = thread;
            for (const ThreadInfo& info : source.Threads())
            {
                if (info.index == thread)
                {
                    lane.name = String(info.name.AsView(), Allocator());
                }
            }
            if (lane.name.IsEmpty())
            {
                lane.name = String(Allocator());
                AppendFormat(lane.name, u8"Thread {}", thread);
            }
            return m_lanes.Insert(at, Move(lane));
        }

        [[nodiscard]] u32 LaneIndexOf(u32 thread) const noexcept
        {
            for (usize i = 0; i < m_lanes.Size(); ++i)
            {
                if (!m_lanes[i].gpu && m_lanes[i].thread == thread)
                {
                    return static_cast<u32>(i);
                }
            }
            return 0;
        }

        void AddBar(u32 lane, u32 depth, NameId name, bool gpu, u64 begin, u64 end)
        {
            const f32 x0 = Max(XAt(static_cast<f64>(begin)), kHeaderWidth);
            const f32 x1 = Min(XAt(static_cast<f64>(end)), Width());
            if (x1 < kHeaderWidth || x0 > Width() || x1 - x0 < kMinBarWidth || lane >= m_lanes.Size())
            {
                return;
            }
            Bar bar;
            bar.lane = lane;
            bar.depth = depth;
            bar.name = name;
            bar.gpu = gpu;
            bar.beginTick = begin;
            bar.endTick = end;
            bar.rect = Rectangle{x0, m_lanes[lane].y + static_cast<f32>(depth) * kRowHeight, x1 - x0, kRowHeight - 1.0f};
            m_bars.PushBack(bar);
        }

        // A name keeps its colour: a shade of the lane's hue picked by the name.
        [[nodiscard]] Color BarColor(const Bar& bar) const noexcept
        {
            const f32 shade = 0.70f + 0.30f * static_cast<f32>((bar.name * 2654435761u) % 7u) / 6.0f;
            const Color base = bar.gpu ? palette.gpu : palette.cpu;
            return Color{base.r * shade, base.g * shade, base.b * shade, 0.85f};
        }

        // The frames' numbers along the top, at their edges.
        void DrawRuler(foundation::ui::UIDrawContext& ctx)
        {
            const IProfileSource& source = SourceOrEmpty();
            for (u64 n = m_frames.empty ? 1 : m_frames.first; !m_frames.empty && n <= m_frames.last; ++n)
            {
                const FrameRecord* frame = source.Frame(n);
                if (frame == nullptr)
                {
                    continue;
                }
                const f32 x = XAt(static_cast<f64>(frame->beginTick));
                const f32 next = XAt(static_cast<f64>(frame->endTick));
                if (next < kHeaderWidth || x > Width() || next - x < 36.0f)
                {
                    continue;
                }
                String label(Allocator());
                AppendFormat(label, u8"#{}  {} ms", n,
                             FormatFixed(TicksToMs(frame->endTick - frame->beginTick, source.TickFrequency()), 2,
                                         Allocator())
                                 .AsView());
                DrawLabel(ctx, label.AsView(), Rectangle{Max(x, kHeaderWidth) + 3, 0, next - Max(x, kHeaderWidth) - 6, kRulerHeight},
                          palette.dim);
            }
        }

        Array<Lane> m_lanes;
        Array<Bar> m_bars;
        Array<f32> m_frameEdges;
        FrameRange m_frames;
        f64 m_viewBegin = 0.0;
        f64 m_viewEnd = 1.0;
        f32 m_scrollY = 0.0f;
        i32 m_hovered = -1;
        Float2 m_pointer{0.0f, 0.0f}; // where the pointer last was over the view
        bool m_pointerIn = false;
        bool m_panning = false;
        f32 m_lastX = 0.0f;
        NameId m_highlight = 0;
        bool m_highlightGpu = false;
        bool m_highlighting = false;
    };
}
