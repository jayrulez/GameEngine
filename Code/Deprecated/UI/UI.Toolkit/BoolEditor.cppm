// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :bool_editor partition
//
// Boolean property editor - a CheckBox. Instant edit: BeginEdit + value change + EndEdit on each toggle.
// Ported from Sedulous.UI.Toolkit/src/PropertyGrid/BoolEditor.bf. Beef `delegate void(bool) Setter` ->
// Function<void(bool)>; `mCheckBox.IsChecked.Value = v` -> IsChecked.SetValue(v); owned `new CheckBox` ->
// a RefPtr returned as the editor view (borrowed raw kept for RefreshView).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:bool_editor;

import foundation.core;
import foundation.vg;
import foundation.ui;
import :property_editor;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    /// Boolean property editor - CheckBox.
    class BoolEditor : public PropertyEditor
    {
        RTTI_OBJECT(BoolEditor, PropertyEditor)
    public:
        Function<void(bool)> Setter;

        BoolEditor(StringView name, bool initialValue, Function<void(bool)> setter = {},
                   StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(initialValue)
        {
        }

        [[nodiscard]] bool Value() const noexcept { return m_value; }
        void SetValue(bool value)
        {
            m_value = value;
            if (m_checkBox != nullptr)
            {
                m_checkBox->IsChecked.SetValue(value);
            }
        }

        void RefreshView() override
        {
            if (m_checkBox != nullptr)
            {
                m_checkBox->IsChecked.SetValue(m_value);
            }
        }

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<CheckBox> checkBox = MakeRef<CheckBox>(MemoryAllocator());
            m_checkBox = checkBox.Get();
            m_checkBox->IsChecked.SetValue(m_value);
            BoolEditor* self = this;
            m_checkBox->OnCheckedChanged.Add(
                [self](CheckBox*, bool val)
                {
                    self->BeginEdit();
                    self->m_value = val;
                    if (self->Setter)
                    {
                        self->Setter(val);
                    }
                    self->NotifyValueChanged();
                    self->EndEdit();
                });
            return checkBox;
        }

    private:
        bool m_value;
        CheckBox* m_checkBox = nullptr; // borrowed; the editor view RefPtr owns it
    };

    RTTI_DEFINE_OBJECT(BoolEditor, "rtti::ui::toolkit")
}
