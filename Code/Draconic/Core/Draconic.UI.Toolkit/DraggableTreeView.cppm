// Draconic UI Toolkit - :draggable_tree_view partition
//
// A TreeView with drag-to-reorder support. Ported from Sedulous.UI.Toolkit/src/DraggableTreeView.bf.
// Three public types in one partition (mirroring the Beef file): IReorderableTreeAdapter (extends
// ITreeAdapter with CanMove/MoveItem), TreeDragData (DragData payload carrying the source flat position),
// and DraggableTreeView (wraps an owned TreeView as a visual child and implements IDragSource/IDropTarget).
//
// Ownership adaptation (Beef `TreeView mTreeView ~ delete _`, owned but NOT in the child list) -> hold
// the TreeView as a RefPtr member exposed via VisualChildCount()/GetVisualChild() (the ScrollView/TreeView
// visual-child pattern). UIContext::AttachView/DetachView recurses VisualChildCount, so the internal
// TreeView attaches/detaches automatically; Parent is wired in the ctor. The adapter is BORROWED (raw
// pattern-B pointer; the consumer owns it).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:draggable_tree_view;

import draconic.foundation;
import draconic.vg;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    // ============================================================================================
    // IReorderableTreeAdapter - tree adapter that supports drag-to-reorder.
    // ============================================================================================
    class IReorderableTreeAdapter : public ITreeAdapter
    {
    public:
        /// Whether the item at fromPosition can be moved to toPosition.
        [[nodiscard]] virtual bool CanMove(i32 fromPosition, i32 toPosition) = 0;

        /// Move an item from one flat position to another.
        virtual void MoveItem(i32 fromPosition, i32 toPosition) = 0;

        /// Whether the item at fromPosition may be dropped INTO the item at toPosition (the
        /// middle drop-zone of a row - e.g. reparenting in a hierarchy). Default: unsupported,
        /// preserving pure-reorder adapters unchanged.
        [[nodiscard]] virtual bool CanDropInto(i32 /*fromPosition*/, i32 /*toPosition*/)
        {
            return false;
        }

        /// Drop the item at fromPosition INTO the item at toPosition.
        virtual void DropInto(i32 /*fromPosition*/, i32 /*toPosition*/) {}
    };

    // ============================================================================================
    // TreeDragData - drag data for tree item reordering.
    // ============================================================================================
    class TreeDragData : public DragData
    {
        DRACONIC_OBJECT(TreeDragData, DragData)
    public:
        explicit TreeDragData(i32 sourcePosition)
            : DragData(u8"tree/reorder"), SourcePosition(sourcePosition)
        {
        }

        i32 SourcePosition = 0;
    };

    // ============================================================================================
    // DraggableTreeView - TreeView with drag-to-reorder support (IDragSource + IDropTarget).
    // ============================================================================================
    class DraggableTreeView : public ViewGroup, public IDragSource, public IDropTarget
    {
        DRACONIC_OBJECT(DraggableTreeView, ViewGroup)
    public:
        Event<void(DraggableTreeView*, i32, i32)> OnItemReordered;
        /// Fired when an item is dropped INTO another (the adapter's DropInto ran).
        Event<void(DraggableTreeView*, i32, i32)> OnItemDroppedInto;

        DraggableTreeView()
        {
            m_treeView = MakeRef<TreeView>(DefaultAllocator());
            m_treeView->Parent = this;
        }

        [[nodiscard]] bool DragEnabled() const { return m_dragEnabled; }
        void SetDragEnabled(bool value) { m_dragEnabled = value; }

        [[nodiscard]] TreeView* InternalTreeView() { return m_treeView.Get(); }
        [[nodiscard]] SelectionModel& Selection() { return m_treeView->Selection(); }

        [[nodiscard]] f32 ItemHeight() const { return m_treeView->ItemHeight(); }
        void SetItemHeight(f32 value) { m_treeView->SetItemHeight(value); }

        // Row content-inset for a given depth (single source of truth for indentation; forwards to
        // the internal TreeView - see TreeView::ContentInset). Adapters must use this, not a literal.
        [[nodiscard]] f32 ContentInset(i32 depth) { return m_treeView->ContentInset(depth); }

        void SetAdapter(IReorderableTreeAdapter* adapter)
        {
            m_adapter = adapter;
            m_treeView->SetAdapter(adapter);
        }

        // === Visual children ===

        [[nodiscard]] usize VisualChildCount() const override { return 1; }
        [[nodiscard]] View* GetVisualChild(usize index) const override
        {
            return (index == 0) ? m_treeView.Get() : nullptr;
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            DrawChildren(ctx);

            // Drop indicator (scroll-corrected, matching ResolveDrop): a line at the insert
            // boundary (reorder), or a row highlight (drop-into).
            const f32 scrollY = m_treeView->InternalListView()->ScrollY();
            if (m_dropIndicatorPos >= 0)
            {
                const Color indicatorColor =
                    ResolveStyleColor(StyleProperty::AccentColor, Rgb(80, 160, 255, 255));
                const f32 y = m_dropIndicatorPos * m_treeView->ItemHeight() - scrollY;
                ctx.VG().FillRect(Rectangle{0, y, Width(), 2}, indicatorColor);
            }
            if (m_dropIntoPos >= 0)
            {
                const Color intoColor =
                    ResolveStyleColor(StyleProperty::AccentColor, Rgb(80, 160, 255, 255));
                const f32 y = m_dropIntoPos * m_treeView->ItemHeight() - scrollY;
                const Rectangle row{0, y, Width(), m_treeView->ItemHeight()};
                const Color fill{intoColor.r, intoColor.g, intoColor.b, 0.25f};
                const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius,
                                                 0.0f); // round in the rounded theme
                if (cr > 0.0f)
                {
                    ctx.VG().FillRoundedRect(row, cr, fill);
                    ctx.VG().StrokeRoundedRect(row, cr, intoColor, 1.0f);
                }
                else
                {
                    ctx.VG().FillRect(row, fill);
                    ctx.VG().StrokeRect(row, intoColor, 1.0f);
                }
            }
        }

        // === IDragSource ===

        [[nodiscard]] IDragSource* AsDragSource() override { return this; }

        [[nodiscard]] RefPtr<DragData> CreateDragData() override
        {
            if (!m_dragEnabled)
            {
                return RefPtr<DragData>{};
            }
            const i32 sel = m_treeView->Selection().FirstSelected();
            if (sel < 0)
            {
                return RefPtr<DragData>{};
            }
            return MakeRef<TreeDragData>(DefaultAllocator(), sel);
        }

        [[nodiscard]] RefPtr<View> CreateDragVisual(DragData* data) override
        {
            (void)data;
            RefPtr<Label> label = MakeRef<Label>(DefaultAllocator());
            label->SetText(u8"Moving item");
            return label;
        }

        void OnDragStarted(DragData* data) override { (void)data; }

        void OnDragCompleted(DragData* data, DragDropEffects effect, bool cancelled) override
        {
            (void)data;
            (void)effect;
            (void)cancelled;
            m_dropIndicatorPos = -1;
        }

        // === IDropTarget ===

        [[nodiscard]] IDropTarget* AsDropTarget() override { return this; }

        [[nodiscard]] DragDropEffects CanAcceptDrop(DragData* data, f32 localX, f32 localY) override
        {
            (void)localX;
            if (data->Format() != u8"tree/reorder")
            {
                return DragDropEffects::None;
            }
            if (auto* treeDrag = Cast<TreeDragData>(data))
            {
                return ResolveDrop(treeDrag->SourcePosition, localY).valid ? DragDropEffects::Move
                                                                           : DragDropEffects::None;
            }
            return DragDropEffects::None;
        }

        void OnDragEnter(DragData* data, f32 localX, f32 localY) override
        {
            (void)localX;
            UpdateDropIndicator(data, localY);
        }

        void OnDragOver(DragData* data, f32 localX, f32 localY) override
        {
            (void)localX;
            UpdateDropIndicator(data, localY);
        }

        void OnDragLeave(DragData* data) override
        {
            (void)data;
            m_dropIndicatorPos = -1;
            m_dropIntoPos = -1;
        }

        [[nodiscard]] DragDropEffects OnDrop(DragData* data, f32 localX, f32 localY) override
        {
            (void)localX;
            m_dropIndicatorPos = -1;
            m_dropIntoPos = -1;

            if (auto* treeDrag = Cast<TreeDragData>(data))
            {
                const DropResolution drop = ResolveDrop(treeDrag->SourcePosition, localY);
                if (drop.valid && drop.into)
                {
                    m_adapter->DropInto(treeDrag->SourcePosition, drop.position);
                    OnItemDroppedInto.Invoke(this, treeDrag->SourcePosition, drop.position);
                    return DragDropEffects::Move;
                }
                if (drop.valid)
                {
                    m_adapter->MoveItem(treeDrag->SourcePosition, drop.position);
                    OnItemReordered.Invoke(this, treeDrag->SourcePosition, drop.position);
                    return DragDropEffects::Move;
                }
            }
            return DragDropEffects::None;
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            m_treeView->Measure(constraints);
            MeasuredSize = m_treeView->MeasuredSize;
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            m_treeView->Layout(0, 0, width, height);
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        // Drop-zone resolution (scroll-corrected). Over a row, the middle band (25%..75%) is a
        // drop-INTO zone (when the adapter supports it - e.g. reparenting); the edge bands are
        // between-rows REORDER boundaries: top edge = insert before this row, bottom edge =
        // before the next (so position ranges 0..count - count = end of list, also the result
        // anywhere below the last row). Each zone falls back to the other's semantics when its
        // own is unsupported, so pure-reorder and pure-reparent adapters work anywhere.
        struct DropResolution
        {
            i32 position =
                -1; // into: the target row; reorder: the insert-before boundary (0..count)
            bool into = false;
            bool valid = false;
        };

        [[nodiscard]] DropResolution ResolveDrop(i32 fromPosition, f32 localY)
        {
            if (m_adapter == nullptr)
            {
                return {};
            }
            ListView* list = m_treeView->InternalListView();
            const i32 count =
                (m_treeView->FlatAdapter() != nullptr) ? m_treeView->FlatAdapter()->ItemCount() : 0;

            const f32 rows = (localY + list->ScrollY()) / m_treeView->ItemHeight();
            const i32 row = static_cast<i32>(rows);
            const f32 frac = rows - static_cast<f32>(row);

            if (row >= count) // below the last row: end-of-list boundary only
            {
                if (m_adapter->CanMove(fromPosition, count))
                {
                    return {count, false, true};
                }
                return {count, false, false};
            }

            const bool wantInto = frac >= 0.25f && frac <= 0.75f;
            const i32 boundary = (frac < 0.5f) ? row : row + 1;

            if (wantInto && m_adapter->CanDropInto(fromPosition, row))
            {
                return {row, true, true};
            }
            if (m_adapter->CanMove(fromPosition, boundary))
            {
                return {boundary, false, true};
            }
            if (!wantInto && m_adapter->CanDropInto(fromPosition, row))
            {
                return {row, true, true};
            }
            return {boundary, false, false};
        }

        void UpdateDropIndicator(DragData* data, f32 localY)
        {
            m_dropIndicatorPos = -1;
            m_dropIntoPos = -1;
            if (auto* treeDrag = Cast<TreeDragData>(data))
            {
                const DropResolution drop = ResolveDrop(treeDrag->SourcePosition, localY);
                if (!drop.valid)
                {
                    return;
                }
                if (drop.into)
                {
                    m_dropIntoPos = drop.position;
                }
                else
                {
                    m_dropIndicatorPos = drop.position;
                }
            }
        }

        RefPtr<TreeView> m_treeView; // owned; visual child (not in the logical child list)
        IReorderableTreeAdapter* m_adapter = nullptr; // borrowed (consumer owns)
        bool m_dragEnabled = true;
        i32 m_dropIndicatorPos = -1;
        i32 m_dropIntoPos = -1; // row highlighted as a drop-INTO target
    };

    DRACONIC_DEFINE_OBJECT(TreeDragData, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(DraggableTreeView, "draconic::ui::toolkit")
}
