// Draconic UI - :list_view partition
//
// Virtualized list: only creates/binds views for the visible range, recycling views that scroll out.
// Uses IListAdapter for data + view creation and ViewRecycler for pooling; supports fixed item height
// (O(1)) and variable height (binary search over a cumulative-offset cache). Ported from Sedulous.UI/
// src/Controls/ListView.bf.
//
// Ownership: mActiveViews is a HashMap<i32, RefPtr<View>> (the active item views, RAII); views move
// between it and the ViewRecycler pool. The single vertical ScrollBar is a VISUAL child held as a
// RefPtr member (UIContext::Attach/DetachView recurse VisualChildCount, so it attaches with the view).
// mAdapter is BORROWED (pattern-B injected; owned by the caller). Beef get/set props -> methods; the
// (_, dy) momentum tuple -> Float2; Dictionary+manual delete -> RefPtr map (no manual delete).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:list_view;

import draconic.foundation;
import draconic.vg;
import :view;
import :property;
import :event;
import :box_constraints;
import :thickness;
import :draw_context;
import :control_state;
import :style_property;
import :event_args;
import :input_enums;
import :enums;
import :scroll_bar;
import :momentum_helper;
import :ilist_adapter;
import :selection_model;
import :view_recycler;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::ui
{
    class ListView : public ViewGroup, public IListAdapterObserver
    {
        DRACONIC_OBJECT(ListView, ViewGroup)
    public:
        SelectionModel Selection;
        Property<f32> ItemHeight{30.0f};

        /// (position, clickCount, localX, localY)
        Event<void(i32, i32, f32, f32)> OnItemClicked;
        /// (position, localX, localY)
        Event<void(i32, f32, f32)> OnItemRightClicked;
        /// (position)
        Event<void(i32)> OnItemLongPress;
        /// (localX, localY)
        Event<void(f32, f32)> OnBackgroundRightClicked;
        /// (position, KeyEventArgs). Set e.Handled to suppress default handling.
        Event<void(i32, KeyEventArgs&)> OnItemKeyDown;

        f32 LongPressTime = 0.5f;

        ListView()
        {
            ClipsContent = true;
            IsFocusable = true;
            IsTabStop = true;
            WantsArrowKeys = true;
            ItemHeight.SetOwner(this);
            ListView* self = this;
            m_scrollBar = MakeRef<ScrollBar>(DefaultAllocator(), false);
            m_scrollBar->Parent = this;
            m_scrollBar->OnValueChanged.Add(
                Event<void(ScrollBar*, f32)>::Handler{[self](ScrollBar*, f32 val)
                                                      {
                                                          self->m_scrollY = val;
                                                          self->Invalidate();
                                                      }});
        }

        [[nodiscard]] f32 ScrollY() const noexcept { return m_scrollY; }
        [[nodiscard]] ViewRecycler& Recycler() noexcept { return m_recycler; }

        [[nodiscard]] IListAdapter* GetAdapter() const noexcept { return m_adapter; }
        void SetAdapter(IListAdapter* adapter)
        {
            if (m_adapter != nullptr)
            {
                m_adapter->SetObserver(nullptr);
            }
            m_adapter = adapter;
            if (m_adapter != nullptr)
            {
                m_adapter->SetObserver(this);
            }
            RebuildOffsets();
            RecycleAllActive();
            Invalidate();
        }

        [[nodiscard]] f32 MaxScrollY() const
        {
            const f32 contentH =
                m_variableHeight
                    ? m_totalContentHeight
                    : ((m_adapter != nullptr) ? m_adapter->ItemCount() * ItemHeight.Value() : 0.0f);
            const f32 viewportH = Height() - Padding.TotalVertical();
            return foundation::Max(0.0f, contentH - viewportH);
        }

        /// Scroll by delta, clamping to valid range.
        void ScrollBy(f32 dy)
        {
            m_scrollY = foundation::Clamp(m_scrollY + dy, 0.0f, MaxScrollY());
            Invalidate();
        }

        /// Rebuild visible items after adapter data changed.
        void NotifyDataChanged()
        {
            RebuildOffsets();
            RecycleAllActive();
            Invalidate();
        }

        // === IListAdapterObserver ===
        void OnDataSetChanged() override { NotifyDataChanged(); }
        void OnItemRangeChanged(i32 start, i32 count) override
        {
            if (m_adapter == nullptr)
            {
                return;
            }
            for (i32 pos = start; pos < start + count; ++pos)
            {
                if (RefPtr<View>* v = m_activeViews.Find(pos))
                {
                    m_adapter->BindView(v->Get(), pos);
                }
            }
            if (m_variableHeight)
            {
                RebuildOffsets();
                Invalidate();
            }
        }

        // === Visual children: active item views + scrollbar ===
        [[nodiscard]] usize VisualChildCount() const override { return m_activeViews.Size() + 1; }
        [[nodiscard]] View* GetVisualChild(usize index) const override
        {
            if (index < m_activeViews.Size())
            {
                usize i = 0;
                for (const auto& kv : m_activeViews)
                {
                    if (i == index)
                    {
                        return kv.value.Get();
                    }
                    ++i;
                }
            }
            if (index == m_activeViews.Size())
            {
                return m_scrollBar.Get();
            }
            return nullptr;
        }

        // === Input ===
        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            if (MaxScrollY() > 0)
            {
                ScrollBy(-e.DeltaY * ItemHeight.Value() * 2);
                m_momentum.VelocityY = -e.DeltaY * 200;
                e.Handled = true;
            }
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            const Float2 local = ScreenToLocal(MouseScreenPos());

            // Click-to-focus: keyboard interaction (arrows, item keys like F2/Delete) follows a
            // click into the list. Interactive rows that keep focus themselves (e.g. an editing
            // EditableLabel) mark their clicks handled, so this never runs for them.
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetFocus(this);
            }

            if (e.Button == MouseButton::Left && MaxScrollY() > 0)
            {
                m_dragging = true;
                m_dragLastY = local.y;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
            }

            if (m_adapter != nullptr && e.Button == MouseButton::Right)
            {
                const i32 itemIndex = GetItemAtY(local.y);
                if (itemIndex >= 0 && itemIndex < m_adapter->ItemCount())
                {
                    if (!Selection.IsSelected(itemIndex))
                    {
                        Selection.Select(itemIndex);
                    }
                    OnItemRightClicked.Invoke(itemIndex, local.x, local.y);
                }
                else
                {
                    OnBackgroundRightClicked.Invoke(local.x, local.y);
                }
                e.Handled = true;
            }

            if (m_adapter != nullptr && e.Button == MouseButton::Left)
            {
                const i32 itemIndex = GetItemAtY(local.y);
                if (itemIndex >= 0 && itemIndex < m_adapter->ItemCount())
                {
                    if (HasFlag(e.Modifiers, KeyModifiers::Ctrl))
                    {
                        Selection.Toggle(itemIndex);
                    }
                    else if (HasFlag(e.Modifiers, KeyModifiers::Shift))
                    {
                        Selection.SelectRange(Selection.FirstSelected(), itemIndex);
                    }
                    else
                    {
                        Selection.Select(itemIndex);
                    }

                    OnItemClicked.Invoke(itemIndex, e.ClickCount, local.x, local.y);
                    m_pressedItem = itemIndex;
                    m_pressTime = 0;
                    m_longPressFired = false;
                }
                e.Handled = true;
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            (void)e;
            if (m_dragging)
            {
                // A drag-and-drop taking over the same gesture wins: stop scroll-dragging, or
                // the list scrolls under the drag and the drop lands on the wrong row.
                if (Context != nullptr && Context->DragDrop()->IsDragging())
                {
                    m_dragging = false;
                    m_momentum.VelocityY = 0;
                    Context->GetFocusManager()->ReleaseCapture();
                    return;
                }
                const Float2 local = ScreenToLocal(MouseScreenPos());
                const f32 dy = m_dragLastY - local.y;
                if (Abs(dy) > 1)
                {
                    ScrollBy(dy);
                    m_momentum.VelocityY = dy * 60;
                    m_dragLastY = local.y;
                }
            }
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            (void)e;
            if (m_dragging)
            {
                m_dragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
            }
            m_pressedItem = -1;
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (m_adapter == nullptr)
            {
                return;
            }
            const i32 sel = Selection.FirstSelected();
            const i32 count = m_adapter->ItemCount();

            if (sel >= 0)
            {
                OnItemKeyDown.Invoke(sel, e);
                if (e.Handled)
                {
                    return;
                }
            }

            const bool shift = HasFlag(e.Modifiers, KeyModifiers::Shift);
            switch (e.Key)
            {
            case KeyCode::Down:
            {
                const i32 next = Min(sel + 1, count - 1);
                if (shift)
                {
                    Selection.SelectRange(sel, next);
                }
                else
                {
                    Selection.Select(next);
                }
                ScrollToPosition(next);
                e.Handled = true;
                break;
            }
            case KeyCode::Up:
            {
                const i32 prev = Max(sel - 1, 0);
                if (shift)
                {
                    Selection.SelectRange(sel, prev);
                }
                else
                {
                    Selection.Select(prev);
                }
                ScrollToPosition(prev);
                e.Handled = true;
                break;
            }
            case KeyCode::Home:
            {
                if (shift)
                {
                    Selection.SelectRange(sel, 0);
                }
                else
                {
                    Selection.Select(0);
                }
                ScrollToPosition(0);
                e.Handled = true;
                break;
            }
            case KeyCode::End:
            {
                if (shift)
                {
                    Selection.SelectRange(sel, count - 1);
                }
                else
                {
                    Selection.Select(count - 1);
                }
                ScrollToPosition(count - 1);
                e.Handled = true;
                break;
            }
            case KeyCode::PageDown:
            {
                const i32 page = static_cast<i32>(Height() / ItemHeight.Value());
                const i32 n = Min(sel + page, count - 1);
                if (shift)
                {
                    Selection.SelectRange(sel, n);
                }
                else
                {
                    Selection.Select(n);
                }
                ScrollToPosition(n);
                e.Handled = true;
                break;
            }
            case KeyCode::PageUp:
            {
                const i32 page = static_cast<i32>(Height() / ItemHeight.Value());
                const i32 p = Max(sel - page, 0);
                if (shift)
                {
                    Selection.SelectRange(sel, p);
                }
                else
                {
                    Selection.Select(p);
                }
                ScrollToPosition(p);
                e.Handled = true;
                break;
            }
            default:
                break;
            }
        }

        /// The currently visible view for a position, or null.
        [[nodiscard]] View* GetActiveView(i32 position) const
        {
            if (const RefPtr<View>* v = m_activeViews.Find(position))
            {
                return v->Get();
            }
            return nullptr;
        }

        /// Scroll so the item at `position` is visible.
        void ScrollToPosition(i32 position)
        {
            if (m_adapter == nullptr || position < 0 || position >= m_adapter->ItemCount())
            {
                return;
            }
            const f32 itemTop = GetItemOffset(position);
            const f32 itemBottom = itemTop + GetItemHeightAt(position);
            const f32 viewportH = Height() - Padding.TotalVertical();
            if (itemTop < m_scrollY)
            {
                m_scrollY = itemTop;
            }
            else if (itemBottom > m_scrollY + viewportH)
            {
                m_scrollY = itemBottom - viewportH;
            }
            m_scrollY = foundation::Clamp(m_scrollY, 0.0f, MaxScrollY());
            Invalidate();
        }

        /// Adapter position of the item at local Y.
        [[nodiscard]] i32 GetItemAtY(f32 localY) const
        {
            const f32 scrolledY = localY + m_scrollY - Padding.Top;
            if (m_variableHeight)
            {
                return FindFirstVisible(scrolledY);
            }
            return static_cast<i32>(scrolledY / ItemHeight.Value());
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 contentH =
                m_variableHeight
                    ? m_totalContentHeight
                    : ((m_adapter != nullptr) ? m_adapter->ItemCount() * ItemHeight.Value() : 0.0f);
            const f32 desiredH = contentH + Padding.TotalVertical();
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(desiredH)};

            m_scrollBarVisible = MaxScrollY() > 0;
            m_scrollBar->Visibility =
                m_scrollBarVisible ? VisibilityValue::Visible : VisibilityValue::Gone;
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (m_adapter == nullptr)
            {
                return;
            }

            const f32 viewportH = height - Padding.TotalVertical();
            const f32 viewportW = width - Padding.TotalHorizontal() -
                                  (m_scrollBarVisible ? m_scrollBar->BarThickness : 0.0f);

            m_scrollY = foundation::Clamp(m_scrollY, 0.0f, MaxScrollY());

            if (m_adapter->ItemCount() == 0)
            {
                m_scrollBarVisible = false;
                m_scrollBar->Visibility = VisibilityValue::Gone;
                return;
            }

            if (Context != nullptr && m_scrollBar->Context == nullptr)
            {
                Context->AttachView(m_scrollBar.Get());
            }

            // Compute the visible range.
            const i32 firstVis = FindFirstVisible(m_scrollY);
            i32 lastVis = firstVis;
            f32 y = GetItemOffset(firstVis) - m_scrollY;
            for (i32 pos = firstVis; pos < m_adapter->ItemCount(); ++pos)
            {
                if (y > viewportH)
                {
                    break;
                }
                lastVis = pos;
                y += GetItemHeightAt(pos);
            }

            RecycleOutOfRange(firstVis, lastVis);

            for (i32 pos = firstVis; pos <= lastVis; ++pos)
            {
                if (m_activeViews.Find(pos) == nullptr)
                {
                    RefPtr<View> view = m_recycler.GetOrCreate(*m_adapter, pos);
                    view->Parent = this;
                    if (Context != nullptr)
                    {
                        Context->AttachView(view.Get());
                    }
                    m_activeViews.InsertOrAssign(pos, Move(view));
                }
                else
                {
                    m_adapter->BindView(m_activeViews.Find(pos)->Get(), pos);
                }

                View* v = m_activeViews.Find(pos)->Get();
                const f32 itemY = Padding.Top + GetItemOffset(pos) - m_scrollY;
                const f32 itemH = GetItemHeightAt(pos);
                v->Measure(BoxConstraints::Tight(viewportW, itemH));
                v->Layout(Padding.Left, itemY, viewportW, itemH);
            }

            m_firstVisible = firstVis;
            m_lastVisible = lastVis;

            if (m_scrollBarVisible)
            {
                m_scrollBar->SetValue(m_scrollY);
                m_scrollBar->SetMaxValue(MaxScrollY());
                m_scrollBar->SetViewportSize(viewportH);
                m_scrollBar->Measure(BoxConstraints::Tight(m_scrollBar->BarThickness, viewportH));
                m_scrollBar->Layout(width - m_scrollBar->BarThickness, Padding.Top,
                                    m_scrollBar->BarThickness, viewportH);
            }
        }

    public:
        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 dt = Context ? Context->DeltaTime() : 0.016f;
            const Float2 d = m_momentum.Update(dt);
            if (d.y != 0)
            {
                ScrollBy(d.y);
            }

            if (m_pressedItem >= 0 && !m_longPressFired && !m_dragging)
            {
                m_pressTime += dt;
                if (m_pressTime >= LongPressTime)
                {
                    m_longPressFired = true;
                    OnItemLongPress.Invoke(m_pressedItem);
                }
            }

            if (m_adapter != nullptr)
            {
                const Color selColor = ResolveStyleColor(
                    StyleProperty::SelectionColor,
                    Color{60.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f, 80.0f / 255.0f});
                const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius,
                                                 0.0f); // round the row in the rounded theme
                for (const auto& kv : m_activeViews)
                {
                    if (Selection.IsSelected(kv.key))
                    {
                        View* v = kv.value.Get();
                        const Rectangle r{v->Bounds.x, v->Bounds.y, v->Width(), v->Height()};
                        if (cr > 0.0f)
                        {
                            ctx.VG().FillRoundedRect(r, cr, selColor);
                        }
                        else
                        {
                            ctx.VG().FillRect(r, selColor);
                        }
                    }
                }
            }

            DrawChildren(ctx);
        }

    private:
        void RebuildOffsets()
        {
            m_itemOffsets.Clear();
            m_variableHeight = false;
            m_totalContentHeight = 0;
            if (m_adapter == nullptr)
            {
                return;
            }

            const i32 count = m_adapter->ItemCount();
            m_itemOffsets.Reserve(static_cast<usize>(count) + 1);
            f32 offset = 0;
            for (i32 i = 0; i < count; ++i)
            {
                m_itemOffsets.PushBack(offset);
                const f32 h = m_adapter->GetItemHeight(i);
                if (h > 0)
                {
                    m_variableHeight = true;
                    offset += h;
                }
                else
                {
                    offset += ItemHeight.Value();
                }
            }
            m_itemOffsets.PushBack(offset);
            m_totalContentHeight = offset;
        }

        [[nodiscard]] f32 GetItemOffset(i32 position) const
        {
            if (m_variableHeight && static_cast<usize>(position) < m_itemOffsets.Size())
            {
                return m_itemOffsets[static_cast<usize>(position)];
            }
            return position * ItemHeight.Value();
        }
        [[nodiscard]] f32 GetItemHeightAt(i32 position) const
        {
            if (m_variableHeight && m_adapter != nullptr)
            {
                const f32 h = m_adapter->GetItemHeight(position);
                if (h > 0)
                {
                    return h;
                }
            }
            return ItemHeight.Value();
        }

        [[nodiscard]] i32 FindFirstVisible(f32 scrollY) const
        {
            if (!m_variableHeight || m_itemOffsets.Size() <= 1)
            {
                return static_cast<i32>(scrollY / ItemHeight.Value());
            }
            i32 lo = 0, hi = static_cast<i32>(m_itemOffsets.Size() - 2);
            while (lo < hi)
            {
                const i32 mid = (lo + hi + 1) / 2;
                if (m_itemOffsets[static_cast<usize>(mid)] <= scrollY)
                {
                    lo = mid;
                }
                else
                {
                    hi = mid - 1;
                }
            }
            return lo;
        }

        void RecycleOutOfRange(i32 first, i32 last)
        {
            Array<i32> toRemove;
            for (const auto& kv : m_activeViews)
            {
                if (kv.key < first || kv.key > last)
                {
                    toRemove.PushBack(kv.key);
                }
            }
            for (i32 pos : toRemove)
            {
                RefPtr<View>* slot = m_activeViews.Find(pos);
                const i32 viewType = (m_adapter != nullptr) ? m_adapter->GetItemViewType(pos) : 0;
                View* v = slot->Get();
                if (v->Context != nullptr)
                {
                    v->Context->DetachView(v);
                }
                v->Parent = nullptr;
                RefPtr<View> owned = Move(*slot);
                m_activeViews.Remove(pos);
                m_recycler.Recycle(Move(owned), viewType);
            }
        }

        void RecycleAllActive()
        {
            for (auto& kv : m_activeViews)
            {
                const i32 viewType =
                    (m_adapter != nullptr) ? m_adapter->GetItemViewType(kv.key) : 0;
                View* v = kv.value.Get();
                if (v->Context != nullptr)
                {
                    v->Context->DetachView(v);
                }
                v->Parent = nullptr;
                m_recycler.Recycle(Move(kv.value), viewType);
            }
            m_activeViews.Clear();
        }

        [[nodiscard]] Float2 MouseScreenPos() const
        {
            if (Context == nullptr)
            {
                return Float2{0, 0};
            }
            return Float2{Context->GetInputManager()->MouseX(),
                          Context->GetInputManager()->MouseY()};
        }

        IListAdapter* m_adapter = nullptr; // borrowed
        ViewRecycler m_recycler;
        f32 m_scrollY = 0.0f;
        MomentumHelper m_momentum{};

        HashMap<i32, RefPtr<View>> m_activeViews;
        i32 m_firstVisible = -1;
        i32 m_lastVisible = -1;

        Array<f32> m_itemOffsets;
        bool m_variableHeight = false;
        f32 m_totalContentHeight = 0.0f;

        RefPtr<ScrollBar> m_scrollBar;
        bool m_scrollBarVisible = false;

        bool m_dragging = false;
        f32 m_dragLastY = 0.0f;

        i32 m_pressedItem = -1;
        f32 m_pressTime = 0.0f;
        bool m_longPressFired = false;
    };

    DRACONIC_DEFINE_OBJECT(ListView, "draconic::ui")
}
