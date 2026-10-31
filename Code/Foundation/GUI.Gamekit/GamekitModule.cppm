// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `foundation.gui.gamekit`: a game's screens, meaning the screen stack and button prompts,
// built on the core's leave transitions, back command, input kind and data tweens.

module;
#include "Core/Prelude.h"

export module foundation.gui.gamekit;

export import :screen_stack; // screens on a stack, covering, blocking, pausing, focus, back
export import :prompt;       // button prompts by input kind, glyphs from the sheet

export namespace foundation::gui
{
    /// Registers the gamekit's element types (Screens, Screen, Prompt) once. A host calls it at
    /// start-up beside RegisterGuiElements, so markup and scripts can make them.
    void RegisterGamekitElements();
}
