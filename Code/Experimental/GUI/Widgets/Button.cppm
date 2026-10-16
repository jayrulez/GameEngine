// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :button partition
//
// Button: a clickable Label. A lean native control modeled on eepp's UIPushButton
// (role only, NOT a line-for-line port: eepp's is ~900 LOC composing a child UIImage icon +
// UITextView with an internal layout). Everything visual is already in place from the base
// layers - the background/skin reacts to the input-driven control state (UINode) and CSS can
// target the default `button` tag - so Button just centers its text and fires a click
// callback (and MouseClick event) when pressed and released on it. Icon + icon/text layout
// are not built in (add via composition when needed).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:button;

import foundation.core; // Function, Move
import :event;        // MouseEvent
import :text;         // TextHAlign / TextVAlign
import :label;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class Button : public Label
    {
        RTTI_OBJECT(Button, Label)
    public:
        Button()
        {
            SetTag(core::StringView(u8"button")); // CSS `button { ... }` targets it by default
            SetTextAlignment(TextHAlign::Center, TextVAlign::Middle);
            SetTabFocusable(true);
        }

        // Invoked on a click (press + release on the button).
        void SetOnClick(core::Function<void()> callback) { m_onClick = core::Move(callback); }
        [[nodiscard]] bool HasOnClick() const noexcept { return static_cast<bool>(m_onClick); }

    protected:
        bool OnActivate() override { return ActivateAsClick(); } // Return, Space: a click
        void OnMouseClick(const MouseEvent& event) override
        {
            (void)event;
            (void)RunBoundCommand(); // a command it is bound to (G7)
            if (m_onClick)
                m_onClick();
        }

        core::Function<void()> m_onClick;
    };
}
