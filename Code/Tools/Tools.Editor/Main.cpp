// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor - the editor executable (the ASSEMBLY point).
// Creates the OS shell + graphics device and runs EditorApplication. Per-subsystem editor
// modules get linked HERE and their RegisterEditor(EditorContext&)
// called on the app's context - the editor core/app libraries never link engine subsystems.
//
// Usage: Tools.Editor [projectDirectory] [--project <dir>]
//   With a project (positional or --project): opens it directly (scaffolding Project.xml +
//   Content/Sources/Cooked/Editor/.cache on first run) - the single-project lifecycle.
//   With NO project: starts on the built-in PROJECT MANAGER (recent projects from the
//   per-user registry, open/create/remove); File > Close Project returns to it.
//   Smoke-test aids: [--exit-after <s>] [--rebuild-after <s>]
//   [--mcp] [--mcp-port <n>] (the MCP host for this run: agent access to the open project over
//     127.0.0.1; the persisted preference is in Preferences)
//   [--screenshot <png> [--screenshot-after <s>] [--screenshot-exit]] (the main window, UI
//   included, as a PNG - a run proves what it drew; --screenshot-exit quits once it is written)
//   [--seed] [--seed-primitives] [--version].
//   GPU: [--vulkan|--dx12|--webgpu|--null-gpu] [--gpu-validation|--no-gpu-validation]
//   (validation defaults ON in Debug, OFF in RelWithDebInfo / Release).

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "Core/Log/Log.h"
#include "Core/Debug/Assert.h"

import foundation.core;
import foundation.shell;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.runtime;
import foundation.runtime.client;
import foundation.runtime.desktop;
import engine.scene;
import engine.render;
import engine.project; // kEngineVersionString (the CMake project(VERSION))
import foundation.content;
import foundation.vfs;            // DataPath (the shader page's include path)
import foundation.shaders.system; // kShaderFolder
import foundation.animation.resource;
import engine.animation;
import foundation.particles.resource;
import foundation.animation;
import foundation.particles;
import engine.particles;
import foundation.resource;
import foundation.geometry;
import foundation.geometry.resource;
import foundation.materials.resource;
import foundation.texture.resource;
import pipeline.core;
import pipeline.registration;
import pipeline.importer; // the import framework (a consumer imports it itself)
import engine.composition; // RegisterAllScriptFacades - the COMPLETE engine facade surface
import editor.core;
import editor.app;
import editor.scene; // RegisterSceneEditor
import editor.play; // RegisterPlayInEditor, the game-audio preference
import editor.spline; // RegisterSplineViewportTools
import editor.terrain; // RegisterTerrainSceneTools
import editor.vegetation; // RegisterVegetationViewportTools, RegisterVegetationToolPanels
import editor.generic; // RegisterGenericAssetEditor
import editor.image; // RegisterImageEditor
import editor.heightfield; // RegisterHeightfieldEditor
import editor.texture; // RegisterTextureEditor
import editor.fonts; // RegisterFontEditor
import editor.geometry; // RegisterMeshEditor
import editor.physics; // RegisterCollisionShapeEditor
import editor.materials; // RegisterMaterialEditor
import editor.render; // RegisterSettingsProfileEditor
import editor.audio; // RegisterAudioClipEditor
import editor.input; // RegisterInputMapEditor
import editor.script; // RegisterScriptEditor
import editor.shaders; // RegisterShaderEditor
import editor.gui; // RegisterGuiEditor
import editor.particles; // RegisterParticleEditor
import editor.animation; // RegisterAnimationClipEditor
import texture.pipeline;
import fonts.pipeline;
import image.pipeline;
import foundation.image.resource;
import geometry.pipeline;
import heightfield.pipeline; // HeightfieldAsset (New Asset > Terrain > Heightfield)
import terrain.pipeline;     // TerrainAsset + SplatmapAsset (New Asset > Terrain)
import vegetation.pipeline;  // VegetationMaskAsset (New Asset > Terrain > Vegetation Mask)
import animation.pipeline;
import materials.pipeline;
import shaders.pipeline;
import particles.pipeline;
import foundation.input;
import foundation.input.resource;
import input.pipeline;
import engine.input;
import foundation.physics;
import foundation.physics.resource;
import physics.pipeline;
import navigation.pipeline;
import editor.navigation; // RegisterNavigationEditorSettings* (domain-contributed prefs)
import foundation.ui.resource;
import ui.pipeline;
import engine.physics;
import modelimporter;
import foundation.audio;
import foundation.audio.resource;
import audio.pipeline;
import foundation.script;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
import script.angelscript.pipeline;
#endif
#ifdef OPTION_HAS_LUAU
import script.luau.pipeline;
#endif
import foundation.script.resource;
import script.pipeline;

using namespace foundation::core;
namespace shell = foundation::shell;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}


namespace
{
    // The builder + type registration set lives in Pipeline::Registration (the composition root):
    // RegisterPipelineTypes() + RegisterAllBuilders() below assemble the same set the CLI cooker
    // and export packager use. The editor ADDS its per-language editor-UI services (CodeEditView
    // lexers) on top - those are editor-only and stay host-side.
    void RegisterEditorBuilders(pipeline::BuilderRegistry& registry, StringView dataRoot)
    {
        // Core reflection FIRST: property bindings (property-animation capture/preview) walk
        // TypeOf<Transform>()'s PATCHED properties. Without this the patch only happened
        // lazily when a script manager spun up - a scriptless session never resolved
        // 'Transform.position'. Idempotent.
        foundation::core::RegisterCoreTypes();
        pipeline::RegisterPipelineTypes();
        // Domain editor-settings SECTIONS register before the app boots (the user settings
        // file loads in the app ctor; see RegisterNavigationEditorSettingsTypes).
        editor::navigation::RegisterNavigationEditorSettingsTypes();
        editor::RegisterGameAudioEditorSettingsTypes();
        pipeline::RegisterAllBuilders(registry, dataRoot);
        // The COMPLETE engine script surface into the global registry (run/ui/physics/audio/...
        // facades), so BOTH the editor's cook AND its live script validation (ScriptApiSurface
        // reads the same global registry) compile against the surface a running game has. The
        // foundation-only registration above is missing every Engine-level facade. Metadata only
        // (no device/GPU/world), idempotent.
        engine::RegisterAllScriptFacades();
    }

    // `--seed-primitives`: the headless seed also creates EVERY primitive the creator table
    // offers (Cylinder/Cone/Torus beyond the starter three) - regenerating a project's
    // primitive meshes at the current source version without opening the editor UI.
    bool g_seedAllPrimitives = false;

    // Starter content for a manager-created project: editor::SeedStarterContent (the baseline
    // font set as the manifest default, the sky, the primitives), from the data root's Assets/.
    // THE data root for this process: resolved once in main (an explicit --data-root, else the
    // Data/.dataroot walk from the executable) - the same mechanism every executable uses. The
    // editor refuses to start without one; there is no compile-time path fallback.
    String g_dataRoot;
    [[nodiscard]] StringView EditorDataRoot() { return g_dataRoot.AsView(); }

    void SeedNewProject(editor::EditorContext& ctx, editor::EditorProject& project)
    {
        // The one seeding function the MCP project_create runs too (editor.project).
        editor::SeedStarterContent(AppRoot(), project, ctx.Creators(), EditorDataRoot(), g_seedAllPrimitives);
    }
}
namespace graphics = foundation::graphics;
namespace runtime = foundation::runtime;

// The embedded fallback font (EmbeddedEditorFont.cpp, generated by EmbedBinary.cmake).
extern const unsigned char g_embeddedEditorFont[];
extern const unsigned long long g_embeddedEditorFontSize;

extern "C" const char* BuildStamp();

int main(int argc, char** argv)
{
    // --version: report engine version + build identity and exit, before any startup work.
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--version") == 0)
        {
            std::printf("Editor %.*s (build %s)\n",
                        static_cast<int>(engine::project::kEngineVersionString.Size()),
                        reinterpret_cast<const char*>(engine::project::kEngineVersionString.Data()),
                        BuildStamp());
            return 0;
        }
    }

    // Log capture FIRST: the editor buffer + console output go on the global
    // logger before shell/device creation, so early startup logs reach the Console panel.
    editor::EditorLogBuffer logBuffer{AppRoot()};
    ConsoleSink consoleSink;
    GlobalLogger().AddSink(&logBuffer);
    GlobalLogger().AddSink(&consoleSink);
    LOG_INFO(u8"Build", u8"Editor {} (build {})", engine::project::kEngineVersionString,
                      reinterpret_cast<const char8_t*>(BuildStamp()));
    GlobalLogger().SetMinLevel(LogLevel::Debug); // the Console panel has a Debug filter toggle

    // Engine data first: everything below (fonts, baseline assets, the embedded runtime's
    // shaders, the export's shader cook) reads from the data root.
    g_dataRoot = foundation::vfs::ResolveDataRoot(argc, argv);
    if (g_dataRoot.IsEmpty())
    {
        std::fprintf(stderr, "Tools.Editor: no data root (put Data/ with its .dataroot marker "
                             "beside the editor, or pass --data-root <dir>)\n");
        return 1;
    }

    editor::app::EditorAppConfig config;
    config.dataRoot = g_dataRoot;
    // Project selection: an explicit project (positional arg or --project <dir>) opens
    // directly, Godot-style single-project lifecycle. NO project => the built-in PROJECT
    // MANAGER screen (recent projects, open/create), and File > Close Project returns there.
    if (argc > 1 && argv[1][0] != '-')
    {
        config.projectDirectory = String(StringView(reinterpret_cast<const utf8char*>(argv[1])));
    }
    for (int i = 1; i < argc - 1; ++i)
    {
        if (std::strcmp(argv[i], "--project") == 0)
        {
            config.projectDirectory =
                String(StringView(reinterpret_cast<const utf8char*>(argv[i + 1])));
        }
        if (std::strcmp(argv[i], "--exit-after") == 0)
        {
            config.autoExitSeconds = static_cast<f32>(std::atof(argv[i + 1]));
        }
        if (std::strcmp(argv[i], "--rebuild-after") == 0)
        {
            config.autoRebuildSeconds = static_cast<f32>(std::atof(argv[i + 1]));
        }
        if (std::strcmp(argv[i], "--screenshot") == 0)
        {
            config.screenshotPath = String(StringView(reinterpret_cast<const utf8char*>(argv[i + 1])));
        }
        if (std::strcmp(argv[i], "--screenshot-after") == 0)
        {
            config.screenshotAfterSeconds = static_cast<f32>(std::atof(argv[i + 1]));
        }
        if (std::strcmp(argv[i], "--mcp-port") == 0)
        {
            config.mcpPort = static_cast<u32>(std::atoi(argv[i + 1]));
            config.mcpEnabled = true; // naming a port means serving on it
        }
    }
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--seed") == 0)
        {
            config.seedOnScaffold = true; // a scaffolded project also gets the starter content
        }
        if (std::strcmp(argv[i], "--screenshot-exit") == 0)
        {
            config.screenshotExit = true;
        }
        if (std::strcmp(argv[i], "--mcp") == 0)
        {
            config.mcpEnabled = true; // the MCP host for this run, whatever the preference says
        }
        if (std::strcmp(argv[i], "--seed-primitives") == 0)
        {
            config.seedOnScaffold = true;
            g_seedAllPrimitives = true; // ...plus every primitive creator, not just the three
        }
    }
    config.startInProjectManager = config.projectDirectory.IsEmpty();
    // The splash's face, from the data root, with an embedded twin as the last resort (the editor's
    // own faces are its sheet's, through its resources).
    const String splashFontPath = foundation::vfs::DataPath(EditorDataRoot(), u8"Assets/fonts/roboto/Roboto-Regular.ttf");
    const Span<const u8> embeddedFont(reinterpret_cast<const u8*>(g_embeddedEditorFont), static_cast<usize>(g_embeddedEditorFontSize));
    config.logBuffer = &logBuffer;

    // Assembly: THIS is where the engine subsystems and per-subsystem editor
    // plugins are chosen - the editor core/app libraries never link engine modules; the app
    // drives scene rendering only through the ISceneRenderer interface injected below.
    // Gameplay subsystems are registered by the embedded DefaultApplication against the
    // editor's runtime context - the editor registers NONE itself.
    config.seedNewProject = [](editor::EditorContext& ctx, editor::EditorProject& project)
    { SeedNewProject(ctx, project); };
    // The pages are parked while the editor moves onto the GUI (Documentation/Specs/editor-gui.md,
    // 4.1): each one's RegisterEditor comes back here with its port.
    config.registerEditors = [](editor::app::EditorApplication& app, foundation::runtime::IApplicationHost& host)
    {
        app.SetSceneRenderer(host.Ctx().GetSubsystem<engine::render::RenderSubsystem>());
        editor::navigation::RegisterNavigationEditorSettings(app.Context());
        // The scene editor: its pages, its actions, its MCP live tools, and its services (the
        // document types, the export's scene seams, a model import's prefab and scene).
        editor::RegisterSceneEditor(app.Context(), host, app);
        // Play in editor: the Game tabs (Play, Play New Instance), the game-audio preference, the
        // PIE tools (layer 7, part 3).
        editor::RegisterPlayInEditor(app.Context(), host, app);
        // The scene view's tools by domain; the bar shows them in this order (layer 5, part 3).
        editor::RegisterTerrainSceneTools(app.Context()); // sculpt, paint layers, cut holes; their panels; splatmap thumbnails
        editor::RegisterVegetationViewportTools(); // paint vegetation, paint props
        editor::RegisterVegetationToolPanels();
        editor::RegisterSplineViewportTools(); // the spline control-point editor
        // The asset pages (X6), each asking the application's one source for its services when it
        // is made; the fallback catches an asset with no page of its own, the bespoke ones win by type.
        const editor::app::AssetPageServicesSource& pageServices = app.PageServicesSource();
        editor::RegisterImageEditor(app.Context(), pageServices);
        editor::RegisterHeightfieldEditor(app.Context(), pageServices); // and its thumbnails
        editor::RegisterTerrainEditor(app.Context(), pageServices);
        editor::RegisterVegetationMaskEditor(app.Context(), pageServices);
        editor::RegisterTextureEditor(app.Context(), pageServices);     // and its thumbnails
        editor::RegisterFontEditor(app.Context(), pageServices);        // and its thumbnails
        editor::RegisterMeshEditor(app.Context(), pageServices);        // static and skinned
        editor::RegisterCollisionShapeEditor(app.Context(), pageServices); // and its thumbnails
        editor::RegisterMaterialEditor(app.Context(), pageServices);
        editor::RegisterSettingsProfileEditor(app.Context(), pageServices);
        editor::RegisterAudioClipEditor(app.Context(), pageServices); // and its thumbnails
        editor::RegisterInputMapEditor(app.Context(), pageServices);
        editor::RegisterScriptEditor(app.Context(), pageServices);
        editor::RegisterShaderEditor(app.Context(), pageServices,
                                     foundation::vfs::DataPath(g_dataRoot.AsView(), foundation::shaders::kShaderFolder).AsView()); // a check's include path
        editor::RegisterGuiEditor(app.Context(), pageServices); // the GUI's documents and sheets
        editor::RegisterParticleEditor(app.Context(), pageServices);
        editor::RegisterAnimationClipEditor(app.Context(), pageServices);
        editor::RegisterGenericAssetEditor(app.Context(), pageServices);
        // The script cooks (idempotent by languageId; RegisterAllBuilders below registers them for
        // the cook service too): the script page's check compiles through them.
#ifdef OPTION_HAS_ANGELSCRIPT
        pipeline::RegisterAngelScriptScriptCook();
#endif
#ifdef OPTION_HAS_LUAU
        pipeline::RegisterLuauScriptCook();
#endif
        // File > New: every pipeline domain's creators, the scripts' among them.
        (void)pipeline::RegisterAllCreators(app.Context().Creators());
        RegisterEditorBuilders(app.Builders(), g_dataRoot.AsView()); // the cook service's set

        // OS-file importers (drag-drop onto the editor) - the same set the MCP asset_import tool
        // uses, from the composition root.
        pipeline::RegisterAllImporters(app.Context().Importers());

        // Resource factories come from the embedded DefaultApplication (registered into
        // the editor's preset ResourceManager at its OnStartup) - none registered here.
    };

    LOG_INFO(u8"Editor", u8"starting (project: {})", config.projectDirectory);

    shell::WindowSettings ws;
    ws.title = u8"AssiduousEngine";
    ws.width = 1600;
    ws.height = 900;
    // Hidden until the editor is ready: the splash shows until then (startupFinished).
    ws.hidden = true;

    auto shellPtr = shell::CreateShell(AppRoot(), ws);
    if (shellPtr.Get() == nullptr || shellPtr->MainWindow() == nullptr)
    {
        std::fprintf(stderr, "Tools.Editor: failed to create the OS shell/window\n");
        return 1;
    }
    shell::IWindow* mainWindow = shellPtr->MainWindow();
    (void)editor::app::ApplyEditorWindowIcon(*mainWindow, EditorDataRoot());

    // The splash, from now until the editor is ready: startup holds the main thread with
    // nothing drawn (the device, the shader compiles, the fonts, the project and its pages).
    UniquePtr<editor::app::EditorSplash> splash = editor::app::OpenEditorSplash(
        *shellPtr, EditorDataRoot(), splashFontPath.AsView(), embeddedFont,
        Format(u8"Version {}", engine::project::kEngineVersionString).AsView(), editor::EditorRootAllocator());
    if (splash)
    {
        splash->Step(u8"Starting the graphics device", 0.05f);
    }
    else
    {
        mainWindow->Show(); // no splash: show the window now, as before
    }
    config.startupProgress = [&splash](StringView status, f32 progress)
    {
        if (splash)
        {
            splash->Step(status, progress);
        }
    };
    config.startupFinished = [&splash, mainWindow]()
    {
        mainWindow->Show();
        if (splash)
        {
            splash->Close();
        }
    };

    // Validation follows the build config (Debug ON, optimized OFF) unless the command line
    // says otherwise - a RelWithDebInfo editor is for measuring, not for the layer's overhead.
    graphics::GraphicsDeviceDesc gdd = graphics::DeviceDescFromArguments(argc, argv);
    auto gpu = graphics::CreateGraphicsDevice(gdd);
    if (!gpu.HasValue())
    {
        std::fprintf(stderr, "Tools.Editor: failed to create the graphics device\n");
        return 1;
    }

    editor::app::EditorApplication app(static_cast<editor::app::EditorAppConfig&&>(config));
    const int code = runtime::RunApplication(app, *shellPtr, gpu.Value().Get());

    // The sinks are stack-owned and about to die; detach before returning.
    GlobalLogger().RemoveSink(&logBuffer);
    GlobalLogger().RemoveSink(&consoleSink);
    return code;
}
