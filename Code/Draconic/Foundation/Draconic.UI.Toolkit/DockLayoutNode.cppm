// Draconic UI Toolkit - :dock_layout_node partition
//
// Serializable snapshot of a dock tree node, used by DockManager.ExportLayout/ApplyLayout for layout
// persistence. Ported from Sedulous.UI.Toolkit/src/Docking/DockLayoutNode.bf. A PLAIN value struct
// (NOT a View/Object). Beef owned children `First/Second ~ delete _` -> UniquePtr<DockLayoutNode>;
// Beef `List<String> PanelIds` -> Array<String>.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui.toolkit:dock_layout_node;

import draconic.foundation;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Describes the type of a dock layout node.
    enum class DockLayoutNodeType
    {
        /// A split node with two children and a divider.
        Split,
        /// A tab group containing one or more panels.
        TabGroup
    };

    /// Serializable snapshot of a dock tree node. The consumer (e.g., editor) is responsible for
    /// serializing this structure to disk in whatever format it chooses.
    struct DockLayoutNode
    {
        /// Node type: Split or TabGroup.
        DockLayoutNodeType Type = DockLayoutNodeType::TabGroup;

        // === Split properties (only valid when Type == Split) ===

        /// Split orientation.
        Orientation Direction = Orientation::Horizontal;

        /// Split ratio (0..1), where the value is the fraction of the first child.
        f32 SplitRatio = 0.5f;

        /// First child (left or top). Owned by this node.
        UniquePtr<DockLayoutNode> First;

        /// Second child (right or bottom). Owned by this node.
        UniquePtr<DockLayoutNode> Second;

        // === TabGroup properties (only valid when Type == TabGroup) ===

        /// Persistence IDs of panels in this tab group, in tab order.
        Array<String> PanelIds;

        /// Index of the active (visible) tab.
        i32 ActiveTabIndex = 0;
    };
}
