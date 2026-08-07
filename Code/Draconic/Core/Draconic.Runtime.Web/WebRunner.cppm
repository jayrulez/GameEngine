// draconic.runtime.web - the browser (Emscripten) application runner.
//
// The web sibling of draconic.runtime.desktop. Same job - drive an IApplication (via
// ApplicationHost) against an IShell, one frame at a time - but the EXECUTION MODEL differs: a
// browser cannot own a blocking wall-clock loop (it must return to the event loop to paint, run
// timers, and resolve WebGPU promises), so instead of a while-loop this registers a per-frame
// callback with emscripten_set_main_loop and returns. The browser then calls back once per
// requestAnimationFrame until the shell/app stops. Like the desktop runner it works entirely
// through the abstract IShell interface, so it is windowing-backend agnostic; the concrete shell
// (a canvas-backed web shell) is constructed by the entry point and handed in.
module;
#include "Draconic.Foundation/Prelude.h"
#include <emscripten/emscripten.h>

export module draconic.runtime.web;

import draconic.foundation;
import draconic.shell;          // IShell (interface only - the concrete shell is handed in)
import draconic.graphics;       // GraphicsDevice (handed to the app)
import draconic.runtime.client; // IApplication + ApplicationHost (the runner drives these)

namespace foundation = draconic::foundation;
using namespace draconic::graphics; // GraphicsDevice (moved from draconic::runtime)

namespace draconic::runtime
{
    // What the emscripten frame callback needs. The callback is a plain C function pointer with a
    // single void* arg (no captures in a -fno-exceptions/-fno-rtti world), so the per-run state
    // lives here and is passed by address.
    struct WebLoopState
    {
        ApplicationHost* host = nullptr;
        shell::IShell* shell = nullptr;
        foundation::TimePoint previous;
    };

    // One page hosts one app: the host and loop state outlive RunApplication's return (the browser
    // keeps calling the frame after main() unwinds), so they live in static storage rather than on
    // the returning stack.
    inline ApplicationHost& WebHost()
    {
        static ApplicationHost host;
        return host;
    }
    inline WebLoopState& WebLoop()
    {
        static WebLoopState state;
        return state;
    }

    // One frame, driven by the browser (requestAnimationFrame). Mirrors the body of the desktop
    // while-loop. When either the shell or the app stops, tear down and cancel the callback.
    inline void WebFrame(void* arg)
    {
        auto* state = static_cast<WebLoopState*>(arg);
        if (!state->shell->IsRunning() || !state->host->IsRunning())
        {
            state->host->Stop();
            emscripten_cancel_main_loop();
            return;
        }
        state->shell->ProcessEvents();
        const foundation::TimePoint now = foundation::Clock::Now();
        foundation::f32 dt = (now - state->previous).AsSecondsF();
        state->previous = now;
        if (dt > state->host->Settings().maxFrameTime)
        {
            dt = state->host->Settings().maxFrameTime;
        }
        state->host->Tick(dt);
    }
}

export namespace draconic::runtime
{
    // Web runner: start the app, then hand the frame cadence to the browser and return. Unlike the
    // desktop runner this does NOT block - the loop runs after main() returns (fps=0 =>
    // requestAnimationFrame; simulate_infinite_loop=0 => control returns here, and the Emscripten
    // runtime is kept alive to keep calling WebFrame). Returns 0; the real exit is when WebFrame
    // cancels the loop. DRACONIC_APP_MAIN calls this on Emscripten instead of the desktop runner.
    inline int RunApplication(IApplication& app, shell::IShell& shell,
                              GraphicsDevice* graphics = nullptr)
    {
        ApplicationHost& host = WebHost();
        host.Start(app, &shell, graphics);
        WebLoopState& state = WebLoop();
        state.host = &host;
        state.shell = &shell;
        state.previous = foundation::Clock::Now();
        emscripten_set_main_loop_arg(&WebFrame, &state, 0, 0);
        return 0;
    }
}
