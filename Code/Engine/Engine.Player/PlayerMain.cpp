// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine.Player: the generic game runner.
//
// Runs a project with no native game code: engine subsystems, the project's content and the
// default scene, simulating, and, when the manifest names one, the project's game script (the
// scripted IApplication counterpart: a `Game` class in AngelScript or Luau with
// launch/update(dt)/exit, orchestrating above scenes; the backend resolves by the script's
// language). With scripting, the player, scripts and cooked content are the game; projects that
// outgrow scripts move to a native IApplication at the same point.
//
// This is the desktop boot, built as the Engine.Player.Main library: Main.cpp is the thin dev
// stub, and ShipMain.cpp, the ship stub, calls PlayerMain with the game's static plugin. The
// browser sibling is WebMain.cpp; both share PlayerApplication.h.
//
// Usage: Engine.Player <projectDir> [--scene <source-db-path>] [--exit-after <seconds>]
//                      [--profile-capture <seconds>] [--profile-overlay]
//                      [--data-root <dir>]  (engine data; default = the Data/.dataroot walk)
//                      [--screenshot <png> [--screenshot-frame N | --screenshot-after S]
//                       [--screenshot-count N] [--screenshot-exit]]
//
// Two modes, detected by layout:
//   Project dir (Project.xml): scenes load from the authored source DB (their cooked form is
//     the authored form; scenes have no builder), and products resolve from Cooked/, the
//     editor's own runtime path. The dev loop.
//   Dist dir (Content.pak and player.xml, staged by Tools.Export): one binary DB inside the pak
//     holds products and scenes; the game script rides in the pak as a raw entry. No editor
//     code links into this binary: the shipping shape.

module;

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include "EmbeddedEngineLogo.h" // the default loading screen's logo

module engine.player.main;

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.fonts;
import foundation.fonts.resource;
import foundation.shell;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.runtime;
import foundation.runtime.client;
import foundation.profiler;
import foundation.profiler.capture;
import foundation.runtime.desktop;
import engine.defaultapp;
import foundation.scene;
import engine.scene;
import foundation.scene.resource;
import foundation.render;
import engine.render;
import foundation.animation;
import foundation.animation.resource;
import engine.animation;
import foundation.particles;
import foundation.particles.resource;
import engine.particles;
import foundation.geometry;
import foundation.geometry.resource;
import foundation.audio;
import foundation.audio.resource;
import engine.audio;
import foundation.materials;
import foundation.materials.resource;
import foundation.texture;
import foundation.texture.resource;
import foundation.image.resource;
import foundation.model.resource;
import foundation.script;
import foundation.script.resource; // ScriptClass (the cooked game script, bound from the content DB)
import foundation.input;
import foundation.physics;
import foundation.physics.resource;
import engine.physics;
import foundation.input.resource;
import engine.input;
import engine.gameinstance; // SceneLoadHandle (the async boot load)
import engine.gui;                 // the engine GUI (its fonts, sheets and boot splash)
import foundation.gui;           // the GUI's nodes and data (the splash's progress)
import foundation.gui.resources; // GuiDocument (the GUI loading screen)
import foundation.xml.serialization;
import foundation.settings;
import engine.project; // manifest and layout (runtime-side, editor-free)
import foundation.vfs.pak; // dist mode: one Content.pak holds products, scenes and scripts

#include "PlayerApplication.h" // the shared runner (uses the imports above)

using namespace foundation::core;
namespace runtime = foundation::runtime;
namespace shell = foundation::shell;
namespace graphics = foundation::graphics;
using engine::player::PlayerApplication;
using engine::player::PlayerOptions;

extern "C" const char* BuildStamp();

int engine::player::PlayerMain(int argc, char** argv,
                               foundation::runtime::IRuntimePlugin* nativeGame)
{
    // --version: report the engine version and build identity and exit, before any startup work.
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--version") == 0)
        {
            std::printf("Player %.*s (build %s)\n",
                        static_cast<int>(engine::project::kEngineVersionString.Size()),
                        reinterpret_cast<const char*>(engine::project::kEngineVersionString.Data()),
                        BuildStamp());
            return 0;
        }
    }

    ConsoleSink consoleSink;
    GlobalLogger().AddSink(&consoleSink);
    LOG_INFO(u8"Build", u8"Player {} (build {})", engine::project::kEngineVersionString,
                      reinterpret_cast<const char8_t*>(BuildStamp()));
    GlobalLogger().SetMinLevel(LogLevel::Info);

    PlayerOptions options;
    f32 exitAfterSeconds = 0.0f;
    f64 profileCaptureSeconds = 0.0; // --profile-capture: seconds to capture from the start
    if (argc > 1 && argv[1][0] != '-')
    {
        options.projectDir = String(StringView(reinterpret_cast<const utf8char*>(argv[1])));
    }
    else
    {
        // No path given: behave like a distributed game binary, whose game is wherever it is.
        // Try the current directory, then the executable's own directory (double-click /
        // run-from-anywhere), then the dev default.
        namespace fs = std::filesystem;
        std::error_code ec;
        auto hasGame = [](const fs::path& dir)
        {
            std::error_code e;
            return fs::exists(dir / "Content.pak", e) || fs::exists(dir / "Project.xml", e);
        };
        const fs::path exeDir =
            fs::weakly_canonical(fs::absolute(fs::path(argv[0]), ec), ec).parent_path();
        fs::path chosen = ".";
        if (!hasGame(chosen) && hasGame(exeDir))
        {
            chosen = exeDir;
        }
        else if (!hasGame(chosen))
        {
            chosen = "EditorProject";
        } // dev fallback
        options.projectDir =
            String(StringView(reinterpret_cast<const utf8char*>(chosen.string().c_str())));
    }
    for (int i = 1; i < argc - 1; ++i)
    {
        if (std::strcmp(argv[i], "--scene") == 0)
        {
            options.sceneOverride =
                String(StringView(reinterpret_cast<const utf8char*>(argv[i + 1])));
        }
        if (std::strcmp(argv[i], "--exit-after") == 0)
        {
            options.exitAfterSeconds = static_cast<f32>(std::atof(argv[i + 1]));
            exitAfterSeconds = options.exitAfterSeconds;
        }
        if (std::strcmp(argv[i], "--profile-capture") == 0)
        {
            profileCaptureSeconds = std::atof(argv[i + 1]);
        }
    }

    // The window the game asked for, read before the window exists: the dist's player.xml, or a
    // dev tree's Project.xml. Neither, and the defaults stand.
    engine::project::ProjectSettings manifest;
    {
        foundation::vfs::NativeFileSystem root(options.projectDir.AsView(), DefaultAllocator());
        if (!engine::project::LoadPlayerManifest(root, manifest).IsOk())
        {
            LOG_WARNING(u8"Player", u8"no manifest in '{}', the default window", options.projectDir);
        }
    }
    // Captures are named after the game.
    foundation::profiler::CaptureRecorder::Get().SetApplication(
        manifest.name.IsEmpty() ? StringView(u8"Player") : manifest.name.AsView());
    shell::WindowSettings ws;
    ws.title = manifest.name.IsEmpty() ? StringView(u8"Player") : manifest.name.AsView();
    ws.width = Max(manifest.windowWidth, 1u);
    ws.height = Max(manifest.windowHeight, 1u);
    ws.resizable = manifest.windowResizable;
    switch (manifest.windowMode)
    {
    case engine::project::WindowMode::Windowed:
        ws.fullscreen = shell::WindowFullscreen::None;
        break;
    case engine::project::WindowMode::Fullscreen:
        ws.fullscreen = shell::WindowFullscreen::Exclusive;
        break;
    case engine::project::WindowMode::Borderless:
        ws.fullscreen = shell::WindowFullscreen::Desktop;
        break;
    }
    auto shellPtr = shell::CreateShell(DefaultAllocator(), ws);
    if (shellPtr.Get() == nullptr || shellPtr->MainWindow() == nullptr)
    {
        std::fprintf(stderr, "Engine.Player: failed to create the OS shell/window\n");
        return 1;
    }

    // The backend comes from the command line (--vulkan, the default, --webgpu or --dx12), so the
    // desktop player can drive the same WebGPU backend the browser uses, with the DXC runtime
    // compiler and shader hot-reload the web build lacks. This debugs web rendering without the
    // wasm and export loop.
    // Validation follows the build config unless --gpu-validation / --no-gpu-validation says
    // otherwise, as in the editor, so their frame times compare.
    graphics::GraphicsDeviceDesc gdd = graphics::DeviceDescFromArguments(argc, argv);
    auto gpu = graphics::CreateGraphicsDevice(gdd);
    if (!gpu.HasValue())
    {
        std::fprintf(stderr, "Engine.Player: failed to create the graphics device\n");
        return 1;
    }

    // The statically-provided native game plugin (ship stubs), carried on the options so dev
    // and ship builds converge on one path.
    options.nativeGame = nativeGame;

    PlayerApplication app(static_cast<PlayerOptions&&>(options));
    // Engine data (shaders, built-in fonts): an explicit --data-root, else the discovery walk
    // from the executable (a dist stages Data/ beside the player). Resolved in Configure.
    app.SetDataRoot(foundation::vfs::DataRootFromArguments(argc, argv).AsView());
    app.OnCommandLine(argc, argv); // the base app's flags (--screenshot ...)
    app.SetExitAfterSeconds(exitAfterSeconds); // --exit-after
#if BUILD_PROFILING
    // --profile-capture <seconds>: a capture from the start, to <user-data>/profiles.
    if (profileCaptureSeconds > 0.0)
    {
        String folder = GetUserDataDirectory();
        folder.Append(u8"/profiles");
        (void)foundation::profiler::CaptureRecorder::Get().Start(foundation::profiler::Profiler::Get(),
                                                                 profileCaptureSeconds, folder.AsView());
    }
#endif
    const int code = runtime::RunApplication(app, *shellPtr, gpu.Value().Get());
    GlobalLogger().RemoveSink(&consoleSink);
    return code;
}
