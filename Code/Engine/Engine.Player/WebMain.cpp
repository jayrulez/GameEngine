// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebMain.cpp - the BROWSER entry point for Engine.Player. Shares PlayerApplication.h with the
// desktop Main.cpp and runs the exact same generic game runner; it differs only in the platform
// trio (web shell + WebGPU + the requestAnimationFrame runner, via APP_MAIN's web body)
// and in how the game reaches it: the browser has no argv, so the player FETCHES the dist from
// the SERVING FOLDER at startup - player.xml + Content.pak (the export output) and the Data/
// tree (Data/.dataroot + Data/Shaders/shaders.dpak, the same layout a desktop dist stages)
// (the export-cooked WGSL engine pack; browsers have no shader compiler) - into the MEMFS root,
// then runs with projectDir ".". Nothing is baked at link time, which is what makes this binary
// a reusable EXPORT TEMPLATE: export any project, drop the files next to the player, serve the
// folder, browse. The fetches are synchronous under ASYNCIFY (the same yield mechanism the GPU
// waits use).

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include <emscripten/emscripten.h>
#include "EmbeddedEngineLogo.h" // the default loading screen's logo

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.fonts;
import foundation.fonts.resource;
import foundation.shell;
import foundation.shell.web; // WebShell (the browser shell) - required by APP_MAIN's web body
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.rhi;        // adapter probe: pick the content pak by compressed-family (P3b)
import foundation.rhi.webgpu; // the browser's WebGPU backend (for the pre-boot adapter probe)
import foundation.runtime;
import foundation.runtime.client;
import foundation.runtime.web; // RunApplication (the rAF runner) - required by APP_MAIN
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
import foundation.script.resource;
import foundation.input;
import foundation.physics;
import foundation.physics.resource;
import engine.physics;
import foundation.input.resource;
import engine.input;
import foundation.ui.resource;
import foundation.ui;           // View / ViewGroup / ProgressBar (the boot-splash controls)
import engine.ui;
import engine.gameinstance; // SceneLoadHandle (PlayerApplication async level load)
import foundation.xml.serialization;
import foundation.settings;
import engine.project;
import foundation.vfs.pak;

#include "PlayerApplication.h"      // the shared runner (uses the imports above)
#include "Runtime.Client/AppMain.h" // APP_MAIN (web body: WebShell + WebGPU + rAF runner)

using namespace foundation::core;

#if defined(ENGINE_SHIP_NATIVE_GAME)
extern "C" foundation::runtime::IRuntimePlugin* CreatePlugin();
#endif


// The IndexedDB mount behind MountUserData. The path is ASCII (the browser's home directory), read
// byte by byte from the heap.
// clang-format off
EM_JS(void, StartUserDataMount, (const char* path), {
    Module.userDataLoaded = false;
    var dir = "";
    for (var at = path; HEAPU8[at] != 0; ++at) dir += String.fromCharCode(HEAPU8[at]);
    try {
        FS.mount(IDBFS, {}, dir);
    } catch (error) {
        console.warn("saves will last this page only: " + error);
        Module.userDataLoaded = true;
        return;
    }
    FS.syncfs(true, function(error) {
        if (error) {
            console.warn("saves will last this page only: " + error);
        } else {
            Module.userDataPersistent = true;
        }
        Module.userDataLoaded = true;
    });
});
EM_JS(int, UserDataLoaded, (), { return Module.userDataLoaded ? 1 : 0; });
EM_JS(int, UserDataPersistent, (), { return Module.userDataPersistent ? 1 : 0; });
// The page's loading card (shell.html) stays up until the game runs; a page without one has
// nothing to take down.
EM_JS(void, PageGameRunning, (), {
    if (typeof loading !== "undefined" && loading.done) loading.done();
});
// A game that quits leaves the page with nothing to draw: the card comes back to say so.
// `started`: whether the game ever ran (a quit), or stopped before its first update (it could not
// start: no GPU, a failed boot).
EM_JS(void, PageGameEnded, (int started), {
    if (typeof loading !== "undefined" && loading.ended) loading.ended(started != 0);
});
// clang-format on

namespace
{
    // Pull one dist file from the serving folder into the MEMFS root. Synchronous under
    // ASYNCIFY (emscripten_wget yields to the browser while the request runs). A build
    // that PRELOADED the file (e.g. the WebScene sample shape) skips the fetch.
    void FetchDistFile(const char* name)
    {
        const StringView path(reinterpret_cast<const utf8char*>(name));
        if (FileExists(path))
        {
            return; // preloaded/bundled - nothing to fetch
        }
        emscripten_wget(name, name);
        if (!FileExists(path))
        {
            LOG_ERROR(u8"Player", u8"could not fetch '{}' from the serving folder - "
                                           u8"is it next to the player page?",
                               path);
        }
    }

    // Fetch `src` from the serving folder, saving it to MEMFS under `dst`. Synchronous under ASYNCIFY.
    // Returns whether the destination file exists afterwards (a 404 leaves it absent).
    bool FetchDistFileAs(const char* src, const char* dst)
    {
        emscripten_wget(src, dst);
        return FileExists(StringView(reinterpret_cast<const utf8char*>(dst)));
    }

    // Pick the content variant pak by the browser's compressed-texture family and fetch it AS
    // Content.pak, so the project loader (which reads Content.pak) needs no change (asset-variants
    // P3b, Fable ruling Q1). A web dist built by this engine ships Content-bc.pak (desktop browsers)
    // + Content-astc.pak (mobile browsers); a single-pak/old bundle falls back to Content.pak.
    //
    // The probe creates a throwaway WebGPU backend to read the adapter's textureCompressionBC/ASTC
    // flags (Asyncify makes requestAdapter synchronous here, the same yield the fetches use), then
    // tears it down before the app boots and creates its own device (two adapter requests/page is
    // fine). If neither family is reported (spec-impossible for a WebGPU device), we log and fall
    // back to the single pak rather than render nothing.
    void SelectAndFetchContentPak()
    {
        if (FileExists(u8"Content.pak"))
        {
            return; // preloaded/bundled single pak - nothing to select
        }

        bool bc = false;
        bool astc = false;
        foundation::rhi::Backend* probe = nullptr;
        if (foundation::rhi::webgpu::CreateBackend(foundation::rhi::webgpu::WebGpuBackendDesc{},
                                                   probe, DefaultAllocator())
                .IsOk() &&
            probe != nullptr)
        {
            const Span<foundation::rhi::Adapter* const> adapters = probe->EnumerateAdapters();
            if (!adapters.IsEmpty())
            {
                const foundation::rhi::AdapterInfo info = adapters[0]->Info();
                bc = info.supportedFeatures.textureCompressionBC;
                astc = info.supportedFeatures.textureCompressionASTC;
            }
            probe->Destroy();
        }

        // Prefer BC (desktop browsers); ASTC is the mobile family. A device won't usually have both.
        const char* variant = bc ? "Content-bc.pak" : (astc ? "Content-astc.pak" : nullptr);
        LOG_INFO(u8"Player", u8"content-variant probe: bc={} astc={} -> {}", bc, astc,
                 StringView(reinterpret_cast<const utf8char*>(variant ? variant : "Content.pak")));

        if (variant != nullptr && FetchDistFileAs(variant, "Content.pak"))
        {
            return; // the matching variant pak is now mounted as Content.pak
        }
        // No variant pak on the server (single-pak / old bundle), or neither family: the single pak.
        FetchDistFile("Content.pak");
    }

    // Saves outlive the page: the user data directory (where a game's save and the user's
    // settings live) is mounted over the browser's IndexedDB, and what the page stored there before
    // is loaded into it, before the game starts reading it. Writers then push changes back with
    // PersistUserData. Synchronous under ASYNCIFY, as the fetches are. A browser without storage
    // (a private window that refuses IndexedDB) keeps the directory in memory for the page.
    void MountUserData()
    {
        const String dir = foundation::core::GetUserDataDirectory();
        (void)CreateDirectories(dir.AsView());
        StartUserDataMount(reinterpret_cast<const char*>(dir.CStr()));
        while (UserDataLoaded() == 0)
        {
            emscripten_sleep(10);
        }
        LOG_INFO(u8"Player", u8"user data at '{}' ({})", dir.AsView(),
                 UserDataPersistent() != 0 ? StringView(u8"kept in the browser's storage")
                                           : StringView(u8"in memory for this page"));
    }

    // Default-constructible so APP_MAIN can own it in static storage: the dist is
    // fetched from the serving folder into the MEMFS root, so the project dir is ".".
    class WebPlayerApplication final : public engine::player::PlayerApplication
    {
    public:
        WebPlayerApplication() : PlayerApplication(MakeOptions()) {}

        // The first update means the game is running: the page's loading card can go.
        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 deltaTime) override
        {
            PlayerApplication::OnUpdate(host, deltaTime);
            if (!m_pageTold)
            {
                m_pageTold = true;
                PageGameRunning();
            }
        }

        // The game has quit (run::requestExit, or the browser shell stopping), or could not start:
        // the page says which, rather than leaving its last frame or black.
        void OnShutdown(foundation::runtime::IApplicationHost& host) override
        {
            PlayerApplication::OnShutdown(host);
            PageGameEnded(m_pageTold ? 1 : 0);
        }

    private:
        bool m_pageTold = false;

        static engine::player::PlayerOptions MakeOptions()
        {
            // Fetch BEFORE the app boots: the project loader reads player.xml/Content.pak
            // during Initialize, and the app resolves its data root (Data/.dataroot) in
            // Configure, then the render subsystem loads Data/Shaders/shaders.dpak on device init.
            MountUserData(); // before the player reads the user's settings or a game its save
            FetchDistFile("player.xml");
            // Content: pick the variant pak by the browser's compressed-texture family (BC vs ASTC)
            // and mount it AS Content.pak, so the loader is unchanged (asset-variants P3b).
            SelectAndFetchContentPak();
            // The data tree, at the SAME layout a desktop dist stages beside the player, so the
            // one discovery walk (cwd "/" -> "/Data") finds it in the browser too.
            (void)CreateDirectories(u8"Data/Shaders");
            FetchDistFile("Data/.dataroot");
            FetchDistFile("Data/Shaders/shaders.dpak");
            engine::player::PlayerOptions options;
            options.projectDir = String(u8".");
#if defined(ENGINE_SHIP_NATIVE_GAME)
            // The web SHIP player (game-native-code.md N5): no dlopen in a browser - the
            // game's plugin is compiled in, same contract as the desktop ship stub.
            options.nativeGame = CreatePlugin();
#endif
            return options;
        }
    };
}

APP_MAIN(WebPlayerApplication)
