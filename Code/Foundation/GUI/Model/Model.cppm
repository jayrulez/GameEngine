// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :model partition
//
// IModel: the abstract data source a model-backed view renders. A model exposes rows of cells
// under a parent (RowCount x ColumnCount; a flat model has only the top level), returns a Variant
// per (index, role), and tells its clients (the views) what changed: rows inserted, removed, moved
// or changed under a parent, or a reset when it cannot say. A client that does not follow ranges
// treats each change as a reset. Each row keeps the same id while other rows come and go, so a
// view stores its selection and expansion as sets of ids. Editing (IsEditable, SetData) and
// sorting (Sort) are optional capabilities.

module;
#include "Core/Prelude.h"

export module foundation.gui:model;

import foundation.core;
import :variant;
import :model_index;
import :event; // DragPayload, DropEffect (a drop on a view)

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// A row's identity. It stays the same while other rows are inserted, removed and moved.
    using ItemId = u64;
    inline constexpr ItemId kNoItem = ~ItemId{0};

    enum class SortOrder
    {
        None,
        Ascending,
        Descending
    };

    class IModel;

    /// Where in a row a drop lands: before it, onto it (a tree's child), or after it.
    enum class DropPlace : u8
    {
        Before,
        On,
        After,
    };

    /// A drop on a view, as the view passes it to its model: what is dropped, where (a row and a
    /// place; an invalid row means the end of the top level), with which effect, and, when the
    /// drag came from a view, the ids of the dragged rows and the model they came from.
    struct ModelDrop
    {
        const DragPayload* Payload = nullptr;
        ModelIndex Index;
        DropPlace Place = DropPlace::After;
        DropEffect Effect = DropEffect::Move;
        core::Array<ItemId> Ids;
        const IModel* Source = nullptr;
    };

    // What a model tells a view. Each range callback defaults to a reset, so a client overrides
    // only the ones it can follow.
    class IModelClient
    {
    public:
        virtual ~IModelClient() = default;
        /// Anything may have changed: read it all again.
        virtual void OnModelReset() = 0;
        /// `count` rows were inserted under `parent`, the first now at `first`.
        virtual void OnRowsInserted(const ModelIndex& parent, i32 first, i32 count)
        {
            (void)parent, (void)first, (void)count;
            OnModelReset();
        }
        /// `count` rows were removed under `parent`, the first having been at `first`.
        virtual void OnRowsRemoved(const ModelIndex& parent, i32 first, i32 count)
        {
            (void)parent, (void)first, (void)count;
            OnModelReset();
        }
        /// `count` rows under `parent` that were at `first` now start at `to` (in the order after
        /// the move).
        virtual void OnRowsMoved(const ModelIndex& parent, i32 first, i32 count, i32 to)
        {
            (void)parent, (void)first, (void)count, (void)to;
            OnModelReset();
        }
        /// The data of `count` rows under `parent` from `first` changed (not their number or order).
        virtual void OnRowsChanged(const ModelIndex& parent, i32 first, i32 count)
        {
            (void)parent, (void)first, (void)count;
            OnModelReset();
        }
    };

    class IModel
    {
    public:
        virtual ~IModel() = default;

        // Number of rows under `parent` (an invalid parent is the top level). Flat models
        // ignore the parent (they have only a top level).
        [[nodiscard]] virtual usize RowCount(const ModelIndex& parent = {}) const = 0;
        [[nodiscard]] virtual usize ColumnCount() const = 0;
        [[nodiscard]] virtual core::String ColumnName(usize /*column*/) const
        {
            return core::String{};
        }
        [[nodiscard]] virtual Variant Data(const ModelIndex& index,
                                           ModelRole role = ModelRole::Display) const = 0;

        // === Identity ===
        /// Returns the id of the row that `index` is in. By default this is a tree's node id
        /// (InternalId), or else the row number; a model whose rows move should override it with
        /// stable ids.
        [[nodiscard]] virtual ItemId IdOf(const ModelIndex& index) const
        {
            if (!index.IsValid())
                return kNoItem;
            return index.InternalId >= 0 ? static_cast<ItemId>(index.InternalId) : static_cast<ItemId>(index.Row);
        }
        /// Returns the top-level row whose id is `id`, or an invalid index when there is none. A
        /// tree view finds its nodes itself.
        [[nodiscard]] virtual ModelIndex IndexOfId(ItemId id) const
        {
            for (usize r = 0; r < RowCount(); ++r)
            {
                const ModelIndex index = Index(static_cast<i32>(r));
                if (IdOf(index) == id)
                    return index;
            }
            return ModelIndex{};
        }

        // === Capabilities ===
        [[nodiscard]] virtual bool IsEditable(const ModelIndex&) const { return false; }
        /// Whether a user may select (and activate) the item. It is false for a list's section
        /// header, which clicks, the arrow keys, type-ahead and Select All skip.
        [[nodiscard]] virtual bool IsSelectable(const ModelIndex&) const { return true; }
        /// Writes an edited value; false when it is refused (the editor then stays open).
        virtual bool SetData(const ModelIndex&, const Variant&) { return false; }
        /// Sorts by `column` (SortOrder::None restores the source's order). Returns false when the
        /// model cannot sort.
        virtual bool Sort(usize column, SortOrder order)
        {
            (void)column, (void)order;
            return false;
        }

        /// Returns the effect a drop would have, or None. A None with a reason refuses the drop
        /// and says why; a None with no reason means the model does not take drops at that place,
        /// and the view tries another one.
        [[nodiscard]] virtual DropEffect CanDrop(const ModelDrop& drop, core::String& reason) const
        {
            (void)drop, (void)reason;
            return DropEffect::None;
        }
        /// Performs the drop; returns false when it could not.
        virtual bool Drop(const ModelDrop& drop)
        {
            (void)drop;
            return false;
        }
        /// Removes rows by id, used when a view has moved those rows into another model. Returns
        /// false when the model cannot remove them.
        virtual bool RemoveIds(const core::Array<ItemId>& ids)
        {
            (void)ids;
            return false;
        }

        // === Tree structure (flat defaults; a tree model overrides these) ===
        // The index of the cell at (row, column) under `parent`.
        [[nodiscard]] virtual ModelIndex Index(i32 row, i32 column = 0,
                                               const ModelIndex& /*parent*/ = {}) const
        {
            return MakeModelIndex(row, column);
        }
        // The parent of `child` (invalid for a top-level node; flat models have no parents).
        [[nodiscard]] virtual ModelIndex ParentIndex(const ModelIndex& /*child*/) const
        {
            return ModelIndex{};
        }
        // Whether `parent` has any child rows.
        [[nodiscard]] virtual bool HasChildren(const ModelIndex& parent = {}) const
        {
            return RowCount(parent) > 0;
        }

        [[nodiscard]] bool IsValidIndex(const ModelIndex& index) const
        {
            return index.Row >= 0 && static_cast<usize>(index.Row) < RowCount() &&
                   index.Column >= 0 && static_cast<usize>(index.Column) < ColumnCount();
        }

        // Client (view) registration. Clients are non-owning and must outlive the model or
        // unregister first.
        void AddClient(IModelClient* client)
        {
            if (client != nullptr)
                m_clients.PushBack(client);
        }
        void RemoveClient(IModelClient* client)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                if (m_clients[i] == client)
                {
                    m_clients.RemoveAt(i);
                    return;
                }
        }

        // === Telling the clients (after the change is made) ===
        void NotifyReset()
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnModelReset();
        }
        void NotifyRowsInserted(const ModelIndex& parent, i32 first, i32 count)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnRowsInserted(parent, first, count);
        }
        void NotifyRowsRemoved(const ModelIndex& parent, i32 first, i32 count)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnRowsRemoved(parent, first, count);
        }
        void NotifyRowsMoved(const ModelIndex& parent, i32 first, i32 count, i32 to)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnRowsMoved(parent, first, count, to);
        }
        void NotifyRowsChanged(const ModelIndex& parent, i32 first, i32 count)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnRowsChanged(parent, first, count);
        }

    protected:
        Array<IModelClient*> m_clients; // non-owning
    };

    /// Ids for a flat model's rows: one id per row, handed out in sequence, which moves with its
    /// row. RowOf finds an id's row with a lookup table, since an accessibility node asks for every
    /// item it describes; the table is rebuilt on the first call after the rows change.
    class RowIds
    {
    public:
        [[nodiscard]] usize Size() const noexcept { return m_ids.Size(); }
        [[nodiscard]] ItemId At(usize row) const { return row < m_ids.Size() ? m_ids[row] : kNoItem; }
        [[nodiscard]] i32 RowOf(ItemId id) const
        {
            if (m_stale)
            {
                m_rows.Clear();
                for (usize i = 0; i < m_ids.Size(); ++i)
                    m_rows.InsertOrAssign(m_ids[i], static_cast<i32>(i));
                m_stale = false;
            }
            const i32* row = m_rows.Find(id);
            return row != nullptr ? *row : -1;
        }
        void Reset(usize count)
        {
            m_ids.Clear();
            for (usize i = 0; i < count; ++i)
                m_ids.PushBack(m_next++);
            m_stale = true;
        }
        void Insert(usize row)
        {
            m_ids.Insert(row, m_next++);
            m_stale = true;
        }
        /// Puts an existing id back at `row` (a row moved by a drop keeps its id).
        void InsertId(usize row, ItemId id)
        {
            m_ids.Insert(row, id);
            m_stale = true;
        }
        void Remove(usize row)
        {
            m_ids.RemoveAt(row);
            m_stale = true;
        }
        void Move(usize from, usize to)
        {
            const ItemId id = m_ids[from];
            m_ids.RemoveAt(from);
            m_ids.Insert(to, id);
            m_stale = true;
        }

    private:
        Array<ItemId> m_ids;
        ItemId m_next = 0;
        mutable HashMap<ItemId, i32> m_rows; // id to row, as of the last RowOf
        mutable bool m_stale = true;
    };
}
