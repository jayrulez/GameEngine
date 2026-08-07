// Draconic UI Toolkit - :enum_editor partition
//
// Property editor for enumeration values - a ComboBox with string items. Instant edit (BeginEdit + change +
// EndEdit per selection). Ported from Sedulous.UI.Toolkit/src/PropertyGrid/EnumEditor.bf. Beef `int32` ->
// i32; `List<String> mItems` (owned) -> Array<String>; `Span<StringView> items` ctor arg -> Span<const
// StringView>; `mComboBox.SelectedIndex = v` -> SetSelectedIndex(v).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:enum_editor;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import :property_editor;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Property editor for enumeration values. Uses a ComboBox with string items.
    class EnumEditor : public PropertyEditor
    {
        DRACONIC_OBJECT(EnumEditor, PropertyEditor)
    public:
        Function<void(i32)> Setter;

        EnumEditor(StringView name, i32 value, Span<const StringView> items,
                   Function<void(i32)> setter = {}, StringView category = {})
            : PropertyEditor(name, category), Setter(Move(setter)), m_value(value)
        {
            for (usize i = 0; i < items.Size(); ++i)
            {
                m_items.PushBack(String(items[i]));
            }
        }

        [[nodiscard]] i32 Value() const noexcept { return m_value; }
        void SetValue(i32 value)
        {
            m_value = value;
            if (!m_syncing)
            {
                RefreshView();
            }
        }

        void RefreshView() override
        {
            if (m_comboBox != nullptr && !m_syncing)
            {
                m_syncing = true;
                m_comboBox->SetSelectedIndex(m_value);
                m_syncing = false;
            }
        }

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<ComboBox> comboBox = MakeRef<ComboBox>(DefaultAllocator());
            m_comboBox = comboBox.Get();
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                m_comboBox->AddItem(m_items[i]);
            }
            m_comboBox->SetSelectedIndex(m_value);
            EnumEditor* self = this;
            m_comboBox->OnSelectionChanged.Add(
                [self](ComboBox*, i32 idx)
                {
                    if (!self->m_syncing)
                    {
                        self->m_syncing = true;
                        self->BeginEdit();
                        self->m_value = idx;
                        if (self->Setter)
                        {
                            self->Setter(self->m_value);
                        }
                        self->NotifyValueChanged();
                        self->EndEdit();
                        self->m_syncing = false;
                    }
                });
            return comboBox;
        }

    private:
        i32 m_value;
        Array<String> m_items;
        ComboBox* m_comboBox = nullptr; // borrowed; the editor view RefPtr owns it
        bool m_syncing = false;
    };

    DRACONIC_DEFINE_OBJECT(EnumEditor, "draconic::ui::toolkit")
}
