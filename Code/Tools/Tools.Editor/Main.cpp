// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor - the editor executable, where the editor is assembled.
// Creates the OS shell and graphics device and runs EditorApplication. The per-subsystem editor
// modules are linked here and registered on the app's context, so the editor core and app
// libraries never link engine subsystems.
//
// Usage: Tools.Editor [projectDirectory] [--project <dir>]
//   With a project (positional or --project): opens it directly, scaffolding Project.xml and
//   Content/Sources/Cooked/Editor/.cache on first run.
//   With no project: starts on the built-in project manager (recent projects from the per-user
//   registry; open, create, remove); File > Close Project returns to it.
//   Smoke-test aids: [--exit-after <s>] [--rebuild-after <s>]
//   [--mcp] [--mcp-port <n>] (serve MCP for this run, giving agents access to the open project
//     over 127.0.0.1; the persistent setting is in Preferences)
//   [--screenshot <png> [--screenshot-after <s>] [--screenshot-exit]] (writes the main window,
//   UI included, as a PNG; --screenshot-exit quits once it is written)
//   [--seed] [--seed-primitives] [--version].
//   GPU: [--vulkan|--dx12|--webgpu|--null-gpu] [--gpu-validation|--no-gpu-validation]
//   (validation defaults to on in Debug, off in RelWithDebInfo and Release).

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
import pipeline.importer; // the import framework, which consumers import directly
import engine.composition; // RegisterAllScriptFacades: the complete engine facade surface
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
import editor.animation; // RegisterAnimationClipEditor, RegisterSkeletonEditor, RegisterAnimationGraphEditor
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
import editor.navigation; // RegisterNavigationEditorSettings* (the navigation domain's preferences)
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
    // This binary's composition root: the one place it chooses the ambient allocator.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}


namespace
{
    // The builder and type registrations live in Pipeline::Registration (the composition root):
    // RegisterPipelineTypes() and RegisterAllBuilders() below assemble the same set the CLI cooker
    // and export packager use.
    void RegisterEditorBuilders(pipeline::BuilderRegistry& registry, StringView dataRoot)
    {
        // Core reflection first: property bindings (property-animation capture and preview) walk
        // the patched properties of TypeOf<Transform>(). Otherwise the patch happens lazily when a
        // script manager starts, and a session without scripts never resolves
        // 'Transform.position'. Idempotent.
        foundation::core::RegisterCoreTypes();
        pipeline::RegisterPipelineTypes();
        // Domain editor-settings sections register before the app starts, because the user
        // settings file loads in the app's constructor (see RegisterNavigationEditorSettingsTypes).
        editor::navigation::RegisterNavigationEditorSettingsTypes();
        editor::RegisterGameAudioEditorSettingsTypes();
        pipeline::RegisterAllBuilders(registry, dataRoot);
        // The complete engine script surface (the run, ui, physics, audio and other facades) goes
        // into the global registry, so both the editor's cook and its live script validation
        // (ScriptApiSurface reads the same registry) compile against the surface a running game
        // has. The foundation-level registration above lacks the Engine-level facades. This
        // registers metadata only (no device, GPU or world) and is idempotent.
        engine::RegisterAllScriptFacades();
    }

    // `--seed-primitives`: the headless seed also creates every primitive the creator table offers
    // (Cylinder, Cone and Torus beyond the starter three), which regenerates a project's primitive
    // meshes at the current source version without opening the editor UI.
    bool g_seedAllPrimitives = false;

    // The data root for this process, resolved once in main (an explicit --data-root, else the
    // Data/.dataroot walk up from the executable), the same way every executable finds it. The
    // editor refuses to start without one; there is no compile-time path fallback.
    String g_dataRoot;
    [[nodiscard]] StringView EditorDataRoot() { return g_dataRoot.AsView(); }

    // Starter content for a project created in the project manager: the baseline font set as
    // the manifest default, the sky and the primitives, from the data root.
    void SeedNewProject(editor::EditorContext& ctx, editor::EditorProject& project)
    {
        // The same seeding function MCP's project_create runs (editor.project).
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
    // --version: print the engine version and build identity and exit, before any startup work.
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

    // Log capture first: the editor buffer and console output attach to the global logger before
    // the shell and device are created, so early startup logs reach the Console panel.
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
    // Project selection: an explicit project (positional argument or --project <dir>) opens
    // directly. Without one the editor starts on the built-in project manager (recent projects,
    // open, create), and File > Close Project returns there.
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
            g_seedAllPrimitives = true; // and every primitive creator, not just the three
        }
    }
    config.startInProjectManager = config.projectDirectory.IsEmpty();
    // The splash screen's font, from the data root, with an embedded copy as the last resort. The
    // editor's own fonts come from its style sheet, through its resources.
    const String splashFontPath = foundation::vfs::DataPath(EditorDataRoot(), u8"Engine/Fonts/roboto/Roboto-Regular.ttf");
    const Span<const u8> embeddedFont(reinterpret_cast<const u8*>(g_embeddedEditorFont), static_cast<usize>(g_embeddedEditorFontSize));
    config.logBuffer = &logBuffer;

    // Assembly: this is where the engine subsystems and per-subsystem editor plugins are chosen.
    // The editor core and app libraries never link engine modules; the app drives scene rendering
    // only through the ISceneRenderer interface injected below. The embedded DefaultApplication
    // registers the gameplay subsystems against the editor's runtime context; the editor
    // registers none itself.
    config.seedNewProject = [](editor::EditorContext& ctx, editor::EditorProject& project)
    { SeedNewProject(ctx, project); };
    // Each editor domain registers its pages, tools and services here.
    config.registerEditors = [](editor::app::EditorApplication& app, foundation::runtime::IApplicationHost& host)
    {
        app.SetSceneRenderer(host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>());
        editor::navigation::RegisterNavigationEditorSettings(app.Context());
        // The scene editor: its pages, actions, live MCP tools and services (the document types,
        // the export's scene hooks, and the prefab and scene a model import makes).
        editor::RegisterSceneEditor(app.Context(), host, app);
        // Play in editor: the Game tabs (Play, Play New Instance), the game-audio preference and
        // the PIE tools.
        editor::RegisterPlayInEditor(app.Context(), host, app);
        // The scene view's tools by domain; the toolbar shows them in this order.
        editor::RegisterTerrainSceneTools(app.Context()); // sculpt, paint layers, cut holes; their panels; splatmap thumbnails
        editor::RegisterVegetationViewportTools(); // paint vegetation, paint props
        editor::RegisterVegetationToolPanels();
        editor::RegisterSplineViewportTools(); // the spline control-point editor
        // The asset pages. Each asks the application's page services source for its services when
        // it is created. The generic page is the fallback for an asset with no page of its own;
        // a type-specific page wins over it.
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
                                     foundation::vfs::DataPath(g_dataRoot.AsView(), foundation::shaders::kShaderFolder).AsView()); // the include path for the shader page's check
        editor::RegisterGuiEditor(app.Context(), pageServices); // the GUI's documents and sheets
        editor::RegisterParticleEditor(app.Context(), pageServices);
        editor::RegisterAnimationClipEditor(app.Context(), pageServices);
        editor::RegisterSkeletonEditor(app.Context(), pageServices);
        editor::RegisterAnimationGraphEditor(app.Context(), pageServices);
        editor::RegisterGenericAssetEditor(app.Context(), pageServices);
        // A tripwire on the thumbnail generators: each domain's registration above registers its
        // generators too. Bump a count when a domain gains one, so a generator that stops
        // registering fails here rather than showing as icons where pictures were. Five draw from
        // an image (texture, font, heightfield, audio clip, splat map); eight stage a scene (mesh,
        // material, prefab, scene, particle effect, skeleton, model manifest, collision shape).
        DIAGNOSTIC_ASSERT(app.Context().Thumbnails() != nullptr && app.Context().Thumbnails()->GeneratorCount() == 5);
        DIAGNOSTIC_ASSERT(app.Context().Thumbnails() != nullptr && app.Context().Thumbnails()->SceneGeneratorCount() == 8);
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

        // Importers for OS files dropped onto the editor: the same set the MCP asset_import tool
        // uses, from the composition root.
        pipeline::RegisterAllImporters(app.Context().Importers());

        // Resource factories come from the embedded DefaultApplication, which registers them into
        // the editor's preset ResourceManager in its OnStartup; none are registered here.
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
        mainWindow->Show(); // no splash: show the window now
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

    // Validation follows the build configuration (on in Debug, off in optimized builds) unless the
    // command line says otherwise: a RelWithDebInfo editor is for measuring, so it skips the
    // validation layer's overhead.
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
