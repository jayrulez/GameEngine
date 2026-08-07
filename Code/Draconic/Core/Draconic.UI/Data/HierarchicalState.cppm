// Draconic UI - :hierarchical_state partition
//
// Captures/restores TreeView state (expansion + selection + scroll) across data reloads or view
// rebuilds. Ported from Sedulous.UI/src/Data/HierarchicalState.bf. Depends on TreeView (its FlatAdapter /
// Selection / InternalListView), so it sits above :tree_view (clean DAG - TreeView doesn't reference it).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:hierarchical_state;

import draconic.foundation;
import :tree_view;
import :flattened_tree_adapter; // FlattenedTreeAdapter (named directly)
import :selection_model;        // SelectionModel (tree.Selection())
import :list_view;              // ListView (tree.InternalListView())

using namespace draconic::foundation;

export namespace draconic::ui
{
    class HierarchicalState
    {
    public:
        HashSet<i32> ExpandedNodes;
        HashSet<i32> SelectedPositions;
        f32 ScrollY = 0.0f;

        /// Capture the current state from a TreeView.
        void CaptureState(TreeView& tree)
        {
            ExpandedNodes.Clear();
            if (FlattenedTreeAdapter* flat = tree.FlatAdapter())
            {
                flat->GetExpandedNodes(ExpandedNodes);
            }

            SelectedPositions.Clear();
            for (i32 pos : tree.Selection().SelectedPositions())
            {
                SelectedPositions.Insert(pos);
            }

            ScrollY = tree.InternalListView()->ScrollY();
        }

        /// Apply previously-captured state to a TreeView.
        void ApplyState(TreeView& tree)
        {
            if (FlattenedTreeAdapter* flat = tree.FlatAdapter())
            {
                flat->SetExpandedNodes(ExpandedNodes);
            }

            tree.Selection().ClearSelection();
            for (i32 pos : SelectedPositions)
            {
                tree.Selection().Select(pos);
            }

            const f32 currentScroll = tree.InternalListView()->ScrollY();
            tree.InternalListView()->ScrollBy(ScrollY - currentScroll);
        }
    };
}
