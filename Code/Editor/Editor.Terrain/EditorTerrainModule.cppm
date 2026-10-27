// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the `editor.terrain` module.
//
// The terrain's editing on the GUI (Documentation/Specs/editor-gui.md, 5.3, layer 5, part 3, and
// 5.4, layer 7): the scene view's brushes (sculpt, paint layers, cut holes), their panels, the
// splatmap thumbnails and the Terrain asset page.

export module editor.terrain;

export import :splatmap_thumbnail;
export import :sculpt;
export import :splat;
export import :hole;
export import :tool_panels;
export import :page;
