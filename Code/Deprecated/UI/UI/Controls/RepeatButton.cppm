// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :repeat_button partition
//
// Button that fires OnClick repeatedly while held down (scroll arrows, numeric steppers, ...).
// Ported from Sedulous.UI/src/Controls/RepeatButton.bf.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:repeat_button;

import foundation.core;
import :button;
import :event_args;
import :input_enums;

using namespace foundation::core;

export namespace foundation::ui
{
    class RepeatButton : public Button
    {
        RTTI_OBJECT(RepeatButton, Button)
    public:
        f32 RepeatDelay = 0.4f;     ///< Delay before repeating starts (s).
        f32 RepeatInterval = 0.05f; ///< Interval between repeats (s).

        explicit RepeatButton(StringView text) : Button(text) {}

        void OnMouseDown(MouseEventArgs& e) override
        {
            Button::OnMouseDown(e);
            if (e.Button == MouseButton::Left)
            {
                m_repeating = true;
                m_holdTime = 0.0f;
            }
        }
        void OnMouseUp(MouseEventArgs& e) override
        {
            m_repeating = false;
            m_holdTime = 0.0f;
            Button::OnMouseUp(e);
        }

        /// Call each frame while held; fires FireClick() after RepeatDelay, then every RepeatInterval.
        void UpdateRepeat(f32 deltaTime)
        {
            if (!m_repeating || !IsPressed())
            {
                return;
            }
            m_holdTime += deltaTime;
            if (m_holdTime >= RepeatDelay)
            {
                m_holdTime -= RepeatInterval;
                FireClick();
            }
        }

    private:
        bool m_repeating = false;
        f32 m_holdTime = 0.0f;
    };

    RTTI_DEFINE_OBJECT(RepeatButton, "rtti::ui")
}
