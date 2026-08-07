// Draconic UI Toolkit - :node_graph_canvas partition
//
// Model-agnostic interactive node graph canvas. Renders nodes with typed ports and bezier connections.
// Supports pan/zoom, selection, node dragging, drag-to-connect (with detach-and-reroute), box-select,
// context menus, hover, and keyboard delete / select-all. Follows the CurveCanvas pattern: the widget
// owns rendering and input; callers push node/connection data in and listen to events. Ported from
// Sedulous.UI.Toolkit/src/NodeGraph/NodeGraphCanvas.bf (a View).
//
// Port taxes: Beef `List<NodeGraphNode*>` (heap, stable pointers) -> Array<UniquePtr<NodeGraphNode>>;
// GetNode returns the raw pointer (m_nodes[i].Get()); AddNode takes UniquePtr by move; RemoveNode's
// `delete + RemoveAt` collapses to RemoveAt (UniquePtr frees). `List<NodeGraphConnection>` /
// `List<int32>` -> Array by value. `Vector2` -> Float2; `Vector2.Distance` -> foundation::Distance. Beef
// tuples -> private DragStart / PortHit structs. `delegate` validator -> Function<...>;
// `Event<delegate ...> ~ _.Dispose()` -> Event<...> fired via .Invoke(). `mDrawOrder.Remove(value)` ->
// RemoveValue find-and-RemoveAt. `%` on floats -> std::fmod; `float.MaxValue/MinValue` ->
// std::numeric_limits<f32>::max()/lowest(). KeyModifiers::Shift/Ctrl with free-function HasFlag;
// KeyCode::Delete/A. byte Color(...) -> private static Rgb().

module;
#include <cmath>
#include <limits>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:node_graph_canvas;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import draconic.fonts;
import :node_graph_types;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// How a canvas anchors + renders its connections.
    enum class ConnectionStyle : u8
    {
        /// Dataflow look: output-port to input-port horizontal-tangent beziers (the default).
        BezierPorts,
        /// State-machine look: node-center to node-center straight edges, clipped to the node
        /// rects, laterally offset so parallel/opposite edges stay distinct, with a mid-edge
        /// direction arrow. Nodes typically have no ports; links are made via StartLinkFrom.
        StraightNodeToNode
    };

    /// Model-agnostic interactive node graph canvas.
    class NodeGraphCanvas : public View
    {
        DRACONIC_OBJECT(NodeGraphCanvas, View)
    private:
        // ========== Nested types (declared first: used as return types below) ==========

        enum class InteractionMode
        {
            None,
            DraggingNode,
            DraggingConnection,
            BoxSelecting,
            Panning,
            PendingLink // StartLinkFrom: rubber edge follows the mouse until click/Escape
        };

        struct DragStart
        {
            i32 idx = 0;
            Float2 startPos{};
        };

        struct PortHit
        {
            i32 nodeIdx = -1;
            i32 portIdx = -1;
            PortDirection dir = PortDirection::Input;
        };

    public:
        // ========== Configuration ==========

        /// Whether the graph is read-only (no editing, only visualization).
        bool ReadOnly = false;

        /// Connection validation delegate. Default: same TypeId or either untyped.
        Function<bool(NodeGraphPortType, NodeGraphPortType)> ConnectionValidator;

        /// Whether to draw a background grid.
        bool ShowGrid = true;

        /// Whether to snap node positions to the grid on drag end.
        bool SnapToGrid = false;

        /// Connection anchoring/rendering style (see ConnectionStyle).
        ConnectionStyle EdgeStyle = ConnectionStyle::BezierPorts;

        // ========== Events ==========

        Event<void()> OnEditBegin;
        Event<void()> OnEditEnd;
        Event<void(i32)> OnNodeMoved;
        Event<void(i32)> OnNodeDeleted;
        Event<void(i32)> OnConnectionCreated;
        Event<void(i32, i32, i32, i32)> OnConnectionRemoved;
        /// Fired at the START of RemoveConnection with the connection INDEX (still valid when the
        /// event fires). Lets an index-mapped caller model (e.g. transitions parallel to
        /// connections) mirror keyboard/interactive deletions unambiguously - OnConnectionRemoved
        /// only carries endpoints, which parallel edges share.
        Event<void(i32)> OnConnectionDeleting;
        Event<void()> OnSelectionChanged;
        Event<void(f32, f32)> OnCanvasContextMenu;
        Event<void(i32)> OnNodeContextMenu;
        Event<void(i32)> OnConnectionContextMenu;
        Event<void(i32)> OnNodeDoubleClicked;
        /// Fired when a StartLinkFrom gesture lands on a target node: (sourceNode, targetNode).
        /// The CALLER decides what a link means (e.g. adds a transition + a connection).
        Event<void(i32, i32)> OnNodeLinkRequested;

        // ========== Constructor ==========

        NodeGraphCanvas() { IsFocusable = true; }

        // ========== Public API ==========

        [[nodiscard]] i32 NodeCount() const { return static_cast<i32>(m_nodes.Size()); }
        [[nodiscard]] i32 ConnectionCount() const { return static_cast<i32>(m_connections.Size()); }
        [[nodiscard]] Float2 PanOffset() const { return m_panOffset; }
        [[nodiscard]] f32 Zoom() const { return m_zoom; }

        /// Adds a node and returns its index.
        i32 AddNode(UniquePtr<NodeGraphNode> node)
        {
            const i32 idx = static_cast<i32>(m_nodes.Size());
            NodeGraphNode* raw = node.Get();
            m_nodes.PushBack(Move(node));
            m_drawOrder.PushBack(idx);
            AutoSizeNode(raw);
            Invalidate();
            return idx;
        }

        /// Removes a node by index. Also removes all connections to/from it.
        void RemoveNode(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_nodes.Size()))
            {
                return;
            }

            OnNodeDeleted.Invoke(index);

            // Remove connections referencing this node.
            for (i32 i = static_cast<i32>(m_connections.Size()) - 1; i >= 0; i--)
            {
                const NodeGraphConnection& c = m_connections[static_cast<usize>(i)];
                if (c.SourceNodeIndex == index || c.DestNodeIndex == index)
                {
                    m_connections.RemoveAt(static_cast<usize>(i));
                }
            }

            // Remap connection indices above the removed node.
            for (i32 i = 0; i < static_cast<i32>(m_connections.Size()); i++)
            {
                NodeGraphConnection& c = m_connections[static_cast<usize>(i)];
                if (c.SourceNodeIndex > index)
                {
                    c.SourceNodeIndex--;
                }
                if (c.DestNodeIndex > index)
                {
                    c.DestNodeIndex--;
                }
            }

            // Remove from draw order and remap.
            RemoveValue(m_drawOrder, index);
            for (i32 i = 0; i < static_cast<i32>(m_drawOrder.Size()); i++)
            {
                if (m_drawOrder[static_cast<usize>(i)] > index)
                {
                    m_drawOrder[static_cast<usize>(i)]--;
                }
            }

            m_nodes.RemoveAt(static_cast<usize>(index)); // UniquePtr frees the node.
            Invalidate();
        }

        [[nodiscard]] NodeGraphNode* GetNode(i32 index)
        {
            if (index >= 0 && index < static_cast<i32>(m_nodes.Size()))
            {
                return m_nodes[static_cast<usize>(index)].Get();
            }
            return nullptr;
        }

        /// Adds a connection. Returns its index, or -1 if validation fails.
        i32 AddConnection(NodeGraphConnection conn)
        {
            if (!ValidateConnection(conn))
            {
                return -1;
            }
            const i32 idx = static_cast<i32>(m_connections.Size());
            m_connections.PushBack(conn);
            Invalidate();
            return idx;
        }

        void RemoveConnection(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_connections.Size()))
            {
                return;
            }
            OnConnectionDeleting.Invoke(index);
            const NodeGraphConnection c = m_connections[static_cast<usize>(index)];
            OnConnectionRemoved.Invoke(c.SourceNodeIndex, c.SourcePortIndex, c.DestNodeIndex,
                                       c.DestPortIndex);
            m_connections.RemoveAt(static_cast<usize>(index));
            Invalidate();
        }

        [[nodiscard]] NodeGraphConnection GetConnection(i32 index) const
        {
            if (index >= 0 && index < static_cast<i32>(m_connections.Size()))
            {
                return m_connections[static_cast<usize>(index)];
            }
            return NodeGraphConnection{};
        }

        void Clear()
        {
            m_connections.Clear();
            m_nodes.Clear();
            m_drawOrder.Clear();
            Invalidate();
        }

        /// Enter link mode: a rubber edge follows the mouse from `nodeIdx` until a left-click
        /// lands on a node (fires OnNodeLinkRequested(source, target)); Escape, a click on empty
        /// space, or any other button cancels. The state-machine "Make Transition" flow for
        /// port-less nodes. No-op when ReadOnly.
        void StartLinkFrom(i32 nodeIdx)
        {
            if (ReadOnly || nodeIdx < 0 || nodeIdx >= static_cast<i32>(m_nodes.Size()))
            {
                return;
            }
            m_interaction = InteractionMode::PendingLink;
            m_linkSourceNode = nodeIdx;
            // Until the first mouse move, aim the rubber edge at the source node itself.
            m_dragConnectionEnd = NodeCenterScreen(nodeIdx);
            Invalidate();
        }

        void GetSelectedNodes(Array<i32>& outIndices)
        {
            for (i32 i = 0; i < static_cast<i32>(m_nodes.Size()); i++)
            {
                if (m_nodes[static_cast<usize>(i)].Get()->IsSelected)
                {
                    outIndices.PushBack(i);
                }
            }
        }

        void SelectNode(i32 index, bool addToSelection = false)
        {
            if (!addToSelection)
            {
                ClearSelectionSilent();
            }
            if (index >= 0 && index < static_cast<i32>(m_nodes.Size()))
            {
                m_nodes[static_cast<usize>(index)].Get()->IsSelected = true;
            }
            OnSelectionChanged.Invoke();
            Invalidate();
        }

        void ClearSelection()
        {
            ClearSelectionSilent();
            ClearConnectionSelection();
            OnSelectionChanged.Invoke();
            Invalidate();
        }

        /// Pans to center all nodes in view.
        void FrameAll()
        {
            if (m_nodes.Size() == 0)
            {
                return;
            }
            Float2 minP{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
            Float2 maxP{std::numeric_limits<f32>::lowest(), std::numeric_limits<f32>::lowest()};
            for (usize i = 0; i < m_nodes.Size(); ++i)
            {
                const NodeGraphNode* node = m_nodes[i].Get();
                minP.x = foundation::Min(minP.x, node->Position.x);
                minP.y = foundation::Min(minP.y, node->Position.y);
                maxP.x = foundation::Max(maxP.x, node->Position.x + node->Size.x);
                maxP.y = foundation::Max(maxP.y, node->Position.y + node->Size.y);
            }
            const Float2 center = (minP + maxP) * 0.5f;
            const Float2 viewCenter{Width() * 0.5f, Height() * 0.5f};
            m_panOffset = viewCenter - center * m_zoom;
            Invalidate();
        }

        void FrameNode(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_nodes.Size()))
            {
                return;
            }
            const NodeGraphNode* node = m_nodes[static_cast<usize>(index)].Get();
            const Float2 center = node->Position + node->Size * 0.5f;
            const Float2 viewCenter{Width() * 0.5f, Height() * 0.5f};
            m_panOffset = viewCenter - center * m_zoom;
            Invalidate();
        }

        // ========== Coordinate transforms ==========

        [[nodiscard]] Float2 ScreenToCanvas(Float2 screen) const
        {
            return Float2{(screen.x - m_panOffset.x) / m_zoom, (screen.y - m_panOffset.y) / m_zoom};
        }

        [[nodiscard]] Float2 CanvasToScreen(Float2 canvas) const
        {
            return Float2{canvas.x * m_zoom + m_panOffset.x, canvas.y * m_zoom + m_panOffset.y};
        }

        // ========== Hit testing ==========

        View* HitTest(Float2 localPoint) override
        {
            if (!IsInteractionEnabled || Visibility != VisibilityValue::Visible)
            {
                return nullptr;
            }
            if (localPoint.x < 0 || localPoint.y < 0 || localPoint.x >= Width() ||
                localPoint.y >= Height())
            {
                return nullptr;
            }
            return this; // Canvas handles all input internally.
        }

        // ========== Drawing ==========

        void OnDraw(UIDrawContext& ctx) override
        {
            // Background.
            Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background);
            if (bgDrawable != nullptr)
            {
                bgDrawable->Draw(ctx, Rectangle{0, 0, Width(), Height()});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Rgb(28, 28, 33, 255));
            }

            ctx.VG().PushClipRect(Rectangle{0, 0, Width(), Height()});

            // Grid.
            if (ShowGrid)
            {
                DrawGrid(ctx);
            }

            // Connections.
            for (i32 i = 0; i < static_cast<i32>(m_connections.Size()); i++)
            {
                DrawConnection(ctx, i, i == m_hoveredConnectionIndex);
            }

            // Box selection.
            if (m_interaction == InteractionMode::BoxSelecting)
            {
                const foundation::Color accentColor =
                    ResolveStyleColor(StyleProperty::AccentColor, Rgb(80, 140, 220, 255));
                const Float2 s1 = CanvasToScreen(m_boxSelectStart);
                const Float2 s2 = CanvasToScreen(m_boxSelectEnd);
                const Rectangle rect{foundation::Min(s1.x, s2.x), foundation::Min(s1.y, s2.y), Abs(s2.x - s1.x),
                                     Abs(s2.y - s1.y)};
                ctx.VG().FillRect(
                    rect, foundation::Color{accentColor.r, accentColor.g, accentColor.b, 40.0f / 255.0f});
                ctx.VG().StrokeRect(
                    rect, foundation::Color{accentColor.r, accentColor.g, accentColor.b, 150.0f / 255.0f},
                    1);
            }

            // Nodes (in draw order).
            for (usize i = 0; i < m_drawOrder.Size(); ++i)
            {
                DrawNode(ctx, m_drawOrder[i]);
            }

            // Connection being dragged.
            if (m_interaction == InteractionMode::PendingLink && m_linkSourceNode >= 0)
            {
                const foundation::Color linkColor =
                    ResolveStyleColor(StyleProperty::TextDimColor, Rgb(180, 200, 220, 180));
                DrawStraightEdge(ctx, NodeCenterScreen(m_linkSourceNode), m_dragConnectionEnd,
                                 linkColor);
            }

            if (m_interaction == InteractionMode::DraggingConnection && m_dragSourceNode >= 0)
            {
                const Float2 srcPort =
                    GetPortScreenPos(m_dragSourceNode, m_dragSourcePort, m_dragSourceDirection);
                const Float2 endPos = m_dragConnectionEnd;
                const bool isOutput = (m_dragSourceDirection == PortDirection::Output);
                const foundation::Color dragColor =
                    ResolveStyleColor(StyleProperty::TextDimColor, Rgb(180, 200, 220, 180));
                DrawBezier(ctx, isOutput ? srcPort : endPos, isOutput ? endPos : srcPort,
                           dragColor);
            }

            // Hovered port highlight.
            if (m_hoveredPortNode >= 0 && m_interaction == InteractionMode::None)
            {
                const Float2 pos =
                    GetPortScreenPos(m_hoveredPortNode, m_hoveredPortIndex, m_hoveredPortDir);
                ctx.VG().FillCircle(pos, PortRadius * m_zoom + 3, Rgb(255, 255, 255, 60));
            }

            ctx.VG().PopClip();
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(400.0f), constraints.ConstrainHeight(300.0f)};
        }

    public:
        // ========== Input ==========

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }

            const Float2 canvasPos = ScreenToCanvas(Float2{e.X, e.Y});

            // Pending link: a LEFT click on a node completes it; anything else cancels.
            if (m_interaction == InteractionMode::PendingLink)
            {
                const i32 source = m_linkSourceNode;
                const i32 target = (e.Button == MouseButton::Left) ? HitTestNode(e.X, e.Y) : -1;
                m_interaction = InteractionMode::None;
                m_linkSourceNode = -1;
                if (source >= 0 && target >= 0 && target != source)
                {
                    OnNodeLinkRequested.Invoke(source, target);
                }
                Invalidate();
                e.Handled = true;
                return;
            }

            // Middle button: pan.
            if (e.Button == MouseButton::Middle)
            {
                m_interaction = InteractionMode::Panning;
                m_panStartMouse = Float2{e.X, e.Y};
                m_panStartOffset = m_panOffset;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
                return;
            }

            if (e.Button == MouseButton::Right)
            {
                // Context menus.
                const i32 nodeHit = HitTestNode(e.X, e.Y);
                const i32 connHit = HitTestConnection(e.X, e.Y);

                if (nodeHit >= 0)
                {
                    OnNodeContextMenu.Invoke(nodeHit);
                }
                else if (connHit >= 0)
                {
                    OnConnectionContextMenu.Invoke(connHit);
                }
                else
                {
                    OnCanvasContextMenu.Invoke(canvasPos.x, canvasPos.y);
                }

                e.Handled = true;
                return;
            }

            if (e.Button != MouseButton::Left)
            {
                return;
            }

            // Double click.
            if (e.ClickCount >= 2)
            {
                const i32 nodeHit = HitTestNode(e.X, e.Y);
                if (nodeHit >= 0)
                {
                    OnNodeDoubleClicked.Invoke(nodeHit);
                    e.Handled = true;
                    return;
                }
            }

            if (ReadOnly)
            {
                e.Handled = true;
                return;
            }

            // Port hit - start connection drag.
            const PortHit portHit = HitTestPort(e.X, e.Y);
            if (portHit.nodeIdx >= 0)
            {
                // If dragging from an input port that already has a connection, detach the existing
                // connection and drag from the original source output port instead - allows re-routing.
                if (portHit.dir == PortDirection::Input)
                {
                    const i32 existingIdx = FindConnectionToInput(portHit.nodeIdx, portHit.portIdx);
                    if (existingIdx >= 0)
                    {
                        const NodeGraphConnection existing =
                            m_connections[static_cast<usize>(existingIdx)];
                        const i32 srcNode = existing.SourceNodeIndex;
                        const i32 srcPort = existing.SourcePortIndex;

                        BeginGesture();
                        RemoveConnection(existingIdx);

                        m_interaction = InteractionMode::DraggingConnection;
                        m_dragSourceNode = srcNode;
                        m_dragSourcePort = srcPort;
                        m_dragSourceDirection = PortDirection::Output;
                        m_dragConnectionEnd = Float2{e.X, e.Y};
                        if (Context != nullptr)
                        {
                            Context->GetFocusManager()->SetCapture(this);
                        }
                        e.Handled = true;
                        return;
                    }
                }

                m_interaction = InteractionMode::DraggingConnection;
                m_dragSourceNode = portHit.nodeIdx;
                m_dragSourcePort = portHit.portIdx;
                m_dragSourceDirection = portHit.dir;
                m_dragConnectionEnd = Float2{e.X, e.Y};
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
                return;
            }

            // Node hit - select and start drag.
            const i32 nodeHit = HitTestNode(e.X, e.Y);
            if (nodeHit >= 0)
            {
                NodeGraphNode* node = m_nodes[static_cast<usize>(nodeHit)].Get();
                const bool shift = HasFlag(e.Modifiers, KeyModifiers::Shift);

                if (shift)
                {
                    node->IsSelected = !node->IsSelected;
                }
                else if (!node->IsSelected)
                {
                    ClearSelectionSilent();
                    ClearConnectionSelection();
                    node->IsSelected = true;
                }
                OnSelectionChanged.Invoke();

                // Bring to front in draw order.
                BringToFront(nodeHit);

                // Start drag if the clicked node is movable.
                if (node->IsMovable)
                {
                    m_interaction = InteractionMode::DraggingNode;
                    m_dragStartMouse = canvasPos;
                    m_dragStarts.Clear();
                    for (i32 i = 0; i < static_cast<i32>(m_nodes.Size()); i++)
                    {
                        NodeGraphNode* n = m_nodes[static_cast<usize>(i)].Get();
                        if (n->IsSelected && n->IsMovable)
                        {
                            m_dragStarts.PushBack(DragStart{i, n->Position});
                        }
                    }
                    BeginGesture();
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->SetCapture(this);
                    }
                }

                e.Handled = true;
                return;
            }

            // Connection hit - select.
            const i32 connHit = HitTestConnection(e.X, e.Y);
            if (connHit >= 0)
            {
                ClearSelectionSilent();
                ClearConnectionSelection();
                m_connections[static_cast<usize>(connHit)].IsSelected = true;
                OnSelectionChanged.Invoke();
                e.Handled = true;
                Invalidate();
                return;
            }

            // Empty space - start box selection.
            if (!HasFlag(e.Modifiers, KeyModifiers::Shift))
            {
                ClearSelectionSilent();
                ClearConnectionSelection();
            }
            m_interaction = InteractionMode::BoxSelecting;
            m_boxSelectStart = canvasPos;
            m_boxSelectEnd = canvasPos;
            if (Context != nullptr)
            {
                Context->GetFocusManager()->SetCapture(this);
            }
            e.Handled = true;
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            const Float2 canvasPos = ScreenToCanvas(Float2{e.X, e.Y});

            switch (m_interaction)
            {
            case InteractionMode::PendingLink:
            {
                m_dragConnectionEnd = Float2{e.X, e.Y};
                Invalidate();
                e.Handled = true;
                break;
            }
            case InteractionMode::Panning:
            {
                const f32 dx = e.X - m_panStartMouse.x;
                const f32 dy = e.Y - m_panStartMouse.y;
                m_panOffset = Float2{m_panStartOffset.x + dx, m_panStartOffset.y + dy};
                Invalidate();
                e.Handled = true;
                break;
            }

            case InteractionMode::DraggingNode:
            {
                const Float2 delta = canvasPos - m_dragStartMouse;
                for (usize i = 0; i < m_dragStarts.Size(); ++i)
                {
                    const DragStart& entry = m_dragStarts[i];
                    m_nodes[static_cast<usize>(entry.idx)].Get()->Position = entry.startPos + delta;
                }
                Invalidate();
                e.Handled = true;
                break;
            }

            case InteractionMode::DraggingConnection:
                m_dragConnectionEnd = Float2{e.X, e.Y};
                // Update hover for drop target feedback.
                UpdatePortHover(e.X, e.Y);
                Invalidate();
                e.Handled = true;
                break;

            case InteractionMode::BoxSelecting:
                m_boxSelectEnd = canvasPos;
                Invalidate();
                e.Handled = true;
                break;

            case InteractionMode::None:
                // Update hover state.
                UpdateHover(e.X, e.Y);
                break;
            }
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            switch (m_interaction)
            {
            case InteractionMode::PendingLink:
                // The link completes/cancels on mouse DOWN; the release is inert.
                break;

            case InteractionMode::Panning:
                m_interaction = InteractionMode::None;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
                break;

            case InteractionMode::DraggingNode:
            {
                // Snap to grid if enabled.
                if (SnapToGrid)
                {
                    for (usize i = 0; i < m_dragStarts.Size(); ++i)
                    {
                        NodeGraphNode* n = m_nodes[static_cast<usize>(m_dragStarts[i].idx)].Get();
                        n->Position.x = foundation::Round(n->Position.x / GridSize) * GridSize;
                        n->Position.y = foundation::Round(n->Position.y / GridSize) * GridSize;
                    }
                }
                for (usize i = 0; i < m_dragStarts.Size(); ++i)
                {
                    OnNodeMoved.Invoke(m_dragStarts[i].idx);
                }
                EndGesture();
                m_interaction = InteractionMode::None;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                Invalidate();
                e.Handled = true;
                break;
            }

            case InteractionMode::DraggingConnection:
            {
                // Try to connect.
                const PortHit portHit = HitTestPort(e.X, e.Y);
                if (portHit.nodeIdx >= 0 && portHit.dir != m_dragSourceDirection)
                {
                    NodeGraphConnection conn{};
                    if (m_dragSourceDirection == PortDirection::Output)
                    {
                        conn.SourceNodeIndex = m_dragSourceNode;
                        conn.SourcePortIndex = m_dragSourcePort;
                        conn.DestNodeIndex = portHit.nodeIdx;
                        conn.DestPortIndex = portHit.portIdx;
                    }
                    else
                    {
                        conn.SourceNodeIndex = portHit.nodeIdx;
                        conn.SourcePortIndex = portHit.portIdx;
                        conn.DestNodeIndex = m_dragSourceNode;
                        conn.DestPortIndex = m_dragSourcePort;
                    }

                    // BeginGesture may already be active (detach-and-reroute case).
                    BeginGesture();
                    const i32 idx = AddConnection(conn);
                    if (idx >= 0)
                    {
                        OnConnectionCreated.Invoke(idx);
                    }
                }
                // End gesture (covers both fresh connect and detach-reroute).
                EndGesture();
                m_dragSourceNode = -1;
                m_interaction = InteractionMode::None;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                Invalidate();
                e.Handled = true;
                break;
            }

            case InteractionMode::BoxSelecting:
            {
                // Select nodes in box.
                const f32 minX = foundation::Min(m_boxSelectStart.x, m_boxSelectEnd.x);
                const f32 minY = foundation::Min(m_boxSelectStart.y, m_boxSelectEnd.y);
                const f32 maxX = foundation::Max(m_boxSelectStart.x, m_boxSelectEnd.x);
                const f32 maxY = foundation::Max(m_boxSelectStart.y, m_boxSelectEnd.y);

                for (i32 i = 0; i < static_cast<i32>(m_nodes.Size()); i++)
                {
                    NodeGraphNode* node = m_nodes[static_cast<usize>(i)].Get();
                    const Float2 nr = node->Position;
                    const Float2 ns = node->Size;
                    // Check overlap.
                    if (nr.x + ns.x > minX && nr.x < maxX && nr.y + ns.y > minY && nr.y < maxY)
                    {
                        node->IsSelected = true;
                    }
                }
                OnSelectionChanged.Invoke();
                m_interaction = InteractionMode::None;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                Invalidate();
                e.Handled = true;
                break;
            }

            case InteractionMode::None:
                break;
            }
        }

        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            // Zoom toward cursor.
            const f32 oldZoom = m_zoom;
            const f32 delta = e.DeltaY * ZoomStep;
            m_zoom = foundation::Clamp(m_zoom + delta, MinZoom, MaxZoom);

            if (m_zoom != oldZoom)
            {
                const Float2 mousePos{e.X, e.Y};
                m_panOffset = mousePos - (mousePos - m_panOffset) * (m_zoom / oldZoom);
                Invalidate();
            }
            e.Handled = true;
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (ReadOnly)
            {
                return;
            }

            if (e.Key == KeyCode::Escape && m_interaction == InteractionMode::PendingLink)
            {
                m_interaction = InteractionMode::None;
                m_linkSourceNode = -1;
                Invalidate();
                e.Handled = true;
            }
            else if (e.Key == KeyCode::Delete)
            {
                DeleteSelected();
                e.Handled = true;
            }
            else if (e.Key == KeyCode::A && HasFlag(e.Modifiers, KeyModifiers::Ctrl))
            {
                // Select all.
                for (usize i = 0; i < m_nodes.Size(); ++i)
                {
                    m_nodes[i].Get()->IsSelected = true;
                }
                OnSelectionChanged.Invoke();
                Invalidate();
                e.Handled = true;
            }
        }

    private:
        // ========== Drawing helpers ==========

        void DrawGrid(UIDrawContext& ctx)
        {
            const f32 gridStep = GridSize * m_zoom;
            if (gridStep < 4)
            {
                return;
            } // Too zoomed out.

            const foundation::Color borderColor =
                ResolveStyleColor(StyleProperty::BorderColor, Rgb(55, 55, 60, 255));
            const foundation::Color gridColor{borderColor.r * 0.7f, borderColor.g * 0.7f,
                                        borderColor.b * 0.7f, borderColor.a};
            const f32 startX = std::fmod(m_panOffset.x, gridStep);
            const f32 startY = std::fmod(m_panOffset.y, gridStep);

            const f32 majorStep = gridStep * 5;
            const f32 majorStartX = std::fmod(m_panOffset.x, majorStep);
            const f32 majorStartY = std::fmod(m_panOffset.y, majorStep);

            // Minor grid.
            f32 x = startX;
            while (x < Width())
            {
                ctx.VG().DrawLine(Float2{x, 0}, Float2{x, Height()}, gridColor, 1);
                x += gridStep;
            }
            f32 y = startY;
            while (y < Height())
            {
                ctx.VG().DrawLine(Float2{0, y}, Float2{Width(), y}, gridColor, 1);
                y += gridStep;
            }

            // Major grid.
            if (majorStep >= 20)
            {
                x = majorStartX;
                while (x < Width())
                {
                    ctx.VG().DrawLine(Float2{x, 0}, Float2{x, Height()}, borderColor, 1);
                    x += majorStep;
                }
                y = majorStartY;
                while (y < Height())
                {
                    ctx.VG().DrawLine(Float2{0, y}, Float2{Width(), y}, borderColor, 1);
                    y += majorStep;
                }
            }
        }

        void DrawNode(UIDrawContext& ctx, i32 nodeIdx)
        {
            const NodeGraphNode* node = m_nodes[static_cast<usize>(nodeIdx)].Get();
            const Float2 pos = CanvasToScreen(node->Position);
            const Float2 size = node->Size * m_zoom;
            const f32 headerH = HeaderHeight * m_zoom;
            const f32 cornerR = ResolveStyleFloat(StyleProperty::CornerRadius, 4) * m_zoom;

            // Body.
            Drawable* contentDrawable =
                ResolvePartDrawable(u8"node-body", StyleProperty::Background, ControlState::Normal);
            if (contentDrawable != nullptr)
            {
                contentDrawable->Draw(ctx, Rectangle{pos.x, pos.y, size.x, size.y});
            }
            else
            {
                ctx.VG().FillRoundedRect(Rectangle{pos.x, pos.y, size.x, size.y}, cornerR,
                                         Rgb(38, 40, 48, 255));
            }

            // Header (uses node's HeaderColor - caller controls this per node).
            ctx.VG().FillRoundedRect(Rectangle{pos.x, pos.y, size.x, headerH}, cornerR,
                                     node->HeaderColor);
            // Square off bottom corners of header.
            ctx.VG().FillRect(Rectangle{pos.x, pos.y + headerH - cornerR, size.x, cornerR},
                              node->HeaderColor);

            // Selection highlight.
            if (node->IsSelected)
            {
                const foundation::Color accentColor =
                    ResolveStyleColor(StyleProperty::AccentColor, Rgb(100, 180, 255, 200));
                ctx.VG().StrokeRoundedRect(Rectangle{pos.x, pos.y, size.x, size.y}, cornerR,
                                           accentColor, 2);
            }

            // Emphasis ring (caller-driven; e.g. the ACTIVE state in a live graph preview).
            if (node->IsHighlighted)
            {
                ctx.VG().StrokeRoundedRect(Rectangle{pos.x - 3, pos.y - 3, size.x + 6, size.y + 6},
                                           cornerR + 3, node->HighlightColor, 3);
            }

            // Title text.
            if (ctx.FontService() != nullptr)
            {
                const foundation::Color textColor =
                    ResolveStyleColor(StyleProperty::TextColor, Rgb(255, 255, 255, 230));
                fonts::CachedFont* titleFont = ctx.FontService()->GetFont(11);
                if (titleFont != nullptr)
                {
                    ctx.VG().DrawText(
                        node->Title, titleFont,
                        Rectangle{pos.x + 8 * m_zoom, pos.y, size.x - 16 * m_zoom, headerH},
                        fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle, textColor);
                }

                // Subtitle.
                if (node->Subtitle.Length() > 0)
                {
                    const foundation::Color dimColor =
                        ResolveStyleColor(StyleProperty::TextDimColor, Rgb(180, 180, 190, 180));
                    fonts::CachedFont* subFont = ctx.FontService()->GetFont(10);
                    if (subFont != nullptr)
                    {
                        ctx.VG().DrawText(node->Subtitle, subFont,
                                          Rectangle{pos.x + 8 * m_zoom, pos.y + headerH,
                                                    size.x - 16 * m_zoom, 16 * m_zoom},
                                          fonts::TextAlignment::Left,
                                          fonts::VerticalAlignment::Middle, dimColor);
                    }
                }

                // Ports.
                fonts::CachedFont* portFont = ctx.FontService()->GetFont(10);
                DrawPorts(ctx, nodeIdx, node->InputPorts, PortDirection::Input, pos, size,
                          portFont);
                DrawPorts(ctx, nodeIdx, node->OutputPorts, PortDirection::Output, pos, size,
                          portFont);
            }
        }

        void DrawPorts(UIDrawContext& ctx, i32 nodeIdx, const Array<NodeGraphPort>& ports,
                       PortDirection dir, Float2 nodeScreenPos, Float2 nodeScreenSize,
                       fonts::CachedFont* portFont)
        {
            (void)nodeScreenPos;
            (void)nodeScreenSize;
            const foundation::Color portLabelColor =
                ResolveStyleColor(StyleProperty::TextDimColor, Rgb(200, 200, 210, 200));

            for (i32 i = 0; i < static_cast<i32>(ports.Size()); i++)
            {
                const NodeGraphPort& port = ports[static_cast<usize>(i)];
                const Float2 portPos = GetPortScreenPos(nodeIdx, i, dir);
                const f32 r = PortRadius * m_zoom;

                // Port circle (colored by port type - not themed, since callers define type colors).
                ctx.VG().FillCircle(portPos, r, port.PortType.Color);
                ctx.VG().StrokeCircle(portPos, r, Rgb(0, 0, 0, 100), 1);

                // Port label.
                if (portFont != nullptr && port.Label.Length() > 0)
                {
                    const f32 labelW = 80 * m_zoom;
                    if (dir == PortDirection::Input)
                    {
                        const f32 labelX = portPos.x + r + 4 * m_zoom;
                        ctx.VG().DrawText(
                            port.Label, portFont,
                            Rectangle{labelX, portPos.y - 8 * m_zoom, labelW, 16 * m_zoom},
                            fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                            portLabelColor);
                    }
                    else
                    {
                        const f32 labelX = portPos.x - r - 4 * m_zoom - labelW;
                        ctx.VG().DrawText(
                            port.Label, portFont,
                            Rectangle{labelX, portPos.y - 8 * m_zoom, labelW, 16 * m_zoom},
                            fonts::TextAlignment::Right, fonts::VerticalAlignment::Middle,
                            portLabelColor);
                    }
                }
            }
        }

        void DrawConnection(UIDrawContext& ctx, i32 connIndex, bool hovered)
        {
            const NodeGraphConnection& conn = m_connections[static_cast<usize>(connIndex)];
            if (conn.SourceNodeIndex < 0 ||
                conn.SourceNodeIndex >= static_cast<i32>(m_nodes.Size()))
            {
                return;
            }
            if (conn.DestNodeIndex < 0 || conn.DestNodeIndex >= static_cast<i32>(m_nodes.Size()))
            {
                return;
            }

            if (EdgeStyle == ConnectionStyle::StraightNodeToNode)
            {
                Float2 start, end;
                if (!ComputeStraightEdge(connIndex, start, end))
                {
                    return;
                }
                foundation::Color color = NodeGraphPortType::Untyped().Color;
                if (conn.IsSelected)
                {
                    color = ResolveStyleColor(StyleProperty::AccentColor, Rgb(100, 180, 255, 230));
                }
                else if (hovered)
                {
                    color = Rgb(230, 230, 240, 230);
                }
                DrawStraightEdge(ctx, start, end, color);
                return;
            }

            const Float2 startPos =
                GetPortScreenPos(conn.SourceNodeIndex, conn.SourcePortIndex, PortDirection::Output);
            const Float2 endPos =
                GetPortScreenPos(conn.DestNodeIndex, conn.DestPortIndex, PortDirection::Input);

            // Color from source port type (port types are caller-defined, not themed).
            foundation::Color color = NodeGraphPortType::Untyped().Color;
            const NodeGraphNode* srcNode = m_nodes[static_cast<usize>(conn.SourceNodeIndex)].Get();
            if (conn.SourcePortIndex >= 0 &&
                conn.SourcePortIndex < static_cast<i32>(srcNode->OutputPorts.Size()))
            {
                color =
                    srcNode->OutputPorts[static_cast<usize>(conn.SourcePortIndex)].PortType.Color;
            }

            if (conn.IsSelected)
            {
                color = ResolveStyleColor(StyleProperty::AccentColor, Rgb(100, 180, 255, 230));
            }
            else if (hovered)
            {
                color = foundation::Color{foundation::Min(1.0f, color.r + 40.0f / 255.0f),
                                    foundation::Min(1.0f, color.g + 40.0f / 255.0f),
                                    foundation::Min(1.0f, color.b + 40.0f / 255.0f), 230.0f / 255.0f};
            }

            DrawBezier(ctx, startPos, endPos, color);
        }

        // ---- straight node-to-node edges (ConnectionStyle::StraightNodeToNode) ----

        [[nodiscard]] Float2 NodeCenterScreen(i32 nodeIdx) const
        {
            const NodeGraphNode* node = m_nodes[static_cast<usize>(nodeIdx)].Get();
            const Float2 pos = CanvasToScreen(node->Position);
            const Float2 size = node->Size * m_zoom;
            return Float2{pos.x + size.x * 0.5f, pos.y + size.y * 0.5f};
        }

        // From `center` along `dir`, the point where the ray exits the node's screen rect.
        [[nodiscard]] Float2 ClipToNodeRect(i32 nodeIdx, Float2 center, Float2 dir) const
        {
            const NodeGraphNode* node = m_nodes[static_cast<usize>(nodeIdx)].Get();
            const Float2 pos = CanvasToScreen(node->Position);
            const Float2 size = node->Size * m_zoom;
            f32 t = std::numeric_limits<f32>::max();
            if (Abs(dir.x) > 0.0001f)
            {
                const f32 tx = ((dir.x > 0 ? pos.x + size.x : pos.x) - center.x) / dir.x;
                t = foundation::Min(t, foundation::Max(tx, 0.0f));
            }
            if (Abs(dir.y) > 0.0001f)
            {
                const f32 ty = ((dir.y > 0 ? pos.y + size.y : pos.y) - center.y) / dir.y;
                t = foundation::Min(t, foundation::Max(ty, 0.0f));
            }
            if (t == std::numeric_limits<f32>::max())
            {
                return center;
            }
            return center + dir * t;
        }

        // Screen-space endpoints of a straight edge: center-to-center, laterally offset by the
        // connection's LANE among edges of the same ordered pair (an opposite-direction edge
        // shifts both directions apart automatically - its perpendicular flips with dir), then
        // clipped to the two node rects. False = degenerate (self edge / overlapping nodes).
        [[nodiscard]] bool ComputeStraightEdge(i32 connIndex, Float2& outStart,
                                               Float2& outEnd) const
        {
            const NodeGraphConnection& conn = m_connections[static_cast<usize>(connIndex)];
            if (conn.SourceNodeIndex == conn.DestNodeIndex)
            {
                return false;
            }
            const Float2 centerA = NodeCenterScreen(conn.SourceNodeIndex);
            const Float2 centerB = NodeCenterScreen(conn.DestNodeIndex);
            const Float2 delta = centerB - centerA;
            const f32 len = foundation::Length(delta);
            if (len < 1.0f)
            {
                return false;
            }
            const Float2 dir = delta * (1.0f / len);
            const Float2 perp{-dir.y, dir.x};

            // Lane among SAME-ordered-pair edges + whether the opposite direction exists.
            i32 lane = 0, sameCount = 0;
            bool hasOpposite = false;
            for (i32 i = 0; i < static_cast<i32>(m_connections.Size()); i++)
            {
                const NodeGraphConnection& other = m_connections[static_cast<usize>(i)];
                if (other.SourceNodeIndex == conn.SourceNodeIndex &&
                    other.DestNodeIndex == conn.DestNodeIndex)
                {
                    if (i < connIndex)
                    {
                        lane++;
                    }
                    sameCount++;
                }
                else if (other.SourceNodeIndex == conn.DestNodeIndex &&
                         other.DestNodeIndex == conn.SourceNodeIndex)
                {
                    hasOpposite = true;
                }
            }
            const f32 spacing = 14.0f * m_zoom;
            // Opposite edges each shift to their own side; a lone pair stays centered.
            const f32 base =
                hasOpposite ? spacing * 0.5f : -spacing * 0.5f * static_cast<f32>(sameCount - 1);
            const f32 shift = base + spacing * static_cast<f32>(lane);

            const Float2 shiftedA = centerA + perp * shift;
            const Float2 shiftedB = centerB + perp * shift;
            outStart = ClipToNodeRect(conn.SourceNodeIndex, shiftedA, dir);
            outEnd = ClipToNodeRect(conn.DestNodeIndex, shiftedB, dir * -1.0f);
            return foundation::Distance(outStart, outEnd) >= 2.0f;
        }

        void DrawStraightEdge(UIDrawContext& ctx, Float2 start, Float2 end, foundation::Color color)
        {
            ctx.VG().BeginPath();
            ctx.VG().MoveTo(start);
            ctx.VG().LineTo(end);
            ctx.VG().Stroke(color, 2);

            // Mid-edge direction arrow.
            const Float2 delta = end - start;
            const f32 len = foundation::Length(delta);
            if (len < 12.0f)
            {
                return;
            }
            const Float2 dir = delta * (1.0f / len);
            const Float2 perp{-dir.y, dir.x};
            const Float2 mid = (start + end) * 0.5f;
            const f32 a = 7.0f * m_zoom;
            ctx.VG().BeginPath();
            ctx.VG().MoveTo(mid + dir * a);
            ctx.VG().LineTo(mid - dir * a + perp * a);
            ctx.VG().LineTo(mid - dir * a - perp * a);
            ctx.VG().ClosePath();
            ctx.VG().Fill(color);
        }

        [[nodiscard]] static f32 DistanceToSegment(Float2 point, Float2 a, Float2 b)
        {
            const Float2 ab = b - a;
            const f32 lenSq = ab.x * ab.x + ab.y * ab.y;
            if (lenSq < 0.0001f)
            {
                return foundation::Distance(point, a);
            }
            const f32 t =
                Clamp(((point.x - a.x) * ab.x + (point.y - a.y) * ab.y) / lenSq, 0.0f, 1.0f);
            return foundation::Distance(point, a + ab * t);
        }

        void DrawBezier(UIDrawContext& ctx, Float2 start, Float2 end, foundation::Color color)
        {
            const f32 dx = Abs(end.x - start.x);
            const f32 ctrlDist = foundation::Max(dx * 0.5f, 50 * m_zoom);

            ctx.VG().BeginPath();
            ctx.VG().MoveTo(start);
            ctx.VG().CubicTo(Float2{start.x + ctrlDist, start.y}, Float2{end.x - ctrlDist, end.y},
                             end);
            ctx.VG().Stroke(color, 2);
        }

        // ========== Port position helpers ==========

        [[nodiscard]] Float2 GetPortScreenPos(i32 nodeIdx, i32 portIdx, PortDirection dir) const
        {
            const NodeGraphNode* node = m_nodes[static_cast<usize>(nodeIdx)].Get();
            const f32 canvasX =
                (dir == PortDirection::Input) ? node->Position.x : node->Position.x + node->Size.x;
            const f32 canvasY = node->Position.y + HeaderHeight + PortMarginTop +
                                portIdx * PortSpacing + PortSpacing * 0.5f;
            return CanvasToScreen(Float2{canvasX, canvasY});
        }

        [[nodiscard]] Float2 GetPortCanvasPos(i32 nodeIdx, i32 portIdx, PortDirection dir) const
        {
            const NodeGraphNode* node = m_nodes[static_cast<usize>(nodeIdx)].Get();
            const f32 x =
                (dir == PortDirection::Input) ? node->Position.x : node->Position.x + node->Size.x;
            const f32 y = node->Position.y + HeaderHeight + PortMarginTop + portIdx * PortSpacing +
                          PortSpacing * 0.5f;
            return Float2{x, y};
        }

        // ========== Hit testing ==========

        [[nodiscard]] PortHit HitTestPort(f32 screenX, f32 screenY) const
        {
            const f32 hitR = PortHitRadius * m_zoom;
            for (i32 ni = static_cast<i32>(m_nodes.Size()) - 1; ni >= 0; ni--)
            {
                const NodeGraphNode* node = m_nodes[static_cast<usize>(ni)].Get();
                for (i32 pi = 0; pi < static_cast<i32>(node->InputPorts.Size()); pi++)
                {
                    const Float2 pos = GetPortScreenPos(ni, pi, PortDirection::Input);
                    if (foundation::Distance(Float2{screenX, screenY}, pos) <= hitR)
                    {
                        return PortHit{ni, pi, PortDirection::Input};
                    }
                }
                for (i32 pi = 0; pi < static_cast<i32>(node->OutputPorts.Size()); pi++)
                {
                    const Float2 pos = GetPortScreenPos(ni, pi, PortDirection::Output);
                    if (foundation::Distance(Float2{screenX, screenY}, pos) <= hitR)
                    {
                        return PortHit{ni, pi, PortDirection::Output};
                    }
                }
            }
            return PortHit{-1, -1, PortDirection::Input};
        }

        [[nodiscard]] i32 HitTestNode(f32 screenX, f32 screenY) const
        {
            // Check in reverse draw order (front to back).
            for (i32 i = static_cast<i32>(m_drawOrder.Size()) - 1; i >= 0; i--)
            {
                const i32 idx = m_drawOrder[static_cast<usize>(i)];
                const NodeGraphNode* node = m_nodes[static_cast<usize>(idx)].Get();
                const Float2 pos = CanvasToScreen(node->Position);
                const Float2 size = node->Size * m_zoom;
                if (screenX >= pos.x && screenX < pos.x + size.x && screenY >= pos.y &&
                    screenY < pos.y + size.y)
                {
                    return idx;
                }
            }
            return -1;
        }

        [[nodiscard]] i32 HitTestConnection(f32 screenX, f32 screenY) const
        {
            const f32 hitDist = ConnectionHitDist * m_zoom;
            for (i32 i = 0; i < static_cast<i32>(m_connections.Size()); i++)
            {
                const NodeGraphConnection& conn = m_connections[static_cast<usize>(i)];
                if (conn.SourceNodeIndex < 0 ||
                    conn.SourceNodeIndex >= static_cast<i32>(m_nodes.Size()))
                {
                    continue;
                }
                if (conn.DestNodeIndex < 0 ||
                    conn.DestNodeIndex >= static_cast<i32>(m_nodes.Size()))
                {
                    continue;
                }

                if (EdgeStyle == ConnectionStyle::StraightNodeToNode)
                {
                    Float2 start, end;
                    if (ComputeStraightEdge(i, start, end) &&
                        DistanceToSegment(Float2{screenX, screenY}, start, end) <= hitDist)
                    {
                        return i;
                    }
                    continue;
                }

                const Float2 start = GetPortScreenPos(conn.SourceNodeIndex, conn.SourcePortIndex,
                                                      PortDirection::Output);
                const Float2 end =
                    GetPortScreenPos(conn.DestNodeIndex, conn.DestPortIndex, PortDirection::Input);

                if (DistanceToBezier(Float2{screenX, screenY}, start, end) <= hitDist)
                {
                    return i;
                }
            }
            return -1;
        }

        [[nodiscard]] f32 DistanceToBezier(Float2 point, Float2 start, Float2 end) const
        {
            const f32 dx = Abs(end.x - start.x);
            const f32 ctrlDist = foundation::Max(dx * 0.5f, 50 * m_zoom);
            const Float2 cp1{start.x + ctrlDist, start.y};
            const Float2 cp2{end.x - ctrlDist, end.y};

            f32 minDist = std::numeric_limits<f32>::max();
            const i32 steps = 24;
            for (i32 s = 0; s <= steps; s++)
            {
                const f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
                const f32 it = 1 - t;
                const Float2 p = start * (it * it * it) + cp1 * (3 * it * it * t) +
                                 cp2 * (3 * it * t * t) + end * (t * t * t);
                const f32 dist = foundation::Distance(point, p);
                if (dist < minDist)
                {
                    minDist = dist;
                }
            }
            return minDist;
        }

        // ========== Hover updates ==========

        void UpdateHover(f32 screenX, f32 screenY)
        {
            const i32 oldNode = m_hoveredNodeIndex;
            const i32 oldConn = m_hoveredConnectionIndex;
            const i32 oldPortNode = m_hoveredPortNode;

            const PortHit portHit = HitTestPort(screenX, screenY);
            m_hoveredPortNode = portHit.nodeIdx;
            m_hoveredPortIndex = portHit.portIdx;
            m_hoveredPortDir = portHit.dir;

            m_hoveredNodeIndex = (portHit.nodeIdx < 0) ? HitTestNode(screenX, screenY) : -1;
            m_hoveredConnectionIndex = (m_hoveredNodeIndex < 0 && portHit.nodeIdx < 0)
                                           ? HitTestConnection(screenX, screenY)
                                           : -1;

            if (m_hoveredNodeIndex != oldNode || m_hoveredConnectionIndex != oldConn ||
                m_hoveredPortNode != oldPortNode)
            {
                Invalidate();
            }
        }

        void UpdatePortHover(f32 screenX, f32 screenY)
        {
            const PortHit portHit = HitTestPort(screenX, screenY);
            const i32 oldPortNode = m_hoveredPortNode;
            m_hoveredPortNode = portHit.nodeIdx;
            m_hoveredPortIndex = portHit.portIdx;
            m_hoveredPortDir = portHit.dir;
            if (m_hoveredPortNode != oldPortNode)
            {
                Invalidate();
            }
        }

        /// Finds the index of a connection targeting the given input port, or -1.
        [[nodiscard]] i32 FindConnectionToInput(i32 nodeIdx, i32 portIdx) const
        {
            for (i32 i = 0; i < static_cast<i32>(m_connections.Size()); i++)
            {
                const NodeGraphConnection& c = m_connections[static_cast<usize>(i)];
                if (c.DestNodeIndex == nodeIdx && c.DestPortIndex == portIdx)
                {
                    return i;
                }
            }
            return -1;
        }

        // ========== Internal helpers ==========

        void AutoSizeNode(NodeGraphNode* node)
        {
            const i32 maxPorts = foundation::Max(static_cast<i32>(node->InputPorts.Size()),
                                           static_cast<i32>(node->OutputPorts.Size()));
            const f32 portsH = HeaderHeight + PortMarginTop + maxPorts * PortSpacing + 8;
            node->Size.y = foundation::Max(node->Size.y, portsH);
            node->Size.x = foundation::Max(node->Size.x, NodeMinWidth);
        }

        [[nodiscard]] bool ValidateConnection(const NodeGraphConnection& conn)
        {
            // No self-connections.
            if (conn.SourceNodeIndex == conn.DestNodeIndex)
            {
                return false;
            }

            // Bounds check.
            if (conn.SourceNodeIndex < 0 ||
                conn.SourceNodeIndex >= static_cast<i32>(m_nodes.Size()))
            {
                return false;
            }
            if (conn.DestNodeIndex < 0 || conn.DestNodeIndex >= static_cast<i32>(m_nodes.Size()))
            {
                return false;
            }

            // Straight node-to-node edges are PORT-LESS: node bounds + no-self is the whole
            // contract, and parallel duplicates are legal (multiple transitions between one
            // state pair render in offset lanes).
            if (EdgeStyle == ConnectionStyle::StraightNodeToNode)
            {
                return true;
            }

            const NodeGraphNode* srcNode = m_nodes[static_cast<usize>(conn.SourceNodeIndex)].Get();
            const NodeGraphNode* dstNode = m_nodes[static_cast<usize>(conn.DestNodeIndex)].Get();

            if (conn.SourcePortIndex < 0 ||
                conn.SourcePortIndex >= static_cast<i32>(srcNode->OutputPorts.Size()))
            {
                return false;
            }
            if (conn.DestPortIndex < 0 ||
                conn.DestPortIndex >= static_cast<i32>(dstNode->InputPorts.Size()))
            {
                return false;
            }

            // No duplicate connections.
            for (usize i = 0; i < m_connections.Size(); ++i)
            {
                const NodeGraphConnection& existing = m_connections[i];
                if (existing.SourceNodeIndex == conn.SourceNodeIndex &&
                    existing.SourcePortIndex == conn.SourcePortIndex &&
                    existing.DestNodeIndex == conn.DestNodeIndex &&
                    existing.DestPortIndex == conn.DestPortIndex)
                {
                    return false;
                }
            }

            // Type validation.
            const NodeGraphPortType srcType =
                srcNode->OutputPorts[static_cast<usize>(conn.SourcePortIndex)].PortType;
            const NodeGraphPortType dstType =
                dstNode->InputPorts[static_cast<usize>(conn.DestPortIndex)].PortType;

            if (ConnectionValidator)
            {
                return ConnectionValidator(srcType, dstType);
            }

            // Default: same TypeId or either untyped.
            if (srcType.TypeId == 0 || dstType.TypeId == 0)
            {
                return true;
            }
            return srcType.TypeId == dstType.TypeId;
        }

        void ClearSelectionSilent()
        {
            for (usize i = 0; i < m_nodes.Size(); ++i)
            {
                m_nodes[i].Get()->IsSelected = false;
            }
        }

        void ClearConnectionSelection()
        {
            for (usize i = 0; i < m_connections.Size(); ++i)
            {
                m_connections[i].IsSelected = false;
            }
        }

        void BringToFront(i32 nodeIdx)
        {
            RemoveValue(m_drawOrder, nodeIdx);
            m_drawOrder.PushBack(nodeIdx);
        }

        void DeleteSelected()
        {
            BeginGesture();

            // Delete selected connections first.
            for (i32 i = static_cast<i32>(m_connections.Size()) - 1; i >= 0; i--)
            {
                if (m_connections[static_cast<usize>(i)].IsSelected)
                {
                    RemoveConnection(i);
                }
            }

            // Delete selected nodes (reverse order to keep indices stable).
            for (i32 i = static_cast<i32>(m_nodes.Size()) - 1; i >= 0; i--)
            {
                NodeGraphNode* n = m_nodes[static_cast<usize>(i)].Get();
                if (n->IsSelected && n->IsDeletable)
                {
                    RemoveNode(i);
                }
            }

            EndGesture();
            OnSelectionChanged.Invoke();
        }

        void BeginGesture()
        {
            if (!m_inGesture)
            {
                m_inGesture = true;
                OnEditBegin.Invoke();
            }
        }

        void EndGesture()
        {
            if (m_inGesture)
            {
                m_inGesture = false;
                OnEditEnd.Invoke();
            }
        }

        // Removes the first element equal to `value` (mirrors Beef List<int32>.Remove(value)).
        static void RemoveValue(Array<i32>& arr, i32 value)
        {
            for (usize i = 0; i < arr.Size(); ++i)
            {
                if (arr[i] == value)
                {
                    arr.RemoveAt(i);
                    return;
                }
            }
        }

        [[nodiscard]] static foundation::Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return foundation::Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        // ========== Layout constants ==========

        static constexpr f32 MinZoom = 0.1f;
        static constexpr f32 MaxZoom = 3.0f;
        static constexpr f32 ZoomStep = 0.1f;

        static constexpr f32 HeaderHeight = 26.0f;
        static constexpr f32 PortRadius = 5.0f;
        static constexpr f32 PortHitRadius = 10.0f;
        static constexpr f32 PortSpacing = 22.0f;
        static constexpr f32 PortMarginTop = 6.0f;
        static constexpr f32 NodeMinWidth = 120.0f;
        static constexpr f32 NodePadding = 10.0f;
        static constexpr f32 ConnectionHitDist = 8.0f;
        static constexpr f32 GridSize = 20.0f;

        // ========== Data ==========

        Array<UniquePtr<NodeGraphNode>> m_nodes;
        Array<NodeGraphConnection> m_connections;
        Array<i32> m_drawOrder; // indices into m_nodes, back-to-front

        // Pan / Zoom.
        Float2 m_panOffset{};
        f32 m_zoom = 1.0f;

        // Interaction state.
        InteractionMode m_interaction = InteractionMode::None;

        // Node dragging.
        Array<DragStart> m_dragStarts;
        Float2 m_dragStartMouse{};

        // Connection dragging.
        i32 m_dragSourceNode = -1;
        i32 m_dragSourcePort = -1;
        PortDirection m_dragSourceDirection = PortDirection::Input;
        Float2 m_dragConnectionEnd{};

        // Box selection.
        Float2 m_boxSelectStart{};
        Float2 m_boxSelectEnd{};

        // Panning.
        Float2 m_panStartMouse{};
        Float2 m_panStartOffset{};

        // Hover tracking.
        i32 m_hoveredNodeIndex = -1;
        i32 m_hoveredPortNode = -1;
        i32 m_hoveredPortIndex = -1;
        PortDirection m_hoveredPortDir = PortDirection::Input;
        i32 m_hoveredConnectionIndex = -1;
        i32 m_linkSourceNode = -1; // StartLinkFrom source while PendingLink is active

        // Gesture tracking (undo grouping).
        bool m_inGesture = false;
    };

    DRACONIC_DEFINE_OBJECT(NodeGraphCanvas, "draconic::ui::toolkit")
}
