// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :list_model partition
//
// ListModel: a list of texts that a script makes and fills, and that a list, table or tree view
// shows through setModel. It is a reference-counted object over a StringListModel, so a script
// can hold it and a view keeps it alive while showing it. Its edits reach the views the same way
// StringListModel's do: as a row inserted, removed or changed, not as a reset.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:list_model;

import foundation.core;
import :string_list_model;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class ListModel : public Object
    {
        RTTI_OBJECT(ListModel, Object)
    public:
        void Add(core::StringView text) { m_model.AddItem(text); }
        void Insert(i32 row, core::StringView text) { m_model.InsertItem(static_cast<usize>(core::Max(row, 0)), text); }
        void Remove(i32 row)
        {
            if (row >= 0)
                m_model.RemoveItem(static_cast<usize>(row));
        }
        void Set(i32 row, core::StringView text)
        {
            if (row >= 0)
                m_model.SetItem(static_cast<usize>(row), text);
        }
        /// The row's text (empty past the end).
        [[nodiscard]] core::String Get(i32 row) const
        {
            return row >= 0 && static_cast<usize>(row) < m_model.RowCount() ? core::String(m_model.ItemAt(static_cast<usize>(row)))
                                                                             : core::String{};
        }
        [[nodiscard]] i32 Count() const { return static_cast<i32>(m_model.RowCount()); }
        void Clear() { m_model.Clear(); }

        [[nodiscard]] StringListModel& Model() noexcept { return m_model; }

    private:
        StringListModel m_model;
    };
    // StaticType: REFLECT_MEMBERS(ListModel), which defines the script surface, is in
    // Scene/GUIReflectionImpl.cpp.
}
