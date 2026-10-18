// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `editor.animation` module.
//
// The animation domain's editor pages: the animation graph's authoring page, the clip's preview
// page and the skeleton's viewer, each with its factory and Register entry point, which the editor
// EXECUTABLE calls. The graph and clip pages draw a skeleton through the graph page's
// DrawSkeletonWireframe.

export module editor.animation;

export import :graph_page;
export import :clip_page;
export import :skeleton_page;
