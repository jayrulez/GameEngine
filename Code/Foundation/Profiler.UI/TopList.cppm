// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.UI - :top_list partition.
///
/// The scopes (by their own time) or the GPU passes that took longest over the view's frames, as
/// labelled bars against the longest, in ms a frame. Thirty frames by default, half a second at
/// 60 fps: steady enough to read, recent enough to follow what the game is doing.

module;
#include "Core/Prelude.h"

export module foundation.profiler.ui:top_list;

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
    class TopList final : public ProfilerView
    {
    public:
        struct Row
        {
            String name;
            f32 msPerFrame = 0.0f;
            f32 callsPerFrame = 0.0f;
            f32 bar = 0.0f; // 0..1 of the longest row
        };

        explicit TopList(IAllocator& allocator, TopLane lane = TopLane::Scopes, u32 rows = 6)
            : ProfilerView(allocator), m_lane(lane), m_rowCount(rows), m_entries(allocator), m_rows(allocator)
        {
            m_frameCount = 30;
        }

        [[nodiscard]] TopLane Lane() const noexcept { return m_lane; }
        /// The model: the rows drawn, longest first.
        [[nodiscard]] Span<const Row> Rows() const noexcept { return m_rows.AsSpan(); }

        void OnDraw(foundation::ui::UIDrawContext& ctx) override
        {
            const Color bar = m_lane == TopLane::GpuPasses ? palette.gpu : palette.cpu;
            DrawLabel(ctx, m_lane == TopLane::GpuPasses ? StringView(u8"GPU passes, ms a frame")
                                                        : StringView(u8"CPU scopes (self), ms a frame"),
                      Rectangle{0, 0, Width(), kLineHeight}, palette.dim);
            for (usize i = 0; i < m_rows.Size(); ++i)
            {
                const f32 y = static_cast<f32>(i + 1) * kLineHeight;
                ctx.VG().FillRect(Rectangle{0, y + 2, Width() * m_rows[i].bar, kLineHeight - 4},
                                  Color{bar.r, bar.g, bar.b, 0.35f});
                DrawLabel(ctx, m_rows[i].name.AsView(), Rectangle{4, y, Width() - 60, kLineHeight}, palette.text);
                DrawLabel(ctx, FormatFixed(m_rows[i].msPerFrame, 2, Allocator()).AsView(),
                          Rectangle{Width() - 58, y, 56, kLineHeight}, palette.text, fonts::TextAlignment::Right);
            }
        }

    protected:
        void OnMeasure(foundation::ui::BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(300.0f),
                                  constraints.ConstrainHeight(static_cast<f32>(m_rowCount + 1) * kLineHeight)};
        }

        void Compute() override
        {
            const IProfileSource& source = SourceOrEmpty();
            CollectTop(source, Range(), m_lane, m_rowCount, m_entries);
            m_rows.Clear();
            const f32 longest = m_entries.IsEmpty() ? 0.0f : m_entries[0].msPerFrame;
            for (const TopEntry& entry : m_entries)
            {
                Row row;
                row.name = String(source.NameOf(entry.name), Allocator());
                row.msPerFrame = entry.msPerFrame;
                row.callsPerFrame = entry.callsPerFrame;
                row.bar = longest > 0.0f ? entry.msPerFrame / longest : 0.0f;
                m_rows.PushBack(Move(row));
            }
        }

    private:
        TopLane m_lane;
        u32 m_rowCount;
        Array<TopEntry> m_entries;
        Array<Row> m_rows;
    };
}
