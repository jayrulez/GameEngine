// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :table_model partition
//
// TableModel: a concrete multi-column Model of named columns and a grid of Variant cells, each row
// with a stable id. Each cell can be any Variant (string, number, bool), so numeric columns sort by
// value (Variant::Compare) rather than as text. Each edit tells the views which rows it touched;
// SetColumns and Clear reset the views.

module;
#include "Core/Prelude.h"

export module foundation.gui:table_model;

import foundation.core;
import :variant;
import :model_index;
import :model;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class TableModel : public IModel
    {
    public:
        TableModel() = default;

        void SetColumns(Array<core::String> columns)
        {
            m_columns = core::Move(columns);
            NotifyReset();
        }
        void AddRow(Array<Variant> row) { InsertRow(m_rows.Size(), core::Move(row)); }
        void InsertRow(usize at, Array<Variant> row)
        {
            at = core::Min(at, m_rows.Size());
            m_rows.Insert(at, core::Move(row));
            m_ids.Insert(at);
            NotifyRowsInserted(ModelIndex{}, static_cast<i32>(at), 1);
        }
        void RemoveRow(usize row)
        {
            if (row >= m_rows.Size())
                return;
            m_rows.RemoveAt(row);
            m_ids.Remove(row);
            NotifyRowsRemoved(ModelIndex{}, static_cast<i32>(row), 1);
        }
        void SetCell(usize row, usize column, Variant value)
        {
            if (row >= m_rows.Size())
                return;
            while (m_rows[row].Size() <= column)
                m_rows[row].PushBack(Variant{});
            m_rows[row][column] = core::Move(value);
            NotifyRowsChanged(ModelIndex{}, static_cast<i32>(row), 1);
        }
        void Clear()
        {
            m_rows.Clear();
            m_ids.Reset(0);
            NotifyReset();
        }

        /// Sets whether a view may edit the cells in place.
        void SetEditable(bool editable) noexcept { m_editable = editable; }

        // The raw cell (no display conversion); empty if out of range.
        [[nodiscard]] Variant Cell(usize row, usize column) const
        {
            if (row >= m_rows.Size() || column >= m_rows[row].Size())
                return Variant{};
            return m_rows[row][column];
        }

        [[nodiscard]] usize RowCount(const ModelIndex& parent = {}) const override
        {
            return parent.IsValid() ? 0 : m_rows.Size();
        }
        [[nodiscard]] usize ColumnCount() const override { return m_columns.Size(); }
        [[nodiscard]] core::String ColumnName(usize column) const override
        {
            return column < m_columns.Size() ? m_columns[column] : core::String{};
        }
        [[nodiscard]] Variant Data(const ModelIndex& index,
                                   ModelRole role = ModelRole::Display) const override
        {
            if (!IsValidIndex(index))
                return Variant{};
            if (role == ModelRole::Display || role == ModelRole::Sort)
                return Cell(static_cast<usize>(index.Row), static_cast<usize>(index.Column));
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
            // An edit keeps the cell's type, so a number column accepts only numbers.
            const Variant before = Cell(static_cast<usize>(index.Row), static_cast<usize>(index.Column));
            Variant next = value;
            if (before.GetType() == Variant::Type::Int || before.GetType() == Variant::Type::Float)
            {
                const Optional<f64> number = core::ParseFloat(value.ToString().AsView());
                if (!number.HasValue())
                    return false;
                next = before.GetType() == Variant::Type::Int ? Variant(static_cast<i64>(number.Value())) : Variant(number.Value());
            }
            SetCell(static_cast<usize>(index.Row), static_cast<usize>(index.Column), core::Move(next));
            return true;
        }

    private:
        Array<core::String> m_columns;
        Array<Array<Variant>> m_rows;
        RowIds m_ids;
        bool m_editable = false;
    };
}
