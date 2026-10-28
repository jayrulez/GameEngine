// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :color_editor partition
//
// Color property editor - a ColorView swatch that opens a ColorPicker (the already-ported toolkit control)
// inside a core Dialog on click. BeginEdit when the picker opens, EndEdit on OK, CancelEdit on Cancel.
// Ported from Sedulous.UI.Toolkit/src/PropertyGrid/ColorEditor.bf. Beef `delegate void(Color) Setter` ->
// Function<void(Color)>; `mSwatch.Color.Value = v` -> Color.SetValue(v). The private inner
// `ClickableColorSwatch : ColorView` becomes a PUBLIC nested class (own RTTI_OBJECT identity) whose
// OnMouseDown (references the enclosing editor + ColorPicker + Dialog) is defined out-of-line after
// ColorEditor is complete. Beef `new Dialog`/`new ColorPicker` -> RefPtr; the popup layer takes ownership
// of the shown dialog (ownsView), so the local RefPtrs may drop.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:color_editor;

import foundation.core;
import foundation.vg;
import foundation.ui;
import :property_editor;
import :color_picker;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui::toolkit
{
    /// Color property editor - ColorView swatch that opens a ColorPicker dialog on click.
    class ColorEditor : public PropertyEditor
    {
        RTTI_OBJECT(ColorEditor, PropertyEditor)
    public:
        Function<void(Color)> Setter;

        ColorEditor(StringView name, Color initialValue, Function<void(Color)> setter = {},
                    StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(initialValue)
        {
        }

        [[nodiscard]] Color Value() const noexcept { return m_value; }
        void SetValue(Color value)
        {
            m_value = value;
            if (m_swatch != nullptr)
            {
                m_swatch->Color.SetValue(value);
            }
        }

        void RefreshView() override
        {
            if (m_swatch != nullptr)
            {
                m_swatch->Color.SetValue(m_value);
            }
        }

        /// ColorView that opens a ColorPicker dialog on click.
        class ClickableColorSwatch : public ColorView
        {
            RTTI_OBJECT(ClickableColorSwatch, ColorView)
        public:
            explicit ClickableColorSwatch(ColorEditor* editor) : m_editor(editor) {}

            void OnMouseDown(MouseEventArgs& e) override;

        private:
            ColorEditor* m_editor;
        };

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<ClickableColorSwatch> swatch =
                MakeRef<ClickableColorSwatch>(MemoryAllocator(), this);
            m_swatch = swatch.Get();
            m_swatch->Color.SetValue(m_value);
            m_swatch->Cursor = CursorType::Hand;
            return swatch;
        }

    private:
        Color m_value;
        ColorView* m_swatch = nullptr; // borrowed; the editor view RefPtr owns it
    };

    // === Inner-view out-of-line body (needs the complete ColorEditor + ColorPicker + Dialog types) ===

    inline void ColorEditor::ClickableColorSwatch::OnMouseDown(MouseEventArgs& e)
    {
        if (e.Button != MouseButton::Left || Context == nullptr)
        {
            return;
        }

        // `Color` names ColorView's shadowing property here, so the type is spelled core::Color.
        ColorEditor* editor = m_editor;
        const core::Color originalColor = editor->m_value;
        editor->BeginEdit();

        RefPtr<ColorPicker> picker = MakeRef<ColorPicker>(MemoryAllocator());
        picker->SetColor(editor->m_value);
        picker->SetOriginalColor(editor->m_value);
        picker->OnColorChanged.Add(
            [editor](ColorPicker*, core::Color color)
            {
                editor->m_value = color;
                editor->m_swatch->Color.SetValue(color);
                if (editor->Setter)
                {
                    editor->Setter(color);
                }
                editor->NotifyValueChanged();
            });

        RefPtr<Dialog> dialog = MakeRef<Dialog>(MemoryAllocator(), StringView(u8"Color Picker"));
        dialog->SetContent(picker.Get());
        dialog->AddButton(u8"OK", DialogResult::OK);
        dialog->AddButton(u8"Cancel", DialogResult::Cancel);
        dialog->OnClosed.Add(
            [editor, originalColor](Dialog*, DialogResult result)
            {
                if (result == DialogResult::OK)
                {
                    editor->EndEdit();
                }
                else
                {
                    editor->m_value = originalColor;
                    editor->m_swatch->Color.SetValue(originalColor);
                    if (editor->Setter)
                    {
                        editor->Setter(originalColor);
                    }
                    editor->CancelEdit();
                }
            });
        dialog->Show(Context);
        e.Handled = true;
    }

    RTTI_DEFINE_OBJECT(ColorEditor, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(ColorEditor::ClickableColorSwatch, "rtti::ui::toolkit")
}
