// Draconic GUI - :abstract_item_view partition
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:abstract_item_view;

import draconic.foundation; // RefPtr, MakeRef, Array, Function, Move, Max, Min, Float2
import :rect;
import :event;
import :draw_context;
import :rectangle_drawable;
import :node;
import :ui_widget;
import :scroll_bar;
import :linear_layout; // Orientation
import :model_index;
import :model;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
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
        DRACONIC_OBJECT(ItemRow, UIWidget)
    public:
        ItemRow() { SetTag(foundation::StringView(u8"itemrow")); }

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
        void SetOnPicked(foundation::Function<void(i32, u32)> callback)
        {
            m_onPicked = foundation::Move(callback);
        }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            if (m_onPicked)
                m_onPicked(m_item, event.Modifiers);
        }
        void OnDraw(DrawContext& ctx, const Rect&) override
        {
            if (m_selected)
                ctx.VG().FillRect(GetLocalBounds().ToRectangle(), m_selectionColor);
        }

    private:
        i32 m_item = -1;
        bool m_selected = false;
        Color m_selectionColor{0.18f, 0.37f, 0.62f, 1.0f};
        foundation::Function<void(i32, u32)> m_onPicked;
    };

    class AbstractItemView : public UIWidget, public IModelClient
    {
        DRACONIC_OBJECT(AbstractItemView, UIWidget)
    public:
        AbstractItemView()
        {
            SetClipChildren(true);
            SetTabFocusable(true);
            SetBackground(
                foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_backgroundColor));

            m_vBar = foundation::MakeRef<ScrollBar>(foundation::DefaultAllocator());
            m_vBar->SetOrientation(Orientation::Vertical);
            AbstractItemView* self = this;
            m_vBar->SetOnValueChanged(
                [self](f32 v)
                {
                    if (!self->m_syncing)
                        self->ScrollToFraction(v);
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
            m_rowHeight = foundation::Max(1.0f, height);
            Relayout();
        }
        [[nodiscard]] f32 GetRowHeight() const noexcept { return m_rowHeight; }
        void SetSelectionColor(Color color)
        {
            m_selectionColor = color;
            for (const RefPtr<ItemRow>& r : m_pool)
                r->SetSelectionColor(color);
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"selection"))
                SetSelectionColor(color);
        }
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"selection"));
        }

        // === Selection ===
        void SetSelectionMode(SelectionMode mode) noexcept { m_selectionMode = mode; }
        [[nodiscard]] SelectionMode GetSelectionMode() const noexcept { return m_selectionMode; }
        void SetOnSelectionChanged(foundation::Function<void(ModelIndex)> callback)
        {
            m_onSelection = foundation::Move(callback);
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

        [[nodiscard]] bool WantsWheel() const override { return true; }

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
            return foundation::Max(0.0f, GetSize().y - ContentTopInset());
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
            ScrollBy(-event.Delta.y * m_rowHeight);
        }

        void OnKeyDown(const KeyEvent& event) override
        {
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (OnExtraKey(key))
                return;
            const i32 items = static_cast<i32>(ItemCount());
            if (items == 0)
                return;
            const bool shift = (event.Modifiers & static_cast<u32>(KeyModShift)) != 0;
            const i32 page = foundation::Max(1, static_cast<i32>(BodyHeight() / m_rowHeight) - 1);
            const i32 primary = GetSelectedItem();
            i32 target = primary;
            switch (key)
            {
            case KeyCode::Up:
                target = (primary <= 0) ? 0 : primary - 1;
                break;
            case KeyCode::Down:
                target = (primary < 0) ? 0 : foundation::Min(items - 1, primary + 1);
                break;
            case KeyCode::Home:
                target = 0;
                break;
            case KeyCode::End:
                target = items - 1;
                break;
            case KeyCode::PageUp:
                target = foundation::Max(0, (primary < 0 ? 0 : primary) - page);
                break;
            case KeyCode::PageDown:
                target = foundation::Min(items - 1, (primary < 0 ? 0 : primary) + page);
                break;
            default:
                return;
            }
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
            return foundation::Max(0.0f, ContentHeight() - BodyHeight());
        }
        void ClampOffset() { m_offset = foundation::Max(0.0f, foundation::Min(m_offset, MaxScroll())); }
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
            const i32 lo = foundation::Min(a, item);
            const i32 hi = foundation::Max(a, item);
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
                AbstractItemView* self = this;
                row->SetOnPicked([self](i32 item, u32 modifiers)
                                 { self->OnRowClicked(item, modifiers); });
                AddChild(row.Get());
                m_pool.PushBack(foundation::Move(row));
            }
        }

        void Relayout()
        {
            const foundation::Float2 size = GetSize();
            const f32 inset = ContentTopInset();
            const bool barVisible = MaxScroll() > 0.0f;
            m_contentWidth = foundation::Max(0.0f, barVisible ? size.x - m_barThickness : size.x);
            OnBeforeLayout(m_contentWidth);

            const i32 items = static_cast<i32>(ItemCount());
            const i32 first = m_rowHeight > 0.0f ? static_cast<i32>(m_offset / m_rowHeight) : 0;
            const i32 span =
                m_rowHeight > 0.0f ? static_cast<i32>(BodyHeight() / m_rowHeight) + 2 : 0;
            const i32 last = foundation::Min(items, first + foundation::Max(0, span));
            const usize needed = static_cast<usize>(foundation::Max(0, last - first));
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
                    foundation::Float2{0.0f, inset + static_cast<f32>(r) * m_rowHeight - m_offset});
                row->SetSize(foundation::Float2{m_contentWidth, m_rowHeight});
                BindItemRow(*row, r, m_contentWidth);
            }
            for (; p < m_pool.Size(); ++p)
                m_pool[p]->SetVisible(false);

            OnLayoutDecorations(); // subclass positions a header etc.

            if (barVisible)
            {
                m_vBar->SetVisible(true);
                m_vBar->SetPosition(foundation::Float2{size.x - m_barThickness, inset});
                m_vBar->SetSize(foundation::Float2{m_barThickness, BodyHeight()});
                m_vBar->SetThumbProportion(ContentHeight() > 0.0f ? BodyHeight() / ContentHeight()
                                                                  : 1.0f);
                m_syncing = true;
                m_vBar->SetValue(MaxScroll() > 0.0f ? m_offset / MaxScroll() : 0.0f);
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
        foundation::Function<void(ModelIndex)> m_onSelection;
        f32 m_offset = 0.0f;
        f32 m_rowHeight = 24.0f;
        f32 m_barThickness = 12.0f;
        f32 m_contentWidth = 0.0f;
        usize m_visibleCount = 0;
        bool m_syncing = false;
        Color m_backgroundColor{0.12f, 0.13f, 0.16f, 1.0f};
        Color m_selectionColor{0.18f, 0.37f, 0.62f, 1.0f};
    };

    DRACONIC_DEFINE_OBJECT(ItemRow, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(AbstractItemView, "draconic::gui")
}
