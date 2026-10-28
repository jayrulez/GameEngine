// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Shaders - the `editor.shaders` module.
//
// The editor side of custom shaders (Specs/custom-shaders.md, phase 3): the shader page
// (:shader_page), on the GUI's code pages (Documentation/Specs/editor-gui.md, 5.4, layer 6). Its
// colours are the shared HLSL definition's (Data/Assets/code/hlsl.xml).

module;
#include "Core/Prelude.h"

export module editor.shaders;

export import :shader_page;
