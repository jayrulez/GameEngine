// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tree_model partition
//
// TreeModel: a concrete hierarchical Model - nodes with a text label and children. Modeled on
// eepp's tree models (role only). Nodes are stored in a flat array; a ModelIndex's InternalId
// is the node's array index, which the tree methods (Index / ParentIndex / RowCount(parent) /
// HasChildren) use to navigate. A removed node keeps its slot (it is detached, not erased), so
// node ids stay stable and a view keeps selection and expansion by id. Each edit tells the
// views the rows it touched under their parent.

module;
#include "Core/Prelude.h"

export module experimental.gui:tree_model;

import foundation.core; // Array, String, StringView, i32, i64
import :variant;
import :model_index;
import :model;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class TreeModel : public IModel
    {
    public:
        static constexpr i32 kRoot = -1; // parent id for a top-level node

        TreeModel() = default;

        // Add a node under `parentId` (kRoot for a top level node); returns the new node's id.
        i32 AddNode(i32 parentId, core::StringView text)
        {
            const i32 id = static_cast<i32>(m_nodes.Size());
            m_nodes.PushBack(Node{core::String(text), parentId, {}, false});
            Array<i32>& siblings = parentId == kRoot ? m_roots : m_nodes[static_cast<usize>(parentId)].Children;
            siblings.PushBack(id);
            NotifyRowsInserted(IndexOfNode(parentId), static_cast<i32>(siblings.Size()) - 1, 1);
            return id;
        }
        /// Removes a node and what is under it (their ids are not reused).
        void RemoveNode(i32 nodeId)
        {
            if (!IsLive(nodeId))
                return;
            const i32 parentId = m_nodes[static_cast<usize>(nodeId)].Parent;
            const ModelIndex parent = IndexOfNode(parentId);
            const i32 row = RowOf(nodeId);
            Array<i32>& siblings = parentId == kRoot ? m_roots : m_nodes[static_cast<usize>(parentId)].Children;
            siblings.RemoveAt(static_cast<usize>(row));
            Detach(nodeId);
            NotifyRowsRemoved(parent, row, 1);
        }
        void SetText(i32 nodeId, core::StringView text)
        {
            if (!IsLive(nodeId))
                return;
            m_nodes[static_cast<usize>(nodeId)].Text = core::String(text);
            NotifyRowsChanged(IndexOfNode(m_nodes[static_cast<usize>(nodeId)].Parent), RowOf(nodeId), 1);
        }
        void Clear()
        {
            m_nodes.Clear();
            m_roots.Clear();
            NotifyReset();
        }
        /// The index of a node (invalid for kRoot or a removed node).
        [[nodiscard]] ModelIndex IndexOfNode(i32 nodeId) const
        {
            return IsLive(nodeId) ? MakeModelIndex(RowOf(nodeId), 0, nodeId) : ModelIndex{};
        }

        [[nodiscard]] core::StringView TextOf(i32 nodeId) const
        {
            return (nodeId >= 0 && static_cast<usize>(nodeId) < m_nodes.Size())
                       ? m_nodes[static_cast<usize>(nodeId)].Text.AsView()
                       : core::StringView{};
        }

        [[nodiscard]] usize ColumnCount() const override { return 1; }

        [[nodiscard]] usize RowCount(const ModelIndex& parent = {}) const override
        {
            if (!parent.IsValid())
                return m_roots.Size();
            const i32 id = static_cast<i32>(parent.InternalId);
            return (id >= 0 && static_cast<usize>(id) < m_nodes.Size())
                       ? m_nodes[static_cast<usize>(id)].Children.Size()
                       : 0;
        }

        [[nodiscard]] ModelIndex Index(i32 row, i32 column = 0,
                                       const ModelIndex& parent = {}) const override
        {
            const i32 childId = ChildIdAt(parent, row);
            if (childId < 0)
                return ModelIndex{};
            return MakeModelIndex(row, column, childId);
        }

        [[nodiscard]] ModelIndex ParentIndex(const ModelIndex& child) const override
        {
            const i32 id = static_cast<i32>(child.InternalId);
            if (id < 0 || static_cast<usize>(id) >= m_nodes.Size())
                return ModelIndex{};
            const i32 parentId = m_nodes[static_cast<usize>(id)].Parent;
            if (parentId == kRoot)
                return ModelIndex{}; // top-level: no parent
            return MakeModelIndex(RowOf(parentId), 0, parentId);
        }

        [[nodiscard]] bool HasChildren(const ModelIndex& parent = {}) const override
        {
            return RowCount(parent) > 0;
        }

        [[nodiscard]] Variant Data(const ModelIndex& index,
                                   ModelRole role = ModelRole::Display) const override
        {
            const i32 id = static_cast<i32>(index.InternalId);
            if (id < 0 || static_cast<usize>(id) >= m_nodes.Size())
                return Variant{};
            if (role == ModelRole::Display || role == ModelRole::Sort)
                return Variant(m_nodes[static_cast<usize>(id)].Text.AsView());
            return Variant{};
        }

    private:
        struct Node
        {
            core::String Text;
            i32 Parent = kRoot;
            Array<i32> Children;
            bool Removed = false; // detached: its slot kept so ids stay stable
        };

        [[nodiscard]] i32 ChildIdAt(const ModelIndex& parent, i32 row) const
        {
            if (!parent.IsValid())
                return (row >= 0 && static_cast<usize>(row) < m_roots.Size())
                           ? m_roots[static_cast<usize>(row)]
                           : -1;
            const i32 id = static_cast<i32>(parent.InternalId);
            if (id < 0 || static_cast<usize>(id) >= m_nodes.Size())
                return -1;
            const Array<i32>& children = m_nodes[static_cast<usize>(id)].Children;
            return (row >= 0 && static_cast<usize>(row) < children.Size())
                       ? children[static_cast<usize>(row)]
                       : -1;
        }

        // The row index of `nodeId` within its parent's child list (or root list).
        [[nodiscard]] i32 RowOf(i32 nodeId) const
        {
            if (nodeId < 0 || static_cast<usize>(nodeId) >= m_nodes.Size())
                return -1;
            const i32 parentId = m_nodes[static_cast<usize>(nodeId)].Parent;
            const Array<i32>& siblings =
                (parentId == kRoot) ? m_roots : m_nodes[static_cast<usize>(parentId)].Children;
            for (usize i = 0; i < siblings.Size(); ++i)
                if (siblings[i] == nodeId)
                    return static_cast<i32>(i);
            return -1;
        }

        [[nodiscard]] bool IsLive(i32 nodeId) const
        {
            return nodeId >= 0 && static_cast<usize>(nodeId) < m_nodes.Size() && !m_nodes[static_cast<usize>(nodeId)].Removed;
        }
        void Detach(i32 nodeId)
        {
            Node& node = m_nodes[static_cast<usize>(nodeId)];
            node.Removed = true;
            for (i32 child : node.Children)
                Detach(child);
            node.Children.Clear();
        }

        Array<Node> m_nodes;
        Array<i32> m_roots;
    };
}
