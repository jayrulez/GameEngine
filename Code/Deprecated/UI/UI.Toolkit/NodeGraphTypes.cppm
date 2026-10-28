// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :node_graph_types partition
//
// Plain data types describing a node graph: ports (typed, colored), nodes (title / position / ports),
// and connections between ports. Model-agnostic - the NodeGraphCanvas renders these and callers push
// their own data in. Ported 1:1 from Sedulous.UI.Toolkit/src/NodeGraph/NodeGraphTypes.bf.
//
// Port taxes: Beef `Vector2` -> Float2; byte `Color(r,g,b,a)` -> float core::Color{...}; Beef heap
// `List<NodeGraphPort>` (owned) -> Array<NodeGraphPort> BY VALUE (ports are index-accessed, never held
// by stable pointer); Beef `String Label = new .() ~ delete _` -> value `String Label`. The member
// named `Color` is declared with the qualified type `core::Color` to stay clear of gcc's
// -Werror=changes-meaning (field name == unqualified type name).

module;
#include "Core/Prelude.h"

export module foundation.ui.toolkit:node_graph_types;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui::toolkit
{
    /// Direction of a port on a node.
    enum class PortDirection : u8
    {
        Input,
        Output
    };

    /// Describes a port type for connection validation and coloring. Uses caller-defined i32 IDs so each
    /// domain (animation, audio, etc.) can define its own type enums. TypeId 0 means "untyped" and
    /// connects to anything.
    struct NodeGraphPortType
    {
        /// Caller-defined type identifier. 0 = untyped (connects to anything).
        i32 TypeId = 0;

        /// Display color for the port circle and compatible connections.
        core::Color Color{};

        NodeGraphPortType() = default;
        NodeGraphPortType(i32 typeId, core::Color color) : TypeId(typeId), Color(color) {}

        /// Untyped port - connects to anything.
        [[nodiscard]] static NodeGraphPortType Untyped()
        {
            return NodeGraphPortType(
                0, core::Color{180.0f / 255.0f, 180.0f / 255.0f, 190.0f / 255.0f, 1.0f});
        }
    };

    /// One port on a node.
    struct NodeGraphPort
    {
        /// Whether this is an input or output port.
        PortDirection Direction = PortDirection::Input;

        /// Type of this port (for connection validation and coloring).
        NodeGraphPortType PortType = NodeGraphPortType::Untyped();

        /// Display label shown next to the port (e.g., "Audio In", "Pose Out").
        String Label;
    };

    /// A node in the graph. Lightweight data object rendered by the canvas.
    struct NodeGraphNode
    {
        /// Caller-assigned identifier. The canvas never interprets this; it is passed back in events so
        /// callers can map to their domain objects.
        i64 UserHandle = 0;

        /// Display title shown in the node's header bar.
        String Title;

        /// Optional subtitle shown below the title in smaller text.
        String Subtitle;

        /// Position in canvas space (before pan/zoom transform).
        Float2 Position{};

        /// Size in canvas space. Auto-computed from ports/title unless set explicitly.
        Float2 Size{160.0f, 80.0f};

        /// Header bar color.
        core::Color HeaderColor{70.0f / 255.0f, 130.0f / 255.0f, 200.0f / 255.0f, 1.0f};

        /// Whether this node is selected.
        bool IsSelected = false;

        /// Extra emphasis ring drawn OUTSIDE the selection outline in HighlightColor - e.g. the
        /// ACTIVE state while a live animation-graph preview runs. Caller-driven; never set by
        /// the canvas itself.
        bool IsHighlighted = false;
        core::Color HighlightColor{255.0f / 255.0f, 170.0f / 255.0f, 60.0f / 255.0f,
                                   220.0f / 255.0f};

        /// Input ports (ordered top to bottom on the left side).
        Array<NodeGraphPort> InputPorts;

        /// Output ports (ordered top to bottom on the right side).
        Array<NodeGraphPort> OutputPorts;

        /// Whether this node can be moved by the user.
        bool IsMovable = true;

        /// Whether this node can be deleted by the user.
        bool IsDeletable = true;
    };

    /// A connection between an output port on one node and an input port on another.
    struct NodeGraphConnection
    {
        /// Source node index.
        i32 SourceNodeIndex = 0;
        /// Source output port index within that node.
        i32 SourcePortIndex = 0;
        /// Destination node index.
        i32 DestNodeIndex = 0;
        /// Destination input port index within that node.
        i32 DestPortIndex = 0;
        /// Whether this connection is selected.
        bool IsSelected = false;
    };
}
