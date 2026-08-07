// Draconic GUI - :grid_layout partition
//
// GridLayout: flows its children into a fixed number of columns, wrapping to a new row when
// a row fills. Columns are evenly spaced across the padding-inset content width; each row is
// as tall as its tallest child. Modeled on eepp's UIGridLayout (role, not a line-for-line
// port). Children keep their own sizes and are placed at their cell's top-left; stretch-to-
// cell and weighted columns are deferred. Re-runs on size change and child add/remove.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:grid_layout;

import draconic.foundation; // Float2, Max
import :rect;
import :node;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class GridLayout : public UIWidget
    {
        DRACONIC_OBJECT(GridLayout, UIWidget)
    public:
        GridLayout() = default;

        // Number of columns (clamped to >= 1). Children flow left-to-right, top-to-bottom.
        void SetColumns(u32 columns)
        {
            m_columns = columns < 1u ? 1u : columns;
            PerformLayout();
        }
        [[nodiscard]] u32 GetColumns() const noexcept { return m_columns; }

        void SetSpacing(f32 horizontal, f32 vertical)
        {
            m_hSpacing = horizontal;
            m_vSpacing = vertical;
            PerformLayout();
        }
        [[nodiscard]] f32 GetHorizontalSpacing() const noexcept { return m_hSpacing; }
        [[nodiscard]] f32 GetVerticalSpacing() const noexcept { return m_vSpacing; }

        // The width of a single column cell (content width shared across the columns).
        [[nodiscard]] f32 CellWidth() const
        {
            const f32 total =
                GetContentBounds().width - static_cast<f32>(m_columns - 1u) * m_hSpacing;
            return foundation::Max(0.0f, total / static_cast<f32>(m_columns));
        }

        void PerformLayout()
        {
            const Rect content = GetContentBounds();
            const f32 cellW = CellWidth();

            u32 col = 0;
            f32 rowY = content.y;
            f32 rowMaxH = 0.0f;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                Node* child = GetChildAt(i);
                if (child == nullptr || !child->IsVisible())
                    continue;

                const f32 x = content.x + static_cast<f32>(col) * (cellW + m_hSpacing);
                child->SetPosition(foundation::Float2{x, rowY});
                rowMaxH = foundation::Max(rowMaxH, child->GetSize().y);

                if (++col >= m_columns) // row complete -> advance
                {
                    col = 0;
                    rowY += rowMaxH + m_vSpacing;
                    rowMaxH = 0.0f;
                }
            }
        }

    protected:
        void OnSizeChange() override { PerformLayout(); }
        void OnChildrenChanged() override { PerformLayout(); }

        u32 m_columns = 1;
        f32 m_hSpacing = 0.0f;
        f32 m_vSpacing = 0.0f;
    };

    DRACONIC_DEFINE_OBJECT(GridLayout, "draconic::gui")
}
