// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebMain.cpp: the browser entry for WebScene; the shared scene that exercises the full renderer
// is in WebSceneApp.h. APP_MAIN's web body provides WebShell, WebGPU and the requestAnimationFrame
// runner. The cooked WGSL shaders.dpak and this sample's preload wiring are in CMakeLists.txt.
// Unlike the Player, the sample bundles its shader pack, since no export step runs before it.

#include "Core/Prelude.h"
// imgui.h must be textually included before `import extensions.imgui`: gcc does not merge the
// module's global-module-fragment declarations into a later textual include (clang does), so
// including it first is the portable order (Sandbox does the same).
#if OPTION_HAS_EXTENSION_IMGUI
#include "imgui.h"
#endif

import foundation.core;
import foundation.rhi;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.web;  // RunApplication (the browser runner), required by APP_MAIN
import foundation.shell.web;    // WebShell, required by APP_MAIN
import foundation.graphics;
import foundation.graphics.gpu;
import engine.defaultapp;
import foundation.scene;
import engine.scene;
import foundation.render; // SkyMode, AoMode and the RenderSubsystem tweak surface
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
