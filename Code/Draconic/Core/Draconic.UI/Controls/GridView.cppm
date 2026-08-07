// Draconic UI - :grid_view partition
//
// Virtualized flowing grid with fixed cell size: items flow left-to-right, wrapping to new rows; only
// creates/binds views for visible rows. Ported from Sedulous.UI/src/Controls/GridView.bf. Same ownership
// model as ListView: mActiveViews = HashMap<i32, RefPtr<View>> (views cycle to/from the ViewRecycler);
// the vertical ScrollBar is a VISUAL child RefPtr member; the adapter is BORROWED (pattern-B). Beef
// get/set props -> methods; the (_, dy) momentum tuple -> Float2.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:grid_view;

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
    class GridView : public ViewGroup, public IListAdapterObserver
    {
        DRACONIC_OBJECT(GridView, ViewGroup)
    public:
        SelectionModel Selection;
        Property<f32> CellWidth{60.0f};
        Property<f32> CellHeight{60.0f};
        Property<f32> CellSpacing{4.0f};

        /// (position, clickCount, localX, localY)
        Event<void(i32, i32, f32, f32)> OnItemClicked;
        /// (position, localX, localY)
        Event<void(i32, f32, f32)> OnItemRightClicked;
        /// Right-click on empty space below/between cells (context menus on the container).
        Event<void(f32, f32)> OnBackgroundRightClicked;
        /// Key pressed while an item is selected (position, args) - dispatched before the
        /// grid's own navigation keys, same contract as ListView::OnItemKeyDown.
        Event<void(i32, KeyEventArgs&)> OnItemKeyDown;

        GridView()
        {
            ClipsContent = true;
            IsFocusable = true;
            IsTabStop = true;
            WantsArrowKeys = true;
            CellWidth.SetOwner(this);
            CellHeight.SetOwner(this);
            CellSpacing.SetOwner(this);
            GridView* self = this;
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
            RecycleAllActive();
            Invalidate();
        }

        [[nodiscard]] f32 MaxScrollY() const
        {
            return foundation::Max(0.0f, m_totalContentHeight - (Height() - Padding.TotalVertical()));
        }

        void ScrollBy(f32 dy)
        {
            m_scrollY = foundation::Clamp(m_scrollY + dy, 0.0f, MaxScrollY());
            Invalidate();
        }

        // === IListAdapterObserver ===
        void OnDataSetChanged() override
        {
            RecycleAllActive();
            Invalidate();
        }
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
        }

        // === Visual children ===
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
                ScrollBy(-e.DeltaY * (CellHeight.Value() + CellSpacing.Value()) * 2);
                m_momentum.VelocityY = -e.DeltaY * 200;
                e.Handled = true;
            }
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (m_adapter == nullptr)
            {
                return;
            }
            const i32 pos = GetItemAtPoint(e.X, e.Y);

            if (e.Button == MouseButton::Right)
            {
                if (pos >= 0 && pos < m_adapter->ItemCount())
                {
                    if (!Selection.IsSelected(pos))
                    {
                        Selection.Select(pos);
                    }
                    OnItemRightClicked.Invoke(pos, e.X, e.Y);
                }
                else
                {
                    OnBackgroundRightClicked.Invoke(e.X, e.Y);
                }
                e.Handled = true;
            }
            else if (e.Button == MouseButton::Left)
            {
                if (pos >= 0 && pos < m_adapter->ItemCount())
                {
                    if (HasFlag(e.Modifiers, KeyModifiers::Ctrl))
                    {
                        Selection.Toggle(pos);
                    }
                    else if (HasFlag(e.Modifiers, KeyModifiers::Shift))
                    {
                        Selection.SelectRange(Selection.FirstSelected(), pos);
                    }
                    else
                    {
                        Selection.Select(pos);
                    }
                    OnItemClicked.Invoke(pos, e.ClickCount, e.X, e.Y);
                }
                e.Handled = true;
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (m_adapter == nullptr || m_columnsCount <= 0)
            {
                return;
            }
            const i32 sel = Selection.FirstSelected();
            const i32 count = m_adapter->ItemCount();

            // Item-scoped keys first (F2 rename, Delete, ...), same contract as ListView.
            if (sel >= 0)
            {
                OnItemKeyDown.Invoke(sel, e);
                if (e.Handled)
                {
                    return;
                }
            }

            switch (e.Key)
            {
            case KeyCode::Right:
                if (sel < count - 1)
                {
                    Selection.Select(sel + 1);
                }
                ScrollToPosition(Selection.FirstSelected());
                e.Handled = true;
                break;
            case KeyCode::Left:
                if (sel > 0)
                {
                    Selection.Select(sel - 1);
                }
                ScrollToPosition(Selection.FirstSelected());
                e.Handled = true;
                break;
            case KeyCode::Down:
            {
                const i32 next = Min(sel + m_columnsCount, count - 1);
                Selection.Select(next);
                ScrollToPosition(next);
                e.Handled = true;
                break;
            }
            case KeyCode::Up:
            {
                const i32 prev = Max(sel - m_columnsCount, 0);
                Selection.Select(prev);
                ScrollToPosition(prev);
                e.Handled = true;
                break;
            }
            case KeyCode::Home:
                Selection.Select(0);
                ScrollToPosition(0);
                e.Handled = true;
                break;
            case KeyCode::End:
                Selection.Select(count - 1);
                ScrollToPosition(count - 1);
                e.Handled = true;
                break;
            case KeyCode::PageDown:
            {
                const i32 rows =
                    static_cast<i32>(Height() / (CellHeight.Value() + CellSpacing.Value()));
                const i32 n = Min(sel + rows * m_columnsCount, count - 1);
                Selection.Select(n);
                ScrollToPosition(n);
                e.Handled = true;
                break;
            }
            case KeyCode::PageUp:
            {
                const i32 rows =
                    static_cast<i32>(Height() / (CellHeight.Value() + CellSpacing.Value()));
                const i32 p = Max(sel - rows * m_columnsCount, 0);
                Selection.Select(p);
                ScrollToPosition(p);
                e.Handled = true;
                break;
            }
            default:
                break;
            }
        }

        /// Adapter position at a local point, or -1.
        [[nodiscard]] i32 GetItemAtPoint(f32 localX, f32 localY) const
        {
            if (m_columnsCount <= 0)
            {
                return -1;
            }
            const f32 scrolledY = localY + m_scrollY - Padding.Top;
            const f32 x = localX - Padding.Left;
            const i32 col = static_cast<i32>(x / (CellWidth.Value() + CellSpacing.Value()));
            const i32 row =
                static_cast<i32>(scrolledY / (CellHeight.Value() + CellSpacing.Value()));
            if (col < 0 || col >= m_columnsCount)
            {
                return -1;
            }
            const i32 pos = row * m_columnsCount + col;
            if (m_adapter != nullptr && pos >= m_adapter->ItemCount())
            {
                return -1;
            }
            return pos;
        }

        /// The currently visible view for a position, or null (same contract as ListView).
        [[nodiscard]] View* GetActiveView(i32 position) const
        {
            if (const RefPtr<View>* v = m_activeViews.Find(position))
            {
                return v->Get();
            }
            return nullptr;
        }

        void ScrollToPosition(i32 position)
        {
            if (m_adapter == nullptr || m_columnsCount <= 0 || position < 0)
            {
                return;
            }
            const i32 row = position / m_columnsCount;
            const f32 rowY = row * (CellHeight.Value() + CellSpacing.Value());
            const f32 viewportH = Height() - Padding.TotalVertical();
            if (rowY < m_scrollY)
            {
                m_scrollY = rowY;
            }
            else if (rowY + CellHeight.Value() > m_scrollY + viewportH)
            {
                m_scrollY = rowY + CellHeight.Value() - viewportH;
            }
            m_scrollY = foundation::Clamp(m_scrollY, 0.0f, MaxScrollY());
            Invalidate();
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 dt = Context ? Context->DeltaTime() : 0.016f;
            const Float2 d = m_momentum.Update(dt);
            if (d.y != 0)
            {
                ScrollBy(d.y);
            }

            if (m_adapter != nullptr)
            {
                const Color selColor = ResolveStyleColor(
                    StyleProperty::SelectionColor,
                    Color{60.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f, 80.0f / 255.0f});
                const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius,
                                                 0.0f); // round the tile in the rounded theme
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

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(constraints.MaxHeight)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const f32 viewportW = width - Padding.TotalHorizontal();
            const f32 viewportH = height - Padding.TotalVertical();

            m_columnsCount =
                foundation::Max(1, static_cast<i32>((viewportW + CellSpacing.Value()) /
                                              (CellWidth.Value() + CellSpacing.Value())));
            const i32 itemCount = (m_adapter != nullptr) ? m_adapter->ItemCount() : 0;
            m_rowCount = (itemCount > 0) ? (itemCount + m_columnsCount - 1) / m_columnsCount : 0;
            m_totalContentHeight =
                (m_rowCount > 0)
                    ? m_rowCount * (CellHeight.Value() + CellSpacing.Value()) - CellSpacing.Value()
                    : 0.0f;

            m_scrollBarVisible = MaxScrollY() > 0;
            m_scrollBar->Visibility =
                m_scrollBarVisible ? VisibilityValue::Visible : VisibilityValue::Gone;
            m_scrollY = foundation::Clamp(m_scrollY, 0.0f, MaxScrollY());

            if (m_adapter == nullptr || itemCount == 0)
            {
                return;
            }

            if (Context != nullptr && m_scrollBar->Context == nullptr)
            {
                Context->AttachView(m_scrollBar.Get());
            }

            const f32 rowStride = CellHeight.Value() + CellSpacing.Value();
            const i32 firstRow = static_cast<i32>(m_scrollY / rowStride);
            const i32 lastRow =
                Min(firstRow + static_cast<i32>(viewportH / rowStride) + 1, m_rowCount - 1);
            const i32 firstPos = firstRow * m_columnsCount;
            const i32 lastPos = Min((lastRow + 1) * m_columnsCount - 1, itemCount - 1);

            RecycleOutOfRange(firstPos, lastPos);

            for (i32 pos = firstPos; pos <= lastPos; ++pos)
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

                const i32 row = pos / m_columnsCount;
                const i32 col = pos % m_columnsCount;
                const f32 cellX = Padding.Left + col * (CellWidth.Value() + CellSpacing.Value());
                const f32 cellY = Padding.Top + row * rowStride - m_scrollY;
                View* v = m_activeViews.Find(pos)->Get();
                v->Measure(BoxConstraints::Tight(CellWidth.Value(), CellHeight.Value()));
                v->Layout(cellX, cellY, CellWidth.Value(), CellHeight.Value());
            }

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

    private:
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

        IListAdapter* m_adapter = nullptr; // borrowed
        ViewRecycler m_recycler;
        f32 m_scrollY = 0.0f;
        MomentumHelper m_momentum{};

        HashMap<i32, RefPtr<View>> m_activeViews;
        RefPtr<ScrollBar> m_scrollBar;
        bool m_scrollBarVisible = false;

        i32 m_columnsCount = 1;
        i32 m_rowCount = 0;
        f32 m_totalContentHeight = 0.0f;
    };

    DRACONIC_DEFINE_OBJECT(GridView, "draconic::ui")
}
