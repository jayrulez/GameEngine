// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :table_view partition
//
// TableView: a virtualised table over a model. ItemView realises the rows, and each row's content
// is one cell (`tablecell`) per column. The header (`tableheader`) shows the column names. It stays
// at the top while the rows scroll vertically beneath it, and scrolls horizontally with them.
// Column widths are pixels, a percentage of the view's width, or auto (an equal share of the
// remaining width, no narrower than a minimum); columns wider than the view make it scroll
// horizontally. Dragging a header cell's right edge resizes its column, which then has a pixel
// width. A click on a header cell reports its column; with no listener it sorts by that column
// where the model can, and the sorted column's cell shows the direction. In markup,
// `columns="Name|Score"` and `<TableItem cells="Ada|42"/>` children give the table its own model,
// behind a sorting proxy, with numbers kept as numbers so they sort by value.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:table_view;

import foundation.core; // RefPtr, MakeRef, Array, Function, Move, Max
import foundation.vg;   // PathBuilder, for the sort indicator
import :rect;
import :event;
import :node;
import :label;
import :text; // TextHAlign
import :ui_widget;
import :model_index;
import :model;
import :item_view;
import :table_model;
import :sorting_proxy_model;
import :variant;
import :list_box; // SplitItems
import :draw_context;
import :layout_style;     // LayoutLength, Overflow
import :event_dispatcher; // SuppressClick, so a resize is not taken as a click

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    class TableView;

    // A column's header cell. A click reports its column, and a drag from its right edge resizes
    // the column. The sorted column's cell shows the direction with a triangle, pointing up for
    // ascending.
    class TableHeaderCell : public Label
    {
        RTTI_OBJECT(TableHeaderCell, Label)
    public:
        explicit TableHeaderCell(TableView& table) : m_table(table)
        {
            SetTag(core::StringView(u8"tableheadercell"));
            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
            SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
        }
        void SetColumn(usize column) noexcept { m_column = column; }
        [[nodiscard]] usize GetColumn() const noexcept { return m_column; }
        [[nodiscard]] bool IsResizing() const noexcept { return m_resizing; }

        // A press on the cell's inline-end edge resizes its column, so no drag is offered to the
        // nodes that hold it.
        [[nodiscard]] bool OwnsPressDrag(core::Float2 local) const override { return OnGripAt(local.x); }

    protected:
        void OnMouseDown(const MouseEvent& event) override;
        void OnMouseMove(const MouseEvent& event) override;
        void OnMouseUp(const MouseEvent&) override { m_resizing = false; }
        void OnPointerCancel(const MouseEvent&) override { m_resizing = false; }
        void OnMouseClick(const MouseEvent& event) override;
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override;

    private:
        static constexpr f32 kGrip = 5.0f; // how near the inline-end edge a press must be to resize
        [[nodiscard]] bool OnGripAt(f32 x) const { return IsRightToLeft() ? x <= kGrip : x >= GetSize().x - kGrip; }
        [[nodiscard]] bool OnGrip(core::Float2 world) const { return OnGripAt(ConvertToNodeSpace(world).x); }

        TableView& m_table; // the view whose header holds this cell
        usize m_column = 0;
        bool m_resizing = false;
        f32 m_pressX = 0.0f;
        f32 m_startWidth = 0.0f;
    };

    // A table row's content: one cell per column, placed by the table's column widths.
    class TableRowContent : public UIWidget
    {
        RTTI_OBJECT(TableRowContent, UIWidget)
    public:
        explicit TableRowContent(const TableView& table) : m_table(table) { SetTag(core::StringView(u8"tablerowcontent")); }

        [[nodiscard]] usize CellCount() const noexcept { return m_cells.Size(); }
        [[nodiscard]] Label* CellAt(usize i) const { return i < m_cells.Size() ? m_cells[i].Get() : nullptr; }
        /// Makes sure the row has `count` cells.
        void EnsureCells(usize count)
        {
            while (m_cells.Size() < count)
            {
                auto cell = core::MakeRef<Label>(MemoryAllocator());
                cell->SetTag(core::StringView(u8"tablecell")); // tagged so sheets style cells apart from generic labels
                StyleWriteScope defaults(*cell);
                cell->SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
                cell->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
                cell->SetHitTestVisible(false); // presses go through to the row
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
        const TableView& m_table; // the view whose row holds this content
        Array<core::RefPtr<Label>> m_cells;
    };

    /// A table row written in markup, holding its cells as text with '|' between them.
    class TableItem : public UIWidget
    {
        RTTI_OBJECT(TableItem, UIWidget)
    public:
        TableItem()
        {
            SetTag(core::StringView(u8"tableitem"));
            SetDisplayed(false); // only data; never drawn
        }
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name != core::StringView(u8"cells"))
                return UIWidget::SetMarkupAttribute(name, value);
            m_cells = core::String(value);
            return true;
        }
        [[nodiscard]] core::String GetCells() const { return m_cells; }
        /// Sets the cells, with '|' between them. When set after the table adopted the item (a
        /// script's `add`, then `cells`), the table's row is updated to match.
        void SetCells(const core::String& cells)
        {
            m_cells = cells;
            if (m_onCellsChanged)
                m_onCellsChanged();
        }
        /// Sets the callback the adopting table uses to hear when the cells change.
        void SetOnCellsChanged(core::Function<void()> callback) { m_onCellsChanged = core::Move(callback); }

    private:
        core::String m_cells;
        core::Function<void()> m_onCellsChanged;
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
            m_header->SetBox(FilledBox(m_headerColor)); // the default face; a style sheet's box replaces it
            AddChild(m_header.Get());
            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            SetTextColor(Color{0.86f, 0.89f, 0.94f, 1.0f});
            EditLayoutStyle([](LayoutStyle& s) { s.OverflowX = Overflow::Auto; }); // scroll when the columns are wider than the view
        }

        // The table's own model is destroyed before the ItemView base releases it, so detach the
        // model first.
        ~TableView() override { SetModel(nullptr); }

        void SetHeaderHeight(f32 height)
        {
            m_headerHeight = core::Max(0.0f, height);
            RequestLayout();
        }
        /// Sets a column's width: pixels, a percentage of the view's width, or auto.
        void SetColumnWidth(usize column, LayoutLength width)
        {
            while (m_columnWidths.Size() <= column)
                m_columnWidths.PushBack(LayoutLength{});
            m_columnWidths[column] = width;
            RequestLayout();
        }
        /// Sets every column's width from text, one value per column (`120px 30% auto`; a bare
        /// number means pixels). This is markup's `column-widths`.
        void SetColumnWidthsText(const core::String& text)
        {
            m_columnWidths.Clear();
            core::StringView rest = core::Trim(text.AsView());
            while (rest.Size() != 0)
            {
                usize end = 0;
                while (end < rest.Size() && rest[end] != u8' ')
                    ++end;
                const core::StringView word = rest.SubStr(0, end);
                rest = core::Trim(rest.SubStr(end, rest.Size() - end));
                LayoutLength width;
                if (word.Size() > 1 && word[word.Size() - 1] == u8'%')
                    width = LayoutLength::Percent(static_cast<f32>(core::ParseFloat(word.SubStr(0, word.Size() - 1)).ValueOr(0.0)));
                else if (word != core::StringView(u8"auto"))
                {
                    const core::StringView digits = word.Size() > 2 && word.SubStr(word.Size() - 2, 2) == core::StringView(u8"px") ? word.SubStr(0, word.Size() - 2) : word;
                    width = LayoutLength::Px(static_cast<f32>(core::ParseFloat(digits).ValueOr(0.0)));
                }
                m_columnWidths.PushBack(width);
            }
            RequestLayout();
        }
        [[nodiscard]] core::String GetColumnWidthsText() const
        {
            core::String text;
            for (usize c = 0; c < m_columnWidths.Size(); ++c)
            {
                if (c > 0)
                    text.Append(core::StringView(u8" "));
                const LayoutLength& w = m_columnWidths[c];
                const core::String written = w.Kind == LayoutLengthKind::Px        ? core::Format(u8"{}px", w.Value)
                                             : w.Kind == LayoutLengthKind::Percent ? core::Format(u8"{}%", w.Value)
                                             : w.Kind == LayoutLengthKind::Calc    ? core::Format(u8"calc({}px + {}%)", w.Value, w.PercentPart)
                                                                                   : core::String(core::StringView(u8"auto"));
                text.Append(written.AsView());
            }
            return text;
        }
        /// Sets the narrowest a column can get, whether an auto column is squeezed or a column is
        /// dragged.
        void SetMinColumnWidth(f32 width)
        {
            m_minColumnWidth = core::Max(1.0f, width);
            RequestLayout();
        }
        [[nodiscard]] f32 GetMinColumnWidth() const noexcept { return m_minColumnWidth; }
        /// Returns the columns' total width in the last layout. When it exceeds the view's width,
        /// the view scrolls horizontally.
        [[nodiscard]] f32 GetColumnsWidth() const
        {
            f32 total = 0.0f;
            for (f32 w : m_widths)
                total += w;
            return total;
        }
        /// Sets the sort indicator: which column shows which direction. A header click that sorts
        /// sets it.
        void SetSortIndicator(i32 column, SortOrder order)
        {
            m_sortColumn = column;
            m_sortOrder = order;
            for (const core::RefPtr<TableHeaderCell>& cell : m_headerCells)
                cell->Invalidate();
        }
        [[nodiscard]] i32 GetSortColumn() const noexcept { return m_sortColumn; }
        [[nodiscard]] SortOrder GetSortOrder() const noexcept { return m_sortOrder; }
        /// Returns a column's width in the last layout.
        [[nodiscard]] f32 GetColumnWidth(usize column) const { return column < m_widths.Size() ? m_widths[column] : 0.0f; }
        [[nodiscard]] usize GetColumnCount() const { return GetModel() != nullptr ? GetModel()->ColumnCount() : 0; }

        // === Markup: the table's own model ===
        /// Sets the column names, with '|' between them (markup's `columns`).
        void SetColumnsText(const core::String& text)
        {
            EnsureOwnModel();
            Array<core::String> columns;
            for (core::StringView name : SplitItems(text.AsView()))
                columns.PushBack(core::String(name));
            m_ownModel->SetColumns(core::Move(columns));
        }
        [[nodiscard]] core::String GetColumnsText() const
        {
            Array<core::String> columns;
            for (usize c = 0; c < GetColumnCount(); ++c)
                columns.PushBack(GetModel()->ColumnName(c));
            return JoinItems(columns);
        }
        /// Sets whether the cells of the table's own model may be edited in place (F2).
        void SetEditable(bool editable)
        {
            EnsureOwnModel();
            m_ownModel->SetEditable(editable);
            m_editable = editable;
        }
        [[nodiscard]] bool IsEditable() const noexcept { return m_editable; }
        /// Returns the host for markup's children, which become rows (TableItems).
        [[nodiscard]] Node* GetChildHost() override
        {
            if (!m_markupHost)
            {
                TableView* self = this;
                m_markupHost = core::MakeRef<MarkupChildHost>(MemoryAllocator(), [self](Node& host)
                {
                    self->EnsureOwnModel();
                    while (host.ChildCount() != 0)
                    {
                        core::RefPtr<Node> child(host.GetChildAt(0));
                        host.RemoveChild(child.Get());
                        TableItem* item = core::Cast<TableItem>(child.Get());
                        if (item == nullptr)
                            continue;
                        // Add the item's row and keep the item. A script that added it may set its
                        // cells later through the handle it holds, and the row then follows.
                        const usize row = self->m_ownModel->RowCount(ModelIndex{});
                        self->m_ownModel->AddRow(ParseCells(item->GetCells()));
                        item->SetOnCellsChanged([self, item, row]()
                        {
                            const Array<Variant> cells = ParseCells(item->GetCells());
                            for (usize c = 0; c < cells.Size(); ++c)
                                self->m_ownModel->SetCell(row, c, cells[c]);
                        });
                        self->m_rowItems.PushBack(core::RefPtr<TableItem>(item));
                    }
                });
            }
            return m_markupHost.Get();
        }

        // Sets the callback for header clicks, passing the column. With no callback set, a click
        // sorts where the model can.
        void SetOnColumnHeaderClicked(core::Function<void(usize)> callback) { m_onHeaderClicked = core::Move(callback); }
        [[nodiscard]] Node* GetHeader() const noexcept { return m_header.Get(); }

    protected:
        // The header cells use the table's font; their colour is their own (tableheadercell { color }).
        void OnTextStyleChange() override
        {
            for (const core::RefPtr<TableHeaderCell>& h : m_headerCells)
                ShareTextStyle(*h, false);
            ItemView::OnTextStyleChange();
        }
        [[nodiscard]] core::StringView RowTag() const override { return core::StringView(u8"tablerow"); }
        [[nodiscard]] f32 ContentTopInset() const override { return m_headerHeight; }
        void OnModelChanged() override { SyncHeader(); }
        // Works out the column widths for this layout from the content box's width. A row is as
        // wide as the view, or as the columns when they are wider.
        [[nodiscard]] f32 RowWidth(f32 width) const override
        {
            ComputeColumnWidths(width);
            return core::Max(width, GetColumnsWidth());
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
            return MirroredIn(Rect{x, box.y, GetColumnWidth(static_cast<usize>(core::Max(0, column))), box.height}, box, IsRightToLeft());
        }

        // Places the header at the top of the view, over the rows that scroll beneath it.
        void OnLayoutDecorations() override
        {
            // The header lines up with where the rows start, with columns from the inline start.
            const Rect box = GetContentBounds();
            m_header->SetVisible(m_headerHeight > 0.0f);
            const f32 width = core::Max(box.width, GetColumnsWidth());
            m_header->Arrange(Rect{RowLeft(box, width), box.y + GetScrollOffset().y, width, m_headerHeight});
            const Rect across{0.0f, 0.0f, width, m_headerHeight};
            const bool rtl = IsRightToLeft();
            f32 x = 0.0f;
            for (usize c = 0; c < m_headerCells.Size(); ++c)
            {
                const f32 w = GetColumnWidth(c);
                m_headerCells[c]->Arrange(MirroredIn(Rect{x, 0.0f, w, m_headerHeight}, across, rtl));
                x += w;
            }
            if (GetChildIndex(m_header.Get()) + 1 != ChildCount())
                m_header->ToFront(); // drawn over the rows and hit before them
        }

    private:
        friend class TableRowContent;
        friend class TableHeaderCell;

        // Keeps the header cells in step with the columns. Existing cells are kept while their
        // columns stay, so a press that sorts does not destroy the cell under the pointer.
        void SyncHeader()
        {
            const usize columns = GetColumnCount();
            while (m_headerCells.Size() > columns)
            {
                m_headerCells[m_headerCells.Size() - 1]->RemoveFromParent();
                m_headerCells.RemoveAt(m_headerCells.Size() - 1);
            }
            while (m_headerCells.Size() < columns)
            {
                auto cell = core::MakeRef<TableHeaderCell>(MemoryAllocator(), *this);
                cell->SetColumn(m_headerCells.Size());
                ShareTextStyle(*cell, false);
                {
                    StyleWriteScope defaults(*cell);
                    cell->SetTextColor(m_headerTextColor);
                }
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
            // Nothing is listening, so sort by the column where the model can: ascending for a new
            // column, reversing the order for the same one.
            const SortOrder order = m_sortColumn == static_cast<i32>(column) && m_sortOrder == SortOrder::Ascending ? SortOrder::Descending : SortOrder::Ascending;
            if (GetModel() != nullptr && GetModel()->Sort(column, order))
                SetSortIndicator(static_cast<i32>(column), order);
        }

        // Pixel and percentage widths are used as given, and auto columns share what is left
        // equally. No column is narrower than the minimum, so the columns may come out wider than
        // the view.
        void ComputeColumnWidths(f32 bodyWidth) const
        {
            const usize columns = GetColumnCount();
            const Array<f32> before = m_widths;
            m_widths.Clear();
            f32 given = 0.0f;
            usize autoCount = 0;
            for (usize c = 0; c < columns; ++c)
            {
                const LayoutLength w = c < m_columnWidths.Size() ? m_columnWidths[c] : LayoutLength{};
                const Optional<f32> px = w.Resolve(bodyWidth);
                if (px.HasValue())
                    given += core::Max(m_minColumnWidth, px.Value());
                else
                    ++autoCount;
            }
            const f32 share = autoCount > 0 ? (bodyWidth - given) / static_cast<f32>(autoCount) : 0.0f;
            for (usize c = 0; c < columns; ++c)
            {
                const LayoutLength w = c < m_columnWidths.Size() ? m_columnWidths[c] : LayoutLength{};
                // Read the value the same way as in the loop above. ValueOr on this temporary is not
                // emitted in an optimized clang build, which then fails to link.
                const core::Optional<f32> px = w.Resolve(bodyWidth);
                m_widths.PushBack(core::Max(m_minColumnWidth, px.HasValue() ? px.Value() : share));
            }
            // The column widths can change while the row's width stays the same (one column
            // narrower, an auto one wider), so lay the rows' cells out again, as the header's are.
            bool changed = before.Size() != m_widths.Size();
            for (usize c = 0; c < m_widths.Size() && !changed; ++c)
                changed = before[c] != m_widths[c];
            if (changed)
                for (const core::RefPtr<ItemRow>& row : LiveRows())
                    if (Node* content = row->GetContent())
                        content->RequestLayout();
        }
        void EnsureOwnModel()
        {
            if (m_ownModel)
                return;
            m_ownModel = core::MakeUnique<TableModel>(MemoryAllocator());
            m_ownProxy = core::MakeUnique<SortingProxyModel>(MemoryAllocator(), m_ownModel.Get());
            SetModel(m_ownProxy.Get());
        }
        // Parses a row's cells from text. A number stays a number, so it sorts by value.
        [[nodiscard]] static Array<Variant> ParseCells(const core::String& text)
        {
            Array<Variant> cells;
            for (core::StringView cell : SplitItems(text.AsView()))
            {
                bool integer = cell.Size() != 0;
                for (usize i = 0; i < cell.Size(); ++i)
                    integer = integer && ((cell[i] >= u8'0' && cell[i] <= u8'9') || (i == 0 && cell[i] == u8'-' && cell.Size() > 1));
                const Optional<f64> number = core::ParseFloat(cell);
                if (integer)
                    cells.PushBack(Variant(static_cast<i64>(number.ValueOr(0.0))));
                else if (number.HasValue() && cell.Size() != 0)
                    cells.PushBack(Variant(number.Value()));
                else
                    cells.PushBack(Variant(cell));
            }
            return cells;
        }
        // Handles a drag of a header cell's edge: the column gets that many pixels.
        void ResizeColumn(usize column, f32 width) { SetColumnWidth(column, LayoutLength::Px(core::Max(m_minColumnWidth, width))); }

        core::RefPtr<UIWidget> m_header;
        Array<core::RefPtr<TableHeaderCell>> m_headerCells;
        Array<LayoutLength> m_columnWidths; // auto when unset
        mutable Array<f32> m_widths;   // resolved widths for the current layout, worked out by RowWidth
        core::Function<void(usize)> m_onHeaderClicked;
        i32 m_sortColumn = -1;
        SortOrder m_sortOrder = SortOrder::None;
        f32 m_headerHeight = 26.0f;
        f32 m_minColumnWidth = 40.0f;
        core::UniquePtr<TableModel> m_ownModel;         // the rows from markup
        Array<core::RefPtr<TableItem>> m_rowItems;      // the item each row came from, in row order
        core::UniquePtr<SortingProxyModel> m_ownProxy; // over m_ownModel, so the header can sort
        core::RefPtr<Node> m_markupHost;               // receives markup's rows before they are adopted
        bool m_editable = false;
        Color m_headerColor{0.20f, 0.22f, 0.27f, 1.0f};
        Color m_headerTextColor{0.86f, 0.89f, 0.94f, 1.0f};
    };

    inline void TableHeaderCell::OnMouseDown(const MouseEvent& event)
    {
        if (event.Button != MouseButton::Left || !OnGrip(event.Position))
            return;
        // A press on the edge is a resize, not a click, so no sort follows.
        event.Handled = true;
        m_resizing = true;
        m_pressX = event.Position.x;
        m_startWidth = m_table.GetColumnWidth(m_column);
        if (EventDispatcher* d = GetEventDispatcher())
            d->SuppressClick();
    }

    inline void TableHeaderCell::OnMouseMove(const MouseEvent& event)
    {
        if (m_resizing)
        {
            // Dragging toward the inline end widens the column.
            const f32 dx = event.Position.x - m_pressX;
            m_table.ResizeColumn(m_column, m_startWidth + (IsRightToLeft() ? -dx : dx));
            return;
        }
        SetCursor(OnGrip(event.Position) ? Cursor::EwResize : Cursor::Auto); // the cursor shows the edge resizes
    }

    inline void TableHeaderCell::OnMouseClick(const MouseEvent& event)
    {
        event.Handled = true;
        m_table.HeaderClicked(m_column);
    }

    inline void TableHeaderCell::OnDraw(DrawContext& ctx, const Rect& localBounds)
    {
        Label::OnDraw(ctx, localBounds);
        if (m_table.GetSortColumn() != static_cast<i32>(m_column) || m_table.GetSortOrder() == SortOrder::None)
            return;
        // A small triangle at the cell's inline end, up for ascending, down for descending.
        const Rect box = GetContentBounds();
        const f32 s = 4.0f;
        const f32 cx = MirroredX(box.x + box.width - s, box, IsRightToLeft());
        const f32 cy = box.y + box.height * 0.5f;
        const f32 dir = m_table.GetSortOrder() == SortOrder::Ascending ? -1.0f : 1.0f;
        vg::PathBuilder pb;
        pb.MoveTo(cx - s, cy - dir * s * 0.5f);
        pb.LineTo(cx + s, cy - dir * s * 0.5f);
        pb.LineTo(cx, cy + dir * s * 0.5f);
        pb.Close();
        ctx.VG().FillPath(pb.ToPath(), GetTextColor());
    }

    inline core::Float2 TableRowContent::MeasureContent(core::Float2 available)
    {
        // The row is as tall as its tallest cell at that cell's column width.
        f32 height = 0.0f;
        for (usize c = 0; c < m_cells.Size(); ++c)
            if (m_cells[c]->IsDisplayed())
            {
                const f32 w = m_table.GetColumnWidth(c);
                height = core::Max(height, m_cells[c]->Measure(LayoutConstraints{core::Float2{w, 0.0f}, core::Float2{w, kUnbounded}, core::Float2{w, -1.0f}, true, false}).y);
            }
        return core::Float2{available.x, height};
    }

    inline void TableRowContent::OnLayout()
    {
        // Place the cells by column from the inline start.
        const Rect box = GetContentBounds();
        const bool rtl = IsRightToLeft();
        f32 x = box.x;
        for (usize c = 0; c < m_cells.Size(); ++c)
        {
            if (!m_cells[c]->IsDisplayed())
                continue;
            const f32 w = m_table.GetColumnWidth(c);
            m_cells[c]->Arrange(MirroredIn(Rect{x, box.y, w, box.height}, box, rtl));
            x += w;
        }
    }

    RTTI_DEFINE_OBJECT(TableHeaderCell, "rtti::gui")
    RTTI_DEFINE_OBJECT(TableRowContent, "rtti::gui")
}
