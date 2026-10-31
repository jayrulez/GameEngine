// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :panel partition
//
// Panel: the plain container element (`<Panel>` in markup, `add("Panel")` in a script). It lays
// out its children according to its display kind, a flex column unless its style sheet or markup
// sets otherwise (`display`, `flex-direction`, `grid-template-columns`, ...). All its behaviour
// comes from the base node.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:panel;

import foundation.core;
import :ui_widget;

export namespace foundation::gui
{
    class Panel : public UIWidget
    {
        RTTI_OBJECT(Panel, UIWidget)
    public:
        Panel() { SetTag(core::StringView(u8"panel")); }
    };
}
