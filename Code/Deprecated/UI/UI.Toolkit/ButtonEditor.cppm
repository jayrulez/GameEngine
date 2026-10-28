// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :button_editor partition
//
// Property editor that displays a clickable Button (used for actions like "Add Condition"). Ported from
// Sedulous.UI.Toolkit/src/PropertyGrid/ButtonEditor.bf. Beef `delegate void() Action` -> Function<void()>;
// `new Button(Name)` -> a RefPtr<Button> returned as the editor view.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:button_editor;

import foundation.core;
import foundation.vg;
import foundation.ui;
import :property_editor;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    /// Property editor that displays a clickable button.
    class ButtonEditor : public PropertyEditor
    {
        RTTI_OBJECT(ButtonEditor, PropertyEditor)
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
            RefPtr<Button> btn = MakeRef<Button>(MemoryAllocator(), Name());
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

    RTTI_DEFINE_OBJECT(ButtonEditor, "rtti::ui::toolkit")
}
