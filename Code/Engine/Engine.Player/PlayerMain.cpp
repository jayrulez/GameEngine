// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine.Player - the generic game runner.
//
// Runs a project with ZERO native game code: engine subsystems + the project's content +
// the default scene, simulating - and, when the manifest names one, the project's GAME SCRIPT
// (the scripted IApplication counterpart: a `Game` class in AngelScript OR Luau with
// launch/update(dt)/exit, orchestrating above scenes - the backend resolves by the script's
// language). With scripting, player + scripts + cooked content IS the game; projects that outgrow
// scripts graduate to a native IApplication at the same seam.
//
// This is the DESKTOP boot, built as the Engine.Player.Main LIBRARY (launcher-as-library,
// game-native-code.md N1): Main.cpp is the thin dev stub, and the exporter's generated ship
// stub calls PlayerMain with the game's static plugin. The browser sibling is WebMain.cpp;
// both share PlayerApplication.h.
//
// Usage: Engine.Player <projectDir> [--scene <source-db-path>] [--exit-after <seconds>]
//                      [--data-root <dir>]  (engine data; default = the Data/.dataroot walk)
//                      [--screenshot <png> [--screenshot-frame N | --screenshot-after S]
//                       [--screenshot-count N] [--screenshot-exit]]
//
// Two modes, detected by layout:
//   PROJECT dir (Project.xml): scenes load from the authored source DB (their cooked form IS
//     the authored form - scenes are builder-less by design), products resolve from Cooked/ -
//     the editor's own runtime path. The dev loop.
//   DIST dir (Content.pak + player.xml, staged by Tools.Export): ONE binary DB inside the pak
//     holds products AND scenes; the game script rides in the pak as a raw entry. Zero editor
//     code links into this binary - the shipping shape.

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
import foundation.ui.resource;  // UITheme (the manifest's default theme)
import foundation.ui;           // View / ViewGroup / ProgressBar (the boot-splash controls)
import engine.ui; // UISubsystem (IME target + default theme + the boot splash overlay)
import engine.gameinstance; // SceneLoadHandle (the async boot load)
import foundation.xml.serialization;
import foundation.settings;
import engine.project; // manifest + layout (runtime-side, editor-free)
import foundation.vfs.pak; // dist mode: one Content.pak holds products + scenes + scripts

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
    // --version: report engine version + build identity and exit, before any startup work.
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
    if (argc > 1 && argv[1][0] != '-')
    {
        options.projectDir = String(StringView(reinterpret_cast<const utf8char*>(argv[1])));
    }
    else
    {
        // No path given: behave like a distributed game binary - the game is wherever we are.
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

    // Backend from the CLI (--vulkan default / --webgpu / --dx12), so the desktop player can drive the
    // SAME WebGPU backend the browser uses - with the DXC runtime compiler + shader hot-reload present,
    // which the web build lacks. Invaluable for debugging web-render issues without the wasm/export loop.
    // Validation follows the build config unless --gpu-validation / --no-gpu-validation says
    // otherwise, as in the editor, so their frame times compare.
    graphics::GraphicsDeviceDesc gdd = graphics::DeviceDescFromArguments(argc, argv);
    auto gpu = graphics::CreateGraphicsDevice(gdd);
    if (!gpu.HasValue())
    {
        std::fprintf(stderr, "Engine.Player: failed to create the graphics device\n");
        return 1;
    }

    // The statically-provided native game plugin (ship stubs); consumed at the host
    // seam in N2 - carried on the options so dev and ship converge on one path.
    options.nativeGame = nativeGame;

    PlayerApplication app(static_cast<PlayerOptions&&>(options));
    // Engine data (shaders, built-in fonts): an explicit --data-root, else the discovery walk
    // from the executable (a dist stages Data/ beside the player). Resolved in Configure.
    app.SetDataRoot(foundation::vfs::DataRootFromArguments(argc, argv).AsView());
    app.OnCommandLine(argc, argv); // the base app's flags (--screenshot ...)
    app.SetExitAfterSeconds(exitAfterSeconds); // was parsed and never applied before
    const int code = runtime::RunApplication(app, *shellPtr, gpu.Value().Get());
    GlobalLogger().RemoveSink(&consoleSink);
    return code;
}
