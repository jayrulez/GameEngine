// Draconic UI - :ilist_adapter partition
//
// Data source contract for ListView (+ its observer). Ported from Sedulous.UI/src/Data/IListAdapter.bf.
// Pattern-B injected interfaces (held-by-reference, not tree-queried), so plain abstract classes; Beef
// interface default methods -> virtual methods with default bodies. CreateView returns RefPtr<View>
// (our RAII ownership; Beef returned a raw owned View).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:ilist_adapter;

import draconic.foundation; // RefPtr
import :view;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Observer for adapter data changes (ListView implements this).
    class IListAdapterObserver
    {
    public:
        virtual ~IListAdapterObserver() = default;
        /// Entire data set changed - rebuild everything.
        virtual void OnDataSetChanged() = 0;
        /// Items in [start, start+count) changed - rebind those views.
        virtual void OnItemRangeChanged(i32 start, i32 count) = 0;
    };

    /// Data source for ListView. Owns both view creation and data binding; supports multiple view types.
    class IListAdapter
    {
    public:
        virtual ~IListAdapter() = default;

        /// Total number of items.
        [[nodiscard]] virtual i32 ItemCount() const = 0;
        /// View type for an item (for recycling pools). Default 0.
        [[nodiscard]] virtual i32 GetItemViewType(i32 position) const
        {
            (void)position;
            return 0;
        }
        /// Create a new view for the given type (owning ref).
        [[nodiscard]] virtual RefPtr<View> CreateView(i32 viewType) = 0;
        /// Bind data at `position` into an existing view.
        virtual void BindView(View* view, i32 position) = 0;
        /// Number of distinct view types (for recycler pool sizing). Default 1.
        [[nodiscard]] virtual i32 ViewTypeCount() const { return 1; }
        /// Height for a specific item; <= 0 uses ListView.ItemHeight (variable-height override).
        [[nodiscard]] virtual f32 GetItemHeight(i32 position) const
        {
            (void)position;
            return -1.0f;
        }
        /// Set the observer for data-change notifications.
        virtual void SetObserver(IListAdapterObserver* observer) = 0;
    };

    /// Base class for adapters with built-in observer support.
    class ListAdapterBase : public IListAdapter
    {
    public:
        void SetObserver(IListAdapterObserver* observer) override { m_observer = observer; }

        /// Notify that the entire data set changed.
        void NotifyDataSetChanged()
        {
            if (m_observer != nullptr)
            {
                m_observer->OnDataSetChanged();
            }
        }
        /// Notify that items in [start, start+count) changed.
        void NotifyRangeChanged(i32 start, i32 count)
        {
            if (m_observer != nullptr)
            {
                m_observer->OnItemRangeChanged(start, count);
            }
        }

    private:
        IListAdapterObserver* m_observer = nullptr;
    };
}
