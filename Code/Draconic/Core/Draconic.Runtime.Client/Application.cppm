// Draconic::RuntimeClient - `:app` partition.
//
// The application contract. There is exactly ONE application per host (not a list
// of modules): the application IS the game/tool. It owns subsystem registration,
// so the subsystem set is declared once by the app and is identical whether the
// app runs standalone or embedded in the editor - subsystems are truly pluggable.
// (This is the lesson from Sedulous, where the HOST - EngineApplication/Editor
// Application - forced in its own default subsystems, so neither standalone nor
// editor honored the game's actual subsystem set.)
//
//   IApplicationHost - what the app sees of its host (Context, services, windows).
//   IApplication     - the app/game: registers subsystems + lifecycle hooks.
//
// DefaultApplication (the opinionated base that registers engine default
// subsystems) lives in a SEPARATE library (draconic.engine.defaultapp) so this base
// client never pulls in the engine subsystem libraries - only apps that opt into
// the defaults link it.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.runtime.client:app;

import draconic.foundation;
import draconic.rhi; // PresentMode for the main window's swapchain
import draconic.runtime;
import draconic.shell;
import draconic.graphics;

namespace foundation = draconic::foundation;
using namespace draconic::shell; // IShell + input/window types (moved from draconic::runtime)
using namespace draconic::
    graphics; // GraphicsDevice/RenderWindow/FrameContext (moved from draconic::runtime)
namespace rhi = draconic::rhi;

export namespace draconic::runtime
{
    // App/loop-level settings (frame pacing). Graphics/window config for the main window lives in its
    // RenderWindowDesc (see IApplication::MainRenderWindow) - the same descriptor runtime windows use.
    struct ApplicationSettings
    {
        foundation::f32 fixedTimeStep = 1.0f / 60.0f; // seconds per fixed update
        foundation::f32 maxFrameTime = 0.25f;         // clamp per frame (avoids the spiral of death)
        foundation::u32 maxFixedStepsPerFrame = 4;    // catch-up cap at the ACCUMULATOR (physics P0):
                                                // excess time is DROPPED, so a hitch (debugger
                                                // pause) never cascades into a step storm -
                                                // independent of the runner's maxFrameTime clamp
    };

    // The fixed-update accumulator (physics.md P0): pure step math, host-owned, unit-tested.
    // Advance() returns how many fixed steps this frame runs (clamped; excess time dropped);
    // Alpha() is the leftover fraction of a step in [0,1) - the interpolation weight render
    // consumers (physics pose smoothing) blend prev->current poses with.
    // The fixed-timestep accumulator moved to Draconic.Foundation (scenes own one each
    // since per-scene time); re-exposed here for the host's app-level lane.
    using FixedStepper = foundation::FixedStepper;

    // The host as seen by the application: register subsystems via Ctx(), reach the
    // shell/graphics services, manage runtime windows, request exit. Implemented
    // by ApplicationHost (and, later, by the editor for its embedded runtime).
    class IApplicationHost
    {
    public:
        virtual ~IApplicationHost() = default;

        [[nodiscard]] virtual Context& Ctx() noexcept = 0;
        [[nodiscard]] virtual IShell* Shell() noexcept = 0;
        [[nodiscard]] virtual GraphicsDevice* Graphics() noexcept = 0;

        // The main window's RenderWindow (windows[0]), created by the host before OnStartup. Null when
        // running headless (no shell/graphics). Apps attach their root UI / main viewport to it - the
        // counterpart to the RenderWindow* returned by OpenWindow for secondary windows.
        [[nodiscard]] virtual RenderWindow* MainRenderWindow() noexcept = 0;

        // Open/close OS windows at runtime (each backed by a RenderWindow). The
        // basis for detachable UI windows. Close is deferred to frame end. Both
        // return null / no-op when running headless (no shell/graphics).
        virtual RenderWindow* OpenWindow(const WindowSettings& windowSettings,
                                         const RenderWindowDesc& renderDesc) = 0;
        virtual void CloseWindow(RenderWindow* window) = 0;

        virtual void RequestExit(int code = 0) = 0;
    };

    // The application/game. Exactly one per host. Configure() registers the app's
    // subsystems (the ONLY place subsystems are registered - pluggable). OnLaunch/
    // OnExit bracket "play": for a standalone host they fire once around the loop;
    // an editor fires them on Play/Stop, so the same app runs embedded or standalone.
    class IApplication
    {
    public:
        virtual ~IApplication() = default;

        // Read once by the host before Configure() (frame pacing).
        [[nodiscard]] virtual ApplicationSettings Settings() const { return {}; }

        // The main window's render config (present mode / swapchain format / buffer count) - the SAME
        // descriptor runtime windows take via OpenWindow, so the main and runtime windows configure
        // through one path. Read once by the host when it wraps the shell's main window. Default =
        // Fifo (vsync), sRGB, double-buffered. Override to uncap the frame rate (Immediate/Mailbox), etc.
        [[nodiscard]] virtual RenderWindowDesc MainRenderWindow() const { return {}; }

        virtual void Configure(IApplicationHost& host) { (void)host; } // register subsystems/types
        virtual void OnStartup(IApplicationHost& host) { (void)host; } // after Context.Startup
        virtual void OnLaunch(IApplicationHost& host) { (void)host; }  // enter play
        virtual void OnUpdate(IApplicationHost& host, foundation::f32 deltaTime)
        {
            (void)host;
            (void)deltaTime;
        }
        virtual void OnFixedUpdate(IApplicationHost& host, foundation::f32 fixedDeltaTime)
        {
            (void)host;
            (void)fixedDeltaTime;
        }
        virtual void OnRenderWindow(IApplicationHost& host, FrameContext& frame)
        {
            (void)host;
            (void)frame;
        }
        virtual void OnExit(IApplicationHost& host) { (void)host; }     // leave play
        virtual void OnShutdown(IApplicationHost& host) { (void)host; } // before Context.Shutdown
    };
}
