// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :table_view partition
//
// TableView: a virtualised table over a model (eepp's UITableView in role). ItemView realises
// its rows; a row's content is one cell (`tablecell`) per column. Its header (`tableheader`) shows
// the column names and stays at the top while the rows scroll under it; a click on a header cell
// reports its column, or with nothing listening sorts by it where the model can sort. Column
// widths are explicit (SetColumnWidth) or an equal share of what is left.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:table_view;

import foundation.core; // RefPtr, MakeRef, Array, Function, Move, Max
import :rect;
import :event;
import :node;
import :label;
import :text; // TextHAlign
import :ui_widget;
import :rectangle_drawable;
import :model_index;
import :model;
import :item_view;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class TableView;

    // A clickable column header cell (reports its column on a click).
    class TableHeaderCell : public Label
    {
        RTTI_OBJECT(TableHeaderCell, Label)
    public:
        TableHeaderCell()
        {
            SetTag(core::StringView(u8"tableheadercell"));
            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
            SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
        }
        void SetColumn(usize column) noexcept { m_column = column; }
        void SetOnClicked(core::Function<void(usize)> callback) { m_onClicked = core::Move(callback); }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            if (m_onClicked)
                m_onClicked(m_column);
        }

    private:
        usize m_column = 0;
        core::Function<void(usize)> m_onClicked;
    };

    // A table row's content: one cell per column, placed by the table's widths.
    class TableRowContent : public UIWidget
    {
        RTTI_OBJECT(TableRowContent, UIWidget)
    public:
        explicit TableRowContent(const TableView& table) : m_table(table) { SetTag(core::StringView(u8"tablerowcontent")); }

        [[nodiscard]] usize CellCount() const noexcept { return m_cells.Size(); }
        [[nodiscard]] Label* CellAt(usize i) const { return i < m_cells.Size() ? m_cells[i].Get() : nullptr; }
        /// As many cells as `count`.
        void EnsureCells(usize count)
        {
            while (m_cells.Size() < count)
            {
                auto cell = core::MakeRef<Label>(MemoryAllocator());
                cell->SetTag(core::StringView(u8"tablecell")); // a row's, not a sheet's generic label
                StyleWriteScope defaults(*cell);
                cell->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
                cell->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
                cell->SetHitTestVisible(false); // a press on it is the row's
                ShareTextStyle(*cell);
                AddChild(cell.Get());
                m_cells.PushBack(core::Move(cell));
            }
            for (usize i = 0; i < m_cells.Size(); ++i)
                m_cells[i]->SetDisplayed(i < count);
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2 available) override;

    protected:
        void OnLayout() override;
        void OnTextStyleChange() override
        {
            for (const core::RefPtr<Label>& cell : m_cells)
                ShareTextStyle(*cell);
        }

    private:
        const TableView& m_table; // the view whose row holds it
        Array<core::RefPtr<Label>> m_cells;
    };

    class TableView : public ItemView
    {
        RTTI_OBJECT(TableView, ItemView)
    public:
        TableView()
        {
            SetTag(core::StringView(u8"tableview"));
            m_header = core::MakeRef<UIWidget>(MemoryAllocator());
            m_header->SetTag(core::StringView(u8"tableheader"));
            m_header->SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_headerColor));
            AddChild(m_header.Get());
            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetTextColor(Color{0.86f, 0.89f, 0.94f, 1.0f});
        }

        void SetHeaderHeight(f32 height)
        {
            m_headerHeight = core::Max(0.0f, height);
            RequestLayout();
        }
        void SetColumnWidth(usize column, f32 width)
        {
            while (m_columnWidths.Size() <= column)
                m_columnWidths.PushBack(0.0f);
            m_columnWidths[column] = width;
            RequestLayout();
        }
        /// A column's width in the last layout.
        [[nodiscard]] f32 GetColumnWidth(usize column) const { return column < m_widths.Size() ? m_widths[column] : 0.0f; }
        [[nodiscard]] usize GetColumnCount() const { return GetModel() != nullptr ? GetModel()->ColumnCount() : 0; }

        // Header clicks (a column); with none set, a click sorts where the model can.
        void SetOnColumnHeaderClicked(core::Function<void(usize)> callback) { m_onHeaderClicked = core::Move(callback); }
        [[nodiscard]] Node* GetHeader() const noexcept { return m_header.Get(); }

    protected:
        // Its header cells write in its font (their colour is their own: tableheadercell { color }).
        void OnTextStyleChange() override
        {
            for (const core::RefPtr<TableHeaderCell>& h : m_headerCells)
                ShareTextStyle(*h, false);
            ItemView::OnTextStyleChange();
        }
        [[nodiscard]] core::StringView RowTag() const override { return core::StringView(u8"tablerow"); }
        [[nodiscard]] f32 ContentTopInset() const override { return m_headerHeight; }
        void OnModelChanged() override { SyncHeader(); }
        // The columns' widths for this layout, from the content box's width.
        [[nodiscard]] f32 RowWidth(f32 width) const override
        {
            ComputeColumnWidths(width);
            return width;
        }

        [[nodiscard]] core::RefPtr<Node> CreateContent(u32) override { return core::MakeRef<TableRowContent>(MemoryAllocator(), *this); }
        void BindContent(Node& content, i32 item, const ModelIndex&) override
        {
            TableRowContent& row = static_cast<TableRowContent&>(content);
            const usize columns = GetColumnCount();
            row.EnsureCells(columns);
            for (usize c = 0; c < columns; ++c)
                row.CellAt(c)->SetText(GetModel()->Data(GetModel()->Index(item, static_cast<i32>(c))).ToString().AsView());
        }
        [[nodiscard]] Rect EditRect(const ItemRow& row, i32 column) const override
        {
            const Rect box = row.GetContentBounds();
            f32 x = box.x;
            for (i32 c = 0; c < column && static_cast<usize>(c) < m_widths.Size(); ++c)
                x += m_widths[static_cast<usize>(c)];
            return Rect{x, box.y, GetColumnWidth(static_cast<usize>(core::Max(0, column))), box.height};
        }

        // The header lies at the top of the view, over the rows scrolled under it.
        void OnLayoutDecorations() override
        {
            const Rect box = GetContentBounds();
            m_header->SetVisible(m_headerHeight > 0.0f);
            m_header->Arrange(Rect{box.x, box.y + GetScrollOffset().y, box.width, m_headerHeight});
            f32 x = 0.0f;
            for (usize c = 0; c < m_headerCells.Size(); ++c)
            {
                const f32 w = GetColumnWidth(c);
                m_headerCells[c]->Arrange(Rect{x, 0.0f, w, m_headerHeight});
                x += w;
            }
            if (GetChildIndex(m_header.Get()) + 1 != ChildCount())
                m_header->ToFront(); // drawn, and pressed, over the rows
        }

    private:
        friend class TableRowContent;

        // The header's cells follow the columns (cells kept while the columns stay, so a press on
        // one that sorts does not free the cell under the pointer).
        void SyncHeader()
        {
            const usize columns = GetColumnCount();
            while (m_headerCells.Size() > columns)
            {
                m_headerCells[m_headerCells.Size() - 1]->RemoveFromParent();
                m_headerCells.RemoveAt(m_headerCells.Size() - 1);
            }
            TableView* self = this;
            while (m_headerCells.Size() < columns)
            {
                auto cell = core::MakeRef<TableHeaderCell>(MemoryAllocator());
                cell->SetColumn(m_headerCells.Size());
                ShareTextStyle(*cell, false);
                {
                    StyleWriteScope defaults(*cell);
                    cell->SetTextColor(m_headerTextColor);
                }
                cell->SetOnClicked([self](usize column) { self->HeaderClicked(column); });
                m_header->AddChild(cell.Get());
                m_headerCells.PushBack(core::Move(cell));
            }
            for (usize c = 0; c < columns; ++c)
                m_headerCells[c]->SetText(GetModel()->ColumnName(c).AsView());
        }
        void HeaderClicked(usize column)
        {
            if (m_onHeaderClicked)
            {
                m_onHeaderClicked(column);
                return;
            }
            // Nothing listening: sort by it where the model can (a new column ascending, the
            // same one turning over).
            const SortOrder order = m_sortColumn == static_cast<i32>(column) && m_sortOrder == SortOrder::Ascending ? SortOrder::Descending : SortOrder::Ascending;
            if (GetModel() != nullptr && GetModel()->Sort(column, order))
            {
                m_sortColumn = static_cast<i32>(column);
                m_sortOrder = order;
            }
        }

        // Explicit widths where set (>0), else an equal share of the rest among the autos.
        void ComputeColumnWidths(f32 bodyWidth) const
        {
            const usize columns = GetColumnCount();
            m_widths.Clear();
            f32 explicitTotal = 0.0f;
            usize autoCount = 0;
            for (usize c = 0; c < columns; ++c)
            {
                const f32 w = c < m_columnWidths.Size() ? m_columnWidths[c] : 0.0f;
                if (w > 0.0f)
                    explicitTotal += w;
                else
                    ++autoCount;
            }
            const f32 autoWidth = autoCount > 0 ? core::Max(0.0f, (bodyWidth - explicitTotal) / static_cast<f32>(autoCount)) : 0.0f;
            for (usize c = 0; c < columns; ++c)
            {
                const f32 w = c < m_columnWidths.Size() ? m_columnWidths[c] : 0.0f;
                m_widths.PushBack(w > 0.0f ? w : autoWidth);
            }
        }

        core::RefPtr<UIWidget> m_header;
        Array<core::RefPtr<TableHeaderCell>> m_headerCells;
        Array<f32> m_columnWidths;     // 0 = auto
        mutable Array<f32> m_widths;   // resolved for the current layout (RowWidth works them out)
        core::Function<void(usize)> m_onHeaderClicked;
        i32 m_sortColumn = -1;
        SortOrder m_sortOrder = SortOrder::None;
        f32 m_headerHeight = 26.0f;
        Color m_headerColor{0.20f, 0.22f, 0.27f, 1.0f};
        Color m_headerTextColor{0.86f, 0.89f, 0.94f, 1.0f};
    };

    inline core::Float2 TableRowContent::MeasureContent(core::Float2 available)
    {
        // As tall as its tallest cell at its column's width.
        f32 height = 0.0f;
        for (usize c = 0; c < m_cells.Size(); ++c)
            if (m_cells[c]->IsDisplayed())
            {
                const f32 w = m_table.GetColumnWidth(c);
                height = core::Max(height, m_cells[c]->Measure(LayoutConstraints{core::Float2{w, 0.0f}, core::Float2{w, kUnbounded}, core::Float2{w, -1.0f}}).y);
            }
        return core::Float2{available.x, height};
    }

    inline void TableRowContent::OnLayout()
    {
        const Rect box = GetContentBounds();
        f32 x = box.x;
        for (usize c = 0; c < m_cells.Size(); ++c)
        {
            if (!m_cells[c]->IsDisplayed())
                continue;
            const f32 w = m_table.GetColumnWidth(c);
            m_cells[c]->Arrange(Rect{x, box.y, w, box.height});
            x += w;
        }
    }

    RTTI_DEFINE_OBJECT(TableHeaderCell, "rtti::gui")
    RTTI_DEFINE_OBJECT(TableRowContent, "rtti::gui")
    RTTI_DEFINE_OBJECT(TableView, "rtti::gui")
}
