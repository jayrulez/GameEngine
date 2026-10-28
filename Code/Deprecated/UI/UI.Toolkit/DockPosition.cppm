// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :dock_position partition
//
// Position for docking a panel relative to a target. Ported 1:1 from
// Sedulous.UI.Toolkit/src/Docking/DockPosition.bf (a plain enum).

module;
#include "Core/Prelude.h"

export module foundation.ui.toolkit:dock_position;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    /// Position for docking a panel relative to a target.
    enum class DockPosition
    {
        Left,
        Right,
        Top,
        Bottom,
        Center,
        Float
    };
}
