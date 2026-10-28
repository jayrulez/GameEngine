// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :overlay partition
//
// The in-game overlay on the GUI (Specs/engine-ui-removal.md, layer 1; Specs/profiler.md, P4): the
// frame time graph (the newest frames' CPU and GPU times against the 60 and 30 fps budgets), the
// frame statistics, the top CPU scopes by their own time and the top GPU passes, and the counters,
// stacked on one translucent panel in a corner of whatever it fills. It never takes the pointer,
// so the game under it plays on. It reads the focus's source (the live store, for a game); the
// host shows it, arms the profiler while it shows, and calls Refresh each frame.
//
// Its colours are its parts' (`profileroverlay::panel`, `::plot`, `::cpu`, `::gpu`, `::budget`,
// `::note`, `::text`), each with the old overlay's colour where no sheet gives one: a game's
// sheet knows nothing of it, and it reads the same over any game.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.profiler.gui:overlay;

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
    enum class OverlayCorner : u8
    {
        TopRight,
        TopLeft,
        BottomRight,
        BottomLeft,
    };

    class ProfilerOverlay final : public ProfilerWidget
    {
        RTTI_OBJECT(ProfilerOverlay, ProfilerWidget)
    public:
        static constexpr f32 kPanelWidth = 360.0f;
        static constexpr f32 kPadding = 8.0f;
        static constexpr f32 kGap = 6.0f;
        static constexpr f32 kMargin = 8.0f;
        static constexpr f32 kLineHeight = 15.0f;
        static constexpr f32 kGraphHeight = 64.0f;
        static constexpr f32 kBudgets[] = {16.667f, 33.333f};
        static constexpr u32 kGraphFrames = 120; // the graph and the statistics: two seconds at 60 fps
        static constexpr u32 kListFrames = 30;   // the lists and counters: steady, yet recent
        static constexpr u32 kScopeRows = 6;
        static constexpr u32 kPassRows = 5;
        static constexpr u32 kCounterColumns = 2;

        /// A row of a top list: a name, its ms a frame, its bar against the longest (0 to 1).
        struct TopRow
        {
            String name;
            f32 msPerFrame = 0.0f;
            f32 bar = 0.0f;
        };
        /// A counter shown: its name and its value, formatted.
        struct CounterRow
        {
            String name;
            String value;
        };

        ProfilerOverlay()
            : m_times(MemoryAllocator()), m_cpuPoints(MemoryAllocator()), m_gpuPoints(MemoryAllocator()),
              m_statLines(MemoryAllocator()), m_scopes(MemoryAllocator()), m_passes(MemoryAllocator()),
              m_counters(MemoryAllocator()), m_entries(MemoryAllocator()), m_readings(MemoryAllocator())
        {
            SetTag(StringView(u8"profileroverlay"));
            SetTabFocusable(false);
            SetHitTestVisible(false); // it shows; the game keeps the pointer
            SetInlineStyle(StringView(u8"font-size"), StringView(u8"11px"));
        }

        /// The parts it draws, which a sheet styles as `profileroverlay::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            ProfilerWidget::CollectStyleParts(out);
            static constexpr const char8_t* kParts[] = {u8"panel", u8"plot", u8"cpu", u8"gpu", u8"budget", u8"note", u8"text"};
            for (const char8_t* part : kParts)
                out.PushBack(StringView(part));
        }

        void SetCorner(OverlayCorner corner)
        {
            m_corner = corner;
            Invalidate();
        }
        [[nodiscard]] OverlayCorner Corner() const noexcept { return m_corner; }

        /// Read the source again (a new frame): what it shows, then a draw.
        void Refresh()
        {
            Compute();
            Invalidate();
        }
        void OnProfileSourceChanged() override { Refresh(); }
        void OnProfileFramesChanged() override { Refresh(); }

        // === What it shows (the model its draw reads; tests read it too) ===
        /// The panel in its own space, for the size it was last laid out at.
        [[nodiscard]] Rect Panel() const noexcept { return PanelIn(GetContentBounds()); }
        [[nodiscard]] f32 PanelHeight() const noexcept
        {
            const f32 counterLines = static_cast<f32>(Max<usize>((m_counters.Size() + kCounterColumns - 1) / kCounterColumns, 1));
            return 2 * kPadding + kGraphHeight + 3 * kLineHeight + static_cast<f32>(kScopeRows + 1) * kLineHeight +
                   static_cast<f32>(kPassRows + 1) * kLineHeight + counterLines * kLineHeight + 4 * kGap;
        }
        /// The graph's top in ms (20 doubled until it holds the highest time) and its lines, in the
        /// graph's own space (newest at its right edge, a step a frame back from there).
        [[nodiscard]] f32 ScaleMs() const noexcept { return m_scaleMs; }
        [[nodiscard]] Span<const Float2> CpuPoints() const noexcept { return m_cpuPoints.AsSpan(); }
        [[nodiscard]] Span<const Float2> GpuPoints() const noexcept { return m_gpuPoints.AsSpan(); }
        [[nodiscard]] static f32 ScaleFor(f32 highestMs) noexcept
        {
            f32 scale = 20.0f;
            while (scale < highestMs && scale < 10240.0f)
                scale *= 2.0f;
            return scale;
        }
        /// The statistics' three lines: the CPU's, the GPU's, the frame rate's.
        [[nodiscard]] Span<const String> StatLines() const noexcept { return m_statLines.AsSpan(); }
        [[nodiscard]] Span<const TopRow> Scopes() const noexcept { return m_scopes.AsSpan(); }
        [[nodiscard]] Span<const TopRow> Passes() const noexcept { return m_passes.AsSpan(); }
        [[nodiscard]] Span<const CounterRow> Counters() const noexcept { return m_counters.AsSpan(); }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect box = GetContentBounds();
            const Rect panel = PanelIn(box);
            if (panel.width <= 2 * kPadding)
                return;
            const Color text = PartText(u8"text", Color{0.94f, 0.95f, 0.97f, 1.0f});
            const Color note = PartText(u8"note", Color{0.66f, 0.69f, 0.74f, 1.0f});
            const Color cpu = PartColor(u8"cpu", Color{0.36f, 0.70f, 1.0f, 1.0f});
            const Color gpu = PartColor(u8"gpu", Color{1.0f, 0.64f, 0.26f, 1.0f});
            vg::VGContext& vg = ctx.VG();
            vg.FillRoundedRect(panel.ToRectangle(), 6.0f, PartColor(u8"panel", Color{0.05f, 0.06f, 0.08f, 0.82f}));
            const f32 x = panel.x + kPadding;
            const f32 width = panel.width - 2 * kPadding;
            f32 y = panel.y + kPadding;

            // The graph: the budgets, the area under the CPU line and the line, the GPU's line.
            const Rect graph{x, y, width, kGraphHeight};
            PlaceGraph(width); // laid out at another width since the last refresh
            vg.FillRect(graph.ToRectangle(), PartColor(u8"plot", Color{1.0f, 1.0f, 1.0f, 0.05f}));
            for (const f32 budget : kBudgets)
                if (budget < m_scaleMs)
                {
                    const f32 by = graph.y + YOf(budget);
                    vg.DrawLine(Float2{graph.x, by}, Float2{graph.x + width, by}, PartColor(u8"budget", Color{1.0f, 1.0f, 1.0f, 0.28f}), 1.0f);
                    DrawLabel(ctx, FormatFixed(budget, 1, MemoryAllocator()).AsView(), Rect{graph.x + 2, by - kLineHeight, 60, kLineHeight}, note);
                }
            if (m_cpuPoints.Size() >= 2)
            {
                vg.BeginPath();
                vg.MoveTo(Float2{graph.x + m_cpuPoints[0].x, graph.y + kGraphHeight});
                for (const Float2 p : m_cpuPoints)
                    vg.LineTo(Float2{graph.x + p.x, graph.y + p.y});
                vg.LineTo(Float2{graph.x + m_cpuPoints[m_cpuPoints.Size() - 1].x, graph.y + kGraphHeight});
                vg.ClosePath();
                vg.Fill(Color{cpu.r, cpu.g, cpu.b, cpu.a * 0.22f});
                StrokeLine(vg, m_cpuPoints.AsSpan(), Float2{graph.x, graph.y}, cpu);
            }
            if (m_gpuPoints.Size() >= 2)
                StrokeLine(vg, m_gpuPoints.AsSpan(), Float2{graph.x, graph.y}, gpu);
            DrawLabel(ctx, FormatFixed(m_scaleMs, 0, MemoryAllocator()).AsView(), Rect{graph.x + width - 62, graph.y, 60, kLineHeight}, note,
                      TextHAlign::Right);
            y += kGraphHeight + kGap;

            // The statistics, each line marked in its lane's colour.
            const Color marks[] = {cpu, gpu, text};
            for (usize i = 0; i < m_statLines.Size(); ++i)
            {
                vg.FillRect(Rectangle{x, y + 4, 3, kLineHeight - 8}, marks[i < 3 ? i : 2]);
                DrawLabel(ctx, m_statLines[i].AsView(), Rect{x + 8, y, width - 8, kLineHeight}, text);
                y += kLineHeight;
            }
            y += kGap;

            y = DrawTop(ctx, StringView(u8"CPU scopes (self), ms a frame"), m_scopes.AsSpan(), kScopeRows, cpu, Rect{x, y, width, 0}, text, note);
            y = DrawTop(ctx, StringView(u8"GPU passes, ms a frame"), m_passes.AsSpan(), kPassRows, gpu, Rect{x, y, width, 0}, text, note);

            // The counters, in two columns.
            const f32 column = width / static_cast<f32>(kCounterColumns);
            for (usize i = 0; i < m_counters.Size(); ++i)
            {
                const f32 cx = x + static_cast<f32>(i % kCounterColumns) * column;
                const f32 cy = y + static_cast<f32>(i / kCounterColumns) * kLineHeight;
                DrawLabel(ctx, m_counters[i].name.AsView(), Rect{cx, cy, column - 74, kLineHeight}, note);
                DrawLabel(ctx, m_counters[i].value.AsView(), Rect{cx + column - 72, cy, 68, kLineHeight}, text, TextHAlign::Right);
            }
        }

    private:
        [[nodiscard]] Rect PanelIn(const Rect& box) const noexcept
        {
            const f32 width = Min(kPanelWidth, Max(box.width - 2 * kMargin, 0.0f));
            const f32 height = PanelHeight();
            const bool right = m_corner == OverlayCorner::TopRight || m_corner == OverlayCorner::BottomRight;
            const bool bottom = m_corner == OverlayCorner::BottomRight || m_corner == OverlayCorner::BottomLeft;
            return Rect{right ? box.x + Max(box.width - kMargin - width, 0.0f) : box.x + kMargin,
                        bottom ? box.y + Max(box.height - kMargin - height, 0.0f) : box.y + kMargin, width, height};
        }
        // A part's text colour, or `fallback` where the sheet gives none (not the cascade's: a
        // game's text colour is the game's, not the overlay's).
        [[nodiscard]] Color PartText(StringView part, Color fallback) const
        {
            const PartStyle& style = GetPartStyle(part);
            return style.HasColor ? style.TextColor : fallback;
        }

        // What it shows, from the source: the graph's frames and the statistics over the newest
        // kGraphFrames, the lists and counters over the newest kListFrames.
        void Compute()
        {
            const IProfileSource* source = Source();
            IAllocator& a = MemoryAllocator();
            m_times.Clear();
            m_statLines.Clear();
            m_scopes.Clear();
            m_passes.Clear();
            m_counters.Clear();
            if (source != nullptr && source->HasFrames())
            {
                CollectFrameTimes(*source, LastFrames(*source, kGraphFrames), m_times);
                const FrameWindowStats stats = StatsOf(m_times.AsSpan(), a);
                f32 newestCpu = 0.0f;
                f32 newestGpu = 0.0f;
                for (const FrameTime& t : m_times) // oldest first: the last ones seen are the newest
                {
                    newestCpu = t.cpuMs;
                    if (t.hasGpu)
                        newestGpu = t.gpuMs;
                }
                const f32 fps = stats.cpu.mean > 0.0f ? 1000.0f / stats.cpu.mean : 0.0f;
                m_statLines.PushBack(StatLine(u8"CPU", newestCpu, stats.cpu));
                m_statLines.PushBack(stats.gpu.count > 0 ? StatLine(u8"GPU", newestGpu, stats.gpu) : String(u8"GPU  no timings", a));
                String rate(a);
                AppendFormat(rate, u8"{} fps over {} frames", FormatFixed(fps, 0, a).AsView(), stats.cpu.count);
                m_statLines.PushBack(core::Move(rate));

                const FrameRange recent = LastFrames(*source, kListFrames);
                CollectTopRows(*source, recent, TopLane::Scopes, kScopeRows, m_scopes);
                CollectTopRows(*source, recent, TopLane::GpuPasses, kPassRows, m_passes);
                CollectCounters(*source, recent, m_readings);
                const Span<const CounterInfo> infos = source->Counters();
                for (const CounterReading& reading : m_readings)
                {
                    if (!reading.present || reading.counter >= infos.Size())
                        continue;
                    const CounterInfo& info = infos[reading.counter];
                    // What the device did a frame, as its mean (the numbers hold still); what it
                    // holds, as the newest.
                    m_counters.PushBack(CounterRow{String(info.name.AsView(), a),
                                                   FormatCounter(info.unit == CounterUnit::Bytes ? reading.latest : reading.mean, info.unit, a)});
                }
            }
            f32 highest = 0.0f;
            for (const FrameTime& t : m_times)
                highest = Max(highest, Max(t.cpuMs, t.hasGpu ? t.gpuMs : 0.0f));
            m_scaleMs = ScaleFor(highest);
            PlaceGraph(Panel().width - 2 * kPadding);
        }
        // The lines' points for a graph `width` wide: a slot for each of kGraphFrames, the newest at
        // the right edge, so a short history grows in from the right rather than stretching.
        void PlaceGraph(f32 width)
        {
            m_cpuPoints.Clear();
            m_gpuPoints.Clear();
            if (m_times.IsEmpty() || width <= 0.0f)
                return;
            const f32 step = width / static_cast<f32>(kGraphFrames - 1);
            const u64 newest = m_times[m_times.Size() - 1].frame;
            for (const FrameTime& t : m_times)
            {
                const f32 x = width - static_cast<f32>(newest - t.frame) * step;
                m_cpuPoints.PushBack(Float2{x, YOf(t.cpuMs)});
                if (t.hasGpu)
                    m_gpuPoints.PushBack(Float2{x, YOf(t.gpuMs)});
            }
        }
        [[nodiscard]] f32 YOf(f32 ms) const noexcept { return kGraphHeight * (1.0f - Min(ms / m_scaleMs, 1.0f)); }

        void CollectTopRows(const IProfileSource& source, FrameRange range, TopLane lane, u32 count, Array<TopRow>& out)
        {
            CollectTop(source, range, lane, count, m_entries);
            const f32 longest = m_entries.IsEmpty() ? 0.0f : m_entries[0].msPerFrame;
            for (const TopEntry& entry : m_entries)
                out.PushBack(TopRow{String(source.NameOf(entry.name), MemoryAllocator()), entry.msPerFrame,
                                    longest > 0.0f ? entry.msPerFrame / longest : 0.0f});
        }
        [[nodiscard]] String StatLine(StringView lane, f32 newest, const TimeStats& stats) const
        {
            IAllocator& a = MemoryAllocator();
            String line(a);
            AppendFormat(line, u8"{}  {} ms   min {}  med {}  max {}", lane, FormatFixed(newest, 2, a).AsView(), FormatFixed(stats.min, 1, a).AsView(),
                         FormatFixed(stats.median, 1, a).AsView(), FormatFixed(stats.max, 1, a).AsView());
            return line;
        }

        // A list's heading and rows, each a bar against the longest; answers the y under it.
        f32 DrawTop(DrawContext& ctx, StringView heading, Span<const TopRow> rows, u32 count, Color bar, const Rect& at, Color text,
                    Color note) const
        {
            DrawLabel(ctx, heading, Rect{at.x, at.y, at.width, kLineHeight}, note);
            for (usize i = 0; i < rows.Size(); ++i)
            {
                const f32 y = at.y + static_cast<f32>(i + 1) * kLineHeight;
                ctx.VG().FillRect(Rectangle{at.x, y + 2, at.width * rows[i].bar, kLineHeight - 4}, Color{bar.r, bar.g, bar.b, bar.a * 0.35f});
                DrawLabel(ctx, rows[i].name.AsView(), Rect{at.x + 4, y, at.width - 64, kLineHeight}, text);
                DrawLabel(ctx, FormatFixed(rows[i].msPerFrame, 2, MemoryAllocator()).AsView(), Rect{at.x + at.width - 58, y, 56, kLineHeight}, text,
                          TextHAlign::Right);
            }
            return at.y + static_cast<f32>(count + 1) * kLineHeight + kGap;
        }
        static void StrokeLine(vg::VGContext& vg, Span<const Float2> points, Float2 origin, Color color)
        {
            vg.BeginPath();
            vg.MoveTo(Float2{origin.x + points[0].x, origin.y + points[0].y});
            for (usize i = 1; i < points.Size(); ++i)
                vg.LineTo(Float2{origin.x + points[i].x, origin.y + points[i].y});
            vg.Stroke(color, 1.5f);
        }

        Array<FrameTime> m_times;
        Array<Float2> m_cpuPoints;
        Array<Float2> m_gpuPoints;
        Array<String> m_statLines;
        Array<TopRow> m_scopes;
        Array<TopRow> m_passes;
        Array<CounterRow> m_counters;
        Array<TopEntry> m_entries;         // a list's, as collected
        Array<CounterReading> m_readings;  // the counters', as collected
        f32 m_scaleMs = 20.0f;
        OverlayCorner m_corner = OverlayCorner::TopRight;
    };
}
