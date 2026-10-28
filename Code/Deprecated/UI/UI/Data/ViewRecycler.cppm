// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :view_recycler partition
//
// Pool of views keyed by view type: recycles views that scroll out of the viewport so ListView doesn't
// allocate per scroll. Ported from Sedulous.UI/src/Data/ViewRecycler.bf. Beef Dictionary<int32,
// List<View>> + manual delete -> HashMap<i32, Array<RefPtr<View>>> (RAII); Acquire moves a ref OUT of
// the pool, Recycle moves one IN (ownership transfers via RefPtr, no manual delete).

module;
#include "Core/Prelude.h"

export module foundation.ui:view_recycler;

import foundation.core; // HashMap, Array, RefPtr, Move
import :view;
import :ilist_adapter;

using namespace foundation::core;

export namespace foundation::ui
{
    class ViewRecycler
    {
    public:
        [[nodiscard]] i32 CreatedCount() const noexcept { return m_createdCount; }
        [[nodiscard]] i32 RecycledCount() const noexcept { return m_recycledCount; }
        [[nodiscard]] i32 ReusedCount() const noexcept { return m_reusedCount; }

        /// Try to get a recycled view of the given type; null RefPtr if none pooled.
        [[nodiscard]] RefPtr<View> Acquire(i32 viewType)
        {
            if (Array<RefPtr<View>>* pool = m_pools.Find(viewType);
                pool != nullptr && pool->Size() > 0)
            {
                RefPtr<View> view = Move(pool->Back());
                pool->PopBack();
                m_reusedCount++;
                return view;
            }
            return {};
        }

        /// Return a view to the pool for reuse.
        void Recycle(RefPtr<View> view, i32 viewType)
        {
            Array<RefPtr<View>>* pool = m_pools.Find(viewType);
            if (pool == nullptr)
            {
                m_pools.InsertOrAssign(viewType, Array<RefPtr<View>>{});
                pool = m_pools.Find(viewType);
            }
            pool->PushBack(Move(view));
            m_recycledCount++;
        }

        /// Get or create a view via the adapter, reusing from the pool when possible.
        [[nodiscard]] RefPtr<View> GetOrCreate(IListAdapter& adapter, i32 position)
        {
            const i32 viewType = adapter.GetItemViewType(position);
            RefPtr<View> view = Acquire(viewType);
            if (!view)
            {
                view = adapter.CreateView(viewType);
                m_createdCount++;
            }
            adapter.BindView(view.Get(), position);
            return view;
        }

        /// Clear all pools (releases pooled views).
        void Clear()
        {
            for (auto& kv : m_pools)
            {
                kv.value.Clear();
            }
        }

    private:
        HashMap<i32, Array<RefPtr<View>>> m_pools;
        i32 m_createdCount = 0;
        i32 m_recycledCount = 0;
        i32 m_reusedCount = 0;
    };
}
