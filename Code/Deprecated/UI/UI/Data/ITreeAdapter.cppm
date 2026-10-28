// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :itree_adapter partition
//
// Tree-shaped data source contract (+ its observer). Ported from Sedulous.UI/src/Data/ITreeAdapter.bf.
// FlattenedTreeAdapter wraps an ITreeAdapter to present it as an IListAdapter for ListView-based
// virtualization. Pattern-B injected interfaces -> plain abstract classes; Beef interface default
// methods -> virtual with default bodies; CreateView returns RefPtr<View> (RAII).

module;
#include "Core/Prelude.h"

export module foundation.ui:itree_adapter;

import foundation.core;
import :view;

using namespace foundation::core;

export namespace foundation::ui
{
    /// Observer for tree-adapter data changes (FlattenedTreeAdapter implements this).
    class ITreeAdapterObserver
    {
    public:
        virtual ~ITreeAdapterObserver() = default;
        /// Entire tree data changed - rebuild everything.
        virtual void OnTreeDataChanged() = 0;
    };

    class ITreeAdapter
    {
    public:
        virtual ~ITreeAdapter() = default;

        /// Number of root-level items.
        [[nodiscard]] virtual i32 RootCount() const = 0;
        /// Number of children of a node (nodeId == -1 = root level).
        [[nodiscard]] virtual i32 GetChildCount(i32 nodeId) const = 0;
        /// nodeId of the Nth child of a parent (parentId == -1 for roots).
        [[nodiscard]] virtual i32 GetChildId(i32 parentId, i32 childIndex) const = 0;
        /// Depth of a node (0 = root).
        [[nodiscard]] virtual i32 GetDepth(i32 nodeId) const = 0;
        /// Whether a node has children (can be expanded).
        [[nodiscard]] virtual bool HasChildren(i32 nodeId) const = 0;
        /// Create a view for a tree item.
        [[nodiscard]] virtual RefPtr<View> CreateView(i32 viewType) = 0;
        /// Bind data into a view for the given nodeId.
        ///
        /// To indent the row so its content clears the expander-chevron column, derive the offset
        /// from TreeView::ContentInset(depth) (or DraggableTreeView::ContentInset) - apply it as the
        /// row label's TextOffsetX or the row container's left Padding. NEVER hardcode a pixel
        /// constant: a literal that drifts from the tree's IndentWidth makes the chevron overlap the
        /// text (the recurring tree-indent bug). ContentInset is the single source of truth.
        virtual void BindView(View* view, i32 nodeId, i32 depth, bool isExpanded) = 0;
        /// View type for a node (for recycler pools). Default 0.
        [[nodiscard]] virtual i32 GetItemViewType(i32 nodeId) const
        {
            (void)nodeId;
            return 0;
        }
        /// Set the observer for data-change notifications (default no-op).
        virtual void SetObserver(ITreeAdapterObserver* observer) { (void)observer; }
    };
}
