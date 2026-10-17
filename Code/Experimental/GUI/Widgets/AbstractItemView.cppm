// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :abstract_item_view partition
//
// AbstractItemView: the shared engine behind ListView / TableView / TreeView. Modeled on eepp's
// UIAbstractView. It owns the parts every model-backed, scrolling, selectable view needs:
//   - the IModel binding (it is an IModelClient, so it refreshes on the model's DidUpdate),
//   - vertical virtualization: only the rows visible in the viewport are realized, from a
//     recycled pool of ItemRow widgets (a 1M-item model costs a handful of widgets),
//   - a vertical ScrollBar overlay + wheel + scroll-into-view,
//   - single OR multi selection (plain / Ctrl-toggle / Shift-range) by flat item index, with
//     keyboard navigation.
//
// Subclasses provide only what differs: how a row widget is built (CreateItemRow) and bound
// (BindItemRow), how many virtual items there are (ItemCount), an optional top inset for a
// header (ContentTopInset), the item -> ModelIndex mapping, and any extra decorations/keys.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:abstract_item_view;

import foundation.core; // RefPtr, MakeRef, Array, Function, Move, Max, Min, Float2
import :rect;
import :event;
import :draw_context;
import :rectangle_drawable;
import :node;
import :ui_widget;
import :scroll_bar;
import :layout_style; // Orientation
import :model_index;
import :model;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    enum class SelectionMode
    {
        Single,
        Multi
    };

    // A recycled row container: draws a selection highlight and reports clicks (with the
    // pointer modifiers, so views can implement Ctrl/Shift selection). Content (a label, cells,
    // a tree row) is added as hit-transparent children so clicks fall through to the row.
    class ItemRow : public UIWidget
    {
        RTTI_OBJECT(ItemRow, UIWidget)
    public:
        ItemRow() { SetTag(core::StringView(u8"itemrow")); }

        void SetItemIndex(i32 item) noexcept { m_item = item; }
        [[nodiscard]] i32 GetItemIndex() const noexcept { return m_item; }
        void SetSelected(bool selected)
        {
            if (m_selected != selected)
            {
                m_selected = selected;
                Invalidate();
            }
        }
        [[nodiscard]] bool IsSelected() const noexcept { return m_selected; }
        void SetSelectionColor(Color color)
        {
            m_selectionColor = color;
            Invalidate();
        }
        /// The view's ::selection style, which a selected row draws.
        void SetSelectionStyle(const PartStyle& style)
        {
            m_selectionStyle = style;
            Invalidate();
        }
        void SetOnPicked(core::Function<void(i32, u32)> callback)
        {
            m_onPicked = core::Move(callback);
        }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            if (m_onPicked)
                m_onPicked(m_item, event.Modifiers);
            event.Handled = true;
        }
        void OnDraw(DrawContext& ctx, const Rect&) override
        {
            if (m_selected)
                m_selectionStyle.Draw(ctx, GetLocalBounds(), FilledBox(m_selectionColor));
        }

    private:
        i32 m_item = -1;
        bool m_selected = false;
        Color m_selectionColor{0.18f, 0.37f, 0.62f, 1.0f};
        PartStyle m_selectionStyle;
        core::Function<void(i32, u32)> m_onPicked;
    };

    class AbstractItemView : public UIWidget, public IModelClient
    {
        RTTI_OBJECT(AbstractItemView, UIWidget)
    public:
        AbstractItemView()
        {
            SetClipChildren(true);
            SetTabFocusable(true);
            SetBackground(
                core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_backgroundColor));

            m_vBar = core::MakeRef<ScrollBar>(MemoryAllocator());
            m_vBar->SetOrientation(Orientation::Vertical);
            AbstractItemView* self = this;
            // The bar's value is the offset in pixels, its page the visible body.
            m_vBar->SetOnValueChanging(
                [self](f64 offset)
                {
                    if (!self->m_syncing)
                        self->ScrollToFraction(self->MaxScroll() > 0.0f ? static_cast<f32>(offset) / self->MaxScroll() : 0.0f);
                });
            AddChild(m_vBar.Get());
        }

        ~AbstractItemView() override
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
            m_selection.Clear();
            m_anchor = -1;
            m_offset = 0.0f;
            OnModelChanged();
            Relayout();
        }
        [[nodiscard]] IModel* GetModel() const noexcept { return m_model; }

        void OnModelUpdated() override
        {
            OnModelChanged();
            ClampSelection();
            ClampOffset();
            Relayout();
        }

        // === Appearance ===
        void SetRowHeight(f32 height)
        {
            m_rowHeight = core::Max(1.0f, height);
            Relayout();
        }
        [[nodiscard]] f32 GetRowHeight() const noexcept { return m_rowHeight; }
        void SetSelectionColor(Color color)
        {
            m_selectionColor = color;
            for (const RefPtr<ItemRow>& r : m_pool)
                r->SetSelectionColor(color);
        }
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"selection"));
        }
        // Its rows draw the selection in ::selection's style.
        void OnPartStyleChange(core::StringView part) override
        {
            if (part == core::StringView(u8"selection"))
                for (const RefPtr<ItemRow>& r : m_pool)
                    r->SetSelectionStyle(GetPartStyle(part));
        }

        // === Selection ===
        void SetSelectionMode(SelectionMode mode) noexcept { m_selectionMode = mode; }
        [[nodiscard]] SelectionMode GetSelectionMode() const noexcept { return m_selectionMode; }
        void SetOnSelectionChanged(core::Function<void(ModelIndex)> callback)
        {
            m_onSelection = core::Move(callback);
        }

        [[nodiscard]] const Array<i32>& SelectedItems() const noexcept { return m_selection; }
        [[nodiscard]] i32 GetSelectedItem() const noexcept
        {
            return m_selection.Size() > 0 ? m_selection[m_selection.Size() - 1] : -1;
        }
        [[nodiscard]] ModelIndex GetSelectedIndex() const
        {
            return GetSelectedItem() >= 0 ? ItemToModelIndex(GetSelectedItem()) : ModelIndex{};
        }
        void SetSelectedItem(i32 item) { SelectSingle(item, /*notify*/ true, /*scroll*/ true); }
        void ClearSelection()
        {
            if (m_selection.Size() == 0)
                return;
            m_selection.Clear();
            m_anchor = -1;
            Relayout();
        }
        [[nodiscard]] bool IsItemSelected(i32 item) const
        {
            for (usize i = 0; i < m_selection.Size(); ++i)
                if (m_selection[i] == item)
                    return true;
            return false;
        }

        [[nodiscard]] usize VisibleRowCount() const noexcept { return m_visibleCount; }
        [[nodiscard]] f32 ScrollOffset() const noexcept { return m_offset; }


    protected:
        // === Subclass hooks ===
        [[nodiscard]] virtual RefPtr<ItemRow> CreateItemRow() = 0;          // a fresh pooled row
        virtual void BindItemRow(ItemRow& row, i32 item, f32 rowWidth) = 0; // set its content
        [[nodiscard]] virtual usize ItemCount() const
        {
            return m_model != nullptr ? m_model->RowCount() : 0;
        }
        [[nodiscard]] virtual f32 ContentTopInset() const { return 0.0f; } // e.g. a header
        [[nodiscard]] virtual ModelIndex ItemToModelIndex(i32 item) const
        {
            return MakeModelIndex(item);
        }
        virtual void OnModelChanged() {}                           // reset on model set/update
        virtual void OnBeforeLayout(f32 /*contentWidth*/) {}       // e.g. compute column widths
        virtual void OnLayoutDecorations() {}                      // position a header, etc.
        virtual bool OnExtraKey(KeyCode /*key*/) { return false; } // Left/Right for a tree
        // A row was clicked (with modifiers). Default = selection; a tree overrides to catch arrow clicks.
        virtual void OnRowClicked(i32 item, u32 modifiers)
        {
            HandleSelectionClick(item, modifiers);
        }

        // === Protected helpers for subclasses ===
        [[nodiscard]] f32 ContentWidth() const noexcept { return m_contentWidth; }
        [[nodiscard]] f32 BodyHeight() const
        {
            return core::Max(0.0f, GetSize().y - ContentTopInset());
        }
        [[nodiscard]] ScrollBar* GetScrollBar() const noexcept { return m_vBar.Get(); }
        [[nodiscard]] const Array<RefPtr<ItemRow>>& RowPool() const noexcept { return m_pool; }
        void RequestRelayout() { Relayout(); }
        void NotifySelectionChanged()
        {
            if (m_onSelection)
                m_onSelection(GetSelectedIndex());
        }

        // Select a single item without notifying or scrolling (a tree uses this to remap the
        // selection after the visible flattened list changes).
        void SelectItemSilent(i32 item) { SelectSingle(item, /*notify*/ false, /*scroll*/ false); }

        void OnMouseWheel(const WheelEvent& event) override
        {
            const f32 before = m_offset;
            ScrollBy(-event.Delta.y * m_rowHeight);
            event.Handled = m_offset != before; // at its end, the scroller around it scrolls
        }

        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Target != this)
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (OnExtraKey(key))
            {
                event.Handled = true;
                return;
            }
            const i32 items = static_cast<i32>(ItemCount());
            if (items == 0)
                return;
            const bool shift = (event.Modifiers & static_cast<u32>(KeyModShift)) != 0;
            const i32 page = core::Max(1, static_cast<i32>(BodyHeight() / m_rowHeight) - 1);
            const i32 primary = GetSelectedItem();
            i32 target = primary;
            switch (key)
            {
            case KeyCode::Up:
                target = (primary <= 0) ? 0 : primary - 1;
                break;
            case KeyCode::Down:
                target = (primary < 0) ? 0 : core::Min(items - 1, primary + 1);
                break;
            case KeyCode::Home:
                target = 0;
                break;
            case KeyCode::End:
                target = items - 1;
                break;
            case KeyCode::PageUp:
                target = core::Max(0, (primary < 0 ? 0 : primary) - page);
                break;
            case KeyCode::PageDown:
                target = core::Min(items - 1, (primary < 0 ? 0 : primary) + page);
                break;
            default:
                return;
            }
            // An arrow at the view's end is given back (focus moves on).
            const bool arrow = key == KeyCode::Up || key == KeyCode::Down;
            if (arrow && target == primary)
                return;
            event.Handled = true;
            if (shift && m_selectionMode == SelectionMode::Multi)
                SelectRange(target, /*scroll*/ true);
            else
                SelectSingle(target, /*notify*/ true, /*scroll*/ true);
        }

        void OnSizeChange() override { Relayout(); }

        // === Selection engine ===
        void HandleSelectionClick(i32 item, u32 modifiers)
        {
            const bool ctrl = (modifiers & static_cast<u32>(KeyModCtrl)) != 0;
            const bool shift = (modifiers & static_cast<u32>(KeyModShift)) != 0;
            if (m_selectionMode == SelectionMode::Multi && shift && m_anchor >= 0)
            {
                SelectRange(item, /*scroll*/ false);
                return;
            }
            if (m_selectionMode == SelectionMode::Multi && ctrl)
            {
                ToggleItem(item);
                return;
            }
            SelectSingle(item, /*notify*/ true, /*scroll*/ false);
        }

    private:
        [[nodiscard]] f32 ContentHeight() const
        {
            return static_cast<f32>(ItemCount()) * m_rowHeight;
        }
        [[nodiscard]] f32 MaxScroll() const
        {
            return core::Max(0.0f, ContentHeight() - BodyHeight());
        }
        void ClampOffset() { m_offset = core::Max(0.0f, core::Min(m_offset, MaxScroll())); }
        void ScrollBy(f32 dy)
        {
            m_offset += dy;
            ClampOffset();
            Relayout();
        }
        void ScrollToFraction(f32 fraction)
        {
            m_offset = fraction * MaxScroll();
            ClampOffset();
            Relayout();
        }
        void ScrollItemIntoView(i32 item)
        {
            const f32 top = static_cast<f32>(item) * m_rowHeight;
            const f32 bottom = top + m_rowHeight;
            if (top < m_offset)
                m_offset = top;
            else if (bottom > m_offset + BodyHeight())
                m_offset = bottom - BodyHeight();
            ClampOffset();
        }

        void SelectSingle(i32 item, bool notify, bool scroll)
        {
            const i32 items = static_cast<i32>(ItemCount());
            if (item < 0 || item >= items)
                return;
            m_selection.Clear();
            m_selection.PushBack(item);
            m_anchor = item;
            if (scroll)
                ScrollItemIntoView(item);
            Relayout();
            if (notify)
                NotifySelectionChanged();
        }
        void ToggleItem(i32 item)
        {
            const i32 items = static_cast<i32>(ItemCount());
            if (item < 0 || item >= items)
                return;
            for (usize i = 0; i < m_selection.Size(); ++i)
                if (m_selection[i] == item)
                {
                    m_selection.RemoveAt(i);
                    m_anchor = item;
                    Relayout();
                    NotifySelectionChanged();
                    return;
                }
            m_selection.PushBack(item);
            m_anchor = item;
            Relayout();
            NotifySelectionChanged();
        }
        void SelectRange(i32 item, bool scroll)
        {
            const i32 items = static_cast<i32>(ItemCount());
            if (item < 0 || item >= items)
                return;
            const i32 a = m_anchor >= 0 ? m_anchor : item;
            const i32 lo = core::Min(a, item);
            const i32 hi = core::Max(a, item);
            m_selection.Clear();
            for (i32 r = lo; r <= hi; ++r)
                m_selection.PushBack(r);
            // anchor stays put; `item` becomes the primary (kept last)
            if (item != hi)
            { /* selection already includes item; ensure it's primary */
            }
            if (scroll)
                ScrollItemIntoView(item);
            Relayout();
            NotifySelectionChanged();
        }
        void ClampSelection()
        {
            const i32 items = static_cast<i32>(ItemCount());
            for (usize i = m_selection.Size(); i-- > 0;)
                if (m_selection[i] >= items)
                    m_selection.RemoveAt(i);
            if (m_anchor >= items)
                m_anchor = -1;
        }

        void EnsurePool(usize needed)
        {
            while (m_pool.Size() < needed)
            {
                RefPtr<ItemRow> row = CreateItemRow();
                row->SetSelectionColor(m_selectionColor);
                row->SetSelectionStyle(GetPartStyle(u8"selection"));
                AbstractItemView* self = this;
                row->SetOnPicked([self](i32 item, u32 modifiers)
                                 { self->OnRowClicked(item, modifiers); });
                AddChild(row.Get());
                m_pool.PushBack(core::Move(row));
            }
        }

        void Relayout()
        {
            const core::Float2 size = GetSize();
            const f32 inset = ContentTopInset();
            const bool barVisible = MaxScroll() > 0.0f;
            m_contentWidth = core::Max(0.0f, barVisible ? size.x - m_barThickness : size.x);
            OnBeforeLayout(m_contentWidth);

            const i32 items = static_cast<i32>(ItemCount());
            const i32 first = m_rowHeight > 0.0f ? static_cast<i32>(m_offset / m_rowHeight) : 0;
            const i32 span =
                m_rowHeight > 0.0f ? static_cast<i32>(BodyHeight() / m_rowHeight) + 2 : 0;
            const i32 last = core::Min(items, first + core::Max(0, span));
            const usize needed = static_cast<usize>(core::Max(0, last - first));
            EnsurePool(needed);
            m_visibleCount = needed;

            usize p = 0;
            for (i32 r = first; r < last; ++r, ++p)
            {
                ItemRow* row = m_pool[p].Get();
                row->SetVisible(true);
                row->SetItemIndex(r);
                row->SetSelected(IsItemSelected(r));
                row->SetPosition(
                    core::Float2{0.0f, inset + static_cast<f32>(r) * m_rowHeight - m_offset});
                row->SetSize(core::Float2{m_contentWidth, m_rowHeight});
                BindItemRow(*row, r, m_contentWidth);
            }
            for (; p < m_pool.Size(); ++p)
                m_pool[p]->SetVisible(false);

            OnLayoutDecorations(); // subclass positions a header etc.

            if (barVisible)
            {
                m_vBar->SetVisible(true);
                m_vBar->SetPosition(core::Float2{size.x - m_barThickness, inset});
                m_vBar->SetSize(core::Float2{m_barThickness, BodyHeight()});
                m_syncing = true;
                m_vBar->SetMax(static_cast<f64>(ContentHeight()));
                m_vBar->SetPage(static_cast<f64>(BodyHeight()));
                m_vBar->SetValue(static_cast<f64>(m_offset));
                m_syncing = false;
                m_vBar->ToFront();
            }
            else
            {
                m_vBar->SetVisible(false);
            }
        }

        IModel* m_model = nullptr;     // non-owning
        Array<RefPtr<ItemRow>> m_pool; // recycled row widgets
        RefPtr<ScrollBar> m_vBar;
        Array<i32> m_selection; // selected item indices (primary = last)
        i32 m_anchor = -1;      // range-selection anchor
        SelectionMode m_selectionMode = SelectionMode::Single;
        core::Function<void(ModelIndex)> m_onSelection;
        f32 m_offset = 0.0f;
        f32 m_rowHeight = 24.0f;
        f32 m_barThickness = 12.0f;
        f32 m_contentWidth = 0.0f;
        usize m_visibleCount = 0;
        bool m_syncing = false;
        Color m_backgroundColor{0.12f, 0.13f, 0.16f, 1.0f};
        Color m_selectionColor{0.18f, 0.37f, 0.62f, 1.0f};
    };

    RTTI_DEFINE_OBJECT(ItemRow, "rtti::gui")
    RTTI_DEFINE_OBJECT(AbstractItemView, "rtti::gui")
}
