// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - module implementation unit for UITypeRegistry::RegisterBuiltins.
//
// Registers every built-in view, layout and control type name so .sss element selectors (View,
// ButtonBase, ComboBox, EditText, ComboBox::arrow, ...) resolve to a concrete type. It lives in an
// implementation unit because it references the whole control set (reached through the module's
// primary interface), which the leaf :ui_type_registry partition cannot import. It runs once per
// process, since the map is global.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.ui;

namespace foundation::ui
{
    // The process-wide ViewId counter (declared in ViewId.cppm).
    ViewId ViewId::Create() noexcept
    {
        static foundation::core::Atomic<foundation::core::u32> s_next{1u};
        ViewId id;
        id.m_value = s_next.fetch_add(1u);
        return id;
    }
} // namespace foundation::ui

namespace foundation::ui
{
    void UITypeRegistry::RegisterBuiltins()
    {
        // Runs once and is thread-safe: a function-local static's initializer runs exactly once and
        // blocks concurrent callers. The UI theme cook reaches this through StyleSheetLoader on job
        // workers, and with an unguarded flag two of them could rehash the type map under each
        // other. Being in an implementation unit, the static is one per process rather than one per
        // shared library.
        static const bool once = []()
        {
            RegisterBuiltinsBody();
            return true;
        }();
        (void)once;
    }

    void UITypeRegistry::RegisterBuiltinsBody()
    {

        // Core.
        Register(u8"View", &View::StaticType());
        Register(u8"ViewGroup", &ViewGroup::StaticType());
        Register(u8"RootView", &RootView::StaticType());

        // Layouts and aliases.
        Register(u8"FlexLayout", &FlexLayout::StaticType());
        Register(u8"Flex", &FlexLayout::StaticType());
        Register(u8"GridLayout", &GridLayout::StaticType());
        Register(u8"Grid", &GridLayout::StaticType());
        Register(u8"DockLayout", &DockLayout::StaticType());
        Register(u8"Dock", &DockLayout::StaticType());
        Register(u8"FrameLayout", &FrameLayout::StaticType());
        Register(u8"Frame", &FrameLayout::StaticType());
        Register(u8"AbsoluteLayout", &AbsoluteLayout::StaticType());
        Register(u8"Absolute", &AbsoluteLayout::StaticType());
        Register(u8"FlowLayout", &FlowLayout::StaticType());
        Register(u8"Flow", &FlowLayout::StaticType());

        // Controls.
        Register(u8"Panel", &Panel::StaticType());
        Register(u8"Label", &Label::StaticType());
        Register(u8"RichText", &RichText::StaticType());
        Register(u8"Button", &Button::StaticType());
        Register(u8"IconButton", &IconButton::StaticType());
        Register(u8"ButtonBase", &ButtonBase::StaticType());
        Register(u8"ContentButton", &ContentButton::StaticType());
        Register(u8"RepeatButton", &RepeatButton::StaticType());
        Register(u8"ToggleButton", &ToggleButton::StaticType());
        Register(u8"CheckBox", &CheckBox::StaticType());
        Register(u8"RadioButton", &RadioButton::StaticType());
        Register(u8"RadioGroup", &RadioGroup::StaticType());
        Register(u8"ToggleSwitch", &ToggleSwitch::StaticType());
        Register(u8"EditText", &EditText::StaticType());
        Register(u8"PasswordBox", &PasswordBox::StaticType());
        Register(u8"NumericField", &NumericField::StaticType());
        Register(u8"EditableLabel", &EditableLabel::StaticType());
        Register(u8"Slider", &Slider::StaticType());
        Register(u8"ProgressBar", &ProgressBar::StaticType());
        Register(u8"ScrollBar", &ScrollBar::StaticType());
        Register(u8"ScrollView", &ScrollView::StaticType());
        Register(u8"ImageView", &ImageView::StaticType());
        Register(u8"ColorView", &ColorView::StaticType());
        Register(u8"DrawableView", &DrawableView::StaticType());
        Register(u8"Separator", &Separator::StaticType());
        Register(u8"Spacer", &Spacer::StaticType());
        Register(u8"ComboBox", &ComboBox::StaticType());
        Register(u8"TabView", &TabView::StaticType());
        Register(u8"Expander", &Expander::StaticType());
        Register(u8"ListView", &ListView::StaticType());
        Register(u8"GridView", &GridView::StaticType());
        Register(u8"TreeView", &TreeView::StaticType());

        // Overlay.
        Register(u8"ContextMenu", &ContextMenu::StaticType());
        Register(u8"Dialog", &Dialog::StaticType());
        Register(u8"TooltipView", &TooltipView::StaticType());
    }
}
