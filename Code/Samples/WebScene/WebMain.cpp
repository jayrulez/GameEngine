// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebMain.cpp - the BROWSER entry for WebScene (see WebSceneApp.h - the shared full-renderer
// exercise scene). Web platform trio: WebShell + WebGPU + the requestAnimationFrame runner via
// APP_MAIN's web body. The cooked WGSL shaders.dpak + this sample's preload wiring live
// in CMakeLists (unlike the Player, the sample still BUNDLES its shader pack - it has no export
// step in front of it).

#include "Core/Prelude.h"
// imgui.h must be TEXTUALLY included before `import extensions.imgui` - gcc does not merge the
// module's global-module-fragment declarations into a LATER textual include (clang does), so
// include-first is the portable order (same as Sandbox).
#if OPTION_HAS_EXTENSION_IMGUI
#include "imgui.h"
#endif

import foundation.core;
import foundation.rhi;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.web;  // RunApplication (browser runner) - required by APP_MAIN
import foundation.shell.web;    // WebShell - required by APP_MAIN
import foundation.graphics;
import foundation.graphics.gpu;
import engine.defaultapp;
import foundation.scene;
import engine.scene;
import foundation.render; // SkyMode/AoMode + the RenderSubsystem tweak surface
import engine.render;
import foundation.geometry;
import foundation.navigation;
import foundation.navigation.resource;
import engine.navigation;
import foundation.materials;
import foundation.particles;
import engine.particles;
import foundation.gui;
import foundation.gui.resources; // GuiDocument (the documents made in code)
import engine.gui;
#if OPTION_HAS_EXTENSION_IMGUI
import extensions.imgui;
#endif

#include "Runtime.Client/AppMain.h"
#include "WebSceneApp.h"

APP_MAIN(samples::WebSceneApp)
