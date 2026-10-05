// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Main.cpp - the DESKTOP entry for TerrainPlayground (see TerrainPlaygroundApp.h - the shared
// terrain showcase). Desktop platform trio via APP_MAIN's desktop body, which also gives the
// backend flags: `TerrainPlayground --vulkan` vs `--webgpu` runs the same terrain per backend,
// and `OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 TerrainPlayground --webgpu` (with a WGSL
// shaders.dpak beside the exe) runs the exact browser shader path on the desktop.

#include "Core/Prelude.h"
// imgui.h must be TEXTUALLY included before `import extensions.imgui` - gcc does not merge the
// module's global-module-fragment declarations into a LATER textual include (clang does), so
// include-first is the portable order.
#include "imgui.h"

import foundation.core;
import foundation.rhi;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.shell.desktop;   // CreateShell - required by APP_MAIN's desktop body
import foundation.runtime.desktop; // RunApplication (blocking runner) - required by APP_MAIN
import foundation.graphics;
import foundation.graphics.gpu;
import engine.defaultapp;
import foundation.scene;
import engine.scene;
import engine.render;
import foundation.render;
import extensions.imgui;
import foundation.geometry;  // Primitives (the orbiting shadow caster)
import foundation.materials; // CreatePBR
import foundation.heightfield;
import foundation.terrain.resource;
import engine.terrain;
import foundation.vegetation; // VegetationPlacement
import engine.vegetation;     // VegetationLayerComponent (the grass layer over the dome)
import foundation.resource;   // Ref (the grass layer's materials)

#include "Runtime.Client/AppMain.h"
#include "TerrainPlaygroundApp.h"

APP_MAIN(samples::TerrainPlaygroundApp)
