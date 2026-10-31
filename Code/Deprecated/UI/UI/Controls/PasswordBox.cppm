// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :password_box partition
//
// Masked password input. Extends EditText to display mask characters instead of the real text, and
// disables clipboard copy/cut for security.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:password_box;

import foundation.core; // String, StringView, DecodeUtf8, AppendUtf8
import :edit_text;
import :property;
import :event_args;
import :input_enums; // KeyCode, KeyModifiers, HasFlag

using namespace foundation::core;

export namespace foundation::ui
{
    class PasswordBox : public EditText
    {
        RTTI_OBJECT(PasswordBox, EditText)
    public:
        /// The character used to mask each real character.
        Property<char32_t> PasswordChar{U'*'};

        PasswordBox()
        {
            Behavior().AllowClipboardCopy = false;
            PasswordChar.SetOwner(this, InvalidationKind::Visual);
        }

        /// A password: the platform's IME composes nothing in it, suggests nothing, learns nothing.
        [[nodiscard]] bool IsTextInputSecret() const override { return true; }

        void GetDisplayText(String& outText) const override
        {
            outText.Clear();
            const StringView text = Text();
            const u32 mask = static_cast<u32>(PasswordChar.Value());
            usize i = 0;
            while (i < text.Size())
            {
                (void)DecodeUtf8(text, i);
                AppendUtf8(outText, mask);
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            // Block copy and cut.
            if (HasFlag(e.Modifiers, KeyModifiers::Ctrl) &&
                (e.Key == KeyCode::C || e.Key == KeyCode::X))
            {
                e.Handled = true;
                return;
            }
            EditText::OnKeyDown(e);
        }
    };

    RTTI_DEFINE_OBJECT(PasswordBox, "rtti::ui")
}
