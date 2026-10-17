// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :string_list_model partition
//
// StringListModel: the simplest concrete Model - a single column of strings, one per row, each
// with an id of its own (eepp's ItemListModel in role). Its edits tell its views the rows they
// touch: an insert, a removal, a move or a change; SetItems and Clear reset them. Editable, a
// view may edit its rows in place.

module;
#include "Core/Prelude.h"

export module experimental.gui:string_list_model;

import foundation.core; // Array, String, StringView, Move
import :variant;
import :model_index;
import :model;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class StringListModel : public IModel
    {
    public:
        StringListModel() = default;
        explicit StringListModel(Array<core::String> items) : m_items(core::Move(items)) { m_ids.Reset(m_items.Size()); }

        void SetItems(Array<core::String> items)
        {
            m_items = core::Move(items);
            m_ids.Reset(m_items.Size());
            NotifyReset();
        }
        void AddItem(core::StringView item) { InsertItem(m_items.Size(), item); }
        void InsertItem(usize row, core::StringView item)
        {
            row = core::Min(row, m_items.Size());
            m_items.Insert(row, core::String(item));
            m_ids.Insert(row);
            NotifyRowsInserted(ModelIndex{}, static_cast<i32>(row), 1);
        }
        void RemoveItem(usize row)
        {
            if (row >= m_items.Size())
                return;
            m_items.RemoveAt(row);
            m_ids.Remove(row);
            NotifyRowsRemoved(ModelIndex{}, static_cast<i32>(row), 1);
        }
        void SetItem(usize row, core::StringView item)
        {
            if (row >= m_items.Size())
                return;
            m_items[row] = core::String(item);
            NotifyRowsChanged(ModelIndex{}, static_cast<i32>(row), 1);
        }
        void MoveItem(usize from, usize to)
        {
            if (from >= m_items.Size() || to >= m_items.Size() || from == to)
                return;
            core::String item = core::Move(m_items[from]);
            m_items.RemoveAt(from);
            m_items.Insert(to, core::Move(item));
            m_ids.Move(from, to);
            NotifyRowsMoved(ModelIndex{}, static_cast<i32>(from), 1, static_cast<i32>(to));
        }
        void Clear()
        {
            m_items.Clear();
            m_ids.Reset(0);
            NotifyReset();
        }

        /// Its rows may be edited in place.
        void SetEditable(bool editable) noexcept { m_editable = editable; }

        [[nodiscard]] core::StringView ItemAt(usize row) const
        {
            return row < m_items.Size() ? m_items[row].AsView() : core::StringView{};
        }

        [[nodiscard]] usize RowCount(const ModelIndex& parent = {}) const override
        {
            return parent.IsValid() ? 0 : m_items.Size();
        }
        [[nodiscard]] usize ColumnCount() const override { return 1; }
        [[nodiscard]] Variant Data(const ModelIndex& index,
                                   ModelRole role = ModelRole::Display) const override
        {
            if (!IsValidIndex(index))
                return Variant{};
            if (role == ModelRole::Display || role == ModelRole::Sort)
                return Variant(m_items[static_cast<usize>(index.Row)].AsView());
            return Variant{};
        }
        [[nodiscard]] ItemId IdOf(const ModelIndex& index) const override
        {
            return index.IsValid() ? m_ids.At(static_cast<usize>(index.Row)) : kNoItem;
        }
        [[nodiscard]] ModelIndex IndexOfId(ItemId id) const override
        {
            const i32 row = m_ids.RowOf(id);
            return row >= 0 ? MakeModelIndex(row) : ModelIndex{};
        }
        [[nodiscard]] bool IsEditable(const ModelIndex& index) const override { return m_editable && IsValidIndex(index); }
        bool SetData(const ModelIndex& index, const Variant& value) override
        {
            if (!IsEditable(index))
                return false;
            SetItem(static_cast<usize>(index.Row), value.ToString().AsView());
            return true;
        }

    private:
        Array<core::String> m_items;
        RowIds m_ids;
        bool m_editable = false;
    };
}
