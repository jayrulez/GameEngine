// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.UI - :scope_table partition.
///
/// The selected frames' scopes and GPU passes by name (Specs/profiler.md, P5): their total time,
/// their own time (children taken out) and how many there were, in ms over the selection. A click
/// on a column's heading sorts by it (again: the other way); a click on a row selects that name,
/// which the panel's timeline highlights; the wheel scrolls the rows.

module;
#include "Core/Prelude.h"

export module foundation.profiler.ui:scope_table;

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
    class ScopeTable final : public ProfilerView
    {
    public:
        static constexpr f32 kNumberWidth = 72.0f; // each number column
        static constexpr f32 kRowHeight = 18.0f;

        enum class Column : u8
        {
            Name,
            Total,
            Self,
            Count,
        };

        struct Row
        {
            NameId name = 0;
            bool gpu = false;
            String label;
            f64 totalMs = 0.0;
            f64 selfMs = 0.0;
            u64 count = 0;
        };

        explicit ScopeTable(IAllocator& allocator) : ProfilerView(allocator), m_totals(allocator), m_rows(allocator)
        {
            IsHitTestVisible = true; // sorts, selects and scrolls
        }

        /// Total these frames.
        void SetFrames(FrameRange frames)
        {
            m_frames = frames;
            m_scroll = 0;
            Refresh();
        }

        /// Sort by `column`, the largest (or Z) first when `descending`.
        void SortBy(Column column, bool descending)
        {
            m_sort = column;
            m_descending = descending;
            Refresh();
        }
        [[nodiscard]] Column SortColumn() const noexcept { return m_sort; }
        [[nodiscard]] bool SortDescending() const noexcept { return m_descending; }

        /// The model: the rows in their order, and the selected one (-1: none).
        [[nodiscard]] Span<const Row> Rows() const noexcept { return m_rows.AsSpan(); }
        [[nodiscard]] i32 SelectedRow() const noexcept { return m_selected; }

        /// Select a row (-1: none); who hears gets its name and lane, or nothing for none.
        void SelectRow(i32 row)
        {
            m_selected = row >= 0 && static_cast<usize>(row) < m_rows.Size() ? row : -1;
            m_selectedName = m_selected >= 0 ? m_rows[static_cast<usize>(m_selected)].name : 0;
            m_selectedGpu = m_selected >= 0 && m_rows[static_cast<usize>(m_selected)].gpu;
            m_hasSelection = m_selected >= 0;
            InvalidateVisual();
            if (onSelect)
            {
                onSelect(m_selected >= 0 ? &m_rows[static_cast<usize>(m_selected)] : nullptr);
            }
        }
        Function<void(const Row*)> onSelect;

        /// The column a heading's x falls in, and the row a point's y does (-1: the heading or none).
        [[nodiscard]] Column ColumnAt(f32 x) const noexcept
        {
            const f32 numbers = Width() - 3 * kNumberWidth;
            if (x < numbers)
            {
                return Column::Name;
            }
            const u32 index = static_cast<u32>((x - numbers) / kNumberWidth);
            return index == 0 ? Column::Total : index == 1 ? Column::Self : Column::Count;
        }
        [[nodiscard]] i32 RowAt(f32 y) const noexcept
        {
            if (y < kRowHeight)
            {
                return -1;
            }
            const i32 row = static_cast<i32>((y - kRowHeight) / kRowHeight) + m_scroll;
            return row < static_cast<i32>(m_rows.Size()) ? row : -1;
        }

        void OnMouseDown(foundation::ui::MouseEventArgs& e) override
        {
            if (e.Button != foundation::ui::MouseButton::Left)
            {
                return;
            }
            if (e.Y < kRowHeight)
            {
                const Column column = ColumnAt(e.X);
                SortBy(column, column == m_sort ? !m_descending : column != Column::Name);
            }
            else
            {
                SelectRow(RowAt(e.Y));
            }
            e.Handled = true;
        }

        void OnMouseWheel(foundation::ui::MouseWheelEventArgs& e) override
        {
            const i32 visible = static_cast<i32>(Height() / kRowHeight) - 1;
            const i32 most = Max(static_cast<i32>(m_rows.Size()) - visible, 0);
            m_scroll = Clamp(m_scroll - static_cast<i32>(e.DeltaY * 3.0f), 0, most);
            InvalidateVisual();
            e.Handled = true;
        }

        void OnDraw(foundation::ui::UIDrawContext& ctx) override
        {
            const f32 w = Width();
            const f32 h = Height();
            ctx.VG().FillRect(Rectangle{0, 0, w, h}, palette.plot);
            ctx.VG().PushClipRect(Rectangle{0, 0, w, h});
            const f32 numbers = w - 3 * kNumberWidth;
            const StringView headings[] = {u8"Name", u8"Total ms", u8"Self ms", u8"Count"};
            for (u32 c = 0; c < 4; ++c)
            {
                String heading(headings[c], Allocator());
                if (static_cast<Column>(c) == m_sort)
                {
                    heading.Append(m_descending ? StringView(u8" v") : StringView(u8" ^"));
                }
                const Rectangle cell = c == 0 ? Rectangle{6, 0, numbers - 12, kRowHeight}
                                              : Rectangle{numbers + static_cast<f32>(c - 1) * kNumberWidth, 0,
                                                          kNumberWidth - 6, kRowHeight};
                DrawLabel(ctx, heading.AsView(), cell, palette.dim,
                          c == 0 ? fonts::TextAlignment::Left : fonts::TextAlignment::Right);
            }
            ctx.VG().DrawLine(Float2{0, kRowHeight}, Float2{w, kRowHeight}, palette.grid, 1.0f);
            for (i32 i = m_scroll; i < static_cast<i32>(m_rows.Size()); ++i)
            {
                const f32 y = kRowHeight + static_cast<f32>(i - m_scroll) * kRowHeight;
                if (y > h)
                {
                    break;
                }
                const Row& row = m_rows[static_cast<usize>(i)];
                if (i == m_selected)
                {
                    ctx.VG().FillRect(Rectangle{0, y, w, kRowHeight}, palette.selection);
                }
                ctx.VG().FillRect(Rectangle{4, y + 5, 4, kRowHeight - 10}, row.gpu ? palette.gpu : palette.cpu);
                DrawLabel(ctx, row.label.AsView(), Rectangle{12, y, numbers - 18, kRowHeight}, palette.text);
                String count(Allocator());
                AppendFormat(count, u8"{}", row.count);
                const String values[] = {FormatFixed(row.totalMs, 3, Allocator()), FormatFixed(row.selfMs, 3, Allocator()),
                                         Move(count)};
                for (u32 c = 0; c < 3; ++c)
                {
                    DrawLabel(ctx, values[c].AsView(),
                              Rectangle{numbers + static_cast<f32>(c) * kNumberWidth, y, kNumberWidth - 6, kRowHeight},
                              palette.text, fonts::TextAlignment::Right);
                }
            }
            ctx.VG().PopClip();
        }

    protected:
        void OnMeasure(foundation::ui::BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(360.0f), constraints.ConstrainHeight(240.0f)};
        }

        void Compute() override
        {
            const IProfileSource& source = SourceOrEmpty();
            CollectTotals(source, m_frames, m_totals);
            m_rows.Clear();
            const u64 frequency = source.TickFrequency();
            auto add = [&](const NameTotal& total)
            {
                Row row;
                row.name = total.name;
                row.gpu = total.gpu;
                row.label = String(source.NameOf(total.name), Allocator());
                row.totalMs = TicksToMs(total.totalTicks, frequency);
                row.selfMs = TicksToMs(total.selfTicks, frequency);
                row.count = total.count;
                m_rows.PushBack(Move(row));
            };
            for (const NameTotal& total : m_totals.scopes)
            {
                add(total);
            }
            for (const NameTotal& total : m_totals.passes)
            {
                add(total);
            }
            const Column sort = m_sort;
            const bool descending = m_descending;
            m_rows.Sort(
                [sort, descending](const Row& a, const Row& b)
                {
                    bool less = false;
                    switch (sort)
                    {
                    case Column::Name: less = a.label.AsView().Compare(b.label.AsView()) < 0; break;
                    case Column::Total: less = a.totalMs < b.totalMs; break;
                    case Column::Self: less = a.selfMs < b.selfMs; break;
                    case Column::Count: less = a.count < b.count; break;
                    }
                    bool greater = false;
                    switch (sort)
                    {
                    case Column::Name: greater = b.label.AsView().Compare(a.label.AsView()) < 0; break;
                    case Column::Total: greater = b.totalMs < a.totalMs; break;
                    case Column::Self: greater = b.selfMs < a.selfMs; break;
                    case Column::Count: greater = b.count < a.count; break;
                    }
                    return descending ? greater : less;
                });
            // The selection follows its name through a re-sort or new frames.
            m_selected = -1;
            if (m_hasSelection)
            {
                for (usize i = 0; i < m_rows.Size(); ++i)
                {
                    if (m_rows[i].name == m_selectedName && m_rows[i].gpu == m_selectedGpu)
                    {
                        m_selected = static_cast<i32>(i);
                    }
                }
            }
        }

    private:
        RangeTotals m_totals;
        Array<Row> m_rows;
        FrameRange m_frames;
        Column m_sort = Column::Self;
        bool m_descending = true;
        i32 m_selected = -1;
        NameId m_selectedName = 0;
        bool m_selectedGpu = false;
        bool m_hasSelection = false;
        i32 m_scroll = 0;
    };
}
