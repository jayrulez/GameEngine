// Draconic UI Toolkit - :float_editor partition
//
// Float property editor - a NumericField with focus-based edit transactions (Escape restores the pre-edit
// value). Ported from Sedulous.UI.Toolkit/src/PropertyGrid/FloatEditor.bf. Beef `double` -> f64; `delegate
// void(double) Setter` -> Function<void(f64)>. The private inner `FloatEditorField : NumericField` becomes
// a PUBLIC nested class (own DRACONIC_OBJECT identity) whose focus/key overrides are defined out-of-line
// after FloatEditor is complete.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:float_editor;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import :property_editor;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Float property editor - NumericField with focus-based edit transactions.
    class FloatEditor : public PropertyEditor
    {
        DRACONIC_OBJECT(FloatEditor, PropertyEditor)
    public:
        Function<void(f64)> Setter;

        FloatEditor(StringView name, f64 initialValue, f64 min = -1e9, f64 max = 1e9,
                    f64 step = 0.1, i32 decimalPlaces = 2, Function<void(f64)> setter = {},
                    StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(initialValue),
              m_min(min), m_max(max), m_step(step), m_decimalPlaces(decimalPlaces)
        {
        }

        [[nodiscard]] f64 Value() const noexcept { return m_value; }
        void SetValue(f64 value)
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
                m_field->SetValue(m_value);
                m_syncing = false;
            }
        }

        /// NumericField subclass that tracks edit transactions via focus.
        class FloatEditorField : public NumericField
        {
            DRACONIC_OBJECT(FloatEditorField, NumericField)
        public:
            explicit FloatEditorField(FloatEditor* editor) : m_editor(editor) {}

            void OnFocusGained() override;
            void OnFocusLost() override;
            void OnKeyDown(KeyEventArgs& e) override;

        private:
            FloatEditor* m_editor;
        };

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<FloatEditorField> field = MakeRef<FloatEditorField>(DefaultAllocator(), this);
            field->AddClass(u8"property-field");
            m_field = field.Get();
            m_field->SetMin(m_min);
            m_field->SetMax(m_max);
            m_field->SetStep(m_step);
            m_field->SetDecimalPlaces(m_decimalPlaces);
            m_field->SetValue(m_value);
            FloatEditor* self = this;
            m_field->OnValueChanged.Add(
                [self](NumericField*, f64 val)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value = val;
                        if (self->Setter)
                        {
                            self->Setter(val);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                });
            return field;
        }

    private:
        f64 m_value;
        f64 m_min;
        f64 m_max;
        f64 m_step;
        i32 m_decimalPlaces;
        f64 m_preEditValue = 0.0;
        NumericField* m_field = nullptr; // borrowed; the editor view RefPtr owns it
        bool m_syncing = false;
    };

    // === Inner-view out-of-line bodies (need the complete FloatEditor type) ===

    inline void FloatEditor::FloatEditorField::OnFocusGained()
    {
        NumericField::OnFocusGained();
        m_editor->m_preEditValue = m_editor->m_value;
        m_editor->BeginEdit();
    }

    inline void FloatEditor::FloatEditorField::OnFocusLost()
    {
        NumericField::OnFocusLost();
        if (m_editor->IsEditing())
        {
            m_editor->EndEdit();
        }
    }

    inline void FloatEditor::FloatEditorField::OnKeyDown(KeyEventArgs& e)
    {
        if (e.Key == KeyCode::Escape && m_editor->IsEditing())
        {
            m_editor->m_value = m_editor->m_preEditValue;
            SetValue(m_editor->m_preEditValue);
            if (m_editor->Setter)
            {
                m_editor->Setter(m_editor->m_preEditValue);
            }
            m_editor->CancelEdit();
            e.Handled = true;
            return;
        }
        NumericField::OnKeyDown(e);
    }

    DRACONIC_DEFINE_OBJECT(FloatEditor, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(FloatEditor::FloatEditorField, "draconic::ui::toolkit")
}
