// Draconic UI - :tree_view partition
//
// Tree view built on a FlattenedTreeAdapter + an internal ListView for virtualization; draws indent +
// expand/collapse chevrons and toggles expansion on arrow-zone clicks / Left-Right keys. Ported from
// Sedulous.UI/src/Controls/TreeView.bf. The internal ListView is a VISUAL child (VisualChildCount()==1)
// held as a RefPtr member; the FlattenedTreeAdapter is owned via UniquePtr (a non-Object); TreeAdapter
// is BORROWED. Beef Selection/ItemHeight passthrough props -> methods.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:tree_view;

import draconic.foundation;
import draconic.vg;
import :view;
import :property;
import :event;
import :box_constraints;
import :draw_context;
import :drawable;
import :control_state;
import :style_property;
import :event_args;
import :input_enums;
import :enums;
import :list_view;
import :selection_model;
import :itree_adapter;
import :flattened_tree_adapter;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::ui
{
    class TreeView : public ViewGroup
    {
        DRACONIC_OBJECT(TreeView, ViewGroup)
    public:
        struct ItemClickInfo
        {
            i32 NodeId = 0;
            i32 ClickCount = 0;
        };

        ITreeAdapter* TreeAdapter = nullptr; // borrowed

        Property<f32> IndentWidth{20.0f};
        Property<f32> ArrowSize{8.0f};

        // The x-pixel where a row at `depth` should start its CONTENT (text/icons) so it clears the
        // expander-chevron column. The chevron is drawn in [depth*IndentWidth, (depth+1)*IndentWidth],
        // so content begins one indent level further, at (depth+1)*IndentWidth.
        //
        // Tree adapters MUST derive their row indent from this - via TextOffsetX on a label row, or a
        // left Padding/Margin on a container row - and NEVER hardcode a pixel constant. A literal that
        // drifts from IndentWidth is exactly how the chevron ends up overlapping the row text (it has
        // bitten the hierarchy, asset-picker, and particle trees). This is the single source of truth.
        [[nodiscard]] f32 ContentInset(i32 depth) const noexcept
        {
            return static_cast<f32>(depth + 1) * IndentWidth.Value();
        }

        Event<void(ItemClickInfo)> OnItemClick;
        Event<void(i32, f32, f32)> OnItemRightClick;
        Event<void(i32, KeyEventArgs&)> OnItemKeyDown;
        Event<void(i32)> OnItemToggled;

        TreeView()
        {
            ClipsContent = true;
            WantsArrowKeys = true;
            IndentWidth.SetOwner(this);
            ArrowSize.SetOwner(this);
            m_listView = MakeRef<ListView>(DefaultAllocator());
            m_listView->Parent = this;

            TreeView* self = this;
            m_listView->OnItemClicked.Add(Event<void(i32, i32, f32, f32)>::Handler{
                [self](i32 position, i32 clickCount, f32 localX, f32 /*localY*/)
                {
                    if (self->IsArrowHit(position, localX))
                    {
                        self->ToggleExpand(position);
                        return;
                    }
                    const i32 nodeId =
                        self->m_flatAdapter ? self->m_flatAdapter->GetNodeId(position) : position;
                    self->OnItemClick.Invoke(ItemClickInfo{nodeId, clickCount});
                }});
            m_listView->OnItemRightClicked.Add(Event<void(i32, f32, f32)>::Handler{
                [self](i32 position, f32 localX, f32 localY)
                {
                    const i32 nodeId =
                        self->m_flatAdapter ? self->m_flatAdapter->GetNodeId(position) : position;
                    self->OnItemRightClick.Invoke(nodeId, localX, localY);
                }});
        }

        // === Passthroughs ===
        [[nodiscard]] SelectionModel& Selection() noexcept { return m_listView->Selection; }
        [[nodiscard]] f32 ItemHeight() const noexcept { return m_listView->ItemHeight.Value(); }
        void SetItemHeight(f32 value) { m_listView->ItemHeight.SetValue(value); }
        [[nodiscard]] FlattenedTreeAdapter* FlatAdapter() const noexcept
        {
            return m_flatAdapter.Get();
        }
        [[nodiscard]] ListView* InternalListView() const noexcept { return m_listView.Get(); }

        /// Set the tree adapter and build the flat list. Safe to call repeatedly (rebuild);
        /// null DETACHES (owners call SetAdapter(nullptr) in their destructors so the view
        /// never outlives an adapter it doesn't own).
        void SetAdapter(ITreeAdapter* adapter)
        {
            TreeAdapter = adapter;
            // Detach the list from the old flat adapter BEFORE the UniquePtr reassignment
            // destroys it - ListView::SetAdapter calls SetObserver on its previous adapter,
            // which would be a use-after-free otherwise.
            m_listView->SetAdapter(nullptr);
            if (adapter == nullptr)
            {
                m_flatAdapter.Reset();
                return;
            }
            m_flatAdapter = MakeUnique<FlattenedTreeAdapter>(DefaultAllocator(), adapter);
            m_listView->SetAdapter(m_flatAdapter.Get());
        }

        /// Toggle expansion of the node at a flat position.
        void ToggleExpand(i32 flatPosition)
        {
            if (!m_flatAdapter)
            {
                return;
            }
            const i32 nodeId = m_flatAdapter->GetNodeId(flatPosition);
            if (nodeId >= 0)
            {
                m_flatAdapter->ToggleExpand(nodeId);
                m_listView->NotifyDataChanged();
                OnItemToggled.Invoke(nodeId);
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!m_flatAdapter)
            {
                return;
            }
            const i32 sel = m_listView->Selection.FirstSelected();
            if (sel < 0)
            {
                return;
            }
            const i32 nodeId = m_flatAdapter->GetNodeId(sel);
            if (nodeId < 0)
            {
                return;
            }

            OnItemKeyDown.Invoke(nodeId, e);
            if (e.Handled)
            {
                return;
            }

            switch (e.Key)
            {
            case KeyCode::Right:
                if (TreeAdapter->HasChildren(nodeId) && !m_flatAdapter->IsExpanded(nodeId))
                {
                    m_flatAdapter->ToggleExpand(nodeId);
                    m_listView->NotifyDataChanged();
                    OnItemToggled.Invoke(nodeId);
                    e.Handled = true;
                }
                break;
            case KeyCode::Left:
                if (TreeAdapter->HasChildren(nodeId) && m_flatAdapter->IsExpanded(nodeId))
                {
                    m_flatAdapter->ToggleExpand(nodeId);
                    m_listView->NotifyDataChanged();
                    OnItemToggled.Invoke(nodeId);
                    e.Handled = true;
                }
                break;
            default:
                break;
            }
        }

        // === Visual children: the internal ListView ===
        [[nodiscard]] usize VisualChildCount() const override { return 1; }
        [[nodiscard]] View* GetVisualChild(usize index) const override
        {
            return (index == 0) ? m_listView.Get() : nullptr;
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            DrawChildren(ctx);
            DrawTreeOverlay(ctx);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            m_listView->Measure(constraints);
            MeasuredSize = m_listView->MeasuredSize;
        }
        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            m_listView->Layout(0, 0, width, height);
        }

    private:
        [[nodiscard]] bool IsArrowHit(i32 position, f32 localX)
        {
            if (!m_flatAdapter || TreeAdapter == nullptr)
            {
                return false;
            }
            const i32 nodeId = m_flatAdapter->GetNodeId(position);
            if (nodeId < 0 || !TreeAdapter->HasChildren(nodeId))
            {
                return false;
            }
            const i32 depth = m_flatAdapter->GetDepth(position);
            const f32 arrowLeft = depth * IndentWidth.Value();
            const f32 arrowRight = arrowLeft + IndentWidth.Value();
            return localX >= arrowLeft && localX < arrowRight;
        }

        void DrawTreeOverlay(UIDrawContext& ctx)
        {
            if (!m_flatAdapter || TreeAdapter == nullptr)
            {
                return;
            }
            const f32 scrollY = m_listView->ScrollY();
            const f32 itemH = m_listView->ItemHeight.Value();
            if (itemH <= 0)
            {
                return;
            }
            const f32 viewportH = Height();

            const i32 firstVisible = static_cast<i32>(scrollY / itemH);
            const i32 lastVisible = Min(firstVisible + static_cast<i32>(viewportH / itemH) + 1,
                                        m_flatAdapter->ItemCount() - 1);

            for (i32 i = firstVisible; i <= lastVisible; ++i)
            {
                const i32 nodeId = m_flatAdapter->GetNodeId(i);
                if (nodeId < 0 || !TreeAdapter->HasChildren(nodeId))
                {
                    continue;
                }

                const i32 depth = m_flatAdapter->GetDepth(i);
                const f32 itemY = i * itemH - scrollY;
                const f32 arrowX =
                    depth * IndentWidth.Value() + (IndentWidth.Value() - ArrowSize.Value()) * 0.5f;
                const f32 arrowCY = itemY + itemH * 0.5f;
                const f32 halfSize = ArrowSize.Value() * 0.5f;
                const bool isExpanded = m_flatAdapter->IsExpanded(nodeId);

                ControlState chevronState = GetControlState();
                if (isExpanded)
                {
                    chevronState |= ControlState::Checked;
                }
                if (Drawable* chevron =
                        ResolvePartDrawable(u8"chevron", StyleProperty::Background, chevronState))
                {
                    chevron->Draw(ctx, Rectangle{arrowX, arrowCY - halfSize, ArrowSize.Value(),
                                                 ArrowSize.Value()});
                }
                else
                {
                    const Color arrowColor = ResolveStyleColor(
                        StyleProperty::TextDimColor,
                        Color{160.0f / 255.0f, 165.0f / 255.0f, 180.0f / 255.0f, 1.0f});
                    ctx.VG().BeginPath();
                    if (isExpanded)
                    {
                        ctx.VG().MoveTo(arrowX, arrowCY - halfSize * 0.6f);
                        ctx.VG().LineTo(arrowX + ArrowSize.Value(), arrowCY - halfSize * 0.6f);
                        ctx.VG().LineTo(arrowX + halfSize, arrowCY + halfSize * 0.6f);
                    }
                    else
                    {
                        ctx.VG().MoveTo(arrowX, arrowCY - halfSize * 0.8f);
                        ctx.VG().LineTo(arrowX + ArrowSize.Value() * 0.6f, arrowCY);
                        ctx.VG().LineTo(arrowX, arrowCY + halfSize * 0.8f);
                    }
                    ctx.VG().ClosePath();
                    ctx.VG().Fill(arrowColor);
                }
            }
        }

        RefPtr<ListView> m_listView;
        UniquePtr<FlattenedTreeAdapter> m_flatAdapter;
    };

    DRACONIC_DEFINE_OBJECT(TreeView, "draconic::ui")
}
