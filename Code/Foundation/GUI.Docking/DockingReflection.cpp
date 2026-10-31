// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Docking - reflection for the docking widgets: the properties a sheet sets on them, such as
// the drop bands, which the theme's tokens size.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.gui.docking;

import foundation.core;
import foundation.gui;

namespace core = foundation::core;

namespace foundation::gui
{
    REFLECT_MEMBERS(DockSpace, "rtti::gui::docking")
    {
        builder.AccessorProperty<&DockSpace::GetEdgeBand, &DockSpace::SetEdgeBand>("edgeBand").PropAttribute("css", core::String(u8"edge-band"));
        builder.AccessorProperty<&DockSpace::GetSurfaceBand, &DockSpace::SetSurfaceBand>("surfaceBand")
            .PropAttribute("css", core::String(u8"surface-band"));
        builder.AccessorProperty<&DockSpace::GetTornTabName, &DockSpace::SetTornTabName>("tornTab").PropAttribute("css", core::String(u8"torn-tab"));
    }

    RTTI_DEFINE_OBJECT(DockStack, "rtti::gui::docking")
    RTTI_DEFINE_OBJECT(DockFloat, "rtti::gui::docking")
    RTTI_DEFINE_OBJECT(DockArea, "rtti::gui::docking")
}
