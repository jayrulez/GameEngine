// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :item_view partition
//
// ItemView: the engine behind ListView, TreeView and TableView. It is a scroll container like any
// other node (wheel, scroll bars, touch, fling, smooth scrolling, the right stick and the
// `::scrollbar` parts all come from Node), and it realises only the rows in view:
//
//   - Rows come from a factory, either a native one or the view's own default. Each row type has
//     a pool of rows; a row scrolled out of view goes back to its pool for reuse. A row is an
//     ItemRow holding the factory's content, and it shows `:selected` while selected.
//   - Row heights are either fixed (`row-height`) or measured. A measured row uses an estimated
//     height until it is first realised and measured. Heights are kept in a sum tree
//     (RowHeights), so finding the row at an offset, or the offset of a row, takes O(log n).
//   - Scrolling is anchored to an item and an offset into it, so when a row above the view
//     measures taller, or rows are inserted above it, the rows in view stay where they are.
//   - The model reports changed row ranges. For a flat model the view follows inserts, removals,
//     moves and changes; for anything else it resets. The selection is a set of row ids, so it
//     survives inserts, moves and sorting.
//   - Grid is a layout mode of the same view: cells of a set size, as many across as fit
//     (stretched to fill the line), in lines going down. The arrow keys move across and down.
//   - Rows from markup: each row is built from a `<template>` child, and its `{{item}}`,
//     `{{item.Column}}` and `{{index}}` attributes are filled in for the item it shows. When
//     templates have a `type`, the text in the view's `row-type-column` picks one. `items="a|b|c"`
//     gives a markup list its own model.
//   - Drag and drop: with a `drag-type` set, dragging a row drags the selection. The payload
//     carries the row ids, and its value is a weak handle to the view. As a drop target, the view
//     works out where in a row the drop lands (before, on or after it, by quarter bands; see
//     DropAt for the fallbacks), asks its model through ModelDrop (CanDrop, then Drop), and draws
//     an insertion line or an outline (`::drop-indicator`). Rows moved to another model are
//     removed from this one.
//   - A double-click, Return or the pad's South button sends `activate`. Typing jumps to the next
//     row whose text starts with what was typed; it uses typed text, so it follows the keyboard
//     layout and any IME. F2 (or BeginEdit) edits a row in place, and hooks can refuse the edit
//     or the new value.
//
// A subclass defines what an item is (ItemCount, ItemToModelIndex; a tree flattens its open
// nodes), makes the default rows (CreateContent, BindContent), and adds decorations such as a
// table's header.

module;
#include "Core/Prelude.h"
#include <algorithm> // sort, for the items that have accessibility nodes
#include "Core/Reflection/Reflect.h"

export module foundation.gui:item_view;

import foundation.core; // RefPtr, Array, HashSet, Function, Move, Min, Max
import :rect;
import :event;
import :draw_context;
import :node;
import :ui_widget;
import :layout_style; // Overflow, ScrollAlign, LayoutConstraints
import :variant;
import :model_index;
import :model;
import :text_field;
import :text;             // TypeAheadSearch
import :event_dispatcher; // GetTime (type-ahead)
import :reactive;         // Observable, which bindings follow
import :element_types;    // ElementSet, the elements a template may use
import :markup;           // MarkupLoader, ItemBinding (template rows)
import :string_list_model;
import :list_model;       // ListModel, a model built by a script
import :list_box;         // SplitItems, JoinItems
import foundation.vg;     // StrokeStyle, for the drop indicator

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    enum class SelectionMode
    {
        Single,
        Multi
    };

    /// How an item view lays out its items: rows going down (List), or cells across then down (Grid).
    enum class ItemLayout : u8
    {
        List,
        Grid,
    };

    /// Row heights in a sum tree (a Fenwick tree). Finding a row's offset, finding the row at an
    /// offset and changing a height each take O(log n); inserts and removals rebuild it in O(n).
    class RowHeights
    {
    public:
        void Reset(usize count, f32 estimate)
        {
            m_heights.Clear();
            m_measured.Clear();
            for (usize i = 0; i < count; ++i)
            {
                m_heights.PushBack(estimate);
                m_measured.PushBack(0);
            }
            Rebuild();
        }
        void Insert(usize at, usize count, f32 estimate)
        {
            at = core::Min(at, m_heights.Size());
            for (usize i = 0; i < count; ++i)
            {
                m_heights.Insert(at, estimate);
                m_measured.Insert(at, u8{0});
            }
            Rebuild();
        }
        void Remove(usize at, usize count)
        {
            for (usize i = 0; i < count && at < m_heights.Size(); ++i)
            {
                m_heights.RemoveAt(at);
                m_measured.RemoveAt(at);
            }
            Rebuild();
        }
        /// Moves `count` rows starting at `first` so they start at `to`, an index in the order after
        /// the move.
        void Move(usize first, usize count, usize to)
        {
            Array<f32> heights;
            Array<u8> measured;
            for (usize i = 0; i < count && first < m_heights.Size(); ++i)
            {
                heights.PushBack(m_heights[first]);
                measured.PushBack(m_measured[first]);
                m_heights.RemoveAt(first);
                m_measured.RemoveAt(first);
            }
            to = core::Min(to, m_heights.Size());
            for (usize i = 0; i < heights.Size(); ++i)
            {
                m_heights.Insert(to + i, heights[i]);
                m_measured.Insert(to + i, measured[i]);
            }
            Rebuild();
        }

        [[nodiscard]] usize Size() const noexcept { return m_heights.Size(); }
        [[nodiscard]] f32 At(usize i) const { return i < m_heights.Size() ? m_heights[i] : 0.0f; }
        [[nodiscard]] bool IsMeasured(usize i) const { return i < m_measured.Size() && m_measured[i] != 0; }
        /// Sets a row's height. With `measured`, the row counts as measured until Unmeasure.
        /// Returns true when the height changed.
        bool Set(usize i, f32 height, bool measured = true)
        {
            if (i >= m_heights.Size())
                return false;
            m_measured[i] = measured ? 1 : 0;
            const f32 delta = height - m_heights[i];
            if (delta == 0.0f)
                return false;
            m_heights[i] = height;
            for (usize k = i + 1; k <= m_heights.Size(); k += k & (~k + 1))
                m_tree[k] += delta;
            return true;
        }
        void Unmeasure(usize i)
        {
            if (i < m_measured.Size())
                m_measured[i] = 0;
        }
        /// Returns where row `i` starts: the sum of the heights before it.
        [[nodiscard]] f32 Top(usize i) const
        {
            f32 sum = 0.0f;
            for (usize k = core::Min(i, m_heights.Size()); k > 0; k -= k & (~k + 1))
                sum += m_tree[k];
            return sum;
        }
        [[nodiscard]] f32 Total() const { return Top(m_heights.Size()); }
        /// Returns the row at offset `y`: the last row when `y` is past the end, 0 when there are
        /// no rows.
        [[nodiscard]] usize Find(f32 y) const
        {
            const usize n = m_heights.Size();
            if (n == 0)
                return 0;
            usize pos = 0;
            for (usize step = m_highBit; step > 0; step >>= 1)
                if (pos + step <= n && m_tree[pos + step] <= y)
                {
                    pos += step;
                    y -= m_tree[pos];
                }
            return core::Min(pos, n - 1);
        }

    private:
        void Rebuild()
        {
            const usize n = m_heights.Size();
            m_tree.Clear();
            for (usize i = 0; i <= n; ++i)
                m_tree.PushBack(0.0f);
            for (usize i = 1; i <= n; ++i)
            {
                m_tree[i] += m_heights[i - 1];
                const usize parent = i + (i & (~i + 1));
                if (parent <= n)
                    m_tree[parent] += m_tree[i];
            }
            m_highBit = 1;
            while (m_highBit * 2 <= n)
                m_highBit *= 2;
        }

        Array<f32> m_heights;
        Array<u8> m_measured;
        Array<f32> m_tree; // 1-based
        usize m_highBit = 1;
    };

    class ItemView;

    /// A native row factory. Create makes a row's content once for each pooled row of a type, and
    /// Bind fills it for each item the row shows.
    struct RowFactory
    {
        /// Returns the row type for an item; when unset, all rows share one type. Each type has its
        /// own pool.
        core::Function<u32(const ModelIndex&)> TypeOf;
        core::Function<core::RefPtr<Node>(u32 type)> Create;
        core::Function<void(Node& content, const ModelIndex& index)> Bind;
    };

    /// One realised row: the view's container around a factory's content. While selected it shows
    /// `:selected` and draws the view's ::selection part under its content.
    class ItemRow : public UIWidget
    {
        RTTI_OBJECT(ItemRow, UIWidget)
    public:
        explicit ItemRow(ItemView& view);

        [[nodiscard]] i32 GetItemIndex() const noexcept { return m_item; }
        [[nodiscard]] ItemId GetItemId() const noexcept { return m_id; }
        [[nodiscard]] u32 GetRowType() const noexcept { return m_type; }
        [[nodiscard]] Node* GetContent() const noexcept { return m_content.Get(); }
        [[nodiscard]] bool IsSelected() const noexcept { return m_selected; }
        [[nodiscard]] u32 GetWidgetStates() const override { return m_selected ? WidgetStateSelected : WidgetStateNone; }

    protected:
        void OnMouseDown(const MouseEvent& event) override;
        void OnMouseClick(const MouseEvent& event) override;
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override;
        void OnLayout() override;
        void OnTextStyleChange() override
        {
            if (UINode* content = core::Cast<UINode>(m_content.Get()))
                ShareTextStyle(*content);
        }

    private:
        friend class ItemView;
        void SetSelectedState(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            MarkStyleStateDirty(StateSelected);
            Invalidate();
        }

        ItemView& m_view; // the parent view, which owns this row
        core::RefPtr<Node> m_content;
        i32 m_item = -1;
        ItemId m_id = kNoItem;
        u32 m_type = 0;
        bool m_selected = false;
        bool m_dirty = true; // the content shows stale data and must be bound again
        bool m_taken = false; // already claimed during the current realisation pass
    };

    // A row made from a markup template. It holds the template's tree and the bindings for the
    // attributes that are filled from the item the row shows.
    class TemplateRowContent : public UIWidget
    {
        RTTI_OBJECT(TemplateRowContent, UIWidget)
    public:
        TemplateRowContent() { SetTag(core::StringView(u8"templaterow")); }
        [[nodiscard]] Array<ItemBinding>& Bindings() noexcept { return m_bindings; }

    protected:
        // The template's root fills the row (or the grid cell), inset by the root's margin.
        void OnLayout() override
        {
            const Rect box = GetContentBounds();
            for (usize i = 0; i < ChildCount(); ++i)
            {
                Node& child = *GetChildAt(i);
                const Thickness m = child.GetMargin();
                child.Arrange(Rect{box.x + m.Left, box.y + m.Top, core::Max(0.0f, box.width - m.TotalHorizontal()),
                                   core::Max(0.0f, box.height - m.TotalVertical())});
            }
        }

    private:
        Array<ItemBinding> m_bindings;
    };

    class ItemView : public UIWidget, public IModelClient
    {
        RTTI_OBJECT(ItemView, UIWidget)
    public:
        ItemView() : m_selected(MemoryAllocator())
        {
            SetTabFocusable(true);
            StyleWriteScope defaults(*this);
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.OverflowY = Overflow::Auto;
            });
            SetScrollStep(m_rowHeight);
        }
        ~ItemView() override
        {
            if (m_model != nullptr)
                m_model->RemoveClient(this);
        }

        // === Model ===
        void SetModel(IModel* model)
        {
            if (m_model != nullptr)
                m_model->RemoveClient(this);
            m_model = model;
            if (m_model != nullptr)
                m_model->AddClient(this);
            m_selected.Clear();
            m_current = kNoItem;
            m_anchorSelection = kNoItem;
            ResetScrollAnchor();
            OnModelReset(); // read the new model's items first, since the scroll below realises and binds rows
            SetScrollOffset(core::Float2{0.0f, 0.0f});
        }
        [[nodiscard]] IModel* GetModel() const noexcept { return m_model; }

        // === Rows ===
        /// Makes rows with a native factory instead of the view's default. All pooled rows are dropped.
        void SetRowFactory(RowFactory factory)
        {
            m_factory = core::Move(factory);
            DropRows();
            RequestLayout();
        }
        /// Adds a markup template for rows. When there are several, `type` names this one and the
        /// row-type-column's text picks between them; an empty `type` marks the default.
        bool SetMarkupTemplate(core::StringView type, core::StringView markup) override
        {
            m_templates.PushBack(RowTemplate{core::String(type), core::String(markup)});
            UseTemplates();
            return true;
        }
        /// Sets the column whose text picks each row's template, by matching the template's `type`.
        void SetRowTypeColumn(const core::String& column)
        {
            m_rowTypeColumn = column;
            if (!m_templates.IsEmpty())
                UseTemplates();
        }
        [[nodiscard]] core::String GetRowTypeColumn() const { return m_rowTypeColumn; }
        /// Shows a script's list model and keeps it alive while it is shown.
        void SetListModel(RefPtr<ListModel> model)
        {
            m_listModel = core::Move(model);
            SetModel(m_listModel ? &m_listModel->Model() : nullptr);
        }
        /// Gives the view its own model built from `text`, with '|' between the items (markup's
        /// `items`).
        void SetItemsText(const core::String& text)
        {
            if (!m_ownModel)
                m_ownModel = core::MakeUnique<StringListModel>(MemoryAllocator());
            Array<core::String> items;
            for (core::StringView item : SplitItems(text.AsView()))
                items.PushBack(core::String(item));
            m_ownModel->SetItems(core::Move(items));
            if (m_model != m_ownModel.Get())
                SetModel(m_ownModel.Get());
        }
        [[nodiscard]] core::String GetItemsText() const
        {
            Array<core::String> items;
            if (m_ownModel)
                for (usize i = 0; i < m_ownModel->RowCount(); ++i)
                    items.PushBack(core::String(m_ownModel->ItemAt(i)));
            return JoinItems(items);
        }
        /// Sets the selection mode from "single" or "multi" (markup's `selection-mode`).
        void SetSelectionModeName(const core::String& name)
        {
            m_selectionMode = core::Trim(name.AsView()) == core::StringView(u8"multi") ? SelectionMode::Multi : SelectionMode::Single;
        }
        [[nodiscard]] core::String GetSelectionModeName() const
        {
            return core::String(m_selectionMode == SelectionMode::Multi ? core::StringView(u8"multi") : core::StringView(u8"single"));
        }

        /// Gives every row this height. With 0, each row is measured, using the estimate until then.
        void SetRowHeight(f32 height)
        {
            m_rowHeight = core::Max(0.0f, height);
            SetScrollStep(m_rowHeight > 0.0f ? m_rowHeight : m_estimate);
            m_heights.Reset(ItemCount(), RowEstimate());
            RequestLayout();
        }
        [[nodiscard]] f32 GetRowHeight() const noexcept { return m_rowHeight; }
        /// Sets the height assumed for a row until it is realised and measured.
        void SetEstimatedRowHeight(f32 height)
        {
            m_estimate = core::Max(1.0f, height);
            if (m_rowHeight <= 0.0f)
                m_heights.Reset(ItemCount(), m_estimate);
            RequestLayout();
        }
        [[nodiscard]] f32 GetEstimatedRowHeight() const noexcept { return m_estimate; }
        /// Returns the number of realised rows: those in view plus one past each edge.
        [[nodiscard]] usize VisibleRowCount() const noexcept { return m_live.Size(); }
        /// Returns the realised row showing `item`, or null when it is not in view.
        [[nodiscard]] ItemRow* RowForItem(i32 item) const
        {
            for (const core::RefPtr<ItemRow>& row : m_live)
                if (row->m_item == item)
                    return row.Get();
            return nullptr;
        }
        /// Returns the item under a point in world space, or -1 for none.
        [[nodiscard]] i32 ItemAt(core::Float2 world) const
        {
            for (const core::RefPtr<ItemRow>& row : m_live)
                if (row->GetScreenBounds().Contains(world))
                    return row->m_item;
            return -1;
        }
        /// Returns the item's bounds in the content. The height is an estimate until the row is
        /// measured.
        [[nodiscard]] Rect ItemBounds(i32 item) const
        {
            const Rect box = GetContentBounds();
            const i32 i = core::Max(0, item);
            if (m_layout == ItemLayout::Grid)
            {
                const i32 across = GridColumns(box.width);
                const f32 w = box.width / static_cast<f32>(across);
                return CellBounds(box, i, across, w, box.y + ContentTopInset() + ItemTop(i), ItemHeight(i));
            }
            const f32 width = RowWidth(box.width);
            return Rect{RowLeft(box, width), box.y + ContentTopInset() + ItemTop(i), width, ItemHeight(i)};
        }
        /// Returns the left edge of a row `width` wide. Normally that is the box's left edge. When the
        /// view scrolls from the right (right to left, with a table wider than its view), the row
        /// ends at the box's right edge and overflows to the left.
        [[nodiscard]] f32 RowLeft(const Rect& box, f32 width) const { return ScrollsFromRight() ? box.Right() - width : box.x; }
        /// Returns a grid cell's bounds, counting across its line from the inline start.
        [[nodiscard]] Rect CellBounds(const Rect& box, i32 item, i32 across, f32 width, f32 y, f32 height) const
        {
            return MirroredIn(Rect{box.x + static_cast<f32>(item % across) * width, y, width, height}, box, IsRightToLeft());
        }

        // === Grid ===
        /// Sets whether items are listed as rows or laid out as grid cells.
        void SetLayoutMode(ItemLayout layout)
        {
            m_layout = layout;
            ForEachRow([](ItemRow& row) { row.m_dirty = true; });
            RequestLayout();
        }
        [[nodiscard]] ItemLayout GetLayoutMode() const noexcept { return m_layout; }
        /// Sets the layout mode from "list" or "grid" (markup's `layout`).
        void SetLayoutModeName(const core::String& name)
        {
            SetLayoutMode(core::Trim(name.AsView()) == core::StringView(u8"grid") ? ItemLayout::Grid : ItemLayout::List);
        }
        [[nodiscard]] core::String GetLayoutModeName() const
        {
            return core::String(m_layout == ItemLayout::Grid ? core::StringView(u8"grid") : core::StringView(u8"list"));
        }
        /// Sets a grid cell's minimum width and its height. As many cells as fit go across each
        /// line, stretched to fill it.
        void SetCellWidth(f32 width)
        {
            m_cellSize.x = core::Max(1.0f, width);
            RequestLayout();
        }
        [[nodiscard]] f32 GetCellWidth() const noexcept { return m_cellSize.x; }
        void SetCellHeight(f32 height)
        {
            m_cellSize.y = core::Max(1.0f, height);
            RequestLayout();
        }
        [[nodiscard]] f32 GetCellHeight() const noexcept { return m_cellSize.y; }
        /// Returns how many cells a grid lays across each line (1 for a list).
        [[nodiscard]] i32 GetColumnsAcross() const { return GridColumns(GetContentBounds().width); }
        /// Scrolls so `item` is in view. Nearest scrolls by the least amount.
        void ScrollToItem(i32 item, ScrollAlign align = ScrollAlign::Nearest)
        {
            if (item < 0 || item >= static_cast<i32>(ItemCount()))
                return;
            const Rect box = GetContentBounds();
            const f32 view = core::Max(0.0f, box.height - ContentTopInset());
            const f32 top = ItemTop(item);
            const f32 bottom = top + ItemHeight(item);
            const f32 now = GetScrollTarget().y;
            f32 to = now;
            switch (align)
            {
            case ScrollAlign::Start: to = top; break;
            case ScrollAlign::End: to = bottom - view; break;
            case ScrollAlign::Center: to = (top + bottom - view) * 0.5f; break;
            case ScrollAlign::Nearest:
            default:
                if (top < now)
                    to = top;
                else if (bottom > now + view)
                    to = core::Min(top, bottom - view);
                break;
            }
            // The item's offset is known now, so update the scroll extent first so the scroll range
            // can reach it.
            SetScrollExtent(core::Float2{GetContentExtent().x, ContentTopInset() + ContentHeight()});
            ScrollTo(core::Float2{GetScrollTarget().x, to});
            if (!m_realizing)
                Realize();
        }

        // === Drag and drop ===
        // Rows drag the selection with the view's `drag-type` as the payload type. With no
        // `drag-type`, rows do not drag.
        /// Returns the ids of the rows in a drag that started from this view, in item order.
        [[nodiscard]] const core::Array<ItemId>& GetDraggedIds() const noexcept { return m_dragIds; }
        /// While a drag is over the view, returns the item a drop would land at, or -1 for the end.
        /// GetDropPlace gives where relative to that item.
        [[nodiscard]] i32 GetDropItem() const noexcept { return m_dropOver ? m_dropItem : -1; }
        [[nodiscard]] DropPlace GetDropPlace() const noexcept { return m_dropPlace; }
        [[nodiscard]] bool IsDropOver() const noexcept { return m_dropOver; }

        // === Appearance ===
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"selection"));
            out.PushBack(core::StringView(u8"drop-indicator"));
        }
        // The rows draw the selection in the ::selection part's style.
        void OnPartStyleChange(core::StringView part) override
        {
            if (part == core::StringView(u8"selection"))
                ForEachRow([](ItemRow& row) { row.Invalidate(); });
        }

        // === Selection ===
        void SetSelectionMode(SelectionMode mode) noexcept { m_selectionMode = mode; }
        [[nodiscard]] SelectionMode GetSelectionMode() const noexcept { return m_selectionMode; }
        /// Sets the callback for selection changes made by the user; changes made from code do not
        /// call it. The index passed is the current item's.
        void SetOnSelectionChanged(core::Function<void(ModelIndex)> callback) { m_onSelection = core::Move(callback); }
        /// Sets the callback for a row being activated by a double-click, Return or the pad's South
        /// button.
        void SetOnActivated(core::Function<void(ModelIndex)> callback) { m_onActivated = core::Move(callback); }
        /// Lets the owner decide what a drag of the view's items carries. The callback receives the
        /// payload as the view filled it (its drag type, the view and the ids, which a drop within
        /// the view relies on) and the dragged ids in item order, and may replace what the payload
        /// carries, as an asset browser does with its assets.
        void SetOnDragPayload(core::Function<void(DragPayload& payload, core::Span<const ItemId> ids)> callback) { m_onDragPayload = core::Move(callback); }

        [[nodiscard]] usize GetSelectionCount() const noexcept { return m_selected.Size(); }
        [[nodiscard]] bool IsIdSelected(ItemId id) const { return m_selected.Contains(id); }
        [[nodiscard]] bool IsItemSelected(i32 item) const { return item >= 0 && IsIdSelected(ItemIdAt(item)); }
        /// Appends the selected ids to `out`.
        void GetSelectedIds(core::Array<ItemId>& out) const
        {
            for (ItemId id : m_selected)
                out.PushBack(id);
        }
        /// Returns the current item (where the keyboard is) when it is selected, otherwise -1.
        [[nodiscard]] i32 GetSelectedItem() const
        {
            m_selectionChanged.Track(); // a binding that reads this follows later changes
            const i32 item = ItemOfId(m_current);
            return item >= 0 && IsIdSelected(m_current) ? item : -1;
        }
        /// Returns the item the keyboard is on, or -1 for none.
        [[nodiscard]] i32 GetCurrentItem() const { return ItemOfId(m_current); }
        [[nodiscard]] ModelIndex GetSelectedIndex() const
        {
            const i32 item = GetSelectedItem();
            return item >= 0 ? ItemToModelIndex(item) : ModelIndex{};
        }

        // === Accessibility ===
        // The GUI builds the accessibility tree (AccessibilityImpl.cpp) and adds the view's items
        // as nodes. Not every item gets one: only the realised rows plus a page of items on each
        // side, the current item and the selected ones. Each node states its position among all
        // the items, so a list of a hundred thousand items sends only a few dozen nodes.
        [[nodiscard]] usize GetItemCount() const { return ItemCount(); }
        [[nodiscard]] core::String GetItemText(i32 item) const { return TypeAheadText(item); }
        [[nodiscard]] ModelIndex GetItemIndex(i32 item) const { return ItemToModelIndex(item); }
        /// Appends the items that get accessibility nodes to `out`, in item order.
        void GetAccessItems(core::Array<i32>& out) const
        {
            const i32 count = static_cast<i32>(ItemCount());
            if (count <= 0)
                return;
            i32 first = count, last = -1;
            for (const core::RefPtr<ItemRow>& row : m_live)
                if (row->m_item >= 0)
                {
                    first = core::Min(first, row->m_item);
                    last = core::Max(last, row->m_item);
                }
            if (last < 0)
                first = last = core::Max(0, GetCurrentItem());
            const i32 page = core::Max(1, last - first + 1);
            core::HashSet<i32> wanted;
            for (i32 i = core::Max(0, first - page); i <= core::Min(count - 1, last + page); ++i)
                (void)wanted.Insert(i);
            if (const i32 current = GetCurrentItem(); current >= 0)
                (void)wanted.Insert(current);
            usize selected = 0;
            for (ItemId id : m_selected)
                if (const i32 item = ItemOfId(id); item >= 0 && selected++ < kAccessSelectedCap)
                    (void)wanted.Insert(item);
            for (i32 item : wanted)
                out.PushBack(item);
            std::sort(out.begin(), out.end());
        }
        /// Returns the item's key among this view's accessibility nodes, creating it the first time
        /// the item is asked for. The key stays with the item as rows are recycled, scrolled and
        /// moved. The GUI combines it with the view's own key.
        [[nodiscard]] u32 GetAccessKeyOfItem(i32 item)
        {
            const ItemId id = ItemIdAt(item);
            if (const u32* key = m_accessKeys.Find(id))
                return *key;
            const u32 key = m_nextAccessKey++;
            m_accessKeys.InsertOrAssign(id, key);
            m_accessItems.InsertOrAssign(key, id);
            return key;
        }
        /// Returns the item a key currently names, or -1 when that item is gone.
        [[nodiscard]] i32 GetItemOfAccessKey(u32 key) const
        {
            const ItemId* id = m_accessItems.Find(key);
            return id != nullptr ? ItemOfId(*id) : -1;
        }
        /// Returns this view's key among the GUI's item views, or 0 until the GUI assigns one.
        [[nodiscard]] u32 GetAccessViewKey() const noexcept { return m_accessViewKey; }
        void SetAccessViewKey(u32 key) noexcept { m_accessViewKey = key; }
        /// Holds what each item's node last reported, by id, so an unchanged node is not sent again.
        [[nodiscard]] core::HashMap<u64, u64>& AccessItemsSent() noexcept { return m_accessSent; }
        /// Moves to an item for a screen reader as the keyboard would: selects it and scrolls to it.
        void FocusItemFromAccess(i32 item)
        {
            RequestFocus();
            SelectByUser(item, 0);
            ScrollToItem(item);
        }
        void ActivateItemFromAccess(i32 item) { ActivateItem(item); }
        /// Selects one item from code and scrolls it into view, without sending events. -1 clears
        /// the selection.
        void SetSelectedItem(i32 item)
        {
            if (item < 0 || item >= static_cast<i32>(ItemCount()))
            {
                ClearSelection();
                return;
            }
            SelectOnly(item);
            ScrollToItem(item);
        }
        /// Selects the items with `ids` from code, without sending events. Without multi-selection
        /// only the first id is selected. The first id becomes the current item and is scrolled
        /// into view. An id that is not shown now (under a closed tree node) stays selected and
        /// shows as selected once it appears. An empty span clears the selection.
        void SetSelectedIds(core::Span<const ItemId> ids)
        {
            m_selected.Clear();
            m_anchorSelection = kNoItem;
            for (const ItemId id : ids)
            {
                if (m_selectionMode != SelectionMode::Multi && !m_selected.IsEmpty())
                    break;
                (void)m_selected.Insert(id);
            }
            if (!ids.IsEmpty())
            {
                m_current = ids[0];
                m_anchorSelection = ids[0];
            }
            RefreshSelectionStates();
            if (const i32 item = ids.IsEmpty() ? -1 : ItemOfId(ids[0]); item >= 0)
                ScrollToItem(item);
        }
        void ClearSelection()
        {
            m_selected.Clear();
            m_anchorSelection = kNoItem;
            RefreshSelectionStates();
        }
        /// Selects every selectable item, as Ctrl+A does. Does nothing without multi-selection.
        void SelectAll()
        {
            if (m_selectionMode != SelectionMode::Multi)
                return;
            for (i32 i = 0; i < static_cast<i32>(ItemCount()); ++i)
                if (IsItemSelectable(i))
                    (void)m_selected.Insert(ItemIdAt(i));
            RefreshSelectionStates();
        }
        /// Returns whether the user may select the item. The model decides; a section header, for
        /// example, is not selectable.
        [[nodiscard]] bool IsItemSelectable(i32 item) const
        {
            return item >= 0 && item < static_cast<i32>(ItemCount()) && (m_model == nullptr || m_model->IsSelectable(ItemToModelIndex(item)));
        }

        // === Editing ===
        /// Edits `item` in place (`column` picks the cell in a table) if the model cell is editable
        /// and the begin hook allows it. Returns false otherwise.
        bool BeginEdit(i32 item, i32 column = 0);
        /// Ends an edit, either committing the value (if the commit hook and the model accept it)
        /// or cancelling. Returns false when the value was refused and the edit continues.
        bool EndEdit(bool commit);
        [[nodiscard]] bool IsEditing() const noexcept { return m_editing; }
        [[nodiscard]] TextField* GetEditor() const noexcept { return m_editor.Get(); }
        /// Sets a hook that may refuse to start an edit by returning false.
        void SetOnEditBegin(core::Function<bool(const ModelIndex&)> hook) { m_onEditBegin = core::Move(hook); }
        /// Sets a hook that may refuse a value. When it returns false the edit continues with the
        /// editor marked `.invalid`; otherwise the model's SetData decides.
        void SetOnEditCommit(core::Function<bool(const ModelIndex&, core::StringView)> hook) { m_onEditCommit = core::Move(hook); }
        void SetOnEditCancel(core::Function<void(const ModelIndex&)> hook) { m_onEditCancel = core::Move(hook); }

        // === IModelClient ===
        void OnModelReset() override
        {
            OnModelChanged();
            if (m_editing && ItemOfId(m_editId) < 0)
                (void)EndEdit(false);
            m_heights.Reset(ItemCount(), RowEstimate());
            ForEachRow([](ItemRow& row) { row.m_dirty = true; });
            PruneSelection();
            ClampScrollAnchor();
            RequestLayout();
        }
        void OnRowsInserted(const ModelIndex& parent, i32 first, i32 count) override
        {
            if (parent.IsValid() || !IsFlat())
            {
                OnModelReset();
                return;
            }
            m_heights.Insert(static_cast<usize>(first), static_cast<usize>(count), RowEstimate());
            if (first <= m_scrollAnchor)
                m_scrollAnchor += count; // keep the rows in view where they are
            RequestLayout();
        }
        void OnRowsRemoved(const ModelIndex& parent, i32 first, i32 count) override
        {
            if (parent.IsValid() || !IsFlat())
            {
                OnModelReset();
                return;
            }
            m_heights.Remove(static_cast<usize>(first), static_cast<usize>(count));
            if (m_editing && ItemOfId(m_editId) < 0)
                (void)EndEdit(false);
            if (m_scrollAnchor >= first + count)
                m_scrollAnchor -= count;
            else if (m_scrollAnchor >= first)
            {
                m_scrollAnchor = first; // the anchor row was removed: anchor to the row now in its place, at its top
                m_scrollAnchorDelta = 0.0f;
            }
            PruneSelection();
            ClampScrollAnchor();
            RequestLayout();
        }
        void OnRowsMoved(const ModelIndex& parent, i32 first, i32 count, i32 to) override
        {
            if (parent.IsValid() || !IsFlat())
            {
                OnModelReset();
                return;
            }
            m_heights.Move(static_cast<usize>(first), static_cast<usize>(count), static_cast<usize>(to));
            RequestLayout();
        }
        void OnRowsChanged(const ModelIndex& parent, i32 first, i32 count) override
        {
            if (parent.IsValid() || !IsFlat())
            {
                OnModelReset();
                return;
            }
            for (i32 i = first; i < first + count; ++i)
            {
                m_heights.Unmeasure(static_cast<usize>(i));
                if (ItemRow* row = RowForItem(i))
                    row->m_dirty = true;
            }
            RequestLayout();
        }

    protected:
        // === Overridden by subclasses ===
        [[nodiscard]] virtual usize ItemCount() const { return m_model != nullptr ? m_model->RowCount() : 0; }
        [[nodiscard]] virtual ModelIndex ItemToModelIndex(i32 item) const
        {
            return m_model != nullptr ? m_model->Index(item) : ModelIndex{};
        }
        [[nodiscard]] virtual ItemId ItemIdAt(i32 item) const
        {
            return m_model != nullptr && item >= 0 ? m_model->IdOf(ItemToModelIndex(item)) : kNoItem;
        }
        [[nodiscard]] virtual i32 ItemOfId(ItemId id) const
        {
            if (m_model == nullptr || id == kNoItem)
                return -1;
            const ModelIndex index = m_model->IndexOfId(id);
            return index.IsValid() ? index.Row : -1;
        }
        /// Returns whether the items are the model's top-level rows. A tree's items are not, so a
        /// tree resets on every range change.
        [[nodiscard]] virtual bool IsFlat() const { return true; }
        /// Creates and fills a default row's content when no factory is set.
        [[nodiscard]] virtual core::RefPtr<Node> CreateContent(u32 type) = 0;
        virtual void BindContent(Node& content, i32 item, const ModelIndex& index) = 0;
        /// Returns the room above the rows, such as a table's header.
        [[nodiscard]] virtual f32 ContentTopInset() const { return 0.0f; }
        /// Returns a row's width in a content box `width` wide; a table's columns may make it wider.
        [[nodiscard]] virtual f32 RowWidth(f32 width) const { return width; }
        /// Called when the model is set or reset; a tree flattens its nodes, a table rebuilds its
        /// header.
        virtual void OnModelChanged() {}
        /// Called after the rows are placed; a table places its header here.
        virtual void OnLayoutDecorations() {}
        /// Handles a key before the view's own handling (a tree's Left and Right). Returns true
        /// when used.
        virtual bool OnExtraKey(KeyCode key, u32 modifiers)
        {
            (void)key, (void)modifiers;
            return false;
        }
        /// Returns where the editor goes over a row, in the row's space.
        [[nodiscard]] virtual Rect EditRect(const ItemRow& row, i32 column) const
        {
            (void)column;
            return row.GetContentBounds();
        }
        /// Returns the text type-ahead matches: the display text of the item's first column.
        [[nodiscard]] virtual core::String TypeAheadText(i32 item) const
        {
            return m_model != nullptr ? m_model->Data(ItemToModelIndex(item)).ToString() : core::String{};
        }
        /// Returns the tag given to rows (`listviewrow`, `treerow`, and so on).
        [[nodiscard]] virtual core::StringView RowTag() const { return core::StringView(u8"itemrow"); }

        // === Helpers for a subclass ===
        template <typename F>
        void ForEachRow(F&& f)
        {
            for (const core::RefPtr<ItemRow>& row : m_live)
                f(*row);
            for (Pool& pool : m_pools)
                for (const core::RefPtr<ItemRow>& row : pool.Rows)
                    f(*row);
        }
        [[nodiscard]] const core::Array<core::RefPtr<ItemRow>>& LiveRows() const noexcept { return m_live; }
        /// Tells the view that the items changed in a way only the subclass knows about (a tree
        /// opened a node). Heights are reset and the rows are filled again.
        void ItemsChanged()
        {
            m_heights.Reset(ItemCount(), RowEstimate());
            ForEachRow([](ItemRow& row) { row.m_dirty = true; });
            PruneSelection();
            ClampScrollAnchor();
            RequestLayout();
            if (!m_realizing && GetSize().y > 0.0f)
                Realize();
        }
        /// Selects as a click with these modifiers does. A subclass's own rows call this.
        void SelectByUser(i32 item, u32 modifiers);

        void OnLayout() override
        {
            Realize();
            OnLayoutDecorations();
        }
        void OnScrollChange() override
        {
            if (m_realizing)
                return;
            UpdateScrollAnchor();
            if (GetSize().y <= 0.0f)
                return;
            Realize(); // realise the rows now in view immediately, so a press lands on them
            OnLayoutDecorations();
        }
        void OnTextStyleChange() override
        {
            ForEachRow([this](ItemRow& row) { ShareTextStyle(row); });
            if (m_rowHeight <= 0.0f)
                m_heights.Reset(ItemCount(), RowEstimate()); // a new font means every row is measured again
            RequestLayout();
        }
        void OnKeyDown(const KeyEvent& event) override;
        // Typed text drives type-ahead, so the view asks for text input while it has items.
        [[nodiscard]] bool WantsTextInput() const override { return IsTreeEnabled() && ItemCount() > 0; }
        void OnTextInput(const TextInputEvent& event) override
        {
            if (event.Target == this && TypeAhead(event.Text))
                event.Handled = true;
        }
        bool OnActivate() override
        {
            const i32 item = GetCurrentItem();
            if (item < 0)
                return false;
            ActivateItem(item);
            return true;
        }
        // A context menu request over an unselected row selects that row first. The event is not
        // handled here, so it goes on up to whoever opens the menu.
        void OnContextMenu(const MouseEvent& event) override
        {
            const i32 item = ItemAt(event.Position);
            if (item >= 0 && !IsItemSelected(item))
                SelectByUser(item, 0);
        }

        // --- Drag and drop ---
        bool OnDragRequested(DragRequest& request) override
        {
            if (GetDragType().IsEmpty() || m_model == nullptr)
                return false;
            const i32 item = request.FromKeyboard ? GetCurrentItem() : ItemAt(ConvertToWorldSpace(request.Press));
            if (item < 0)
                return false;
            m_deferredItem = -1;
            if (!IsItemSelected(item))
            {
                SelectOnly(item);
                NotifySelectionByUser();
            }
            // Collect the selection in item order; a single selected row needs no scan.
            m_dragIds.Clear();
            if (m_selected.Size() == 1)
                m_dragIds.PushBack(ItemIdAt(item));
            else
                for (i32 i = 0; i < static_cast<i32>(ItemCount()); ++i)
                    if (IsItemSelected(i))
                        m_dragIds.PushBack(ItemIdAt(i));
            request.Payload.Type = GetDragType();
            request.Payload.Value = HandleAs(Node::StaticType()); // a weak handle to the view, from which a target reads the ids
            request.Payload.Label = m_dragIds.Size() == 1 ? TypeAheadText(item) : core::Format(u8"{} items", m_dragIds.Size());
            request.Payload.Allowed = DropEffectsCopy | DropEffectsMove;
            if (m_onDragPayload)
                m_onDragPayload(request.Payload, m_dragIds.AsSpan());
            return true;
        }
        void OnDragQuery(DropQuery& query) override;
        void OnDragLeave(const DragEvent&) override { SetDropIndicator(false, -1, DropPlace::After); }
        void OnDrop(const DragEvent& event) override;
        void OnDragEnd(const DragResult& result) override
        {
            // Rows moved to another model are removed from this one. A move within this model was
            // already done by the drop.
            if (result.Effect == DropEffect::Move && m_model != nullptr && !m_dragIds.IsEmpty())
            {
                const ItemView* target = core::Cast<ItemView>(result.Target);
                if (target == nullptr || target->GetModel() != m_model)
                    (void)m_model->RemoveIds(m_dragIds);
            }
            m_dragIds.Clear();
        }
        void OnDrawAbove(DrawContext& ctx) override;

    private:
        friend class ItemRow;

        // Works out where a drop at `query.Local` lands, as a ModelDrop for the model, and returns
        // the model's answer in `effect` and `reason`.
        ModelDrop DropAt(const DropQuery& query, DropEffect& effect, core::String& reason);
        void SetDropIndicator(bool over, i32 item, DropPlace place)
        {
            if (over == m_dropOver && item == m_dropItem && place == m_dropPlace)
                return;
            m_dropOver = over;
            m_dropItem = item;
            m_dropPlace = place;
            Invalidate();
        }
        // A press on a row of a multi-selection keeps the selection so it can be dragged. If the
        // press ends in a click instead, the clicked row is now selected alone.
        void RowClicked(ItemRow& row, const MouseEvent&)
        {
            if (m_deferredItem >= 0 && m_deferredItem == row.m_item)
            {
                SelectOnly(m_deferredItem);
                NotifySelectionByUser();
            }
            m_deferredItem = -1;
        }

        struct Pool
        {
            u32 Type = 0;
            core::Array<core::RefPtr<ItemRow>> Rows;
        };

        struct RowTemplate
        {
            core::String Type;
            core::String Markup;
        };
        // Switches to rows made from the markup templates. Each row type is made by loading its
        // template's markup with item bindings, and the bindings fill it for each item.
        void UseTemplates()
        {
            ItemView* self = this;
            RowFactory factory;
            factory.TypeOf = [self](const ModelIndex& index) { return self->TemplateFor(index); };
            factory.Create = [self](u32 type) -> core::RefPtr<Node>
            {
                auto content = core::MakeRef<TemplateRowContent>(self->MemoryAllocator());
                const ElementSet* elements = self->GetElementSet();
                if (elements == nullptr || type >= self->m_templates.Size())
                    return content;
                MarkupLoader loader(*elements, self->MemoryAllocator());
                loader.SetItemBindings(&content->Bindings());
                if (core::RefPtr<Node> tree = loader.LoadFromString(self->m_templates[type].Markup.AsView()))
                    content->AddChild(tree.Get());
                return content;
            };
            factory.Bind = [self](Node& content, const ModelIndex& index)
            {
                for (const ItemBinding& binding : static_cast<TemplateRowContent&>(content).Bindings())
                {
                    core::String text;
                    for (const MarkupTemplatePart& part : binding.Parts)
                        text.Append(part.IsName ? self->ItemField(index, part.Text.AsView()).AsView() : part.Text.AsView());
                    MarkupLoader::ApplyAttribute(*binding.Target, binding.Attribute.AsView(), text.AsView());
                }
            };
            m_factory = core::Move(factory);
            DropRows();
            RequestLayout();
        }
        // Returns the template for an item: the one whose type matches the item's text in the
        // row-type column, else the default. The default is the first template without a type, or
        // the first template when all have one.
        [[nodiscard]] u32 TemplateFor(const ModelIndex& index) const
        {
            u32 fallback = 0;
            for (usize t = 0; t < m_templates.Size(); ++t)
                if (m_templates[t].Type.IsEmpty())
                {
                    fallback = static_cast<u32>(t);
                    break;
                }
            if (m_rowTypeColumn.IsEmpty())
                return fallback;
            const core::String field = core::Format(u8"item.{}", m_rowTypeColumn);
            const core::String kind = ItemField(index, field.AsView());
            for (usize t = 0; t < m_templates.Size(); ++t)
                if (!m_templates[t].Type.IsEmpty() && m_templates[t].Type.AsView() == kind.AsView())
                    return static_cast<u32>(t);
            return fallback;
        }
        // Returns the text a template name stands for: `item` is the first column, `item.Column`
        // the column of that name, and `index` the item's row.
        [[nodiscard]] core::String ItemField(const ModelIndex& index, core::StringView name) const
        {
            if (m_model == nullptr)
                return core::String{};
            if (name == core::StringView(u8"index"))
                return core::Format(u8"{}", index.Row);
            if (name == core::StringView(u8"item"))
                return m_model->Data(index).ToString();
            const core::StringView prefix(u8"item.");
            if (name.Size() <= prefix.Size() || name.SubStr(0, prefix.Size()) != prefix)
                return core::String{};
            const core::StringView column = name.SubStr(prefix.Size(), name.Size() - prefix.Size());
            for (usize c = 0; c < m_model->ColumnCount(); ++c)
                if (m_model->ColumnName(c).AsView() == column)
                    return m_model->Data(m_model->Index(index.Row, static_cast<i32>(c), m_model->ParentIndex(index))).ToString();
            return core::String{};
        }

        // --- Geometry: a list places rows by their heights, a grid places lines by the cell height ---
        [[nodiscard]] i32 GridColumns(f32 width) const
        {
            return m_layout == ItemLayout::Grid ? core::Max(1, static_cast<i32>(width / m_cellSize.x)) : 1;
        }
        [[nodiscard]] i32 Across() const { return GridColumns(GetContentBounds().width); }
        [[nodiscard]] f32 ItemTop(i32 item) const
        {
            if (m_layout == ItemLayout::Grid)
                return static_cast<f32>(item / Across()) * m_cellSize.y;
            return m_heights.Top(static_cast<usize>(item));
        }
        [[nodiscard]] f32 ItemHeight(i32 item) const
        {
            return m_layout == ItemLayout::Grid ? m_cellSize.y : m_heights.At(static_cast<usize>(item));
        }
        [[nodiscard]] f32 ContentHeight() const
        {
            if (m_layout == ItemLayout::Grid)
            {
                const i32 across = Across();
                return static_cast<f32>((static_cast<i32>(ItemCount()) + across - 1) / across) * m_cellSize.y;
            }
            return m_heights.Total();
        }
        void RealizeGrid();

        [[nodiscard]] static bool StylePending(const Node& node)
        {
            if (node.IsStyleDirty())
                return true;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (StylePending(*node.GetChildAt(i)))
                    return true;
            return false;
        }
        [[nodiscard]] f32 RowEstimate() const noexcept { return m_rowHeight > 0.0f ? m_rowHeight : m_estimate; }
        [[nodiscard]] u32 RowTypeOf(i32 item) const
        {
            return m_factory.TypeOf ? m_factory.TypeOf(ItemToModelIndex(item)) : 0u;
        }

        // Realises, measures and places the rows in view plus one past each edge. When rows above
        // the scroll anchor measure differently from their estimate, the anchor stays in place.
        void Realize();
        bool TypeAhead(core::StringView typed);
        // Returns a row for `item`: the live row already showing its id, else one from its type's
        // pool, else a new one.
        ItemRow* TakeRow(i32 item, core::Array<core::RefPtr<ItemRow>>& next);
        void ReleaseRow(ItemRow& row)
        {
            if (m_editing && row.m_id == m_editId)
                (void)EndEdit(true); // the edited row scrolled away: commit what was typed
            row.SetDisplayed(false);
            row.m_id = kNoItem;
            row.m_item = -1;
            PoolOf(row.m_type).Rows.PushBack(core::RefPtr<ItemRow>(&row));
        }
        Pool& PoolOf(u32 type)
        {
            for (Pool& pool : m_pools)
                if (pool.Type == type)
                    return pool;
            m_pools.PushBack(Pool{type, {}});
            return m_pools[m_pools.Size() - 1];
        }
        void DropRows()
        {
            ForEachRow([](ItemRow& row) { row.RemoveFromParent(); });
            m_live.Clear();
            m_pools.Clear();
        }

        // --- The scroll anchor: an item, and how far below its top the view's top is ---
        void ResetScrollAnchor()
        {
            m_scrollAnchor = 0;
            m_scrollAnchorDelta = 0.0f;
        }
        void UpdateScrollAnchor()
        {
            if (m_layout == ItemLayout::Grid)
                return; // grid cells do not change height
            const f32 y = GetScrollOffset().y;
            m_scrollAnchor = static_cast<i32>(m_heights.Find(y));
            m_scrollAnchorDelta = y - m_heights.Top(static_cast<usize>(m_scrollAnchor));
        }
        void ClampScrollAnchor()
        {
            const i32 count = static_cast<i32>(ItemCount());
            if (m_scrollAnchor >= count)
                ResetScrollAnchor();
        }

        // --- Selection ---
        void SelectOnly(i32 item)
        {
            const ItemId id = ItemIdAt(item);
            m_selected.Clear();
            (void)m_selected.Insert(id);
            m_current = id;
            m_anchorSelection = id;
            RefreshSelectionStates();
        }
        void SelectRange(i32 item)
        {
            const i32 anchor = ItemOfId(m_anchorSelection);
            const i32 from = anchor >= 0 ? anchor : item;
            m_selected.Clear();
            for (i32 i = core::Min(from, item); i <= core::Max(from, item); ++i)
                if (IsItemSelectable(i))
                    (void)m_selected.Insert(ItemIdAt(i));
            m_current = ItemIdAt(item);
            RefreshSelectionStates();
        }
        void ToggleSelected(i32 item)
        {
            const ItemId id = ItemIdAt(item);
            if (!m_selected.Remove(id))
                (void)m_selected.Insert(id);
            m_current = id;
            m_anchorSelection = id;
            RefreshSelectionStates();
        }
        void PruneSelection()
        {
            core::Array<ItemId> gone;
            for (ItemId id : m_selected)
                if (ItemOfId(id) < 0)
                    gone.PushBack(id);
            for (ItemId id : gone)
                (void)m_selected.Remove(id);
            if (ItemOfId(m_current) < 0)
                m_current = kNoItem;
            if (ItemOfId(m_anchorSelection) < 0)
                m_anchorSelection = kNoItem;
        }
        void RefreshSelectionStates()
        {
            m_selectionChanged.Notify();
            for (const core::RefPtr<ItemRow>& row : m_live)
                row->SetSelectedState(IsIdSelected(row->m_id));
        }
        void NotifySelectionByUser()
        {
            if (m_onSelection)
                m_onSelection(GetSelectedIndex());
            SendEvent(Event(EventType::Changed, this));
        }
        void ActivateItem(i32 item)
        {
            if (!IsItemSelectable(item))
                return;
            const ModelIndex index = ItemToModelIndex(item);
            if (m_onActivated)
                m_onActivated(index);
            SendEvent(ItemEvent(EventType::ItemActivate, this, item, index));
        }

        // Handles a press on a row. The left button selects, and a double press activates. A
        // right-button press is left to OnContextMenu, which selects the row for its menu.
        void RowPressed(ItemRow& row, const MouseEvent& event)
        {
            if (row.m_item < 0)
                return;
            if (event.Button == MouseButton::Left)
            {
                event.Handled = true;
                RequestFocus();
                if (event.ClickCount == 2)
                {
                    ActivateItem(row.m_item);
                    return;
                }
                // A plain press on a row of a multi-selection leaves the selection as it is, so a
                // drag takes all of it. If no drag starts, the click selects the row alone.
                if (m_selectionMode == SelectionMode::Multi && event.Modifiers == 0 && m_selected.Size() > 1 && IsItemSelected(row.m_item))
                {
                    m_deferredItem = row.m_item;
                    m_current = ItemIdAt(row.m_item);
                    return;
                }
                SelectByUser(row.m_item, event.Modifiers);
            }
        }

        IModel* m_model = nullptr; // non-owning, unless it points into m_ownModel or m_listModel
        RefPtr<ListModel> m_listModel; // a script's model being shown
        core::UniquePtr<StringListModel> m_ownModel; // built from markup's `items`
        RowFactory m_factory;
        Array<RowTemplate> m_templates;
        core::String m_rowTypeColumn;
        mutable Observable m_selectionChanged; // notifies bindings that read the selection
        core::Array<ItemId> m_dragIds;        // the rows in a drag that started from this view
        i32 m_deferredItem = -1;              // a pressed row of a multi-selection, selected alone if the press ends in a click
        bool m_dropOver = false;              // a drag is over the view and a drop would land here
        i32 m_dropItem = -1;
        DropPlace m_dropPlace = DropPlace::After;
        ModelDrop m_lastDrop;                 // the drop the last OnDragQuery found
        RowHeights m_heights;
        core::Array<core::RefPtr<ItemRow>> m_live; // realised rows, in item order
        // Accessibility: the key of each item and the item of each key, this view's key, and what
        // each node last reported.
        core::HashMap<ItemId, u32> m_accessKeys;
        core::HashMap<u32, ItemId> m_accessItems;
        u32 m_nextAccessKey = 1;
        u32 m_accessViewKey = 0;
        core::HashMap<u64, u64> m_accessSent;
        static constexpr usize kAccessSelectedCap = 50; // at most this many selected items get nodes
        core::Array<Pool> m_pools;                 // released rows, by type
        core::HashSet<ItemId> m_selected;
        ItemId m_current = kNoItem;         // the item the keyboard is on
        ItemId m_anchorSelection = kNoItem; // where a Shift range selection starts
        SelectionMode m_selectionMode = SelectionMode::Single;
        core::Function<void(ModelIndex)> m_onSelection;
        core::Function<void(ModelIndex)> m_onActivated;
        f32 m_rowHeight = 24.0f;
        f32 m_estimate = 24.0f;
        ItemLayout m_layout = ItemLayout::List;
        core::Function<void(DragPayload&, core::Span<const ItemId>)> m_onDragPayload; // lets the owner fill the payload
        core::Float2 m_cellSize{96.0f, 96.0f};
        i32 m_scrollAnchor = 0;
        f32 m_scrollAnchorDelta = 0.0f;
        bool m_realizing = false;
        // Type-ahead.
        TypeAheadSearch m_typeAhead;
        // Editing.
        core::RefPtr<TextField> m_editor;
        bool m_editing = false;
        ItemId m_editId = kNoItem;
        i32 m_editColumn = 0;
        core::Function<bool(const ModelIndex&)> m_onEditBegin;
        core::Function<bool(const ModelIndex&, core::StringView)> m_onEditCommit;
        core::Function<void(const ModelIndex&)> m_onEditCancel;
    };

    // === ItemRow ===

    inline ItemRow::ItemRow(ItemView& view) : m_view(view)
    {
        SetTag(view.RowTag());
    }

    inline void ItemRow::OnMouseDown(const MouseEvent& event) { m_view.RowPressed(*this, event); }
    inline void ItemRow::OnMouseClick(const MouseEvent& event) { m_view.RowClicked(*this, event); }

    inline void ItemRow::OnDraw(DrawContext& ctx, const Rect& localBounds)
    {
        UIWidget::OnDraw(ctx, localBounds);
        if (m_selected)
            m_view.GetPartStyle(u8"selection").Draw(ctx, GetLocalBounds(), FilledBox(Color{0.18f, 0.37f, 0.62f, 1.0f}));
    }

    // The content fills the row, and an active editor lies over it where EditRect says.
    inline void ItemRow::OnLayout()
    {
        const Rect box = GetContentBounds();
        if (m_content)
            m_content->Arrange(box);
        TextField* editor = m_view.m_editor.Get();
        if (editor != nullptr && editor->GetParent() == this && editor->IsDisplayed())
            editor->Arrange(m_view.EditRect(*this, m_view.m_editColumn));
    }

    // === ItemView ===

    inline ItemRow* ItemView::TakeRow(i32 item, core::Array<core::RefPtr<ItemRow>>& next)
    {
        const ItemId id = ItemIdAt(item);
        const u32 type = RowTypeOf(item);
        ItemRow* row = nullptr;
        for (const core::RefPtr<ItemRow>& live : m_live)
            if (!live->m_taken && live->m_id == id && live->m_type == type)
            {
                row = live.Get();
                break;
            }
        if (row == nullptr)
        {
            Pool& pool = PoolOf(type);
            if (pool.Rows.Size() != 0)
            {
                row = pool.Rows[pool.Rows.Size() - 1].Get();
                next.PushBack(pool.Rows[pool.Rows.Size() - 1]);
                pool.Rows.RemoveAt(pool.Rows.Size() - 1);
            }
            else
            {
                auto made = core::MakeRef<ItemRow>(MemoryAllocator(), *this);
                made->m_type = type;
                made->m_content = m_factory.Create ? m_factory.Create(type) : CreateContent(type);
                if (made->m_content)
                    made->AddChild(made->m_content.Get());
                ShareTextStyle(*made);
                AddChild(made.Get());
                row = made.Get();
                next.PushBack(made);
            }
            row->m_dirty = true;
            row->m_id = id;
        }
        else
            next.PushBack(core::RefPtr<ItemRow>(row));
        row->m_taken = true;
        row->m_item = item;
        if (row->m_dirty)
        {
            const ModelIndex index = ItemToModelIndex(item);
            if (row->m_content)
            {
                if (m_factory.Bind)
                    m_factory.Bind(*row->m_content, index);
                else
                    BindContent(*row->m_content, item, index);
            }
            row->m_dirty = false;
        }
        row->SetDisplayed(true);
        row->SetSelectedState(IsIdSelected(id));
        return row;
    }

    inline void ItemView::Realize()
    {
        if (m_realizing)
            return;
        if (m_layout == ItemLayout::Grid)
        {
            RealizeGrid();
            return;
        }
        m_realizing = true;
        const Rect box = GetContentBounds();
        const f32 inset = ContentTopInset();
        const f32 width = RowWidth(box.width);
        const usize count = ItemCount();
        if (m_heights.Size() != count)
            m_heights.Reset(count, RowEstimate());
        const bool measured = m_rowHeight <= 0.0f;
        const LayoutConstraints across{core::Float2{width, 0.0f}, core::Float2{width, kUnbounded}, core::Float2{width, -1.0f}};

        // Realise from the anchor. When a row above the anchor measures differently from its
        // estimate, the anchor's offset moves; the offset is restored and the rows are taken
        // again. A few passes are enough to settle.
        for (int pass = 0; pass < 3; ++pass)
        {
            const f32 top = GetScrollOffset().y;
            const f32 bottom = top + core::Max(0.0f, box.height - inset);
            core::Array<core::RefPtr<ItemRow>> next;
            usize first = count > 0 ? m_heights.Find(top) : 0;
            if (first > 0)
                --first; // include one row above the view
            // Return the rows leaving the view to their pools first, so the rows coming in reuse
            // them rather than making new ones. The range uses the heights as they stand.
            {
                usize last = first;
                while (last < count && m_heights.Top(last) < bottom)
                    ++last;
                // Compare each live row's id with the ids in range instead of looking up each row's
                // item, since finding an id's item is a search through a list.
                core::Array<ItemId> inRange;
                for (usize i = first; i <= core::Min(last, count - (count > 0 ? 1 : 0)) && count > 0; ++i)
                    inRange.PushBack(ItemIdAt(static_cast<i32>(i)));
                core::Array<core::RefPtr<ItemRow>> kept;
                for (const core::RefPtr<ItemRow>& row : m_live)
                {
                    bool stays = false;
                    for (ItemId id : inRange)
                        stays = stays || id == row->m_id;
                    if (stays)
                        kept.PushBack(row);
                    else
                        ReleaseRow(*row);
                }
                m_live = core::Move(kept);
            }
            for (const core::RefPtr<ItemRow>& row : m_live)
                row->m_taken = false;
            if (count > 0)
            {
                bool pastEnd = false;
                for (usize i = first; i < count && !pastEnd; ++i)
                {
                    pastEnd = m_heights.Top(i) >= bottom; // include one row below the view
                    ItemRow* row = TakeRow(static_cast<i32>(i), next);
                    // A row that is not styled yet (just made, or its template's attributes just
                    // written) is not marked measured, so it is measured again once styled. An
                    // unstyled row often measures zero because it has no font yet. In that case it
                    // keeps its estimate: rows of zero height would all start at the same offset,
                    // and the view would scroll to the last of them.
                    if (measured && !m_heights.IsMeasured(i))
                    {
                        const bool pending = StylePending(*row);
                        const f32 height = row->Measure(across).y;
                        if (!pending || height > 0.0f)
                            (void)m_heights.Set(i, height, !pending);
                    }
                }
            }
            for (const core::RefPtr<ItemRow>& row : m_live)
                if (!row->m_taken)
                    ReleaseRow(*row);
            m_live = core::Move(next);
            for (const core::RefPtr<ItemRow>& row : m_live)
                row->m_taken = false;

            SetScrollExtent(core::Float2{width, inset + m_heights.Total()});
            // The new scroll range moved the offset (a scroll requested before layout is clamped
            // now). Take the rows again at the new offset, since OnScrollChange ignores scrolls made
            // while realising.
            if (core::Abs(GetScrollOffset().y - top) >= 0.5f)
            {
                UpdateScrollAnchor();
                continue;
            }
            if (IsFlinging() || IsSmoothScrolling() || m_scrollAnchor >= static_cast<i32>(count))
                break;
            const f32 want = m_heights.Top(static_cast<usize>(m_scrollAnchor)) + m_scrollAnchorDelta;
            if (core::Abs(want - GetScrollOffset().y) < 0.5f || GetScrollRange().y < want - 0.5f)
                break;
            ApplyScrollOffset(core::Float2{GetScrollOffset().x, want});
        }

        for (const core::RefPtr<ItemRow>& row : m_live)
        {
            const usize i = static_cast<usize>(row->m_item);
            row->Arrange(Rect{RowLeft(box, width), box.y + inset + m_heights.Top(i), width, m_heights.At(i)});
        }
        m_realizing = false;
    }

    // Realises the grid cells in view plus one line past each edge, at the cell size, stretched across.
    inline void ItemView::RealizeGrid()
    {
        m_realizing = true;
        const Rect box = GetContentBounds();
        const f32 inset = ContentTopInset();
        const i32 count = static_cast<i32>(ItemCount());
        const i32 across = GridColumns(box.width);
        const f32 cellW = box.width / static_cast<f32>(across);
        const f32 top = GetScrollOffset().y;
        const f32 bottom = top + core::Max(0.0f, box.height - inset);
        const i32 firstLine = core::Max(0, static_cast<i32>(top / m_cellSize.y) - 1);
        const i32 lastLine = static_cast<i32>(bottom / m_cellSize.y) + 1;
        const i32 first = core::Min(count, firstLine * across);
        const i32 last = core::Min(count, (lastLine + 1) * across);

        core::Array<ItemId> inRange;
        for (i32 i = first; i < last; ++i)
            inRange.PushBack(ItemIdAt(i));
        core::Array<core::RefPtr<ItemRow>> kept;
        for (const core::RefPtr<ItemRow>& row : m_live)
        {
            bool stays = false;
            for (ItemId id : inRange)
                stays = stays || id == row->m_id;
            if (stays)
                kept.PushBack(row);
            else
                ReleaseRow(*row);
        }
        m_live = core::Move(kept);
        for (const core::RefPtr<ItemRow>& row : m_live)
            row->m_taken = false;
        core::Array<core::RefPtr<ItemRow>> next;
        for (i32 i = first; i < last; ++i)
            (void)TakeRow(i, next);
        // A row kept for its item but not taken (because the item's row type changed) also goes
        // back to its pool.
        for (const core::RefPtr<ItemRow>& row : m_live)
            if (!row->m_taken)
                ReleaseRow(*row);
        m_live = core::Move(next);
        for (const core::RefPtr<ItemRow>& row : m_live)
        {
            row->m_taken = false;
            const i32 i = row->m_item;
            row->Arrange(CellBounds(box, i, across, cellW, box.y + inset + static_cast<f32>(i / across) * m_cellSize.y, m_cellSize.y));
        }
        SetScrollExtent(core::Float2{box.width, inset + ContentHeight()});
        m_realizing = false;
    }

    inline void ItemView::SelectByUser(i32 item, u32 modifiers)
    {
        if (!IsItemSelectable(item))
            return;
        const bool ctrl = (modifiers & KeyModCtrl) != 0;
        const bool shift = (modifiers & KeyModShift) != 0;
        if (m_selectionMode == SelectionMode::Multi && shift && m_anchorSelection != kNoItem)
            SelectRange(item);
        else if (m_selectionMode == SelectionMode::Multi && ctrl)
            ToggleSelected(item);
        else
            SelectOnly(item);
        NotifySelectionByUser();
    }

    inline void ItemView::OnKeyDown(const KeyEvent& event)
    {
        const KeyCode key = static_cast<KeyCode>(event.KeyCode);
        if (m_editing && event.Target == m_editor.Get())
        {
            if (key == KeyCode::Escape)
            {
                (void)EndEdit(false);
                event.Handled = true;
            }
            return;
        }
        if (event.Target != this)
            return;
        if (OnExtraKey(key, event.Modifiers))
        {
            event.Handled = true;
            return;
        }
        const i32 items = static_cast<i32>(ItemCount());
        if (items == 0)
            return;
        const bool shift = (event.Modifiers & KeyModShift) != 0;
        const bool ctrl = (event.Modifiers & KeyModCtrl) != 0;
        if (key == KeyCode::F2 && event.Modifiers == 0)
        {
            if (GetCurrentItem() >= 0 && BeginEdit(GetCurrentItem()))
                event.Handled = true;
            return;
        }
        if (key == KeyCode::A && ctrl && m_selectionMode == SelectionMode::Multi)
        {
            SelectAll();
            NotifySelectionByUser();
            event.Handled = true;
            return;
        }
        if (key == KeyCode::Space && ctrl && m_selectionMode == SelectionMode::Multi && GetCurrentItem() >= 0)
        {
            ToggleSelected(GetCurrentItem());
            NotifySelectionByUser();
            event.Handled = true;
            return;
        }
        // A key that types text belongs to type-ahead. Its text arrives next as a text input event
        // (through the keyboard layout and any IME), so mark the key handled and let no binding of
        // the bare key take it.
        if ((event.Modifiers & ~static_cast<u32>(KeyModShift)) == 0 && KeyTypesText(key) && ItemCount() > 0)
        {
            event.Handled = true;
            return;
        }
        const i32 current = GetCurrentItem();
        // In a grid, one line is `across` items, and a page is as many lines (or rows) as fit.
        const i32 across = Across();
        const f32 rowH = core::Max(1.0f, ItemHeight(core::Max(0, current)));
        const i32 page = core::Max(1, static_cast<i32>((GetContentBounds().height - ContentTopInset()) / rowH) - 1) * across;
        const bool grid = m_layout == ItemLayout::Grid;
        i32 target = current;
        switch (key)
        {
        case KeyCode::Up: target = current < 0 ? 0 : current - across >= 0 ? current - across : current; break;
        case KeyCode::Down: target = current < 0 ? 0 : current + across < items ? current + across : current; break;
        case KeyCode::Left:
        case KeyCode::Right:
            // Left and Right move along a grid's lines; toward the inline end is the next item.
            if (!grid)
                return;
            target = InlineStep(key) < 0 ? (current <= 0 ? 0 : current - 1) : (current < 0 ? 0 : core::Min(items - 1, current + 1));
            break;
        case KeyCode::Home: target = 0; break;
        case KeyCode::End: target = items - 1; break;
        case KeyCode::PageUp: target = core::Max(0, core::Max(0, current) - page); break;
        case KeyCode::PageDown: target = core::Min(items - 1, core::Max(0, current) + page); break;
        default: return;
        }
        // Skip items that may not be selected (a section header) in the key's direction. If there is
        // no selectable item that way, stay where it is.
        if (!IsItemSelectable(target))
        {
            const bool back = key == KeyCode::Up || key == KeyCode::PageUp || key == KeyCode::End ||
                              ((key == KeyCode::Left || key == KeyCode::Right) && InlineStep(key) < 0);
            const i32 step = (key == KeyCode::Up || key == KeyCode::Down) ? across : 1;
            i32 next = target;
            while (next >= 0 && next < items && !IsItemSelectable(next))
                next += back ? -step : step;
            target = next >= 0 && next < items ? next : current;
        }
        // An arrow key at the view's edge is left unhandled, so focus can move on.
        const bool arrow = key == KeyCode::Up || key == KeyCode::Down || key == KeyCode::Left || key == KeyCode::Right;
        if (arrow && target == current && IsItemSelected(current))
            return;
        event.Handled = true;
        const EventDispatcher* dispatcher = GetEventDispatcher();
        if (dispatcher != nullptr && dispatcher->IsKeyboardDrag())
        {
            m_current = ItemIdAt(target); // keyboard drag: move the current item, keep the selection
            Invalidate();
        }
        else if (ctrl && m_selectionMode == SelectionMode::Multi)
            m_current = ItemIdAt(target); // Ctrl moves only the current item; Ctrl+Space toggles it
        else if (shift && m_selectionMode == SelectionMode::Multi)
        {
            SelectRange(target);
            NotifySelectionByUser();
        }
        else
        {
            SelectOnly(target);
            NotifySelectionByUser();
        }
        ScrollToItem(target);
    }

    // Typing jumps to the next item whose text starts with what was typed, counting text typed
    // within a second of the last. Typing the same single character again cycles through the items
    // that start with it. The input is typed text, so it follows the keyboard layout and any IME.
    // Returns false when there is no text or no items.
    inline bool ItemView::TypeAhead(core::StringView typed)
    {
        if (typed.Size() == 0 || ItemCount() == 0)
            return false;
        const EventDispatcher* d = GetEventDispatcher();
        core::String text; // holds the item's text alive while the search compares it
        const i32 item = m_typeAhead.Find(typed, d != nullptr ? d->GetTime() : 0.0f, static_cast<i32>(ItemCount()), GetCurrentItem(),
                                          [this, &text](i32 i) { text = IsItemSelectable(i) ? TypeAheadText(i) : core::String{}; return text.AsView(); });
        if (item >= 0)
        {
            SelectOnly(item);
            NotifySelectionByUser();
            ScrollToItem(item);
        }
        return true;
    }

    inline bool ItemView::BeginEdit(i32 item, i32 column)
    {
        if (m_model == nullptr || item < 0 || item >= static_cast<i32>(ItemCount()))
            return false;
        ModelIndex index = ItemToModelIndex(item);
        index.Column = column;
        if (!m_model->IsEditable(index) || (m_onEditBegin && !m_onEditBegin(index)))
            return false;
        if (m_editing && !EndEdit(true))
            return false;
        ScrollToItem(item);
        Realize();
        ItemRow* row = RowForItem(item);
        if (row == nullptr)
            return false;
        if (!m_editor)
        {
            m_editor = core::MakeRef<TextField>(MemoryAllocator());
            m_editor->AddClass(core::StringView(u8"editor"));
            ItemView* self = this;
            m_editor->SetOnSubmit([self](core::StringView) { (void)self->EndEdit(true); });
            (void)m_editor->AddEventListener(EventType::FocusLost, [self](const Event&)
            {
                // Focus went elsewhere: commit if the value is accepted, otherwise cancel.
                if (self->m_editing && !self->EndEdit(true))
                    (void)self->EndEdit(false);
            });
        }
        m_editing = true;
        m_editId = ItemIdAt(item);
        m_editColumn = column;
        row->AddChild(m_editor.Get());
        ShareTextStyle(*m_editor);
        m_editor->RemoveClass(core::StringView(u8"invalid"));
        m_editor->SetText(m_model->Data(index).ToString().AsView());
        m_editor->SetDisplayed(true);
        m_editor->GetModel().SelectAll();
        m_editor->Arrange(EditRect(*row, column));
        m_editor->RequestFocus();
        return true;
    }

    inline bool ItemView::EndEdit(bool commit)
    {
        if (!m_editing)
            return true;
        const i32 item = ItemOfId(m_editId);
        ModelIndex index = item >= 0 ? ItemToModelIndex(item) : ModelIndex{};
        index.Column = m_editColumn;
        if (commit && index.IsValid())
        {
            const core::String text(m_editor->GetText());
            const bool taken = (!m_onEditCommit || m_onEditCommit(index, text.AsView())) && m_model->SetData(index, Variant(text.AsView()));
            if (!taken)
            {
                m_editor->AddClass(core::StringView(u8"invalid"));
                return false;
            }
        }
        else if (!commit && m_onEditCancel && index.IsValid())
            m_onEditCancel(index);
        m_editing = false;
        m_editId = kNoItem;
        const bool hadFocus = m_editor->IsFocused();
        m_editor->SetDisplayed(false);
        if (hadFocus)
            RequestFocus(); // the view takes over the focus the editor had
        return true;
    }

    inline ModelDrop ItemView::DropAt(const DropQuery& query, DropEffect& effect, core::String& reason)
    {
        ModelDrop drop;
        drop.Payload = &query.Payload;
        drop.Effect = query.Preferred;
        // A drag from an item view carries that view's dragged ids and its model.
        if (const ItemView* source = core::Cast<ItemView>(Node::FromHandle(query.Payload.Value)))
        {
            drop.Ids = source->m_dragIds;
            drop.Source = source->m_model;
        }
        // A keyboard drop targets the current row: onto it when the model accepts that, else after it.
        if (query.FromKeyboard)
        {
            const i32 current = GetCurrentItem();
            drop.Index = current >= 0 ? ItemToModelIndex(current) : ModelIndex{};
            drop.Place = current >= 0 ? DropPlace::On : DropPlace::After;
            effect = m_model->CanDrop(drop, reason);
            if (effect == DropEffect::None && reason.IsEmpty() && current >= 0)
            {
                drop.Place = DropPlace::After;
                effect = m_model->CanDrop(drop, reason);
            }
            return drop;
        }
        const i32 item = ItemAt(ConvertToWorldSpace(query.Local));
        if (item < 0)
        {
            drop.Index = ModelIndex{}; // past the last row: drop at the end
            drop.Place = DropPlace::After;
            effect = m_model->CanDrop(drop, reason);
            return drop;
        }
        drop.Index = ItemToModelIndex(item);
        // Where in the row: the quarter at each edge means before or after, the middle half means
        // onto it. A grid measures across the cell instead of down. When the model refuses the
        // place without a reason, try the other kind: from onto, before or after by halves; from
        // before or after, onto.
        const Rect b = ItemBounds(item);
        const core::Float2 content{query.Local.x + GetScrollOffset().x, query.Local.y + GetScrollOffset().y};
        const bool across = m_layout == ItemLayout::Grid;
        f32 at = across ? (content.x - b.x) / core::Max(1.0f, b.width) : (content.y - b.y) / core::Max(1.0f, b.height);
        if (across && IsRightToLeft())
            at = 1.0f - at; // before is the inline start, which is the right in right to left
        drop.Place = at < 0.25f ? DropPlace::Before : at > 0.75f ? DropPlace::After : DropPlace::On;
        effect = m_model->CanDrop(drop, reason);
        if (effect == DropEffect::None && reason.IsEmpty())
        {
            drop.Place = drop.Place == DropPlace::On ? (at < 0.5f ? DropPlace::Before : DropPlace::After) : DropPlace::On;
            effect = m_model->CanDrop(drop, reason);
        }
        return drop;
    }

    inline void ItemView::OnDragQuery(DropQuery& query)
    {
        if (m_model == nullptr)
            return;
        DropEffect effect = DropEffect::None;
        core::String reason;
        const ModelDrop drop = DropAt(query, effect, reason);
        if (effect == DropEffect::None)
        {
            SetDropIndicator(false, -1, DropPlace::After);
            if (!reason.IsEmpty())
                query.Refuse(reason.AsView());
            return;
        }
        query.Accept(effect);
        query.Effect = effect; // the model chooses the effect (a list takes text as a copy)
        query.Reason = reason;
        const i32 item = drop.Index.IsValid() ? ItemOfId(m_model->IdOf(drop.Index)) : -1;
        query.Place = static_cast<i32>(drop.Place);
        SetDropIndicator(true, item, drop.Place);
        m_lastDrop = drop; // OnDrop uses this; the release queries once more at its position first
    }

    // Applies the drop the last OnDragQuery found. The release has just queried again where it landed.
    inline void ItemView::OnDrop(const DragEvent& event)
    {
        if (m_model == nullptr || !m_dropOver)
            return;
        ModelDrop drop = m_lastDrop;
        drop.Effect = event.Effect;
        SetDropIndicator(false, -1, DropPlace::After);
        (void)m_model->Drop(drop);
    }

    // Draws the insertion line before or after a row (along a cell's side edge in a grid), or an
    // outline around the row a drop goes onto.
    inline void ItemView::OnDrawAbove(DrawContext& ctx)
    {
        if (!m_dropOver)
            return;
        const PartStyle& part = GetPartStyle(u8"drop-indicator");
        const Color color = part.HasBackground ? part.Box.BackgroundColor : Color{0.31f, 0.61f, 1.0f, 1.0f};
        const Rect box = GetContentBounds();
        Rect row;
        if (m_dropItem < 0)
        {
            const usize count = ItemCount();
            row = count > 0 ? ItemBounds(static_cast<i32>(count) - 1) : Rect{box.x, box.y + ContentTopInset(), box.width, 0.0f};
        }
        else
            row = ItemBounds(m_dropItem);
        row.x -= GetScrollOffset().x;
        row.y -= GetScrollOffset().y;
        ctx.PushClip(box);
        const bool across = m_layout == ItemLayout::Grid;
        if (m_dropPlace == DropPlace::On && m_dropItem >= 0)
        {
            vg::PathBuilder pb;
            pb.MoveTo(row.x + 1.0f, row.y + 1.0f);
            pb.LineTo(row.x + row.width - 1.0f, row.y + 1.0f);
            pb.LineTo(row.x + row.width - 1.0f, row.y + row.height - 1.0f);
            pb.LineTo(row.x + 1.0f, row.y + row.height - 1.0f);
            pb.Close();
            ctx.VG().StrokePath(pb.ToPath(), color, vg::StrokeStyle(2.0f));
        }
        else
        {
            const bool before = m_dropPlace == DropPlace::Before && m_dropItem >= 0;
            const bool left = before != IsRightToLeft(); // before is the inline start
            const Rect line = across ? Rect{left ? row.x - 1.0f : row.x + row.width - 1.0f, row.y, 2.0f, row.height}
                                     : Rect{row.x, (before ? row.y : row.y + row.height) - 1.0f, row.width, 2.0f};
            ctx.VG().FillRect(core::Rectangle{line.x, line.y, line.width, line.height}, color);
        }
        ctx.PopClip();
    }

    RTTI_DEFINE_OBJECT(TemplateRowContent, "rtti::gui")
    RTTI_DEFINE_OBJECT(ItemRow, "rtti::gui")
}
