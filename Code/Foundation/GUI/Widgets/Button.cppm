// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :button partition
//
// Button: the button core (ButtonBase) with its content centred, whether a text, an icon and a
// text, or whatever markup puts inside it. What other toolkits make separate kinds of button are
// attributes here: an icon button has an icon, a toggle button has `toggle`, and a repeat button
// has `repeat`.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:button;

import foundation.core; // StringView
import :layout_style;
import :button_base;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class Button : public ButtonBase
    {
        RTTI_OBJECT(Button, ButtonBase)
    public:
        Button()
        {
            SetTag(core::StringView(u8"button")); // CSS `button { ... }` targets it by default
            StyleWriteScope defaults(*this);
            EditLayoutStyle([](LayoutStyle& s) { s.JustifyContent = Justify::Center; });
        }
    };
}
