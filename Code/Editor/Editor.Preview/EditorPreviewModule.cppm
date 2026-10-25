// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Preview - the `editor.preview` module.
//
// On the GUI in parts (Documentation/Specs/editor-gui.md, 5.3, layer 2): the thumbnail stage, the
// GPU half of asset thumbnails, is UI-free and back in the build; the preview viewport the asset
// pages contain is still on foundation.ui and comes back with them (X6), a partition of this root.

export module editor.preview;

export import :thumbnail_stage; // the GPU thumbnail renderer
