// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebMain.cpp: the browser entry for TerrainPlayground; the shared terrain showcase is in
// TerrainPlaygroundApp.h. APP_MAIN's web body provides WebShell, WebGPU and the
// requestAnimationFrame runner. The cooked WGSL shaders.dpak and this sample's preload wiring are
// in CMakeLists.txt: the sample bundles its shader pack because the browser has no compiler.

#include "Core/Prelude.h"
// imgui.h must be textually included before `import extensions.imgui`: gcc does not merge the
// module's global-module-fragment declarations into a later textual include (clang does), so
// including it first is the portable order.
#include "imgui.h"

import foundation.core;
import foundation.rhi;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.web; // RunApplication (the browser runner), required by APP_MAIN
import foundation.shell.web;   // WebShell, required by APP_MAIN
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
import foundation.shaders;        // ShaderEffect, ShaderFlags (the grass's Wind group)
import foundation.shaders.system; // ShaderSystem::EffectForFlag
import foundation.heightfield;
import foundation.terrain.resource;
import engine.terrain;
import foundation.vegetation; // VegetationPlacement
import engine.vegetation;     // VegetationLayerComponent (the grass layer over the dome)
import foundation.resource;   // Ref (the grass layer's materials)

#include "Runtime.Client/AppMain.h"
#include "TerrainPlaygroundApp.h"

APP_MAIN(samples::TerrainPlaygroundApp)
