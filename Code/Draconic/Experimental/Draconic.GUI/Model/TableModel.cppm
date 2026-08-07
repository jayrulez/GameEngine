// Draconic GUI - :table_model partition
//
// TableModel: a concrete multi-column Model - named columns and a grid of Variant cells.
// Modeled on eepp's item/table models (role only). Backs a TableView; each cell can be any
// Variant (string/number/bool), so numeric columns sort by value (via Variant::Compare) rather
// than lexicographically. Mutating it notifies attached views via DidUpdate.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:table_model;

import draconic.foundation; // Array, String, StringView, Move
import :variant;
import :model_index;
import :model;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class TableModel : public IModel
    {
    public:
        TableModel() = default;

        void SetColumns(Array<foundation::String> columns)
        {
            m_columns = foundation::Move(columns);
            DidUpdate();
        }
        void AddRow(Array<Variant> row)
        {
            m_rows.PushBack(foundation::Move(row));
            DidUpdate();
        }
        void Clear()
        {
            m_rows.Clear();
            DidUpdate();
        }

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
        [[nodiscard]] foundation::String ColumnName(usize column) const override
        {
            return column < m_columns.Size() ? m_columns[column] : foundation::String{};
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

    private:
        Array<foundation::String> m_columns;
        Array<Array<Variant>> m_rows;
    };
}
