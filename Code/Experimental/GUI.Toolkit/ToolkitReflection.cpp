// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the toolkit widgets' reflection: what markup, sheets and scripts set on them, and
// their registration as elements.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module experimental.gui.toolkit;

import foundation.core;
import experimental.gui;

namespace core = foundation::core;

namespace experimental::gui
{
    REFLECT_MEMBERS(ToolButton, "rtti::gui::toolkit")
    {
        builder.AccessorProperty<&ToolButton::ShowsText, &ToolButton::SetShowsText>("showsText").PropAttribute("css", core::String(u8"shows-text"));
        builder.AccessorProperty<&ToolButton::GetOverflowPriority, &ToolButton::SetOverflowPriority>("overflowPriority")
            .PropAttribute("css", core::String(u8"overflow-priority"));
        builder.Creator();
    }

    REFLECT_MEMBERS(ToolMenuButton, "rtti::gui::toolkit")
    {
        builder.AccessorProperty<&ToolMenuButton::IsSplit, &ToolMenuButton::SetSplit>("split").PropAttribute("css", core::String(u8"split"));
        builder.Creator();
    }

    REFLECT_MEMBERS(ToolBar, "rtti::gui::toolkit")
    {
        builder.Attribute("accessRole", core::String(u8"group")); // the core has no tool bar role yet
        builder.AccessorProperty<&ToolBar::GetOrientationName, &ToolBar::SetOrientationName>("orientation")
            .PropAttribute("css", core::String(u8"orientation"));
        builder.Creator();
    }

    REFLECT_MEMBERS(StatusItem, "rtti::gui::toolkit")
    {
        builder.AccessorProperty<&StatusItem::GetZoneName, &StatusItem::SetZoneName>("zone").PropAttribute("css", core::String(u8"zone"));
        builder.AccessorProperty<&StatusItem::GetTitleString, &StatusItem::SetTitleString>("title").PropAttribute("css", core::String(u8"title")).PropAttribute("translatable", true);
        builder.Creator();
    }

    REFLECT_MEMBERS(StatusBar, "rtti::gui::toolkit")
    {
        builder.Attribute("accessRole", core::String(u8"group")); // the core has no status role yet
        builder.Creator();
    }

    RTTI_DEFINE_OBJECT(PropertyEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(UnknownPropertyEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(SingleControlEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(BoolPropertyEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(NumberPropertyEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(StringPropertyEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(EnumPropertyEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(PropertyFlagsList, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(FlagsPropertyEditor, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(ToolMenuArrow, "rtti::gui::toolkit")
    RTTI_DEFINE_OBJECT(ToolBarOverflow, "rtti::gui::toolkit")

    void RegisterToolkitElements()
    {
        static const bool once = []()
        {
            const core::TypeInfo* const kElements[] = {
                &ToolButton::StaticType(),
                &ToolMenuButton::StaticType(),
                &ToolBar::StaticType(),
                &StatusItem::StaticType(),
                &StatusBar::StaticType(),
            };
            for (const core::TypeInfo* type : kElements)
                (void)ElementTypes::Register(*type);
            return true;
        }();
        (void)once;
    }
}
