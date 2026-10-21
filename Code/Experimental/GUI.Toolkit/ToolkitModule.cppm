// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `experimental.gui.toolkit`: editor widgets: tool and status bars, the property grid, colour,
// curves, gradients, the timeline, node graphs. Docking is not here (GUI.Docking), nor the code
// editor (GUI.Code).
// foundation.ui's counterpart: UI.Toolkit less docking and the code editor. Round three of the lab's
// evolution (Specs/gui-lab-evolution-3.md): T, after its survey.

module;
#include "Core/Prelude.h"

export module experimental.gui.toolkit;

export import :sheet;    // the default sheet, over the theme's tokens
export import :tool_bar;   // T0: tool bars over the command model
export import :status_bar; // T0: status bars of registered items and messages

export namespace experimental::gui
{
    /// Registers the toolkit's element types (ElementTypes::Register), once: a host calls it at
    /// start-up beside RegisterGuiElements, so markup and scripts can make them.
    void RegisterToolkitElements();
}
