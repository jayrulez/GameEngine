// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :radio_group partition
//
// Groups RadioButtons for mutual exclusion (a vertical FlexLayout). Ported from
// Sedulous.UI/src/Controls/RadioGroup.bf.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:radio_group;

import foundation.core;
import :view;
import :flex_layout;
import :event;
import :radio_button;

using namespace foundation::core;

export namespace foundation::ui
{
    class RadioGroup : public FlexLayout
    {
        RTTI_OBJECT(RadioGroup, FlexLayout)
    public:
        Event<void(RadioGroup*, RadioButton*)> OnSelectionChanged;

        RadioGroup()
        {
            Direction = Orientation::Vertical;
            Spacing = 4.0f;
        }

        [[nodiscard]] RadioButton* CheckedButton() const noexcept { return m_checkedButton; }

        /// Add a radio button to the group (wires mutual-exclusion).
        void AddRadioButton(RadioButton* radio)
        {
            AddView(radio);
            RadioGroup* self = this;
            radio->OnCheckedChanged.Add(Event<void(RadioButton*, bool)>::Handler{
                [self](RadioButton* r, bool isChecked)
                { self->OnRadioCheckedChanged(r, isChecked); }});
        }

        /// Programmatically select a radio button by index.
        void CheckAt(usize index)
        {
            for (usize i = 0; i < ChildCount(); ++i)
            {
                if (RadioButton* radio = Cast<RadioButton>(GetChildAt(i)))
                {
                    if (i == index)
                    {
                        radio->IsChecked.SetValue(true);
                    }
                }
            }
        }

        /// Clear selection (all unchecked).
        void ClearCheck()
        {
            if (m_updating)
            {
                return;
            }
            m_updating = true;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                if (RadioButton* radio = Cast<RadioButton>(GetChildAt(i)))
                {
                    radio->IsChecked.SetValue(false);
                }
            }
            m_checkedButton = nullptr;
            m_updating = false;
        }

    private:
        void OnRadioCheckedChanged(RadioButton* radio, bool isChecked)
        {
            if (m_updating || !isChecked)
            {
                return;
            }
            m_updating = true;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                if (RadioButton* other = Cast<RadioButton>(GetChildAt(i)))
                {
                    if (other != radio)
                    {
                        other->IsChecked.SetValue(false);
                    }
                }
            }
            m_checkedButton = radio;
            m_updating = false;
            OnSelectionChanged.Invoke(this, radio);
        }

        RadioButton* m_checkedButton = nullptr;
        bool m_updating = false;
    };

    RTTI_DEFINE_OBJECT(RadioGroup, "rtti::ui")
}
