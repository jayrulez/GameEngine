// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.UI - :frame_stats partition.
///
/// The newest frame's CPU and GPU times and the frame rate, with min, median and max over the
/// view's frames, one line each: what a glance at the overlay should answer.

module;
#include "Core/Prelude.h"

export module foundation.profiler.ui:frame_stats;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;
import foundation.profiler;
import :view;

using namespace foundation::core;

export namespace foundation::profiler::ui
{
    class FrameStats final : public ProfilerView
    {
    public:
        explicit FrameStats(IAllocator& allocator) : ProfilerView(allocator), m_times(allocator), m_lines(allocator) {}

        /// The model: the statistics, the newest times, and the lines drawn.
        [[nodiscard]] const FrameWindowStats& Stats() const noexcept { return m_stats; }
        [[nodiscard]] f32 NewestCpuMs() const noexcept { return m_newestCpuMs; }
        [[nodiscard]] f32 NewestGpuMs() const noexcept { return m_newestGpuMs; }
        [[nodiscard]] f32 Fps() const noexcept { return m_fps; }
        [[nodiscard]] Span<const String> Lines() const noexcept { return m_lines.AsSpan(); }

        void OnDraw(foundation::ui::UIDrawContext& ctx) override
        {
            const Color colors[] = {palette.cpu, palette.gpu, palette.text};
            for (usize i = 0; i < m_lines.Size(); ++i)
            {
                const Rectangle row{0, static_cast<f32>(i) * kLineHeight, Width(), kLineHeight};
                ctx.VG().FillRect(Rectangle{0, row.y + 4, 3, kLineHeight - 8}, colors[i < 3 ? i : 2]);
                DrawLabel(ctx, m_lines[i].AsView(), Rectangle{8, row.y, Width() - 8, kLineHeight}, palette.text);
            }
        }

    protected:
        void OnMeasure(foundation::ui::BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(300.0f), constraints.ConstrainHeight(3 * kLineHeight)};
        }

        void Compute() override
        {
            CollectFrameTimes(SourceOrEmpty(), Range(), m_times);
            m_stats = StatsOf(m_times.AsSpan(), Allocator());
            m_newestCpuMs = 0.0f;
            m_newestGpuMs = 0.0f;
            for (const FrameTime& t : m_times) // oldest first: the last ones seen are the newest
            {
                m_newestCpuMs = t.cpuMs;
                if (t.hasGpu)
                {
                    m_newestGpuMs = t.gpuMs;
                }
            }
            m_fps = m_stats.cpu.mean > 0.0f ? 1000.0f / m_stats.cpu.mean : 0.0f;
            m_lines.Clear();
            m_lines.PushBack(Line(u8"CPU", m_newestCpuMs, m_stats.cpu));
            m_lines.PushBack(m_stats.gpu.count > 0 ? Line(u8"GPU", m_newestGpuMs, m_stats.gpu)
                                                   : String(u8"GPU  no timings", Allocator()));
            String fps(Allocator());
            AppendFormat(fps, u8"{} fps over {} frames", FormatFixed(m_fps, 0, Allocator()).AsView(), m_stats.cpu.count);
            m_lines.PushBack(Move(fps));
        }

    private:
        [[nodiscard]] String Line(StringView lane, f32 newest, const TimeStats& stats) const
        {
            IAllocator& a = Allocator();
            String line(a);
            AppendFormat(line, u8"{}  {} ms   min {}  med {}  max {}", lane, FormatFixed(newest, 2, a).AsView(),
                         FormatFixed(stats.min, 1, a).AsView(), FormatFixed(stats.median, 1, a).AsView(),
                         FormatFixed(stats.max, 1, a).AsView());
            return line;
        }

        Array<FrameTime> m_times;
        Array<String> m_lines;
        FrameWindowStats m_stats;
        f32 m_newestCpuMs = 0.0f;
        f32 m_newestGpuMs = 0.0f;
        f32 m_fps = 0.0f;
    };
}
