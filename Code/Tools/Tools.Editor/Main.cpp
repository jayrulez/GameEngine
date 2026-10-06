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
//   [--screenshot <png> [--screenshot-after <s>]] (the main window, UI included, as a PNG -
//   a run proves what it drew) [--seed] [--seed-primitives] [--version].
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
import foundation.animation.resource;
import engine.animation;
import foundation.particles.resource;
import foundation.animation;
import foundation.particles;
import engine.particles;
import foundation.ui.runtime;
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
import editor.scene;
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
import editor.input;
import editor.propertyanimation;
import engine.input;
import foundation.physics;
import foundation.physics.resource;
import physics.pipeline;
import navigation.pipeline;
import editor.navigation; // RegisterNavigationEditorSettings* (domain-contributed prefs)
import foundation.ui.resource;
import ui.pipeline;
import editor.gameui;
import editor.audio;
import editor.texture;
import editor.image;
import editor.heightfield;
import editor.terrain;
import editor.vegetation; // the Paint Vegetation brush + its panel
import editor.spline;
import editor.fonts;
import editor.physics;
import editor.generic;
import editor.script;
import engine.physics;
import modelimporter;
import foundation.audio;
import foundation.audio.resource;
import audio.pipeline;
import foundation.script;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
import script.angelscript.pipeline;
import editor.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import script.luau.pipeline;
import editor.script.luau;
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
    void RegisterEditorBuilders(pipeline::BuilderRegistry& registry)
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
        pipeline::RegisterAllBuilders(registry);
        // The COMPLETE engine script surface into the global registry (run/ui/physics/audio/...
        // facades), so BOTH the editor's cook AND its live script validation (ScriptApiSurface
        // reads the same global registry) compile against the surface a running game has. The
        // foundation-only registration above is missing every Engine-level facade. Metadata only
        // (no device/GPU/world), idempotent.
        engine::RegisterAllScriptFacades();
        // Per-language EDITOR-UI services (CodeEditView lexers; completion providers later).
#ifdef OPTION_HAS_ANGELSCRIPT
        editor::RegisterAngelScriptEditorUI();
#endif
#ifdef OPTION_HAS_LUAU
        editor::RegisterLuauEditorUI();
#endif
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
    {
        // Fonts resolve from the data root. The UI font also has an embedded twin (below) as a
        // last resort, so UI text renders even if the data file is missing.
        config.fontPath = foundation::vfs::DataPath(EditorDataRoot(),
                                                    u8"Assets/fonts/roboto/Roboto-Regular.ttf");
        config.monoFontPath = foundation::vfs::DataPath(
            EditorDataRoot(), u8"Assets/fonts/dejavu/DejaVuSansMono.ttf");
    }
    config.embeddedFont = reinterpret_cast<const u8*>(g_embeddedEditorFont);
    config.embeddedFontSize = static_cast<usize>(g_embeddedEditorFontSize);
    config.logBuffer = &logBuffer;

    // Assembly: THIS is where the engine subsystems and per-subsystem editor
    // plugins are chosen - the editor core/app libraries never link engine modules; the app
    // drives scene rendering only through the ISceneRenderer interface injected below.
    // Gameplay subsystems are registered by the embedded DefaultApplication against the
    // editor's runtime context - the editor registers NONE itself.
    config.seedNewProject = [](editor::EditorContext& ctx, editor::EditorProject& project)
    { SeedNewProject(ctx, project); };
    config.registerEditors = [](editor::app::EditorApplication& app,
                                foundation::runtime::IApplicationHost& host,
                                foundation::ui::runtime::UIHost& uiHost)
    {
        app.SetSceneRenderer(host.Ctx().GetSubsystem<engine::render::RenderSubsystem>());
        editor::RegisterSceneEditor(app.Context(), host, uiHost,
                                              app.EmbeddedApplication());
        editor::RegisterMaterialEditor(app.Context(), host, uiHost);
        editor::RegisterSettingsProfileEditor(app.Context(), host, uiHost);
        editor::RegisterMeshEditor(app.Context(), host, uiHost);
        editor::RegisterParticleEditor(app.Context(), host, uiHost);
        editor::RegisterAnimationGraphEditor(app.Context(), host, uiHost);
        editor::RegisterAnimationClipEditor(app.Context(), host, uiHost);
        editor::RegisterSkeletonEditor(app.Context(), host, uiHost);
        editor::RegisterInputEditor(app.Context(), host);
        editor::navigation::RegisterNavigationEditorSettings(app.Context());
        editor::RegisterGameAudioEditorSettings(app.Context());
        editor::RegisterPropertyAnimationEditor(app.Context(), host);
        editor::RegisterGameUIEditor(app.Context(), host, uiHost);
        editor::RegisterAudioClipEditor(app.Context(), host);
        editor::RegisterBusLayoutEditor(app.Context(), host);
        editor::RegisterTextureEditor(app.Context());
        editor::RegisterImageEditor(app.Context());
        editor::RegisterHeightfieldEditor(app.Context());
        editor::RegisterTerrainEditor(app.Context(), host, uiHost);
        // Palette order = registration order: the Terrain dropdown, the Vegetation dropdown,
        // then the lone Spline tool.
        editor::RegisterTerrainViewportTools(); // the scene-viewport terrain sculpt + splat brushes
        editor::RegisterTerrainToolPanels();    // their bottom-dock brush settings panels
        editor::RegisterVegetationViewportTools(); // the scene-viewport vegetation mask brush
        editor::RegisterVegetationToolPanels();    // its brush settings panel
        editor::RegisterSplineViewportTools(); // the scene-viewport spline control-point editor
        editor::RegisterFontEditor(app.Context());
        editor::RegisterCollisionShapeEditor(app.Context(), host, uiHost);
        // The FALLBACK page registers like any factory: nearest-base dispatch routes every
        // bespoke page first; anything else lands on the generic serialize-driven form
        // instead of the hard "No editor registered" failure.
        editor::RegisterGenericAssetEditor(app.Context());
        // Thumbnail-generator tripwire: each domain's Register<X>Editor
        // above also registers its thumbnail generator - bump the expected count when a domain
        // gains one, so a silently-unregistered generator fails loudly here, not as icons.
        DIAGNOSTIC_ASSERT(app.Context().Thumbnails() != nullptr &&
                          app.Context().Thumbnails()->GeneratorCount() == 5);
        // 2 = mesh + material (RegisterSceneEditor); the GPU lane's stage renders these.
        DIAGNOSTIC_ASSERT(app.Context().Thumbnails()->SceneGeneratorCount() == 8);
        // Script behavior page + per-backend "New Asset > <Lang> Script" creators.
        // RegisterScriptEditor fans creators over backends that have a registered COOK, so the
        // cooks must be registered FIRST - RegisterAllBuilders (below) also registers them for the
        // cook service, but that runs later, so register them here too (idempotent by languageId).
#ifdef OPTION_HAS_ANGELSCRIPT
        pipeline::RegisterAngelScriptScriptCook();
#endif
#ifdef OPTION_HAS_LUAU
        pipeline::RegisterLuauScriptCook();
#endif
        editor::RegisterScriptEditor(app.Context());
        // File > New: every pipeline domain's creators, the scripts' among them.
        (void)pipeline::RegisterAllCreators(app.Context().Creators());
        RegisterEditorBuilders(app.Builders()); // the cook service routes through this set

        // OS-file importers (drag-drop onto the editor) - the same set the MCP asset_import tool
        // uses, from the composition root.
        pipeline::RegisterAllImporters(app.Context().Importers());

        // Resource factories come from the embedded DefaultApplication (registered into
        // the editor's preset ResourceManager at its OnStartup) - none registered here.
    };

    LOG_INFO(u8"Editor", u8"starting (project: {})", config.projectDirectory);

    shell::WindowSettings ws;
    ws.title = u8"Editor";
    ws.width = 1600;
    ws.height = 900;

    auto shellPtr = shell::CreateShell(AppRoot(), ws);
    if (shellPtr.Get() == nullptr || shellPtr->MainWindow() == nullptr)
    {
        std::fprintf(stderr, "Tools.Editor: failed to create the OS shell/window\n");
        return 1;
    }

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
