// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Preview - `editor.preview`: what the editor's asset pages preview with: the thumbnail
// stage (the GPU thumbnail renderer, fed by the editor core's ThumbnailService) and the preview
// viewport, which lives apart (editor.preview.viewport) so a tool can have it without the editor's
// core.

module;
#include "Core/Prelude.h"

export module editor.preview;

export import :thumbnail_stage;
export import editor.preview.viewport;
