// Draconic UI Toolkit - :button_editor partition
//
// Property editor that displays a clickable Button (used for actions like "Add Condition"). Ported from
// Sedulous.UI.Toolkit/src/PropertyGrid/ButtonEditor.bf. Beef `delegate void() Action` -> Function<void()>;
// `new Button(Name)` -> a RefPtr<Button> returned as the editor view.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:button_editor;

import draconic.foundation;
import draconic.vg;
import draconic.ui;
import :property_editor;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Property editor that displays a clickable button.
    class ButtonEditor : public PropertyEditor
    {
        DRACONIC_OBJECT(ButtonEditor, PropertyEditor)
    public:
        Function<void()> Action;

        ButtonEditor(StringView name, Function<void()> action, StringView category = {})
            : PropertyEditor(name, category), Action(Move(action))
        {
        }

        void RefreshView() override {}

        /// Enable/disable the live button (grayed + click-inert while disabled). Safe to
        /// call before the view exists - the state applies at creation.
        void SetButtonEnabled(bool enabled)
        {
            m_buttonEnabled = enabled;
            if (m_button != nullptr && m_button->IsEnabled != enabled)
            {
                m_button->IsEnabled = enabled;
                m_button->Invalidate();
            }
        }
        [[nodiscard]] bool ButtonEnabled() const noexcept { return m_buttonEnabled; }

    protected:
        RefPtr<View> CreateEditorView() override
        {
            RefPtr<Button> btn = MakeRef<Button>(DefaultAllocator(), Name());
            btn->IsEnabled = m_buttonEnabled;
            ButtonEditor* self = this;
            btn->OnClick.Add(
                [self](ButtonBase*)
                {
                    if (self->Action)
                    {
                        self->Action();
                    }
                });
            m_button = btn.Get();
            return btn;
        }

    private:
        Button* m_button = nullptr; // borrowed; the cached editor view owns it
        bool m_buttonEnabled = true;
    };

    DRACONIC_DEFINE_OBJECT(ButtonEditor, "draconic::ui::toolkit")
}
