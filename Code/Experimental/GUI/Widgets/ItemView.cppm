// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :item_view partition
//
// ItemView: the engine behind ListView, TreeView and TableView (E6 of Specs/gui-lab-evolution.md).
// It is a scroll container like any other node (its wheel, bars, touch, fling, smooth scrolling,
// right stick and `::scrollbar` parts are Node's), which realises only the rows in view:
//
//   - Rows come from a factory (a native one, or the view's own default): each row type has a
//     pool of rows, and a row scrolled out of view goes back to its pool for the next one.
//     A row is an ItemRow holding the factory's content; it shows `:selected`.
//   - Heights are fixed (`row-height`) or measured: a measured row's height is estimated until it
//     is first realised, then measured, and kept in a sum tree (RowHeights), so finding the row at
//     an offset or the offset of a row is a logarithmic walk (GPUI's list in role).
//   - Scrolling is anchored to an item and an offset into it: a row above measuring taller, or
//     rows inserted above, leave what is in view where it is.
//   - The model reports row ranges (E6); the view follows inserts, removals, moves and changes of
//     a flat model and resets for the rest. Selection is a set of row ids, so it survives inserts,
//     moves and sorting.
//   - Rows from markup: a `<template>` child is made into each row, its `{{item}}`,
//     `{{item.Column}}` and `{{index}}` attributes filled for the item it shows; templates of a
//     `type` are picked by the text in the view's `row-type-column`. `items="a|b|c"` gives a
//     markup list a model of its own.
//   - A double-click, Return or the pad's South reports `activate`; typing letters and digits
//     jumps to the next row that starts with them (read from the keys, not text input, which the
//     shell turns on for fields alone); F2 (or BeginEdit) edits a row in place, a hook able to
//     refuse the edit or the value.
//
// A subclass says what an item is (ItemCount, ItemToModelIndex: a tree flattens its open nodes),
// makes its default rows (CreateContent, BindContent), and adds decorations (a table's header).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:item_view;

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
import :event_dispatcher; // GetTime (type-ahead)
import :reactive;         // Observable (what bindings follow)
import :element_types;    // ElementSet (a template's elements)
import :markup;           // MarkupLoader, ItemBinding (template rows)
import :string_list_model;
import :list_box;         // SplitItems, JoinItems

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    enum class SelectionMode
    {
        Single,
        Multi
    };

    /// Row heights in a sum tree (a Fenwick tree): the offset of a row and the row at an offset
    /// in O(log n), a height changed in O(log n); inserts and removals rebuild it, O(n).
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
        /// `count` rows from `first` now start at `to` (in the order after the move).
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
        /// Sets a row's height (measured: it stays until Unmeasure); true when it changed.
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
        /// Where row `i` starts: the sum of the heights before it.
        [[nodiscard]] f32 Top(usize i) const
        {
            f32 sum = 0.0f;
            for (usize k = core::Min(i, m_heights.Size()); k > 0; k -= k & (~k + 1))
                sum += m_tree[k];
            return sum;
        }
        [[nodiscard]] f32 Total() const { return Top(m_heights.Size()); }
        /// The row at offset `y` (the last row past the end; 0 when there are none).
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

    /// A native row factory: a row's content made once per pooled row of a type, and filled for
    /// each item it shows.
    struct RowFactory
    {
        /// The row type an item takes (unset: one type); each type has its own pool.
        core::Function<u32(const ModelIndex&)> TypeOf;
        core::Function<core::RefPtr<Node>(u32 type)> Create;
        core::Function<void(Node& content, const ModelIndex& index)> Bind;
    };

    /// One realised row: the view's container round a factory's content. It shows `:selected`
    /// and draws the view's ::selection part under its content while selected.
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

        ItemView& m_view; // its parent, which owns it
        core::RefPtr<Node> m_content;
        i32 m_item = -1;
        ItemId m_id = kNoItem;
        u32 m_type = 0;
        bool m_selected = false;
        bool m_dirty = true; // its content shows stale data
        bool m_taken = false; // claimed in this realisation
    };

    // A row made from a markup template: the template's tree, and the attributes it fills from
    // the item it shows.
    class TemplateRowContent : public UIWidget
    {
        RTTI_OBJECT(TemplateRowContent, UIWidget)
    public:
        TemplateRowContent() { SetTag(core::StringView(u8"templaterow")); }
        [[nodiscard]] Array<ItemBinding>& Bindings() noexcept { return m_bindings; }

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
            SetScrollOffset(core::Float2{0.0f, 0.0f});
            OnModelReset();
        }
        [[nodiscard]] IModel* GetModel() const noexcept { return m_model; }

        // === Rows ===
        /// Rows from a native factory instead of the view's own (the pools start again).
        void SetRowFactory(RowFactory factory)
        {
            m_factory = core::Move(factory);
            DropRows();
            RequestLayout();
        }
        /// A markup template for rows (`type` names one of several, picked by the
        /// row-type-column's text; empty: the default).
        bool SetMarkupTemplate(core::StringView type, core::StringView markup) override
        {
            m_templates.PushBack(RowTemplate{core::String(type), core::String(markup)});
            UseTemplates();
            return true;
        }
        /// The column whose text picks a row's template (by its `type`).
        void SetRowTypeColumn(const core::String& column)
        {
            m_rowTypeColumn = column;
            if (!m_templates.IsEmpty())
                UseTemplates();
        }
        [[nodiscard]] core::String GetRowTypeColumn() const { return m_rowTypeColumn; }
        /// A model of its own from text, '|' between the items (markup's `items`).
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
        /// "single" or "multi" (markup's `selection-mode`).
        void SetSelectionModeName(const core::String& name)
        {
            m_selectionMode = core::Trim(name.AsView()) == core::StringView(u8"multi") ? SelectionMode::Multi : SelectionMode::Single;
        }
        [[nodiscard]] core::String GetSelectionModeName() const
        {
            return core::String(m_selectionMode == SelectionMode::Multi ? core::StringView(u8"multi") : core::StringView(u8"single"));
        }

        /// Every row this tall (0: each measured, estimated until it is).
        void SetRowHeight(f32 height)
        {
            m_rowHeight = core::Max(0.0f, height);
            SetScrollStep(m_rowHeight > 0.0f ? m_rowHeight : m_estimate);
            m_heights.Reset(ItemCount(), RowEstimate());
            RequestLayout();
        }
        [[nodiscard]] f32 GetRowHeight() const noexcept { return m_rowHeight; }
        /// What an unmeasured row is taken to be until it is realised.
        void SetEstimatedRowHeight(f32 height)
        {
            m_estimate = core::Max(1.0f, height);
            if (m_rowHeight <= 0.0f)
                m_heights.Reset(ItemCount(), m_estimate);
            RequestLayout();
        }
        [[nodiscard]] f32 GetEstimatedRowHeight() const noexcept { return m_estimate; }
        /// The rows realised now (in view, and one past each edge).
        [[nodiscard]] usize VisibleRowCount() const noexcept { return m_live.Size(); }
        /// The realised row showing `item` (null when it is not in view).
        [[nodiscard]] ItemRow* RowForItem(i32 item) const
        {
            for (const core::RefPtr<ItemRow>& row : m_live)
                if (row->m_item == item)
                    return row.Get();
            return nullptr;
        }
        /// The item under a point in world space (-1: none).
        [[nodiscard]] i32 ItemAt(core::Float2 world) const
        {
            for (const core::RefPtr<ItemRow>& row : m_live)
                if (row->GetScreenBounds().Contains(world))
                    return row->m_item;
            return -1;
        }
        /// Where an item starts in the content and how tall it is (estimated until measured).
        [[nodiscard]] Rect ItemBounds(i32 item) const
        {
            const Rect box = GetContentBounds();
            const usize i = static_cast<usize>(core::Max(0, item));
            return Rect{box.x, box.y + ContentTopInset() + m_heights.Top(i), RowWidth(box.width), m_heights.At(i)};
        }
        /// Scrolls so `item` shows (Nearest: the least move).
        void ScrollToItem(i32 item, ScrollAlign align = ScrollAlign::Nearest)
        {
            if (item < 0 || item >= static_cast<i32>(ItemCount()))
                return;
            const Rect box = GetContentBounds();
            const f32 view = core::Max(0.0f, box.height - ContentTopInset());
            const f32 top = m_heights.Top(static_cast<usize>(item));
            const f32 bottom = top + m_heights.At(static_cast<usize>(item));
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
            // Its offset is known now, so the extent is brought up to date first (the range).
            SetScrollExtent(core::Float2{GetContentExtent().x, ContentTopInset() + m_heights.Total()});
            ScrollTo(core::Float2{GetScrollTarget().x, to});
            if (!m_realizing)
                Realize();
        }

        // === Appearance ===
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"selection"));
        }
        // Its rows draw the selection in ::selection's style.
        void OnPartStyleChange(core::StringView part) override
        {
            if (part == core::StringView(u8"selection"))
                ForEachRow([](ItemRow& row) { row.Invalidate(); });
        }

        // === Selection ===
        void SetSelectionMode(SelectionMode mode) noexcept { m_selectionMode = mode; }
        [[nodiscard]] SelectionMode GetSelectionMode() const noexcept { return m_selectionMode; }
        /// The user changed the selection (code's changes report nothing); the index is the
        /// current item's.
        void SetOnSelectionChanged(core::Function<void(ModelIndex)> callback) { m_onSelection = core::Move(callback); }
        /// A row was activated: a double-click, Return or the pad's South.
        void SetOnActivated(core::Function<void(ModelIndex)> callback) { m_onActivated = core::Move(callback); }

        [[nodiscard]] usize GetSelectionCount() const noexcept { return m_selected.Size(); }
        [[nodiscard]] bool IsIdSelected(ItemId id) const { return m_selected.Contains(id); }
        [[nodiscard]] bool IsItemSelected(i32 item) const { return item >= 0 && IsIdSelected(ItemIdAt(item)); }
        /// The selected ids.
        void GetSelectedIds(core::Array<ItemId>& out) const
        {
            for (ItemId id : m_selected)
                out.PushBack(id);
        }
        /// The current item (where the keyboard is) when it is selected, else -1.
        [[nodiscard]] i32 GetSelectedItem() const
        {
            m_selectionChanged.Track(); // a read a binding records
            const i32 item = ItemOfId(m_current);
            return item >= 0 && IsIdSelected(m_current) ? item : -1;
        }
        /// The item the keyboard is on (-1: none).
        [[nodiscard]] i32 GetCurrentItem() const { return ItemOfId(m_current); }
        [[nodiscard]] ModelIndex GetSelectedIndex() const
        {
            const i32 item = GetSelectedItem();
            return item >= 0 ? ItemToModelIndex(item) : ModelIndex{};
        }
        /// Selects one item from code (scrolled into view; no events). -1 clears.
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
        void ClearSelection()
        {
            m_selected.Clear();
            m_anchorSelection = kNoItem;
            RefreshSelectionStates();
        }
        /// Every item (multi-selection), as Ctrl+A does.
        void SelectAll()
        {
            if (m_selectionMode != SelectionMode::Multi)
                return;
            for (i32 i = 0; i < static_cast<i32>(ItemCount()); ++i)
                (void)m_selected.Insert(ItemIdAt(i));
            RefreshSelectionStates();
        }

        // === Editing ===
        /// Edits `item` in place (`column` for a table), if its model cell is editable and the
        /// begin hook allows; false otherwise.
        bool BeginEdit(i32 item, i32 column = 0);
        /// Ends an edit: commits (if the commit hook and the model take it) or cancels. False
        /// when the value was refused and it is still editing.
        bool EndEdit(bool commit);
        [[nodiscard]] bool IsEditing() const noexcept { return m_editing; }
        [[nodiscard]] TextField* GetEditor() const noexcept { return m_editor.Get(); }
        /// May refuse to start an edit.
        void SetOnEditBegin(core::Function<bool(const ModelIndex&)> hook) { m_onEditBegin = core::Move(hook); }
        /// May refuse a value (false: it keeps editing, marked `.invalid`); otherwise the model's
        /// SetData decides.
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
                m_scrollAnchor += count; // what is in view stays in view
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
                m_scrollAnchor = first; // its row went: the one now in its place, from its top
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
        // === What a subclass says ===
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
        /// Its items are the model's top-level rows (a tree's are not: it resets on every range).
        [[nodiscard]] virtual bool IsFlat() const { return true; }
        /// A default row's content, and filling it (when no factory is set).
        [[nodiscard]] virtual core::RefPtr<Node> CreateContent(u32 type) = 0;
        virtual void BindContent(Node& content, i32 item, const ModelIndex& index) = 0;
        /// Room above the rows (a table's header).
        [[nodiscard]] virtual f32 ContentTopInset() const { return 0.0f; }
        /// How wide a row is in a content box `width` wide (a table's columns may be wider).
        [[nodiscard]] virtual f32 RowWidth(f32 width) const { return width; }
        /// The model was set or reset (a tree flattens, a table rebuilds its header).
        virtual void OnModelChanged() {}
        /// After the rows are placed (a table places its header).
        virtual void OnLayoutDecorations() {}
        /// A key before the view's own (a tree's Left and Right); true when used.
        virtual bool OnExtraKey(KeyCode key, u32 modifiers)
        {
            (void)key, (void)modifiers;
            return false;
        }
        /// Where an editor goes over a row (in the row's space).
        [[nodiscard]] virtual Rect EditRect(const ItemRow& row, i32 column) const
        {
            (void)column;
            return row.GetContentBounds();
        }
        /// The text type-ahead matches (the display text of the item's first column).
        [[nodiscard]] virtual core::String TypeAheadText(i32 item) const
        {
            return m_model != nullptr ? m_model->Data(ItemToModelIndex(item)).ToString() : core::String{};
        }
        /// Its rows' tag (`listviewrow`, `treerow`, ...).
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
        /// The items changed in a way only the subclass knows (a tree opened a node): heights
        /// start again, rows are refilled.
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
        /// Selects as a click does, with its modifiers (a subclass's own rows call it).
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
            Realize(); // laid out: the rows now in view, at once (a press lands on them)
            OnLayoutDecorations();
        }
        void OnTextStyleChange() override
        {
            ForEachRow([this](ItemRow& row) { ShareTextStyle(row); });
            if (m_rowHeight <= 0.0f)
                m_heights.Reset(ItemCount(), RowEstimate()); // a new font measures anew
            RequestLayout();
        }
        void OnKeyDown(const KeyEvent& event) override;
        bool OnActivate() override
        {
            const i32 item = GetCurrentItem();
            if (item < 0)
                return false;
            ActivateItem(item);
            return true;
        }
        // A context menu asked for over a row selects it first (unless it is selected already),
        // and goes on up (to whoever opens a menu).
        void OnContextMenu(const MouseEvent& event) override
        {
            const i32 item = ItemAt(event.Position);
            if (item >= 0 && !IsItemSelected(item))
                SelectByUser(item, 0);
        }

    private:
        friend class ItemRow;

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
        // Rows from the markup templates: each type made by loading its markup with item
        // bindings, and filled by them.
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
        // The template an item takes: the one whose type is its row-type-column's text, else the
        // default (the one with no type, else the first).
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
        // A template name's text for an item: `item` (its first column), `item.Column` (a column
        // by name), `index` (its row).
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

        // The rows in view (and one past each edge) realised, measured and placed; the scroll
        // anchor kept where it was when rows above it measured differently from their estimate.
        void Realize();
        bool TypeAhead(KeyCode key);
        // A row for `item`: the live one showing its id, else one from its type's pool, else new.
        ItemRow* TakeRow(i32 item, core::Array<core::RefPtr<ItemRow>>& next);
        void ReleaseRow(ItemRow& row)
        {
            if (m_editing && row.m_id == m_editId)
                (void)EndEdit(true); // scrolled away: what was typed is kept
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

        // --- The scroll anchor: an item and how far into it the top of the view is ---
        void ResetScrollAnchor()
        {
            m_scrollAnchor = 0;
            m_scrollAnchorDelta = 0.0f;
        }
        void UpdateScrollAnchor()
        {
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
            const ModelIndex index = ItemToModelIndex(item);
            if (m_onActivated)
                m_onActivated(index);
            SendEvent(ItemEvent(EventType::ItemActivate, this, item, index));
        }

        // A press on a row: the left button selects (a second press activates), the right one
        // selects for its context menu.
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
                SelectByUser(row.m_item, event.Modifiers);
            }
        }

        IModel* m_model = nullptr; // non-owning (unless it is m_ownModel)
        core::UniquePtr<StringListModel> m_ownModel; // markup's `items`
        RowFactory m_factory;
        Array<RowTemplate> m_templates;
        core::String m_rowTypeColumn;
        mutable Observable m_selectionChanged; // what bindings reading the selection follow
        RowHeights m_heights;
        core::Array<core::RefPtr<ItemRow>> m_live; // realised, in item order
        core::Array<Pool> m_pools;                 // released rows, by type
        core::HashSet<ItemId> m_selected;
        ItemId m_current = kNoItem;         // where the keyboard is
        ItemId m_anchorSelection = kNoItem; // where a Shift range starts
        SelectionMode m_selectionMode = SelectionMode::Single;
        core::Function<void(ModelIndex)> m_onSelection;
        core::Function<void(ModelIndex)> m_onActivated;
        f32 m_rowHeight = 24.0f;
        f32 m_estimate = 24.0f;
        i32 m_scrollAnchor = 0;
        f32 m_scrollAnchorDelta = 0.0f;
        bool m_realizing = false;
        // Type-ahead.
        core::String m_typed;
        f32 m_typedAt = -10.0f;
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

    inline void ItemRow::OnDraw(DrawContext& ctx, const Rect& localBounds)
    {
        UIWidget::OnDraw(ctx, localBounds);
        if (m_selected)
            m_view.GetPartStyle(u8"selection").Draw(ctx, GetLocalBounds(), FilledBox(Color{0.18f, 0.37f, 0.62f, 1.0f}));
    }

    // Its content fills it; an editor lies over it where the view says.
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
        m_realizing = true;
        const Rect box = GetContentBounds();
        const f32 inset = ContentTopInset();
        const f32 width = RowWidth(box.width);
        const usize count = ItemCount();
        if (m_heights.Size() != count)
            m_heights.Reset(count, RowEstimate());
        const bool measured = m_rowHeight <= 0.0f;
        const LayoutConstraints across{core::Float2{width, 0.0f}, core::Float2{width, kUnbounded}, core::Float2{width, -1.0f}};

        // Realise from the anchor; a row above it measuring differently moves the anchor's
        // offset, which is put back, and the rows are taken again (a few passes settle it).
        for (int pass = 0; pass < 3; ++pass)
        {
            const f32 top = GetScrollOffset().y;
            const f32 bottom = top + core::Max(0.0f, box.height - inset);
            core::Array<core::RefPtr<ItemRow>> next;
            usize first = count > 0 ? m_heights.Find(top) : 0;
            if (first > 0)
                --first; // one above the view
            // The rows leaving the view go back to their pools first, so those coming in take
            // them rather than new ones (judged by the heights as they stand).
            {
                usize last = first;
                while (last < count && m_heights.Top(last) < bottom)
                    ++last;
                // (By the ids in range, not each row's lookup: an id's row is a search in a list.)
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
                    pastEnd = m_heights.Top(i) >= bottom; // one below the view
                    ItemRow* row = TakeRow(static_cast<i32>(i), next);
                    // A row not yet styled (just made, or its template's attributes just
                    // written) is measured again once it is.
                    if (measured && !m_heights.IsMeasured(i))
                        (void)m_heights.Set(i, row->Measure(across).y, !StylePending(*row));
                }
            }
            for (const core::RefPtr<ItemRow>& row : m_live)
                if (!row->m_taken)
                    ReleaseRow(*row);
            m_live = core::Move(next);
            for (const core::RefPtr<ItemRow>& row : m_live)
                row->m_taken = false;

            SetScrollExtent(core::Float2{width, inset + m_heights.Total()});
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
            row->Arrange(Rect{box.x, box.y + inset + m_heights.Top(i), width, m_heights.At(i)});
        }
        m_realizing = false;
    }

    inline void ItemView::SelectByUser(i32 item, u32 modifiers)
    {
        if (item < 0 || item >= static_cast<i32>(ItemCount()))
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
        if ((event.Modifiers & ~static_cast<u32>(KeyModShift)) == 0 && TypeAhead(key))
        {
            event.Handled = true;
            return;
        }
        const i32 current = GetCurrentItem();
        // A page: the rows that fit, by the current row's height.
        const f32 rowH = core::Max(1.0f, m_heights.At(static_cast<usize>(core::Max(0, current))));
        const i32 page = core::Max(1, static_cast<i32>((GetContentBounds().height - ContentTopInset()) / rowH) - 1);
        i32 target = current;
        switch (key)
        {
        case KeyCode::Up: target = current <= 0 ? 0 : current - 1; break;
        case KeyCode::Down: target = current < 0 ? 0 : core::Min(items - 1, current + 1); break;
        case KeyCode::Home: target = 0; break;
        case KeyCode::End: target = items - 1; break;
        case KeyCode::PageUp: target = core::Max(0, core::Max(0, current) - page); break;
        case KeyCode::PageDown: target = core::Min(items - 1, core::Max(0, current) + page); break;
        default: return;
        }
        // An arrow at the view's end is given back (focus moves on).
        if ((key == KeyCode::Up || key == KeyCode::Down) && target == current && IsItemSelected(current))
            return;
        event.Handled = true;
        if (ctrl && m_selectionMode == SelectionMode::Multi)
            m_current = ItemIdAt(target); // Ctrl moves the keyboard alone (Ctrl+Space picks)
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

    // Typing jumps to the next item whose text starts with what was typed (within a second of
    // the last key; one letter again cycles through the items starting with it). False when the
    // key is no letter or digit.
    inline bool ItemView::TypeAhead(KeyCode key)
    {
        char8_t typed = 0;
        for (usize i = 0; i < 26; ++i)
            if (kLetterKeys[i] == key)
                typed = static_cast<char8_t>(u8'a' + i);
        if (key >= KeyCode::Num0 && key <= KeyCode::Num9)
            typed = static_cast<char8_t>(u8'0' + (static_cast<u32>(key) - static_cast<u32>(KeyCode::Num0)));
        if (typed == 0)
            return false;
        const EventDispatcher* d = GetEventDispatcher();
        const f32 now = d != nullptr ? d->GetTime() : 0.0f;
        if (now - m_typedAt > 1.0f)
            m_typed = core::String{};
        m_typedAt = now;
        const char8_t one[1] = {typed};
        m_typed.Append(core::StringView(one, 1));
        const auto lower = [](char8_t c) { return (c >= u8'A' && c <= u8'Z') ? static_cast<char8_t>(c + 32) : c; };
        const auto startsWith = [&](core::StringView text, core::StringView prefix)
        {
            if (text.Size() < prefix.Size())
                return false;
            for (usize i = 0; i < prefix.Size(); ++i)
                if (lower(text[i]) != lower(prefix[i]))
                    return false;
            return true;
        };
        // One letter typed again: the next item starting with it.
        core::StringView prefix = m_typed.AsView();
        bool repeated = prefix.Size() > 1;
        for (usize i = 1; i < prefix.Size() && repeated; ++i)
            repeated = prefix[i] == prefix[0];
        if (repeated)
            prefix = prefix.SubStr(0, 1);
        const i32 items = static_cast<i32>(ItemCount());
        const i32 current = GetCurrentItem();
        const i32 start = (prefix.Size() == 1 || current < 0) ? current + 1 : current;
        for (i32 n = 0; n < items; ++n)
        {
            const i32 item = ((start + n) % items + items) % items;
            if (startsWith(TypeAheadText(item).AsView(), prefix))
            {
                SelectOnly(item);
                NotifySelectionByUser();
                ScrollToItem(item);
                break;
            }
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
                // Focus went elsewhere: committed if it may be, else put back.
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
            RequestFocus(); // the view keeps the focus the editor had
        return true;
    }

    RTTI_DEFINE_OBJECT(TemplateRowContent, "rtti::gui")
    RTTI_DEFINE_OBJECT(ItemRow, "rtti::gui")
}
