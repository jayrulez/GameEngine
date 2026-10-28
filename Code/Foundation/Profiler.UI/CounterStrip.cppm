// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.UI - :counter_strip partition.
///
/// The counters as compact values in two columns: what the device did a frame (draws, passes,
/// binds; their mean over the view's frames, so the numbers hold still) and what it holds (memory,
/// the newest value). A counter no frame in the window set is left out.

module;
#include "Core/Prelude.h"

export module foundation.profiler.ui:counter_strip;

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
    class CounterStrip final : public ProfilerView
    {
    public:
        static constexpr u32 kColumns = 2;

        struct Row
        {
            String name;
            String value;
        };

        explicit CounterStrip(IAllocator& allocator) : ProfilerView(allocator), m_readings(allocator), m_rows(allocator)
        {
            m_frameCount = 30;
        }

        /// The model: a row per counter shown, in registration order.
        [[nodiscard]] Span<const Row> Rows() const noexcept { return m_rows.AsSpan(); }

        void OnDraw(foundation::ui::UIDrawContext& ctx) override
        {
            const f32 column = Width() / static_cast<f32>(kColumns);
            ctx.VG().PushClipRect(Rectangle{0, 0, Width(), Height()}); // given less room, it clips
            for (usize i = 0; i < m_rows.Size(); ++i)
            {
                const f32 x = static_cast<f32>(i % kColumns) * column;
                const f32 y = static_cast<f32>(i / kColumns) * kLineHeight;
                DrawLabel(ctx, m_rows[i].name.AsView(), Rectangle{x, y, column - 70, kLineHeight}, palette.dim);
                DrawLabel(ctx, m_rows[i].value.AsView(), Rectangle{x + column - 72, y, 68, kLineHeight}, palette.text,
                          fonts::TextAlignment::Right);
            }
            ctx.VG().PopClip();
        }

    protected:
        void OnMeasure(foundation::ui::BoxConstraints constraints) override
        {
            // The rows a source of this many counters needs; at least one.
            const u32 rows = Max<u32>((static_cast<u32>(m_rows.Size()) + kColumns - 1) / kColumns, 1);
            MeasuredSize = Float2{constraints.ConstrainWidth(300.0f),
                                  constraints.ConstrainHeight(static_cast<f32>(rows) * kLineHeight)};
        }

        void Compute() override
        {
            const IProfileSource& source = SourceOrEmpty();
            CollectCounters(source, Range(), m_readings);
            const usize before = m_rows.Size();
            m_rows.Clear();
            const Span<const CounterInfo> infos = source.Counters();
            for (const CounterReading& reading : m_readings)
            {
                if (!reading.present || reading.counter >= infos.Size())
                {
                    continue;
                }
                const CounterInfo& info = infos[reading.counter];
                Row row;
                row.name = String(info.name.AsView(), Allocator());
                row.value = FormatCounter(info.unit == CounterUnit::Bytes ? reading.latest : reading.mean, info.unit,
                                          Allocator());
                m_rows.PushBack(Move(row));
            }
            if (m_rows.Size() != before)
            {
                Invalidate(); // a row more or fewer: its height changes
            }
        }

    private:
        Array<CounterReading> m_readings;
        Array<Row> m_rows;
    };
}
