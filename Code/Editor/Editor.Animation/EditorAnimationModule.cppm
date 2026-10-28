// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `editor.animation` module.
//
// The animation domain's editor pages on the GUI (Documentation/Specs/editor-gui.md, 5.4, layer 10),
// each with its factory and Register entry point, which the editor executable calls: the clip's,
// the skeleton's and the graph's. What their previews share (the bones drawn, a clip's travelled
// path, the rig a pose is shown on) is the :preview partition; the graph as a graph view reads it
// is :graph_states.

export module editor.animation;

export import :preview;
export import :clip_page;
export import :skeleton_page;
export import :graph_states;
export import :graph_page;
