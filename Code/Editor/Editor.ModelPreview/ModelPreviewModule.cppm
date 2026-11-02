// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::ModelPreview - `editor.modelpreview`: a model file opened for viewing without a project,
// and its clips driven. It needs nothing of the editor's core, so the model viewer tool links it as
// the editor does (the import window's model preview).

module;
#include "Core/Prelude.h"

export module editor.modelpreview;

export import :library;  // opening a model file: cooked into a cache folder and bound, its numbers
export import :playback; // which clip plays, play, pause, step, scrub, loop and speed
