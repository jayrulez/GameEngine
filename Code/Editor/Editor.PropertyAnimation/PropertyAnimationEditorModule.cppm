// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `editor.propertyanimation` module.
//
// Property animation is authored in the scene, in a panel under the scene page's viewport: the
// clip document with its own history (:clip_document), the clip as the dopesheet's and the curve
// view's source (:clip_source), the track seeds, the preview and the transport's clock
// (:clip_preview), and the panel on the GUI (:panel), which the scene page holds.

export module editor.propertyanimation;

export import :clip_document;
export import :clip_source;
export import :clip_preview;
export import :panel;
