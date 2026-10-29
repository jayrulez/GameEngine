// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer`: what the model viewer tool (Tools.ModelViewer) does apart from its
// window, so it is tested headless (Documentation/Specs/model-viewer.md).

module;
#include "Core/Prelude.h"

export module modelviewer;

export import :library; // a model file opened, cooked into the viewer's cache and bound
