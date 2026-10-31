// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :tool_panels partition.
//
// The terrain brushes' panels, made from the editor's tool panel kit so they look consistent.
// Sculpt has its mode (Raise, Lower, Smooth, Flatten); Paint Layers has its layer (each shown by
// its albedo's picture, numbered from 1 like the keys) and its mode (Paint, Erase, Smooth); Cut
// Holes has its mode (Cut, Fill). Each also has its radius and other settings, and a line of help
// for its keys. This partition also holds the terrain's single composition entry point for the
// scene view.

module;
#include "Core/Prelude.h"

export module editor.terrain:tool_panels;

import foundation.core;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    /// Registers the terrain brushes' panels in the viewport-tool-panel registry
    /// (editor.app:tool_panel).
    void RegisterTerrainToolPanels();

    /// Registers the terrain's part of the scene view: its brushes, their panels and the
    /// splatmap thumbnails. Call once at editor start.
    void RegisterTerrainSceneTools(EditorContext& context);
}
