// Draconic UI Toolkit - :int_editor partition
//
// Integer property editor - a NumericField with 0 decimal places and focus-based edit transactions. Ported
// from Sedulous.UI.Toolkit/src/PropertyGrid/IntEditor.bf. Beef `int64` -> i64; `delegate void(int64)
// Setter` -> Function<void(i64)>; `int64.MinValue/MaxValue` -> std::numeric_limits. The private inner
// `IntEditorField : NumericField` becomes a PUBLIC nested class (own DRACONIC_OBJECT identity) whose
// focus/key overrides are defined out-of-line after IntEditor is complete.

module;
#include <limits>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:int_editor;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import :property_editor;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Integer property editor - NumericField with 0 decimal places.
    class IntEditor : public PropertyEditor
    {
        DRACONIC_OBJECT(IntEditor, PropertyEditor)
    public:
        Function<void(i64)> Setter;

        IntEditor(StringView name, i64 initialValue, i64 min = std::numeric_limits<i64>::min(),
                  i64 max = std::numeric_limits<i64>::max(), Function<void(i64)> setter = {},
                  StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(initialValue),
              m_min(static_cast<f64>(min)), m_max(static_cast<f64>(max))
        {
        }

        [[nodiscard]] i64 Value() const noexcept { return m_value; }
        void SetValue(i64 value)
        {
            m_value = value;
            if (!m_syncing)
            {
                RefreshView();
            }
        }

        void RefreshView() override
        {
            if (m_field != nullptr && !m_syncing)
            {
                m_syncing = true;
                m_field->SetValue(static_cast<f64>(m_value));
                m_syncing = false;
            }
        }

        /// NumericField subclass that tracks edit transactions via focus.
        class IntEditorField : public NumericField
        {
            DRACONIC_OBJECT(IntEditorField, NumericField)
        public:
            explicit IntEditorField(IntEditor* editor) : m_editor(editor) {}

            void OnFocusGained() override;
            void OnFocusLost() override;
            void OnKeyDown(KeyEventArgs& e) override;

        private:
            IntEditor* m_editor;
        };

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<IntEditorField> field = MakeRef<IntEditorField>(DefaultAllocator(), this);
            field->AddClass(u8"property-field");
            m_field = field.Get();
            m_field->SetMin(m_min);
            m_field->SetMax(m_max);
            m_field->SetStep(1);
            m_field->SetDecimalPlaces(0);
            m_field->SetValue(static_cast<f64>(m_value));
            IntEditor* self = this;
            m_field->OnValueChanged.Add(
                [self](NumericField*, f64 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value = static_cast<i64>(val);
                        if (self->Setter)
                        {
                            self->Setter(self->m_value);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            return field;
        }

    private:
        i64 m_value;
        f64 m_min;
        f64 m_max;
        f64 m_preEditValue = 0.0;
        NumericField* m_field = nullptr; // borrowed; the editor view RefPtr owns it
        bool m_syncing = false;
    };

    // === Inner-view out-of-line bodies (need the complete IntEditor type) ===

    inline void IntEditor::IntEditorField::OnFocusGained()
    {
        NumericField::OnFocusGained();
        m_editor->m_preEditValue = static_cast<f64>(m_editor->m_value);
        m_editor->BeginEdit();
    }

    inline void IntEditor::IntEditorField::OnFocusLost()
    {
        NumericField::OnFocusLost();
        if (m_editor->IsEditing())
        {
            m_editor->EndEdit();
        }
    }

    inline void IntEditor::IntEditorField::OnKeyDown(KeyEventArgs& e)
    {
        if (e.Key == KeyCode::Escape && m_editor->IsEditing())
        {
            m_editor->m_value = static_cast<i64>(m_editor->m_preEditValue);
            SetValue(m_editor->m_preEditValue);
            if (m_editor->Setter)
            {
                m_editor->Setter(m_editor->m_value);
            }
            m_editor->CancelEdit();
            e.Handled = true;
            return;
        }
        NumericField::OnKeyDown(e);
    }

    DRACONIC_DEFINE_OBJECT(IntEditor, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(IntEditor::IntEditorField, "draconic::ui::toolkit")
}
