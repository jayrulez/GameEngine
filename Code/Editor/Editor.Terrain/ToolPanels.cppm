// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :tool_panels partition.
//
// The terrain brushes' panels (Documentation/Specs/editor-gui.md, 5.3, layer 5, part 3), made of
// the editor's tool panel kit so they read as one: Sculpt's mode (Raise, Lower, Smooth, Flatten),
// Paint Layers' layer (each its albedo's picture, numbered from 1 as the keys are) and its mode
// (Paint, Erase, Smooth; they were letters on the layer row), Cut Holes' mode (Cut, Fill), each
// with its radius and what else it has, and a line of help for its keys. And the terrain's one
// composition entry point for the scene view.

module;
#include "Core/Prelude.h"

export module editor.terrain:tool_panels;

import foundation.core;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    /// The terrain brushes' panels into the viewport-tool-panel registry (editor.app:tool_panel).
    void RegisterTerrainToolPanels();

    /// The terrain's part of the scene view: its brushes, their panels and the splatmap
    /// thumbnails. Call once at editor start.
    void RegisterTerrainSceneTools(EditorContext& context);
}
