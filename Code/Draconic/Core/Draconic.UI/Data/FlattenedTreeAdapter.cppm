// Draconic UI - :flattened_tree_adapter partition
//
// Wraps an ITreeAdapter to present as an IListAdapter for ListView virtualization: maintains expansion
// state and a flat list of currently-visible nodes (in display order). Ported from Sedulous.UI/src/Data/
// FlattenedTreeAdapter.bf. mSource is BORROWED (pattern-B). Beef List<int32>/HashSet<int32> -> core
// Array<i32>/HashSet<i32>; CreateView returns RefPtr<View>.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:flattened_tree_adapter;

import draconic.foundation;
import :view;
import :ilist_adapter;
import :itree_adapter;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class FlattenedTreeAdapter : public IListAdapter, public ITreeAdapterObserver
    {
    public:
        explicit FlattenedTreeAdapter(ITreeAdapter* source) : m_source(source)
        {
            m_source->SetObserver(this);
            RebuildVisibleList();
        }

        [[nodiscard]] ITreeAdapter* Source() const noexcept { return m_source; }

        // === IListAdapter ===
        void SetObserver(IListAdapterObserver* observer) override { m_observer = observer; }
        [[nodiscard]] i32 ItemCount() const override
        {
            return static_cast<i32>(m_visibleNodes.Size());
        }
        [[nodiscard]] i32 GetItemViewType(i32 position) const override
        {
            if (position < 0 || position >= static_cast<i32>(m_visibleNodes.Size()))
            {
                return 0;
            }
            return m_source->GetItemViewType(m_visibleNodes[static_cast<usize>(position)]);
        }
        [[nodiscard]] RefPtr<View> CreateView(i32 viewType) override
        {
            return m_source->CreateView(viewType);
        }
        void BindView(View* view, i32 position) override
        {
            if (position < 0 || position >= static_cast<i32>(m_visibleNodes.Size()))
            {
                return;
            }
            const i32 nodeId = m_visibleNodes[static_cast<usize>(position)];
            const i32 depth = m_depths[static_cast<usize>(position)];
            const bool expanded = m_expanded.Contains(nodeId);
            m_source->BindView(view, nodeId, depth, expanded);
        }

        // === Expansion ===
        [[nodiscard]] bool IsExpanded(i32 nodeId) const { return m_expanded.Contains(nodeId); }

        void ToggleExpand(i32 nodeId)
        {
            if (m_expanded.Contains(nodeId))
            {
                m_expanded.Remove(nodeId);
            }
            else if (m_source->HasChildren(nodeId))
            {
                m_expanded.Insert(nodeId);
            }
            RebuildVisibleList();
        }
        void Expand(i32 nodeId)
        {
            if (!m_expanded.Contains(nodeId) && m_source->HasChildren(nodeId))
            {
                m_expanded.Insert(nodeId);
                RebuildVisibleList();
            }
        }
        void Collapse(i32 nodeId)
        {
            if (m_expanded.Remove(nodeId))
            {
                RebuildVisibleList();
            }
        }

        [[nodiscard]] i32 GetNodeId(i32 position) const
        {
            if (position < 0 || position >= static_cast<i32>(m_visibleNodes.Size()))
            {
                return -1;
            }
            return m_visibleNodes[static_cast<usize>(position)];
        }
        [[nodiscard]] i32 GetDepth(i32 position) const
        {
            if (position < 0 || position >= static_cast<i32>(m_depths.Size()))
            {
                return 0;
            }
            return m_depths[static_cast<usize>(position)];
        }

        void GetExpandedNodes(HashSet<i32>& output) const
        {
            for (i32 nodeId : m_expanded)
            {
                output.Insert(nodeId);
            }
        }
        void SetExpandedNodes(const HashSet<i32>& nodes)
        {
            m_expanded.Clear();
            for (i32 nodeId : nodes)
            {
                if (m_source->HasChildren(nodeId))
                {
                    m_expanded.Insert(nodeId);
                }
            }
            RebuildVisibleList();
        }

        /// Rebuild the flat visible list by walking expanded nodes; notifies the observer.
        void RebuildVisibleList()
        {
            m_visibleNodes.Clear();
            m_depths.Clear();
            const i32 rootCount = m_source->RootCount();
            for (i32 i = 0; i < rootCount; ++i)
            {
                AddNodeRecursive(m_source->GetChildId(-1, i), 0);
            }
            if (m_observer != nullptr)
            {
                m_observer->OnDataSetChanged();
            }
        }

        // === ITreeAdapterObserver ===
        void OnTreeDataChanged() override { RebuildVisibleList(); }

    private:
        void AddNodeRecursive(i32 nodeId, i32 depth)
        {
            m_visibleNodes.PushBack(nodeId);
            m_depths.PushBack(depth);
            if (m_expanded.Contains(nodeId))
            {
                const i32 childCount = m_source->GetChildCount(nodeId);
                for (i32 i = 0; i < childCount; ++i)
                {
                    AddNodeRecursive(m_source->GetChildId(nodeId, i), depth + 1);
                }
            }
        }

        ITreeAdapter* m_source; // borrowed
        Array<i32> m_visibleNodes;
        Array<i32> m_depths;
        HashSet<i32> m_expanded;
        IListAdapterObserver* m_observer = nullptr;
    };
}
