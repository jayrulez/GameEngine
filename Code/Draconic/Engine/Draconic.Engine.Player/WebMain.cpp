// WebMain.cpp - the BROWSER entry point for Draconic.Engine.Player. Shares PlayerApplication.h with the
// desktop Main.cpp and runs the exact same generic game runner; it differs only in the platform
// trio (web shell + WebGPU + the requestAnimationFrame runner, via DRACONIC_APP_MAIN's web body)
// and in how the game reaches it: the browser has no argv, so the player FETCHES the dist from
// the SERVING FOLDER at startup - player.xml + Content.pak (the export output) and shaders.dpak
// (the export-cooked WGSL engine pack; browsers have no shader compiler) - into the MEMFS root,
// then runs with projectDir ".". Nothing is baked at link time, which is what makes this binary
// a reusable EXPORT TEMPLATE: export any project, drop the files next to the player, serve the
// folder, browse. The fetches are synchronous under ASYNCIFY (the same yield mechanism the GPU
// waits use).

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <emscripten/emscripten.h>

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.fonts;
import draconic.fonts.resource;
import draconic.shell;
import draconic.shell.web; // WebShell (the browser shell) - required by DRACONIC_APP_MAIN's web body
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.runtime;
import draconic.runtime.client;
import draconic.runtime.web; // RunApplication (the rAF runner) - required by DRACONIC_APP_MAIN
import draconic.engine.defaultapp;
import draconic.scene;
import draconic.engine.scene;
import draconic.scene.resource;
import draconic.render;
import draconic.engine.render;
import draconic.animation;
import draconic.animation.resource;
import draconic.engine.animation;
import draconic.particles;
import draconic.particles.resource;
import draconic.engine.particles;
import draconic.geometry;
import draconic.geometry.resource;
import draconic.audio;
import draconic.audio.resource;
import draconic.engine.audio;
import draconic.materials;
import draconic.materials.resource;
import draconic.texture;
import draconic.texture.resource;
import draconic.image.resource;
import draconic.model.resource;
import draconic.script;
import draconic.script.resource;
import draconic.input;
import draconic.physics;
import draconic.physics.resource;
import draconic.engine.physics;
import draconic.input.resource;
import draconic.engine.input;
import draconic.ui.resource;
import draconic.engine.ui;
import draconic.xml.serialization;
import draconic.settings;
import draconic.engine.project;
import draconic.vfs.pak;

#include "PlayerApplication.h"      // the shared runner (uses the imports above)
#include "Draconic.Runtime.Client/AppMain.h" // DRACONIC_APP_MAIN (web body: WebShell + WebGPU + rAF runner)

using namespace draconic::foundation;

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
            DRACONIC_LOG_ERROR(u8"Player", u8"could not fetch '{}' from the serving folder - "
                                           u8"is it next to the player page?",
                               path);
        }
    }

    // Default-constructible so DRACONIC_APP_MAIN can own it in static storage: the dist is
    // fetched from the serving folder into the MEMFS root, so the project dir is ".".
    class WebPlayerApplication final : public draconic::player::PlayerApplication
    {
    public:
        WebPlayerApplication() : PlayerApplication(MakeOptions()) {}

    private:
        static draconic::player::PlayerOptions MakeOptions()
        {
            // Fetch BEFORE the app boots: the project loader reads player.xml/Content.pak
            // during Initialize, and the render subsystem loads shaders.dpak on device init.
            FetchDistFile("player.xml");
            FetchDistFile("Content.pak");
            FetchDistFile("shaders.dpak");
            draconic::player::PlayerOptions options;
            options.projectDir = String(u8".");
            return options;
        }
    };
}

DRACONIC_APP_MAIN(WebPlayerApplication)
