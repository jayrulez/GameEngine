// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :grid_layout partition
//
// Row/column grid with Auto/Fixed/Flex track sizing and auto-flow placement. Per-child placement
// comes from the child's LayoutStyle (GridRow/GridColumn/spans); auto-flow cells are resolved per
// pass, never written back.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:grid_layout;

import foundation.core; // Max, Min, Array
import :view;
import :layout_style;
import :box_constraints;

using namespace foundation::core;

namespace foundation::ui::detail
{
    [[nodiscard]] inline i32 ClampI(i32 v, i32 lo, i32 hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}

export namespace foundation::ui
{
    /// Grid track sizing mode.
    enum class TrackSizeMode
    {
        Auto,
        Fixed,
        Flex
    };

    /// Size of a grid track (row or column).
    struct TrackSize
    {
        TrackSizeMode Mode = TrackSizeMode::Auto;
        f32 Value = 0.0f;

        [[nodiscard]] static TrackSize Auto() { return TrackSize{TrackSizeMode::Auto, 0.0f}; }
        [[nodiscard]] static TrackSize Fixed(f32 px) { return TrackSize{TrackSizeMode::Fixed, px}; }
        [[nodiscard]] static TrackSize Flex(f32 weight = 1.0f)
        {
            return TrackSize{TrackSizeMode::Flex, weight};
        }
    };

    class GridLayout : public ViewGroup
    {
        RTTI_OBJECT(GridLayout, ViewGroup)
    public:
        Array<TrackSize> Columns;
        Array<TrackSize> Rows;
        f32 ColumnSpacing = 0.0f;
        f32 RowSpacing = 0.0f;
        bool AutoFlow = true;

        GridLayout() = default;

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const i32 cols = ColCount();
            const i32 rows = RowCount();
            ResolvePlacements(cols, rows);

            Array<f32> colWidths;
            colWidths.Resize(static_cast<usize>(cols), 0.0f);
            Array<f32> rowHeights;
            rowHeights.Resize(static_cast<usize>(rows), 0.0f);
            InitFixedTracks(Columns, colWidths, cols);
            InitFixedTracks(Rows, rowHeights, rows);

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                const Cell cell = m_cells[i];
                const i32 col = cell.Column;
                const i32 row = cell.Row;
                // Bounded loose constraints (unbounded fill-style leaves would measure to
                // kFloatMax and break auto tracks); tracks aggregate margin boxes so cell
                // placement and the base margin inset compose.
                child->Measure(BoxConstraints{
                    0, Max(0.0f, constraints.MaxWidth - chrome.TotalHorizontal()), 0,
                    Max(0.0f, constraints.MaxHeight - chrome.TotalVertical())});
                const Float2 mb = child->MarginBoxSize();

                const TrackSize colDef = static_cast<usize>(col) < Columns.Size()
                                             ? Columns[static_cast<usize>(col)]
                                             : TrackSize::Auto();
                const TrackSize rowDef = static_cast<usize>(row) < Rows.Size()
                                             ? Rows[static_cast<usize>(row)]
                                             : TrackSize::Auto();
                if (colDef.Mode == TrackSizeMode::Auto)
                {
                    colWidths[static_cast<usize>(col)] =
                        Max(colWidths[static_cast<usize>(col)], mb.x);
                }
                if (rowDef.Mode == TrackSizeMode::Auto)
                {
                    rowHeights[static_cast<usize>(row)] =
                        Max(rowHeights[static_cast<usize>(row)], mb.y);
                }
            }

            const f32 totalAvailW = constraints.MaxWidth - chrome.TotalHorizontal() -
                                    ColumnSpacing * static_cast<f32>(Max(0, cols - 1));
            const f32 totalAvailH = constraints.MaxHeight - chrome.TotalVertical() -
                                    RowSpacing * static_cast<f32>(Max(0, rows - 1));
            DistributeFlex(Columns, colWidths, cols, totalAvailW);
            DistributeFlex(Rows, rowHeights, rows, totalAvailH);

            f32 totalW =
                chrome.TotalHorizontal() + ColumnSpacing * static_cast<f32>(Max(0, cols - 1));
            f32 totalH = chrome.TotalVertical() + RowSpacing * static_cast<f32>(Max(0, rows - 1));
            for (f32 w : colWidths)
            {
                totalW += w;
            }
            for (f32 h : rowHeights)
            {
                totalH += h;
            }

            MeasuredSize =
                Float2{constraints.ConstrainWidth(totalW), constraints.ConstrainHeight(totalH)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            (void)left;
            (void)top;
            const i32 cols = ColCount();
            const i32 rows = RowCount();
            if (m_cells.Size() != ChildCount())
            {
                ResolvePlacements(cols, rows); // layout without a preceding measure
            }

            Array<f32> colWidths;
            colWidths.Resize(static_cast<usize>(cols), 0.0f);
            Array<f32> rowHeights;
            rowHeights.Resize(static_cast<usize>(rows), 0.0f);
            InitFixedTracks(Columns, colWidths, cols);
            InitFixedTracks(Rows, rowHeights, rows);

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                const Cell cell = m_cells[i];
                const i32 col = cell.Column;
                const i32 row = cell.Row;
                const TrackSize colDef = static_cast<usize>(col) < Columns.Size()
                                             ? Columns[static_cast<usize>(col)]
                                             : TrackSize::Auto();
                const TrackSize rowDef = static_cast<usize>(row) < Rows.Size()
                                             ? Rows[static_cast<usize>(row)]
                                             : TrackSize::Auto();
                const Float2 mb = child->MarginBoxSize();
                if (colDef.Mode == TrackSizeMode::Auto)
                {
                    colWidths[static_cast<usize>(col)] =
                        Max(colWidths[static_cast<usize>(col)], mb.x);
                }
                if (rowDef.Mode == TrackSizeMode::Auto)
                {
                    rowHeights[static_cast<usize>(row)] =
                        Max(rowHeights[static_cast<usize>(row)], mb.y);
                }
            }

            const f32 contentW = width - chrome.TotalHorizontal() -
                                 ColumnSpacing * static_cast<f32>(Max(0, cols - 1));
            const f32 contentH =
                height - chrome.TotalVertical() - RowSpacing * static_cast<f32>(Max(0, rows - 1));
            DistributeFlex(Columns, colWidths, cols, contentW);
            DistributeFlex(Rows, rowHeights, rows, contentH);

            Array<f32> colX;
            colX.Resize(static_cast<usize>(cols), 0.0f);
            Array<f32> rowY;
            rowY.Resize(static_cast<usize>(rows), 0.0f);
            colX[0] = chrome.Left;
            for (i32 c = 1; c < cols; ++c)
            {
                colX[static_cast<usize>(c)] = colX[static_cast<usize>(c - 1)] +
                                              colWidths[static_cast<usize>(c - 1)] + ColumnSpacing;
            }
            rowY[0] = chrome.Top;
            for (i32 r = 1; r < rows; ++r)
            {
                rowY[static_cast<usize>(r)] = rowY[static_cast<usize>(r - 1)] +
                                              rowHeights[static_cast<usize>(r - 1)] + RowSpacing;
            }

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                const Cell cell = m_cells[i];
                const i32 col = cell.Column;
                const i32 row = cell.Row;
                const i32 colSpan = cell.ColumnSpan;
                const i32 rowSpan = cell.RowSpan;

                f32 cellW = 0;
                for (i32 c = col; c < col + colSpan; ++c)
                {
                    cellW += colWidths[static_cast<usize>(c)];
                    if (c > col)
                    {
                        cellW += ColumnSpacing;
                    }
                }
                f32 cellH = 0;
                for (i32 r = row; r < row + rowSpan; ++r)
                {
                    cellH += rowHeights[static_cast<usize>(r)];
                    if (r > row)
                    {
                        cellH += RowSpacing;
                    }
                }

                child->Layout(colX[static_cast<usize>(col)], rowY[static_cast<usize>(row)], cellW,
                              cellH);
            }
        }

    private:
        [[nodiscard]] i32 ColCount() const
        {
            return static_cast<i32>(Max<usize>(1, Columns.Size()));
        }
        [[nodiscard]] i32 RowCount() const { return static_cast<i32>(Max<usize>(1, Rows.Size())); }

        /// A child's resolved cell for this pass. Auto-flow children (GridRow/GridColumn < 0)
        /// take the next free cursor cell; the child's LayoutStyle keeps its -1 intent, so
        /// reordering or reparenting re-flows instead of pinning the first placement.
        struct Cell
        {
            i32 Row = 0;
            i32 Column = 0;
            i32 RowSpan = 1;
            i32 ColumnSpan = 1;
        };
        Array<Cell> m_cells; ///< Index-aligned with the children; rebuilt each OnMeasure.

        void ResolvePlacements(i32 cols, i32 rows)
        {
            m_cells.Clear();
            m_cells.Resize(ChildCount(), Cell{});
            i32 nextRow = 0, nextCol = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                const LayoutStyle& ls = child->Layout();
                Cell cell;
                if (AutoFlow && (ls.GridRow < 0 || ls.GridColumn < 0))
                {
                    cell.Row = nextRow;
                    cell.Column = nextCol;
                    ++nextCol;
                    if (nextCol >= cols)
                    {
                        nextCol = 0;
                        ++nextRow;
                    }
                }
                else
                {
                    cell.Row = ls.GridRow;
                    cell.Column = ls.GridColumn;
                }
                cell.Column = detail::ClampI(cell.Column, 0, cols - 1);
                cell.Row = detail::ClampI(cell.Row, 0, rows - 1);
                cell.ColumnSpan = detail::ClampI(ls.GridColumnSpan, 1, cols - cell.Column);
                cell.RowSpan = detail::ClampI(ls.GridRowSpan, 1, rows - cell.Row);
                m_cells[i] = cell;
            }
        }

        static void InitFixedTracks(const Array<TrackSize>& defs, Array<f32>& sizes, i32 count)
        {
            for (i32 i = 0; i < count; ++i)
            {
                const TrackSize def = static_cast<usize>(i) < defs.Size()
                                          ? defs[static_cast<usize>(i)]
                                          : TrackSize::Auto();
                if (def.Mode == TrackSizeMode::Fixed)
                {
                    sizes[static_cast<usize>(i)] = def.Value;
                }
            }
        }

        static void DistributeFlex(const Array<TrackSize>& defs, Array<f32>& sizes, i32 count,
                                   f32 totalAvail)
        {
            f32 usedByFixed = 0, totalFlexWeight = 0;
            for (i32 i = 0; i < count; ++i)
            {
                const TrackSize def = static_cast<usize>(i) < defs.Size()
                                          ? defs[static_cast<usize>(i)]
                                          : TrackSize::Auto();
                if (def.Mode == TrackSizeMode::Flex)
                {
                    totalFlexWeight += def.Value;
                }
                else
                {
                    usedByFixed += sizes[static_cast<usize>(i)];
                }
            }
            if (totalFlexWeight > 0)
            {
                const f32 remaining = Max(0.0f, totalAvail - usedByFixed);
                for (i32 i = 0; i < count; ++i)
                {
                    const TrackSize def = static_cast<usize>(i) < defs.Size()
                                              ? defs[static_cast<usize>(i)]
                                              : TrackSize::Auto();
                    if (def.Mode == TrackSizeMode::Flex)
                    {
                        sizes[static_cast<usize>(i)] = remaining * def.Value / totalFlexWeight;
                    }
                }
            }
        }
    };

    RTTI_DEFINE_OBJECT(GridLayout, "rtti::ui")
}
