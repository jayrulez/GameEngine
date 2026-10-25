// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the `editor.terrain` module.
//
// The terrain's editing on the GUI so far (Documentation/Specs/editor-gui.md, 5.3, layer 5, part
// 3): the scene view's brushes (sculpt, paint layers, cut holes), their panels and the splatmap
// thumbnails. The Terrain asset page (TerrainEditorPage) stays out of the build, unedited, until
// its port.

export module editor.terrain;

export import :splatmap_thumbnail;
export import :sculpt;
export import :splat;
export import :hole;
export import :tool_panels;
