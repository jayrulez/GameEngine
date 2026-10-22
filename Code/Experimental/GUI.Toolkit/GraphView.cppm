// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :graph_view partition
//
// The graph view (T6 of Specs/gui-lab-evolution-3.md): a graph source's nodes on a canvas that pans
// and zooms over a grid. A node is a widget (its header, title and a row per pin, which the sheet
// styles and right to left mirrors), made only while it is in view and reused as the view moves
// (Godot, traktor); a node never seen is made once to learn its size. Mouse first:
//   - a click selects a node (Shift or Ctrl adds or takes it away), a drag from nothing selects by
//     a box (Shift or Ctrl adding to what is selected);
//   - a drag moves the selected nodes (one gesture through the source, on the grid where it snaps;
//     Escape cancels it);
//   - a double click on a node is the application's (a subgraph opened);
//   - the wheel zooms about the pointer within limits, a middle drag pans.
// Read only, it selects and does not edit.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui.toolkit:graph_view;

import foundation.core;
import foundation.vg;
import experimental.gui;
import :graph_source;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    /// A pin's row in a node: its name, the pin drawn on the node's edge by the node.
    class GraphPinRow : public UIWidget
    {
        RTTI_OBJECT(GraphPinRow, UIWidget)
    public:
        GraphPinRow()
        {
            SetTag(core::StringView(u8"graphpin"));
            {
                StyleWriteScope defaults(*this);
                EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Direction = FlexDirection::Row;
                    s.AlignItems = Align::Center;
                    s.MinHeight = LayoutLength::Px(20.0f);
                });
                SetPadding(Thickness{12.0f, 0.0f, 12.0f, 0.0f});
            }
            m_name = core::MakeRef<Label>(MemoryAllocator());
            m_name->SetHitTestVisible(false);
            AddChild(m_name.Get());
        }

        void Show(const GraphPinInfo& pin)
        {
            m_pin = pin.Id;
            m_direction = pin.Direction;
            m_color = pin.TypeColor;
            m_name->SetText(pin.Name.AsView());
            if (pin.Direction == GraphPinDirection::Output)
            {
                RemoveClass(core::StringView(u8"input"));
                AddClass(core::StringView(u8"output"));
            }
            else
            {
                RemoveClass(core::StringView(u8"output"));
                AddClass(core::StringView(u8"input"));
            }
        }
        [[nodiscard]] GraphId GetPinId() const noexcept { return m_pin; }
        [[nodiscard]] GraphPinDirection GetDirection() const noexcept { return m_direction; }
        [[nodiscard]] Color GetTypeColor() const noexcept { return m_color; }
        [[nodiscard]] Label& GetNameLabel() noexcept { return *m_name; }

    private:
        core::RefPtr<Label> m_name;
        GraphId m_pin = 0;
        GraphPinDirection m_direction = GraphPinDirection::Input;
        Color m_color{};
    };

    /// A node's header: its title and subtitle over the source's header colour.
    class GraphNodeHeader : public UIWidget
    {
        RTTI_OBJECT(GraphNodeHeader, UIWidget)
    public:
        GraphNodeHeader()
        {
            SetTag(core::StringView(u8"graphnodeheader"));
            StyleWriteScope defaults(*this);
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Column;
                s.MinHeight = LayoutLength::Px(22.0f);
                s.JustifyContent = Justify::Center;
            });
            SetPadding(Thickness{8.0f, 3.0f, 8.0f, 3.0f});
        }
        void SetHeaderColor(Color color)
        {
            if (color.r == m_color.r && color.g == m_color.g && color.b == m_color.b && color.a == m_color.a)
                return;
            m_color = color;
            Invalidate();
        }
        [[nodiscard]] Color GetHeaderColor() const noexcept { return m_color; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            // Rounded as the node's top corners are, inside its border.
            vg::CornerRadii radii{};
            if (const Node* node = GetParent())
            {
                const NodeBox& box = node->GetBox();
                radii.topLeft = core::Max(0.0f, box.Radii.topLeft - core::Max(box.BorderWidths.top, box.BorderWidths.left));
                radii.topRight = core::Max(0.0f, box.Radii.topRight - core::Max(box.BorderWidths.top, box.BorderWidths.right));
            }
            ctx.VG().FillRoundedRect(Rectangle{0.0f, 0.0f, GetSize().x, GetSize().y}, radii, m_color);
            UIWidget::OnDraw(ctx, localBounds);
        }

    private:
        Color m_color{0.27f, 0.47f, 0.73f, 1.0f};
    };

    /// A node of a graph view: its header and a row per pin, inputs at its inline start and outputs at
    /// its inline end, each pin drawn on the node's edge. A view fills it from the source.
    class GraphNodeWidget : public UIWidget
    {
        RTTI_OBJECT(GraphNodeWidget, UIWidget)
    public:
        static constexpr f32 kPinRadius = 4.5f;

        GraphNodeWidget()
        {
            SetTag(core::StringView(u8"graphnode"));
            {
                StyleWriteScope defaults(*this);
                EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Direction = FlexDirection::Column;
                    s.MinWidth = LayoutLength::Px(140.0f);
                });
            }
            m_header = core::MakeRef<GraphNodeHeader>(MemoryAllocator());
            AddChild(m_header.Get());
            m_title = core::MakeRef<Label>(MemoryAllocator());
            m_title->AddClass(core::StringView(u8"title"));
            m_title->SetHitTestVisible(false);
            m_header->AddChild(m_title.Get());
            m_subtitle = core::MakeRef<Label>(MemoryAllocator());
            m_subtitle->AddClass(core::StringView(u8"subtitle"));
            m_subtitle->SetHitTestVisible(false);
            m_subtitle->SetDisplayed(false);
            m_header->AddChild(m_subtitle.Get());

            m_body = MakePart(*this, u8"graphnodebody");
            {
                StyleWriteScope defaults(*m_body);
                m_body->EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Direction = FlexDirection::Row;
                    s.JustifyContent = Justify::SpaceBetween;
                    s.ColumnGap = 8.0f;
                });
                m_body->SetPadding(Thickness{0.0f, 4.0f, 0.0f, 4.0f});
            }
            m_inputs = MakePart(*m_body, u8"graphpins");
            m_inputs->AddClass(core::StringView(u8"inputs"));
            m_outputs = MakePart(*m_body, u8"graphpins");
            m_outputs->AddClass(core::StringView(u8"outputs"));
            {
                StyleWriteScope defaults(*m_inputs);
                m_inputs->EditLayoutStyle([](LayoutStyle& s) { s.Direction = FlexDirection::Column; });
            }
            {
                StyleWriteScope defaults(*m_outputs);
                m_outputs->EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Direction = FlexDirection::Column;
                    s.AlignItems = Align::End;
                });
            }
        }

        /// Shows `node` (what changed since it last showed one).
        void Show(const GraphNodeInfo& node)
        {
            m_node = node.Id;
            m_title->SetText(node.Title.AsView());
            m_subtitle->SetText(node.Subtitle.AsView());
            m_subtitle->SetDisplayed(!node.Subtitle.IsEmpty());
            m_header->SetHeaderColor(node.HeaderColor);
            usize inputs = 0, outputs = 0;
            for (const GraphPinInfo& pin : node.Pins)
            {
                const bool out = pin.Direction == GraphPinDirection::Output;
                RowAt(out ? m_outputRows : m_inputRows, out ? *m_outputs : *m_inputs, out ? outputs++ : inputs++).Show(pin);
            }
            Trim(m_inputRows, *m_inputs, inputs);
            Trim(m_outputRows, *m_outputs, outputs);
        }
        [[nodiscard]] GraphId GetNodeId() const noexcept { return m_node; }

        /// Shown selected (the sheet's `graphnode.selected`).
        void SetSelected(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            if (selected)
                AddClass(core::StringView(u8"selected"));
            else
                RemoveClass(core::StringView(u8"selected"));
        }
        [[nodiscard]] bool IsSelected() const noexcept { return m_selected; }

        /// Where pin `pin` is drawn, in the node's own space (on its inline start edge for an input, its
        /// inline end for an output, across from its row's middle); false when it has no such pin.
        [[nodiscard]] bool PinPoint(GraphId pin, core::Float2& out) const
        {
            const bool rtl = IsRightToLeft();
            const auto find = [&](const core::Array<core::RefPtr<GraphPinRow>>& rows, bool output)
            {
                for (const core::RefPtr<GraphPinRow>& row : rows)
                {
                    if (row->GetPinId() != pin)
                        continue;
                    const core::Float2 at = OffsetIn(*row);
                    const bool right = output != rtl;
                    out = core::Float2{right ? GetSize().x : 0.0f, at.y + row->GetSize().y * 0.5f};
                    return true;
                }
                return false;
            };
            return find(m_inputRows, false) || find(m_outputRows, true);
        }
        /// The pin rows, inputs then outputs.
        [[nodiscard]] usize PinRowCount() const noexcept { return m_inputRows.Size() + m_outputRows.Size(); }
        [[nodiscard]] GraphPinRow& PinRowAt(usize index) const noexcept
        {
            return index < m_inputRows.Size() ? *m_inputRows[index] : *m_outputRows[index - m_inputRows.Size()];
        }
        [[nodiscard]] Label& GetTitleLabel() noexcept { return *m_title; }
        [[nodiscard]] Label& GetSubtitleLabel() noexcept { return *m_subtitle; }
        [[nodiscard]] GraphNodeHeader& GetHeader() noexcept { return *m_header; }
        /// Under the pin rows: where an application puts widgets of its own.
        [[nodiscard]] UIWidget& GetBody() noexcept { return *m_body; }

    protected:
        void OnDrawAbove(DrawContext& ctx) override
        {
            for (usize i = 0; i < PinRowCount(); ++i)
            {
                const GraphPinRow& row = PinRowAt(i);
                core::Float2 at{};
                if (!PinPoint(row.GetPinId(), at))
                    continue;
                vg::PathBuilder ring;
                vg::ShapeBuilder::BuildCircle(at, kPinRadius + 1.0f, ring);
                ctx.VG().FillPath(ring.ToPath(), Color{0.0f, 0.0f, 0.0f, 0.55f});
                vg::PathBuilder dot;
                vg::ShapeBuilder::BuildCircle(at, kPinRadius, dot);
                ctx.VG().FillPath(dot.ToPath(), row.GetTypeColor());
            }
        }

    private:
        static core::RefPtr<UIWidget> MakePart(UIWidget& parent, const char8_t* tag)
        {
            core::RefPtr<UIWidget> part = core::MakeRef<UIWidget>(parent.MemoryAllocator());
            part->SetTag(core::StringView(tag));
            parent.AddChild(part.Get());
            return part;
        }
        GraphPinRow& RowAt(core::Array<core::RefPtr<GraphPinRow>>& rows, UIWidget& column, usize index)
        {
            while (rows.Size() <= index)
            {
                rows.PushBack(core::MakeRef<GraphPinRow>(MemoryAllocator()));
                column.AddChild(rows.Back().Get());
            }
            return *rows[index];
        }
        static void Trim(core::Array<core::RefPtr<GraphPinRow>>& rows, UIWidget& column, usize count)
        {
            while (rows.Size() > count)
            {
                column.RemoveChild(rows.Back().Get());
                rows.RemoveAt(rows.Size() - 1);
            }
        }
        // `inner`'s top-left in this node's space (nothing between them is transformed).
        [[nodiscard]] core::Float2 OffsetIn(const Node& inner) const
        {
            core::Float2 at{};
            for (const Node* n = &inner; n != nullptr && n != this; n = n->GetParent())
                at = core::Float2{at.x + n->GetPosition().x, at.y + n->GetPosition().y};
            return at;
        }

        core::RefPtr<GraphNodeHeader> m_header;
        core::RefPtr<Label> m_title;
        core::RefPtr<Label> m_subtitle;
        core::RefPtr<UIWidget> m_body;
        core::RefPtr<UIWidget> m_inputs;
        core::RefPtr<UIWidget> m_outputs;
        core::Array<core::RefPtr<GraphPinRow>> m_inputRows;
        core::Array<core::RefPtr<GraphPinRow>> m_outputRows;
        GraphId m_node = 0;
        bool m_selected = false;
    };

    class GraphView : public UIWidget, public IGraphSourceClient
    {
        RTTI_OBJECT(GraphView, UIWidget)
    public:
        static constexpr f32 kDragThreshold = 3.0f;
        /// How far past its edges nodes are made before they come into view.
        static constexpr f32 kViewMargin = 64.0f;
        /// The size a node is taken to have before it was measured.
        [[nodiscard]] static core::Float2 EstimatedNodeSize() noexcept { return core::Float2{160.0f, 80.0f}; }

        GraphView()
        {
            SetTag(core::StringView(u8"graphview"));
            SetTabFocusable(true);
            SetClipChildren(true);
        }
        ~GraphView() override
        {
            if (m_source)
                m_source->RemoveGraphClient(this);
        }

        // === What it shows ===
        /// The graph it shows and edits (not owned: the source outlives the view or is set to null first).
        void SetSource(IGraphSource* source)
        {
            if (m_source)
                m_source->RemoveGraphClient(this);
            m_source = source;
            if (m_source)
                m_source->AddGraphClient(this);
            m_selection.Clear();
            m_sizes.Clear();
            RequestLayout();
        }
        [[nodiscard]] IGraphSource* GetSource() const noexcept { return m_source; }
        /// Read only: it selects, and moves and edits nothing.
        void SetReadOnly(bool readOnly) noexcept { m_readOnly = readOnly; }
        [[nodiscard]] bool IsReadOnly() const noexcept { return m_readOnly; }

        // === The canvas ===
        /// The graph's point at its content box's top-left.
        void SetPan(core::Float2 pan)
        {
            m_pan = pan;
            RequestLayout();
        }
        [[nodiscard]] core::Float2 GetPan() const noexcept { return m_pan; }
        /// The zoom (within its limits), the graph's point at `about` (its own space) kept in place.
        void SetZoom(f32 zoom, core::Float2 about)
        {
            const core::Float2 fixed = ViewToGraph(about);
            m_zoom = core::Clamp(zoom, m_zoomMin, m_zoomMax);
            const Rect box = GetContentBounds();
            m_pan = core::Float2{fixed.x - (about.x - box.x) / m_zoom, fixed.y - (about.y - box.y) / m_zoom};
            RequestLayout();
        }
        void SetZoom(f32 zoom) { SetZoom(zoom, core::Float2{GetContentBounds().x, GetContentBounds().y}); }
        [[nodiscard]] f32 GetZoom() const noexcept { return m_zoom; }
        void SetZoomLimits(f32 least, f32 most)
        {
            m_zoomMin = core::Max(0.01f, least);
            m_zoomMax = core::Max(m_zoomMin, most);
            SetZoom(m_zoom);
        }
        [[nodiscard]] f32 ZoomMin() const noexcept { return m_zoomMin; }
        [[nodiscard]] f32 ZoomMax() const noexcept { return m_zoomMax; }
        /// The grid's cell in the graph's units, what a move snaps to when it snaps.
        void SetGridSize(f32 size)
        {
            m_gridSize = core::Max(1.0f, size);
            Invalidate();
        }
        [[nodiscard]] f32 GetGridSize() const noexcept { return m_gridSize; }
        void SetSnapToGrid(bool snap) noexcept { m_snap = snap; }
        [[nodiscard]] bool GetSnapToGrid() const noexcept { return m_snap; }

        [[nodiscard]] core::Float2 GraphToView(core::Float2 graph) const
        {
            const Rect box = GetContentBounds();
            return core::Float2{box.x + (graph.x - m_pan.x) * m_zoom, box.y + (graph.y - m_pan.y) * m_zoom};
        }
        [[nodiscard]] core::Float2 ViewToGraph(core::Float2 local) const
        {
            const Rect box = GetContentBounds();
            return core::Float2{m_pan.x + (local.x - box.x) / m_zoom, m_pan.y + (local.y - box.y) / m_zoom};
        }

        /// Fits every node in view (at most at the zoom of 1).
        void FrameAll() { Frame(false); }
        /// Fits the selected nodes in view; every node with none selected.
        void FrameSelection() { Frame(!m_selection.IsEmpty()); }

        // === Nodes ===
        /// A node's size as last measured (an estimate before it was).
        [[nodiscard]] core::Float2 NodeSize(GraphId node) const
        {
            const core::Float2* size = m_sizes.Find(node);
            return size != nullptr ? *size : EstimatedNodeSize();
        }
        /// Where a node is, in its own space; empty when there is no such node.
        [[nodiscard]] Rect NodeRect(GraphId node) const
        {
            const GraphNodeInfo* info = m_source ? m_source->GetNode(node) : nullptr;
            if (info == nullptr)
                return Rect{};
            const core::Float2 at = GraphToView(info->Position);
            const core::Float2 size = NodeSize(node);
            return Rect{at.x, at.y, size.x * m_zoom, size.y * m_zoom};
        }
        /// The topmost node at `local` (its own space); 0 when none.
        [[nodiscard]] GraphId HitNode(core::Float2 local) const
        {
            if (!m_source)
                return 0;
            core::Array<GraphId> order;
            PaintOrder(order);
            for (usize i = order.Size(); i-- > 0;)
                if (NodeRect(order[i]).Contains(local))
                    return order[i];
            return 0;
        }
        /// A node's widget while it is made (in view); null when it is not.
        [[nodiscard]] GraphNodeWidget* GetNodeWidget(GraphId node) const
        {
            const core::RefPtr<GraphNodeWidget>* widget = m_live.Find(node);
            return widget != nullptr ? widget->Get() : nullptr;
        }
        /// How many node widgets are made now (those in view), and how many wait to be reused.
        [[nodiscard]] usize MadeNodeCount() const noexcept { return m_live.Size(); }
        [[nodiscard]] usize PooledNodeCount() const noexcept { return m_pool.Size(); }

        // === The selection ===
        [[nodiscard]] const core::Array<GraphId>& GetSelectedNodes() const noexcept { return m_selection; }
        void SetSelectedNodes(core::Span<const GraphId> nodes)
        {
            core::Array<GraphId> next;
            for (const GraphId node : nodes)
                if (m_source && m_source->GetNode(node) != nullptr && !Holds(next, node))
                    next.PushBack(node);
            Reselect(core::Move(next));
        }
        [[nodiscard]] bool IsNodeSelected(GraphId node) const noexcept { return Holds(m_selection, node); }
        /// Told each time the selection changes.
        void SetOnSelectionChanged(core::Function<void()> callback) { m_onSelectionChanged = core::Move(callback); }
        /// A double click on a node: the application's (a subgraph opened).
        void SetOnNodeDoubleClick(core::Function<void(GraphId)> callback) { m_onNodeDoubleClick = core::Move(callback); }

        void OnGraphChanged() override
        {
            if (!m_source)
                return;
            core::Array<GraphId> kept; // an undo may take nodes away
            for (const GraphId node : m_selection)
                if (m_source->GetNode(node) != nullptr)
                    kept.PushBack(node);
            if (kept.Size() != m_selection.Size())
                Reselect(core::Move(kept));
            RequestLayout();
            Invalidate();
        }

    protected:
        void OnLayout() override;
        void OnMouseDown(const MouseEvent& event) override;
        void OnMouseMove(const MouseEvent& event) override;
        void OnMouseUp(const MouseEvent& event) override;
        void OnPointerCancel(const MouseEvent& event) override;
        void OnPointerCaptureLost() override { EndDrag(false); }
        void OnMouseWheel(const WheelEvent& event) override;
        void OnKeyDown(const KeyEvent& event) override;
        [[nodiscard]] Cursor GetCursorNow() const override { return m_drag == Drag::Pan ? Cursor::Grabbing : UIWidget::GetCursorNow(); }
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override;
        void OnDrawAbove(DrawContext& ctx) override;

    private:
        enum class Drag : u8
        {
            None,
            Pending, // pressed on a node, not moved past the threshold
            Nodes,
            Box,
            Pan,
        };

        [[nodiscard]] static bool Holds(const core::Array<GraphId>& ids, GraphId id) noexcept
        {
            for (const GraphId at : ids)
                if (at == id)
                    return true;
            return false;
        }
        void Reselect(core::Array<GraphId> next)
        {
            bool same = next.Size() == m_selection.Size();
            for (usize i = 0; same && i < next.Size(); ++i)
                same = Holds(m_selection, next[i]);
            m_selection = core::Move(next);
            if (same)
                return;
            RequestLayout(); // selected nodes stand on top
            Invalidate();
            if (m_onSelectionChanged)
                m_onSelectionChanged();
        }
        void Toggle(GraphId node)
        {
            core::Array<GraphId> next;
            for (const GraphId at : m_selection)
                if (at != node)
                    next.PushBack(at);
            if (next.Size() == m_selection.Size())
                next.PushBack(node);
            Reselect(core::Move(next));
        }
        void SelectOnly(GraphId node)
        {
            core::Array<GraphId> next;
            if (node != 0)
                next.PushBack(node);
            Reselect(core::Move(next));
        }
        // The source's nodes in the order they are drawn: the selected last, over the rest.
        void PaintOrder(core::Array<GraphId>& out) const
        {
            out.Clear();
            core::Array<GraphId> ids;
            m_source->GetNodes(ids);
            for (const GraphId id : ids)
                if (!Holds(m_selection, id))
                    out.PushBack(id);
            for (const GraphId id : ids)
                if (Holds(m_selection, id))
                    out.PushBack(id);
        }
        void CapturePointer()
        {
            if (EventDispatcher* dispatcher = GetEventDispatcher())
                dispatcher->CapturePointer(*this);
        }
        void ReleasePointer()
        {
            if (EventDispatcher* dispatcher = GetEventDispatcher())
                dispatcher->ReleasePointer(*this);
        }
        void BeginMove();
        void MoveTo(core::Float2 local);
        void EndDrag(bool commit);
        void Frame(bool selectionOnly);
        [[nodiscard]] core::RefPtr<GraphNodeWidget> TakeWidget();

        IGraphSource* m_source = nullptr;
        bool m_readOnly = false;
        core::Float2 m_pan{};
        f32 m_zoom = 1.0f;
        f32 m_zoomMin = 0.2f;
        f32 m_zoomMax = 2.0f;
        f32 m_gridSize = 16.0f;
        bool m_snap = true;
        core::Array<GraphId> m_selection;
        core::Function<void()> m_onSelectionChanged;
        core::Function<void(GraphId)> m_onNodeDoubleClick;

        core::HashMap<GraphId, core::RefPtr<GraphNodeWidget>> m_live; // the nodes made, by id
        core::Array<core::RefPtr<GraphNodeWidget>> m_pool;            // made, waiting to be reused
        core::HashMap<GraphId, core::Float2> m_sizes;                  // each node's size as last measured

        Drag m_drag = Drag::None;
        core::Float2 m_pressAt{};   // where the gesture began, its own space
        core::Float2 m_boxTo{};
        GraphId m_pressed = 0;      // the node pressed
        core::Float2 m_pressedFrom{}; // its place in the graph when the move began
        bool m_additive = false;    // Shift or Ctrl held at the press
        core::Array<GraphId> m_boxBase; // what a box adds to
        core::Float2 m_panFrom{};
    };

    // === Making the nodes in view ===

    inline core::RefPtr<GraphNodeWidget> GraphView::TakeWidget()
    {
        if (!m_pool.IsEmpty())
        {
            core::RefPtr<GraphNodeWidget> widget = m_pool.Back();
            m_pool.RemoveAt(m_pool.Size() - 1);
            return widget;
        }
        return core::MakeRef<GraphNodeWidget>(MemoryAllocator());
    }

    inline void GraphView::OnLayout()
    {
        core::HashMap<GraphId, core::RefPtr<GraphNodeWidget>> live;
        core::HashMap<GraphId, core::Float2> sizes;
        core::Array<GraphId> order;
        core::Array<GraphNodeWidget*> unseen; // made only to be measured
        if (m_source)
            PaintOrder(order);
        const Rect box = GetContentBounds();
        const Rect reach{box.x - kViewMargin, box.y - kViewMargin, box.width + 2.0f * kViewMargin, box.height + 2.0f * kViewMargin};
        usize index = 0;
        for (const GraphId id : order)
        {
            const GraphNodeInfo* info = m_source->GetNode(id);
            const core::Float2* known = m_sizes.Find(id);
            const core::Float2 at = GraphToView(info->Position);
            const core::Float2 guess = known != nullptr ? *known : EstimatedNodeSize();
            const bool inView = reach.Intersects(Rect{at.x, at.y, guess.x * m_zoom, guess.y * m_zoom});
            if (!inView && known != nullptr)
            {
                sizes.InsertOrAssign(id, *known);
                continue;
            }
            core::RefPtr<GraphNodeWidget> widget;
            if (core::RefPtr<GraphNodeWidget>* had = m_live.Find(id))
            {
                widget = *had;
                (void)m_live.Remove(id);
            }
            else
            {
                widget = TakeWidget();
                if (widget->GetParent() != this)
                    AddChild(widget.Get());
            }
            widget->Show(*info);
            widget->SetSelected(Holds(m_selection, id));
            const core::Float2 size = widget->Measure(LayoutConstraints{});
            sizes.InsertOrAssign(id, size);
            widget->Arrange(Rect{at.x, at.y, size.x, size.y});
            if (widget->GetScale().x != m_zoom || widget->GetScale().y != m_zoom)
                widget->SetScale(core::Float2{m_zoom, m_zoom});
            if (index < ChildCount() && GetChildAt(index) != widget.Get())
                SetChildIndex(widget.Get(), index); // drawn in the paint order
            ++index;
            if (!reach.Intersects(Rect{at.x, at.y, size.x * m_zoom, size.y * m_zoom}))
                unseen.PushBack(widget.Get());
            live.InsertOrAssign(id, core::Move(widget));
        }
        // Those that left the view (or the graph) go back to the pool, as do those made only to be measured.
        for (auto& entry : m_live)
        {
            RemoveChild(entry.value.Get());
            m_pool.PushBack(core::Move(entry.value));
        }
        for (GraphNodeWidget* widget : unseen)
            if (core::RefPtr<GraphNodeWidget>* held = live.Find(widget->GetNodeId()))
            {
                core::RefPtr<GraphNodeWidget> keep = *held;
                (void)live.Remove(widget->GetNodeId());
                RemoveChild(keep.Get());
                m_pool.PushBack(core::Move(keep));
            }
        m_live = core::Move(live);
        m_sizes = core::Move(sizes);
    }

    // === Gestures ===

    inline void GraphView::OnMouseDown(const MouseEvent& event)
    {
        if (!m_source || m_drag != Drag::None)
            return;
        const core::Float2 local = ConvertToNodeSpace(event.Position); // events come in screen space
        if (event.Button == MouseButton::Middle)
        {
            event.Handled = true;
            m_drag = Drag::Pan;
            m_pressAt = local;
            m_panFrom = m_pan;
            CapturePointer();
            return;
        }
        if (event.Button != MouseButton::Left)
            return;
        event.Handled = true;
        RequestFocus();
        m_pressAt = local;
        m_boxTo = local;
        m_additive = (event.Modifiers & (KeyModShift | KeyModCtrl)) != 0;
        const GraphId hit = HitNode(local);
        if (hit != 0 && event.ClickCount == 2 && !m_additive)
        {
            SelectOnly(hit);
            if (m_onNodeDoubleClick)
                m_onNodeDoubleClick(hit);
            return;
        }
        if (hit != 0)
        {
            m_pressed = hit;
            if (m_additive)
                Toggle(hit);
            else if (!IsNodeSelected(hit))
                SelectOnly(hit);
            m_drag = IsNodeSelected(hit) ? Drag::Pending : Drag::None;
        }
        else
        {
            if (!m_additive)
                SelectOnly(0);
            m_boxBase = m_selection;
            m_drag = Drag::Box;
        }
        if (m_drag != Drag::None)
            CapturePointer();
    }
    inline void GraphView::OnMouseMove(const MouseEvent& event)
    {
        if (m_drag == Drag::None || !m_source)
            return;
        const core::Float2 local = ConvertToNodeSpace(event.Position);
        switch (m_drag)
        {
        case Drag::Pending:
            if (core::Abs(local.x - m_pressAt.x) < kDragThreshold && core::Abs(local.y - m_pressAt.y) < kDragThreshold)
                return;
            if (m_readOnly)
                return;
            m_drag = Drag::Nodes;
            BeginMove();
            MoveTo(local);
            break;
        case Drag::Nodes:
            MoveTo(local);
            break;
        case Drag::Box:
        {
            m_boxTo = local;
            const Rect area{core::Min(m_pressAt.x, local.x), core::Min(m_pressAt.y, local.y), core::Abs(local.x - m_pressAt.x),
                            core::Abs(local.y - m_pressAt.y)};
            core::Array<GraphId> next = m_boxBase;
            core::Array<GraphId> ids;
            m_source->GetNodes(ids);
            for (const GraphId id : ids)
                if (!Holds(next, id) && area.Intersects(NodeRect(id)))
                    next.PushBack(id);
            Reselect(core::Move(next));
            Invalidate();
            break;
        }
        case Drag::Pan:
            SetPan(core::Float2{m_panFrom.x - (local.x - m_pressAt.x) / m_zoom, m_panFrom.y - (local.y - m_pressAt.y) / m_zoom});
            break;
        default:
            break;
        }
    }
    inline void GraphView::OnMouseUp(const MouseEvent& event)
    {
        if (m_drag == Drag::Pan)
        {
            if (event.Button == MouseButton::Middle)
            {
                m_drag = Drag::None;
                ReleasePointer();
            }
            return;
        }
        if (event.Button != MouseButton::Left)
            return;
        if (m_drag == Drag::Pending && !m_additive)
            SelectOnly(m_pressed); // a click on a node of several selected selects it alone
        EndDrag(true);
    }
    inline void GraphView::OnPointerCancel(const MouseEvent&) { EndDrag(false); }
    inline void GraphView::OnKeyDown(const KeyEvent& event)
    {
        const KeyCode key = static_cast<KeyCode>(event.KeyCode);
        if (key == KeyCode::Escape && m_drag == Drag::Nodes)
        {
            event.Handled = true;
            EndDrag(false);
        }
    }
    inline void GraphView::OnMouseWheel(const WheelEvent& event)
    {
        const f32 steps = event.Delta.y != 0.0f ? event.Delta.y : event.Delta.x;
        if (steps == 0.0f)
            return;
        event.Handled = true;
        SetZoom(m_zoom * core::Pow(1.15f, steps), ConvertToNodeSpace(event.Position));
    }

    inline void GraphView::BeginMove()
    {
        const GraphNodeInfo* pressed = m_source->GetNode(m_pressed);
        m_pressedFrom = pressed != nullptr ? pressed->Position : core::Float2{};
        m_source->BeginMove();
    }
    inline void GraphView::MoveTo(core::Float2 local)
    {
        core::Float2 delta{(local.x - m_pressAt.x) / m_zoom, (local.y - m_pressAt.y) / m_zoom};
        if (m_snap) // the pressed node lands on the grid; the rest keep their places to it
        {
            const f32 x = core::Round((m_pressedFrom.x + delta.x) / m_gridSize) * m_gridSize;
            const f32 y = core::Round((m_pressedFrom.y + delta.y) / m_gridSize) * m_gridSize;
            delta = core::Float2{x - m_pressedFrom.x, y - m_pressedFrom.y};
        }
        m_source->PreviewMove(m_selection.AsSpan(), core::Span<const GraphId>{}, delta);
    }
    inline void GraphView::EndDrag(bool commit)
    {
        const Drag drag = m_drag;
        m_drag = Drag::None;
        if (drag == Drag::None)
            return;
        ReleasePointer();
        if (drag == Drag::Box)
            Invalidate();
        if (drag != Drag::Nodes || !m_source)
            return;
        if (commit)
            m_source->CommitMove();
        else
            m_source->CancelMove();
    }

    inline void GraphView::Frame(bool selectionOnly)
    {
        if (!m_source)
            return;
        core::Array<GraphId> ids;
        if (selectionOnly)
            ids = m_selection;
        else
            m_source->GetNodes(ids);
        bool any = false;
        f32 left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
        for (const GraphId id : ids)
        {
            const GraphNodeInfo* info = m_source->GetNode(id);
            if (info == nullptr)
                continue;
            const core::Float2 size = NodeSize(id);
            const f32 l = info->Position.x, t = info->Position.y, r = l + size.x, b = t + size.y;
            left = any ? core::Min(left, l) : l;
            top = any ? core::Min(top, t) : t;
            right = any ? core::Max(right, r) : r;
            bottom = any ? core::Max(bottom, b) : b;
            any = true;
        }
        if (!any)
            return;
        const Rect box = GetContentBounds();
        const f32 margin = 32.0f;
        const f32 width = right - left + 2.0f * margin, height = bottom - top + 2.0f * margin;
        f32 zoom = 1.0f;
        if (box.width > 0.0f && box.height > 0.0f)
            zoom = core::Min(1.0f, core::Min(box.width / width, box.height / height));
        m_zoom = core::Clamp(zoom, m_zoomMin, m_zoomMax);
        const core::Float2 middle{(left + right) * 0.5f, (top + bottom) * 0.5f};
        SetPan(core::Float2{middle.x - box.width * 0.5f / m_zoom, middle.y - box.height * 0.5f / m_zoom});
    }

    // === Drawing ===

    inline void GraphView::OnDraw(DrawContext& ctx, const Rect& localBounds)
    {
        UIWidget::OnDraw(ctx, localBounds);
        const Rect box = GetContentBounds();
        ctx.PushClip(box);
        const auto partColor = [this](const char8_t* name, Color fallback)
        {
            const PartStyle& part = GetPartStyle(name);
            Color color = part.HasBackground ? part.Box.BackgroundColor : fallback;
            color.a *= part.Opacity;
            return color;
        };
        // The grid: a line each cell while they stand apart, and a stronger one each eighth.
        const Color minor = partColor(u8"grid", Color{1.0f, 1.0f, 1.0f, 0.04f});
        const Color major = partColor(u8"grid-major", Color{1.0f, 1.0f, 1.0f, 0.09f});
        const f32 cell = m_gridSize * m_zoom;
        const bool fine = cell >= 6.0f;
        const f32 step = fine ? m_gridSize : m_gridSize * 8.0f;
        const core::Float2 first = ViewToGraph(core::Float2{box.x, box.y});
        const core::Float2 last = ViewToGraph(core::Float2{box.Right(), box.Bottom()});
        for (i64 i = static_cast<i64>(core::Floor(first.x / step)); static_cast<f32>(i) * step <= last.x; ++i)
        {
            const bool strong = !fine || i % 8 == 0;
            const f32 x = GraphToView(core::Float2{static_cast<f32>(i) * step, 0.0f}).x;
            ctx.VG().FillRect(Rectangle{core::Floor(x), box.y, 1.0f, box.height}, strong ? major : minor);
        }
        for (i64 i = static_cast<i64>(core::Floor(first.y / step)); static_cast<f32>(i) * step <= last.y; ++i)
        {
            const bool strong = !fine || i % 8 == 0;
            const f32 y = GraphToView(core::Float2{0.0f, static_cast<f32>(i) * step}).y;
            ctx.VG().FillRect(Rectangle{box.x, core::Floor(y), box.width, 1.0f}, strong ? major : minor);
        }
        ctx.PopClip();
    }
    inline void GraphView::OnDrawAbove(DrawContext& ctx)
    {
        if (m_drag != Drag::Box)
            return;
        const PartStyle& part = GetPartStyle(u8"selection-box");
        Color fill = part.HasBackground ? part.Box.BackgroundColor : Color{0.31f, 0.61f, 1.0f, 0.2f};
        fill.a *= part.Opacity;
        const Rect area{core::Min(m_pressAt.x, m_boxTo.x), core::Min(m_pressAt.y, m_boxTo.y), core::Abs(m_boxTo.x - m_pressAt.x),
                        core::Abs(m_boxTo.y - m_pressAt.y)};
        ctx.PushClip(GetContentBounds());
        ctx.VG().FillRect(area.ToRectangle(), fill);
        ctx.PopClip();
    }
}
