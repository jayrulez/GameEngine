// WebMain.cpp - the BROWSER entry for WebScene (see WebSceneApp.h - the shared full-renderer
// exercise scene). Web platform trio: WebShell + WebGPU + the requestAnimationFrame runner via
// DRACONIC_APP_MAIN's web body. The cooked WGSL shaders.dpak + this sample's preload wiring live
// in CMakeLists (unlike the Player, the sample still BUNDLES its shader pack - it has no export
// step in front of it).

#include "Draconic.Foundation/Prelude.h"
// imgui.h must be TEXTUALLY included before `import draconic.imgui` - gcc does not merge the
// module's global-module-fragment declarations into a LATER textual include (clang does), so
// include-first is the portable order (same as Sandbox).
#if DRACONIC_HAS_EXTENSION_IMGUI
#include "imgui.h"
#endif

import draconic.foundation;
import draconic.rhi;
import draconic.runtime;
import draconic.runtime.client;
import draconic.shell;
import draconic.runtime.web;  // RunApplication (browser runner) - required by DRACONIC_APP_MAIN
import draconic.shell.web;    // WebShell - required by DRACONIC_APP_MAIN
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.engine.defaultapp;
import draconic.scene;
import draconic.engine.scene;
import draconic.render; // SkyMode/AoMode + the RenderSubsystem tweak surface
import draconic.engine.render;
import draconic.geometry;
import draconic.materials;
import draconic.particles;
import draconic.engine.particles;
import draconic.ui;
import draconic.ui.resource; // UIDocument (runtime markup documents)
import draconic.engine.ui;
#if DRACONIC_HAS_EXTENSION_IMGUI
import draconic.imgui;
#endif

#include "Draconic.Runtime.Client/AppMain.h"
#include "WebSceneApp.h"

DRACONIC_APP_MAIN(draconic::samples::WebSceneApp)
