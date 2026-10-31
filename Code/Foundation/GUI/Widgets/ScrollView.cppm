// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :scroll_view partition
//
// ScrollView: a Panel that scrolls (`overflow: auto` on both axes) and takes focus, so the keys
// scroll it. The scrolling itself comes from the base node and works for any node a sheet gives
// `overflow: scroll` or `auto`: the wheel, the keys while focused, the scroll bars, and bringing a
// focused descendant into view. The scroll view's children are its content.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:scroll_view;

import foundation.core;
import :layout_style;
import :ui_widget;

export namespace foundation::gui
{
    class ScrollView : public UIWidget
    {
        RTTI_OBJECT(ScrollView, UIWidget)
    public:
        ScrollView()
        {
            SetTag(core::StringView(u8"scrollview"));
            SetTabFocusable(true);
            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            EditLayoutStyle(
                [](LayoutStyle& s)
                {
                    s.OverflowX = Overflow::Auto;
                    s.OverflowY = Overflow::Auto;
                });
        }
    };
}
