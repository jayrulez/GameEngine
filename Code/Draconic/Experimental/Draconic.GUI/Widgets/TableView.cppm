// Draconic GUI - :table_view partition
//
// TableView: a virtualized, multi-column model-backed table. Modeled on eepp's UITableView. The
// virtualization / scrolling / selection live in AbstractItemView; TableView adds the multiple
// columns (one hit-transparent cell Label per column in each row) and a fixed header row that
// shows the column names and reports header clicks (which a SortingProxyModel wires to sort).
// Column widths are explicit (SetColumnWidth) or an equal split of the body width for the auto
// columns. Multi-selection is inherited.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:table_view;

import draconic.foundation;  // RefPtr, MakeRef, Array, Function, Move, Max, Float2
import draconic.fonts; // CachedFont
import :rect;
import :event;
import :label;
import :ui_widget;
import :rectangle_drawable;
import :model_index;
import :model;
import :abstract_item_view;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    // A clickable column header cell (reports its column index on click).
    class TableHeaderCell : public Label
    {
        DRACONIC_OBJECT(TableHeaderCell, Label)
    public:
        TableHeaderCell()
        {
            SetTag(foundation::StringView(u8"tableheadercell"));
            SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
            SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
        }
        void SetColumn(usize column) noexcept { m_column = column; }
        void SetOnClicked(foundation::Function<void(usize)> callback)
        {
            m_onClicked = foundation::Move(callback);
        }

    protected:
        void OnMouseClick(const MouseEvent&) override
        {
            if (m_onClicked)
                m_onClicked(m_column);
        }

    private:
        usize m_column = 0;
        foundation::Function<void(usize)> m_onClicked;
    };

    // A table row: an ItemRow with one hit-transparent cell Label per column.
    class TableRow : public ItemRow
    {
        DRACONIC_OBJECT(TableRow, ItemRow)
    public:
        TableRow() { SetTag(foundation::StringView(u8"tablerow")); }

        [[nodiscard]] usize CellCount() const noexcept { return m_cells.Size(); }
        [[nodiscard]] Label* CellAt(usize i) const
        {
            return i < m_cells.Size() ? m_cells[i].Get() : nullptr;
        }
        void EnsureCells(usize count, fonts::CachedFont* font, Color textColor)
        {
            while (m_cells.Size() < count)
            {
                auto cell = foundation::MakeRef<Label>(foundation::DefaultAllocator());
                cell->SetTag(
                    foundation::StringView(u8"tablecell")); // container-owned, not a generic label
                cell->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
                cell->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
                cell->SetFont(font);
                cell->SetTextColor(textColor);
                cell->SetHitTestVisible(false); // clicks fall through to the row
                AddChild(cell.Get());
                m_cells.PushBack(foundation::Move(cell));
            }
        }

    private:
        Array<RefPtr<Label>> m_cells;
    };

    class TableView : public AbstractItemView
    {
        DRACONIC_OBJECT(TableView, AbstractItemView)
    public:
        TableView()
        {
            SetTag(foundation::StringView(u8"tableview"));
            m_header = foundation::MakeRef<UIWidget>(foundation::DefaultAllocator());
            m_header->SetTag(foundation::StringView(u8"tableheader"));
            m_header->SetBackground(
                foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_headerColor));
            AddChild(m_header.Get());
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (const RefPtr<TableHeaderCell>& h : m_headerCells)
                h->SetFont(font);
            for (const RefPtr<ItemRow>& r : RowPool())
            {
                TableRow* row = static_cast<TableRow*>(r.Get());
                for (usize c = 0; c < row->CellCount(); ++c)
                    row->CellAt(c)->SetFont(font);
            }
            RequestRelayout();
        }
        void SetHeaderHeight(f32 height)
        {
            m_headerHeight = foundation::Max(0.0f, height);
            RequestRelayout();
        }
        void SetColumnWidth(usize column, f32 width)
        {
            while (m_columnWidths.Size() <= column)
                m_columnWidths.PushBack(0.0f);
            m_columnWidths[column] = width;
            RequestRelayout();
        }
        // `tableview { color }` propagates to the body cells (container-owned).
        void SetThemeTextColor(Color color) override
        {
            m_textColor = color;
            for (const RefPtr<ItemRow>& r : RowPool())
            {
                TableRow* row = static_cast<TableRow*>(r.Get());
                for (usize c = 0; c < row->CellCount(); ++c)
                    row->CellAt(c)->SetTextColor(color);
            }
        }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); }

        // Header clicks (a column index) - a SortingProxyModel wires this to sort.
        void SetOnColumnHeaderClicked(foundation::Function<void(usize)> callback)
        {
            m_onHeaderClicked = foundation::Move(callback);
        }

        // Back-compat row-oriented aliases.
        [[nodiscard]] i32 GetSelectedRow() const noexcept { return GetSelectedItem(); }
        void SetSelectedRow(i32 row) { SetSelectedItem(row); }

    protected:
        [[nodiscard]] f32 ContentTopInset() const override { return m_headerHeight; }
        void OnModelChanged() override { RebuildHeader(); }

        [[nodiscard]] RefPtr<ItemRow> CreateItemRow() override
        {
            return foundation::MakeRef<TableRow>(foundation::DefaultAllocator());
        }

        void OnBeforeLayout(f32 contentWidth) override
        {
            ComputeColumnWidths(contentWidth, m_widths);
        }

        void BindItemRow(ItemRow& row, i32 item, f32 /*rowWidth*/) override
        {
            TableRow& tableRow = static_cast<TableRow&>(row);
            const usize columns = ColumnCountOf();
            tableRow.EnsureCells(columns, m_font, m_textColor);
            f32 x = 0.0f;
            for (usize c = 0; c < columns; ++c)
            {
                const f32 w = c < m_widths.Size() ? m_widths[c] : 0.0f;
                Label* cell = tableRow.CellAt(c);
                cell->SetText(GetModel()
                                  ->Data(MakeModelIndex(item, static_cast<i32>(c)))
                                  .ToString()
                                  .AsView());
                cell->SetPosition(foundation::Float2{x, 0.0f});
                cell->SetSize(foundation::Float2{w, GetRowHeight()});
                x += w;
            }
        }

        void OnLayoutDecorations() override
        {
            m_header->SetVisible(m_headerHeight > 0.0f);
            m_header->SetPosition(foundation::Float2{0.0f, 0.0f});
            m_header->SetSize(foundation::Float2{ContentWidth(), m_headerHeight});
            f32 x = 0.0f;
            for (usize c = 0; c < m_headerCells.Size(); ++c)
            {
                const f32 w = c < m_widths.Size() ? m_widths[c] : 0.0f;
                m_headerCells[c]->SetPosition(foundation::Float2{x, 0.0f});
                m_headerCells[c]->SetSize(foundation::Float2{w, m_headerHeight});
                x += w;
            }
            m_header->ToFront(); // over rows scrolled up under it
        }

    private:
        [[nodiscard]] usize ColumnCountOf() const
        {
            return GetModel() != nullptr ? GetModel()->ColumnCount() : 0;
        }

        void RebuildHeader()
        {
            for (const RefPtr<TableHeaderCell>& h : m_headerCells)
                h->RemoveFromParent();
            m_headerCells.Clear();
            if (GetModel() == nullptr)
                return;
            const usize columns = GetModel()->ColumnCount();
            TableView* self = this;
            for (usize c = 0; c < columns; ++c)
            {
                auto cell = foundation::MakeRef<TableHeaderCell>(foundation::DefaultAllocator());
                cell->SetColumn(c);
                cell->SetFont(m_font);
                cell->SetTextColor(m_headerTextColor);
                cell->SetText(GetModel()->ColumnName(c).AsView());
                cell->SetOnClicked(
                    [self](usize col)
                    {
                        if (self->m_onHeaderClicked)
                            self->m_onHeaderClicked(col);
                    });
                m_header->AddChild(cell.Get());
                m_headerCells.PushBack(foundation::Move(cell));
            }
        }

        // Explicit widths where set (>0), else an equal split of the remaining width among autos.
        void ComputeColumnWidths(f32 bodyWidth, Array<f32>& out) const
        {
            const usize columns = ColumnCountOf();
            out.Clear();
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
            const f32 autoWidth =
                autoCount > 0
                    ? foundation::Max(0.0f, (bodyWidth - explicitTotal) / static_cast<f32>(autoCount))
                    : 0.0f;
            for (usize c = 0; c < columns; ++c)
            {
                const f32 w = c < m_columnWidths.Size() ? m_columnWidths[c] : 0.0f;
                out.PushBack(w > 0.0f ? w : autoWidth);
            }
        }

        RefPtr<UIWidget> m_header;
        Array<RefPtr<TableHeaderCell>> m_headerCells;
        Array<f32> m_columnWidths; // 0 = auto
        Array<f32> m_widths;       // resolved widths for the current layout pass
        fonts::CachedFont* m_font = nullptr;
        foundation::Function<void(usize)> m_onHeaderClicked;
        f32 m_headerHeight = 26.0f;
        Color m_headerColor{0.20f, 0.22f, 0.27f, 1.0f};
        Color m_headerTextColor{0.86f, 0.89f, 0.94f, 1.0f};
        Color m_textColor{0.86f, 0.89f, 0.94f, 1.0f};
    };

    DRACONIC_DEFINE_OBJECT(TableHeaderCell, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(TableRow, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(TableView, "draconic::gui")
}
