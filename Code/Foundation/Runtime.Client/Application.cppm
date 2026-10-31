// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Runtime.Client - `:app` partition.
//
// The application contract. There is exactly one application per host (not a list
// of modules): the application is the game or tool. It owns subsystem registration,
// so the subsystem set is declared once by the app and is identical whether the
// app runs standalone or embedded in the editor. A host never forces in subsystems
// of its own.
//
//   IApplicationHost: what the app sees of its host (SubsystemManager, services, windows).
//   IApplication:     the app or game; registers subsystems and lifecycle hooks.
//
// DefaultApplication (the opinionated base that registers engine default
// subsystems) lives in a separate library (engine.defaultapp) so this base
// client never pulls in the engine subsystem libraries; only apps that opt into
// the defaults link it.

module;
#include "Core/Prelude.h"

export module foundation.runtime.client:app;

import foundation.core;
import foundation.rhi; // PresentMode for the main window's swapchain
import foundation.runtime;
import foundation.shell;
import foundation.graphics;

namespace core = foundation::core;
using namespace foundation::shell; // IShell and the input/window types
using namespace foundation::graphics; // GraphicsDevice/RenderWindow/FrameContext

export namespace foundation::runtime
{
    // App and loop settings (frame pacing). Graphics and window config for the main window lives in
    // its RenderWindowDesc (see IApplication::MainRenderWindow), the same descriptor runtime windows use.
    struct ApplicationSettings
    {
        core::f32 fixedTimeStep = 1.0f / 60.0f; // seconds per fixed update
        core::f32 maxFrameTime = 0.25f;         // clamp per frame (avoids the spiral of death)
        core::u32 maxFixedStepsPerFrame = 4;    // catch-up cap at the accumulator:
                                                // excess time is dropped, so a hitch (debugger
                                                // pause) never cascades into a step storm;
                                                // independent of the runner's maxFrameTime clamp
    };

    // The fixed-update accumulator: pure step math, host-owned.
    // Advance() returns how many fixed steps this frame runs (clamped; excess time dropped);
    // Alpha() is the leftover fraction of a step in [0,1), the weight render consumers
    // (physics pose smoothing) use to blend the previous and current poses.
    // It lives in foundation.core (scenes own one each for per-scene time) and is
    // re-exposed here for the host's app-level fixed hook.
    using FixedStepper = core::FixedStepper;

    // The host as seen by the application: register subsystems via Subsystems(), reach the
    // shell/graphics services, manage runtime windows, request exit. Implemented
    // by ApplicationHost (and by the editor for its embedded runtime).
    class IApplicationHost
    {
    public:
        virtual ~IApplicationHost() = default;

        [[nodiscard]] virtual SubsystemManager& Subsystems() noexcept = 0;
        [[nodiscard]] virtual IShell* Shell() noexcept = 0;
        [[nodiscard]] virtual GraphicsDevice* Graphics() noexcept = 0;

        // The main window's RenderWindow (windows[0]), created by the host before OnStartup. Null when
        // running headless (no shell/graphics). Apps attach their root UI or main viewport to it; it is
        // the counterpart to the RenderWindow* returned by OpenWindow for secondary windows.
        [[nodiscard]] virtual RenderWindow* MainRenderWindow() noexcept = 0;

        // Open/close OS windows at runtime (each backed by a RenderWindow). The
        // basis for detachable UI windows. Close is deferred to frame end. Both
        // return null / no-op when running headless (no shell/graphics).
        virtual RenderWindow* OpenWindow(const WindowSettings& windowSettings,
                                         const RenderWindowDesc& renderDesc) = 0;
        virtual void CloseWindow(RenderWindow* window) = 0;

        virtual void RequestExit(int code = 0) = 0;
    };

    // The application or game. Exactly one per host. Configure() registers the app's
    // subsystems (the only place subsystems are registered). OnLaunch/
    // OnExit bracket "play": for a standalone host they fire once around the loop;
    // an editor fires them on Play/Stop, so the same app runs embedded or standalone.
    class IApplication
    {
    public:
        virtual ~IApplication() = default;

        // Read once by the host before Configure() (frame pacing).
        [[nodiscard]] virtual ApplicationSettings Settings() const { return {}; }

        // The main window's render config (present mode, swapchain format, buffer count): the same
        // descriptor runtime windows take via OpenWindow, so the main and runtime windows configure
        // through one path. Read once by the host when it wraps the shell's main window. The default
        // is Fifo (vsync), sRGB, double-buffered. Override to uncap the frame rate (Immediate/Mailbox).
        [[nodiscard]] virtual RenderWindowDesc MainRenderWindow() const { return {}; }

        /// The process's command line, before Configure: the entry (APP_MAIN or a hand-written
        /// main) hands it over; an app reads the flags it knows (DefaultApplication: --screenshot).
        /// Web entries have no argv and never call it.
        virtual void OnCommandLine(int argc, char** argv) { (void)argc; (void)argv; }
        virtual void Configure(IApplicationHost& host) { (void)host; } // register subsystems/types
        virtual void OnStartup(IApplicationHost& host) { (void)host; } // after SubsystemManager.Startup
        virtual void OnLaunch(IApplicationHost& host) { (void)host; }  // enter play
        virtual void OnUpdate(IApplicationHost& host, core::f32 deltaTime)
        {
            (void)host;
            (void)deltaTime;
        }
        virtual void OnFixedUpdate(IApplicationHost& host, core::f32 fixedDeltaTime)
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
        virtual void OnShutdown(IApplicationHost& host) { (void)host; } // before SubsystemManager.Shutdown
    };
}
