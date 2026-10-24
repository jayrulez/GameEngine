// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Main.cpp - the DESKTOP entry for WebScene (see WebSceneApp.h - the shared full-renderer
// exercise scene). Desktop platform trio via APP_MAIN's desktop body, which also gives
// the backend flags: `WebScene --vulkan` vs `--webgpu` compares the SAME scene across backends,
// and `OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 WebScene --webgpu` (with a WGSL
// shaders.dpak beside the exe) runs the exact browser shader path on the desktop - the fast,
// debuggable repro for web-render bugs before ever opening a browser.

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
import foundation.shell.desktop;   // CreateShell - required by APP_MAIN's desktop body
import foundation.runtime.desktop; // RunApplication (blocking runner) - required by APP_MAIN
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
