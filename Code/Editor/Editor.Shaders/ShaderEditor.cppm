// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Shaders - the `editor.shaders` module.
//
// The editor side of custom shaders: the shader page (:shader_page), built on the GUI's code
// pages. Its colours come from the shared HLSL definition (Data/Editor/Code/hlsl.xml).

module;
#include "Core/Prelude.h"

export module editor.shaders;

export import :shader_page;
