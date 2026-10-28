// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :string_editor partition
//
// String property editor - an EditText with focus-based edit transactions. BeginEdit on focus gained,
// EndEdit on focus lost or Enter (OnSubmit); Escape cancels and restores the pre-edit value. Ported from
// Sedulous.UI.Toolkit/src/PropertyGrid/StringEditor.bf.
//
// The Beef private inner `StringEditorEditText : EditText` becomes a PUBLIC nested class so it carries its
// own RTTI_OBJECT identity. Its overrides dereference the enclosing StringEditor (incomplete inside the
// class body), so those bodies are defined out-of-line after StringEditor is complete - the same idiom
// ColorPicker/Toolbar use. Beef `delegate void(StringView) Setter` -> Function<void(StringView)>.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:string_editor;

import foundation.core;
import foundation.vg;
import foundation.ui;
import :property_editor;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    /// String property editor - EditText with focus-based edit transactions.
    class StringEditor : public PropertyEditor
    {
        RTTI_OBJECT(StringEditor, PropertyEditor)
    public:
        Function<void(StringView)> Setter;

        StringEditor(StringView name, StringView initialValue,
                     Function<void(StringView)> setter = {}, StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(initialValue)
        {
        }

        [[nodiscard]] StringView Value() const { return m_value; }
        void SetValue(StringView value)
        {
            m_value = String(value);
            if (!m_syncing)
            {
                RefreshView();
            }
        }

        void RefreshView() override
        {
            if (m_editText != nullptr && !m_syncing)
            {
                m_syncing = true;
                m_editText->SetText(m_value);
                m_syncing = false;
            }
        }

        /// EditText subclass that notifies the StringEditor on focus changes.
        class StringEditorEditText : public EditText
        {
            RTTI_OBJECT(StringEditorEditText, EditText)
        public:
            explicit StringEditorEditText(StringEditor* editor) : m_editor(editor) {}

            void OnFocusGained() override;
            void OnFocusLost() override;
            void OnKeyDown(KeyEventArgs& e) override;

        private:
            StringEditor* m_editor;
        };

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<StringEditorEditText> editText =
                MakeRef<StringEditorEditText>(MemoryAllocator(), this);
            m_editText = editText.Get();
            m_editText->SetText(m_value);
            StringEditor* self = this;
            m_editText->OnSubmit.Add(
                [self](EditText* et)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->m_value = String(et->Text());
                        if (self->Setter)
                        {
                            self->Setter(self->m_value);
                        }
                        self->NotifyValueChanged();
                        self->m_syncing = false;
                    }
                    self->EndEdit();
                });
            return editText;
        }

    private:
        String m_value;
        String m_preEditValue;
        EditText* m_editText = nullptr; // borrowed; the editor view RefPtr owns it
        bool m_syncing = false;
    };

    // === Inner-view out-of-line bodies (need the complete StringEditor type) ===

    inline void StringEditor::StringEditorEditText::OnFocusGained()
    {
        EditText::OnFocusGained();
        m_editor->m_preEditValue = m_editor->m_value;
        m_editor->BeginEdit();
    }

    inline void StringEditor::StringEditorEditText::OnFocusLost()
    {
        EditText::OnFocusLost();
        if (m_editor->IsEditing())
        {
            m_editor->m_syncing = true;
            m_editor->m_value = String(Text());
            if (m_editor->Setter)
            {
                m_editor->Setter(m_editor->m_value);
            }
            m_editor->NotifyValueChanged();
            m_editor->m_syncing = false;
            m_editor->EndEdit();
        }
    }

    inline void StringEditor::StringEditorEditText::OnKeyDown(KeyEventArgs& e)
    {
        if (e.Key == KeyCode::Escape && m_editor->IsEditing())
        {
            m_editor->m_value = m_editor->m_preEditValue;
            SetText(m_editor->m_preEditValue);
            if (m_editor->Setter)
            {
                m_editor->Setter(m_editor->m_value);
            }
            m_editor->CancelEdit();
            e.Handled = true;
            return;
        }
        EditText::OnKeyDown(e);
    }

    RTTI_DEFINE_OBJECT(StringEditor, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(StringEditor::StringEditorEditText, "rtti::ui::toolkit")
}
