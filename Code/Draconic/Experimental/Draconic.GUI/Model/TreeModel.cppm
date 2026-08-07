// Draconic GUI - :tree_model partition
//
// TreeModel: a concrete hierarchical Model - nodes with a text label and children. Modeled on
// eepp's tree models (role only). Nodes are stored in a flat array; a ModelIndex's InternalId
// is the node's array index, which the tree methods (Index / ParentIndex / RowCount(parent) /
// HasChildren) use to navigate. Node ids are stable (nodes are not removed in this v1), so a
// view can track selection/expansion by id across expand/collapse.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:tree_model;

import draconic.foundation; // Array, String, StringView, i32, i64
import :variant;
import :model_index;
import :model;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class TreeModel : public IModel
    {
    public:
        static constexpr i32 kRoot = -1; // parent id for a top-level node

        TreeModel() = default;

        // Add a node under `parentId` (kRoot for a top level node); returns the new node's id.
        i32 AddNode(i32 parentId, foundation::StringView text)
        {
            const i32 id = static_cast<i32>(m_nodes.Size());
            m_nodes.PushBack(Node{foundation::String(text), parentId, {}});
            if (parentId == kRoot)
                m_roots.PushBack(id);
            else
                m_nodes[static_cast<usize>(parentId)].Children.PushBack(id);
            return id;
        }
        void Clear()
        {
            m_nodes.Clear();
            m_roots.Clear();
            DidUpdate();
        }
        void Refresh() { DidUpdate(); } // call after a batch of AddNode

        [[nodiscard]] foundation::StringView TextOf(i32 nodeId) const
        {
            return (nodeId >= 0 && static_cast<usize>(nodeId) < m_nodes.Size())
                       ? m_nodes[static_cast<usize>(nodeId)].Text.AsView()
                       : foundation::StringView{};
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
            foundation::String Text;
            i32 Parent = kRoot;
            Array<i32> Children;
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

        Array<Node> m_nodes;
        Array<i32> m_roots;
    };
}
