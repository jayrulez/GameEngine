// Main.cpp - the DESKTOP entry for WebScene (see WebSceneApp.h - the shared full-renderer
// exercise scene). Desktop platform trio via DRACONIC_APP_MAIN's desktop body, which also gives
// the backend flags: `WebScene --vulkan` vs `--webgpu` compares the SAME scene across backends,
// and `DRACONIC_USE_SHADER_PACK=1 DRACONIC_WEBGPU_WGSL=1 WebScene --webgpu` (with a WGSL
// shaders.dpak beside the exe) runs the exact browser shader path on the desktop - the fast,
// debuggable repro for web-render bugs before ever opening a browser.

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
import draconic.shell.desktop;   // CreateShell - required by DRACONIC_APP_MAIN's desktop body
import draconic.runtime.desktop; // RunApplication (blocking runner) - required by DRACONIC_APP_MAIN
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
