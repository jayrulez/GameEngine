// Draconic UI - :grid_layout partition
//
// Row/column grid with Auto/Fixed/Flex track sizing and auto-flow placement. Ported from
// Sedulous.UI/src/Layout/GridLayout.bf. (Beef nested LayoutParams -> GridLayoutParams; scope float[]
// -> Array<f32>; Math.Clamp -> local clamp helpers.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:grid_layout;

import draconic.foundation; // Max, Min, Array
import :view;
import :layout_params;
import :box_constraints;

using namespace draconic::foundation;

namespace draconic::ui::detail
{
    [[nodiscard]] inline i32 ClampI(i32 v, i32 lo, i32 hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}

export namespace draconic::ui
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

    /// LayoutParams for a GridLayout child (row/column placement + spans).
    class GridLayoutParams : public LayoutParams
    {
        DRACONIC_OBJECT(GridLayoutParams, LayoutParams)
    public:
        i32 Row = -1;    ///< -1 = auto-flow.
        i32 Column = -1; ///< -1 = auto-flow.
        i32 RowSpan = 1;
        i32 ColumnSpan = 1;
        GridLayoutParams() = default;
    };

    class GridLayout : public ViewGroup
    {
        DRACONIC_OBJECT(GridLayout, ViewGroup)
    public:
        Array<TrackSize> Columns;
        Array<TrackSize> Rows;
        f32 ColumnSpacing = 0.0f;
        f32 RowSpacing = 0.0f;
        bool AutoFlow = true;

        GridLayout() = default;

    protected:
        LayoutParamsPtr CreateDefaultLayoutParams() override
        {
            return MakeRef<GridLayoutParams>(DefaultAllocator());
        }

        void OnMeasure(BoxConstraints constraints) override
        {
            const i32 cols = ColCount();
            const i32 rows = RowCount();
            if (AutoFlow)
            {
                AssignAutoFlow(cols, rows);
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
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                GridLayoutParams* glp = Cast<GridLayoutParams>(child->LayoutParams.Get());
                const i32 col = detail::ClampI(glp != nullptr ? glp->Column : 0, 0, cols - 1);
                const i32 row = detail::ClampI(glp != nullptr ? glp->Row : 0, 0, rows - 1);
                child->Measure(BoxConstraints::Expand());

                const TrackSize colDef = static_cast<usize>(col) < Columns.Size()
                                             ? Columns[static_cast<usize>(col)]
                                             : TrackSize::Auto();
                const TrackSize rowDef = static_cast<usize>(row) < Rows.Size()
                                             ? Rows[static_cast<usize>(row)]
                                             : TrackSize::Auto();
                if (colDef.Mode == TrackSizeMode::Auto)
                {
                    colWidths[static_cast<usize>(col)] =
                        Max(colWidths[static_cast<usize>(col)], child->MeasuredSize.x);
                }
                if (rowDef.Mode == TrackSizeMode::Auto)
                {
                    rowHeights[static_cast<usize>(row)] =
                        Max(rowHeights[static_cast<usize>(row)], child->MeasuredSize.y);
                }
            }

            const f32 totalAvailW = constraints.MaxWidth - Padding.TotalHorizontal() -
                                    ColumnSpacing * static_cast<f32>(Max(0, cols - 1));
            const f32 totalAvailH = constraints.MaxHeight - Padding.TotalVertical() -
                                    RowSpacing * static_cast<f32>(Max(0, rows - 1));
            DistributeFlex(Columns, colWidths, cols, totalAvailW);
            DistributeFlex(Rows, rowHeights, rows, totalAvailH);

            f32 totalW =
                Padding.TotalHorizontal() + ColumnSpacing * static_cast<f32>(Max(0, cols - 1));
            f32 totalH = Padding.TotalVertical() + RowSpacing * static_cast<f32>(Max(0, rows - 1));
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
            (void)left;
            (void)top;
            const i32 cols = ColCount();
            const i32 rows = RowCount();

            Array<f32> colWidths;
            colWidths.Resize(static_cast<usize>(cols), 0.0f);
            Array<f32> rowHeights;
            rowHeights.Resize(static_cast<usize>(rows), 0.0f);
            InitFixedTracks(Columns, colWidths, cols);
            InitFixedTracks(Rows, rowHeights, rows);

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                GridLayoutParams* glp = Cast<GridLayoutParams>(child->LayoutParams.Get());
                const i32 col = detail::ClampI(glp != nullptr ? glp->Column : 0, 0, cols - 1);
                const i32 row = detail::ClampI(glp != nullptr ? glp->Row : 0, 0, rows - 1);
                const TrackSize colDef = static_cast<usize>(col) < Columns.Size()
                                             ? Columns[static_cast<usize>(col)]
                                             : TrackSize::Auto();
                const TrackSize rowDef = static_cast<usize>(row) < Rows.Size()
                                             ? Rows[static_cast<usize>(row)]
                                             : TrackSize::Auto();
                if (colDef.Mode == TrackSizeMode::Auto)
                {
                    colWidths[static_cast<usize>(col)] =
                        Max(colWidths[static_cast<usize>(col)], child->MeasuredSize.x);
                }
                if (rowDef.Mode == TrackSizeMode::Auto)
                {
                    rowHeights[static_cast<usize>(row)] =
                        Max(rowHeights[static_cast<usize>(row)], child->MeasuredSize.y);
                }
            }

            const f32 contentW = width - Padding.TotalHorizontal() -
                                 ColumnSpacing * static_cast<f32>(Max(0, cols - 1));
            const f32 contentH =
                height - Padding.TotalVertical() - RowSpacing * static_cast<f32>(Max(0, rows - 1));
            DistributeFlex(Columns, colWidths, cols, contentW);
            DistributeFlex(Rows, rowHeights, rows, contentH);

            Array<f32> colX;
            colX.Resize(static_cast<usize>(cols), 0.0f);
            Array<f32> rowY;
            rowY.Resize(static_cast<usize>(rows), 0.0f);
            colX[0] = Padding.Left;
            for (i32 c = 1; c < cols; ++c)
            {
                colX[static_cast<usize>(c)] = colX[static_cast<usize>(c - 1)] +
                                              colWidths[static_cast<usize>(c - 1)] + ColumnSpacing;
            }
            rowY[0] = Padding.Top;
            for (i32 r = 1; r < rows; ++r)
            {
                rowY[static_cast<usize>(r)] = rowY[static_cast<usize>(r - 1)] +
                                              rowHeights[static_cast<usize>(r - 1)] + RowSpacing;
            }

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                GridLayoutParams* glp = Cast<GridLayoutParams>(child->LayoutParams.Get());
                const i32 col = detail::ClampI(glp != nullptr ? glp->Column : 0, 0, cols - 1);
                const i32 row = detail::ClampI(glp != nullptr ? glp->Row : 0, 0, rows - 1);
                const i32 colSpan =
                    detail::ClampI(glp != nullptr ? glp->ColumnSpan : 1, 1, cols - col);
                const i32 rowSpan =
                    detail::ClampI(glp != nullptr ? glp->RowSpan : 1, 1, rows - row);

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

        void AssignAutoFlow(i32 cols, i32 rows)
        {
            (void)rows;
            i32 nextRow = 0, nextCol = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                GridLayoutParams* glp = Cast<GridLayoutParams>(child->LayoutParams.Get());
                if (glp == nullptr)
                {
                    continue;
                }
                if (glp->Row < 0 || glp->Column < 0)
                {
                    glp->Row = nextRow;
                    glp->Column = nextCol;
                    ++nextCol;
                    if (nextCol >= cols)
                    {
                        nextCol = 0;
                        ++nextRow;
                    }
                }
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

    DRACONIC_DEFINE_OBJECT(GridLayoutParams, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(GridLayout, "draconic::ui")
}
