// Foundation::Runtime.Client - the `foundation.runtime.client` module.
//
// ApplicationHost: the concrete, generic host that drives exactly ONE IApplication.
// Owns a Context, an optional (borrowed) GraphicsDevice, and the LIST of
// RenderWindows it presents. It is infrastructure, NOT subclassed - all behavior
// lives in the IApplication. It is deliberately LOOP-AGNOSTIC: the shell layer
// drives Start/Tick/Stop (a blocking loop on desktop, a callback on Emscripten),
// so the host contains no run loop.
//
// The host implements IApplicationHost (the view the application gets of it). The
// application registers its subsystems in Configure() - the host forces in none -
// so the subsystem set is the application's alone and identical standalone or in
// the editor.
//
// Multi-window is uniform: the main window is windows[0]; every frame renders the
// whole list. Windows can be opened/closed at runtime (OpenWindow/CloseWindow) -
// the basis for detachable UI windows - with close deferred to frame end.

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h"

export module foundation.runtime.client;

export import :app;           // ApplicationSettings, IApplicationHost, IApplication
export import :embedded_host; // EmbeddedApplicationHost (editor-embedded runtime)

import foundation.core;
import foundation.runtime;
import foundation.shell;
import foundation.graphics;
import foundation.profiler;

namespace core = foundation::core;
using namespace foundation::shell; // IShell + input/window types (moved from foundation::runtime)
using namespace foundation::graphics; // GraphicsDevice/RenderWindow/FrameContext (moved from foundation::runtime)

export namespace foundation::runtime
{
    class ApplicationHost final : public IApplicationHost
    {
    public:
        ApplicationHost() = default;
        ~ApplicationHost() override = default;

        ApplicationHost(const ApplicationHost&) = delete;
        ApplicationHost& operator=(const ApplicationHost&) = delete;

        // Bring the application up: read settings, register subsystems (app's
        // Configure), start the Context, create the main RenderWindow (if shell
        // +graphics), then enter play (app OnLaunch). Idempotent. The application,
        // shell, and graphics device are all BORROWED (owned by the entry point);
        // shell/graphics stay null for headless runs.
        void Start(IApplication& app, IShell* shell = nullptr, GraphicsDevice* graphics = nullptr)
        {
            if (m_started)
            {
                return;
            }
            m_app = &app;
            m_shell = shell;
            m_graphics = graphics;
            m_settings = app.Settings();

            // Bring up the engine-wide JobSystem before any subsystem starts, so it is
            // available to all of them and outlives them (torn down last, in Stop()).
            core::InitGlobalJobSystem();

            m_app->Configure(*this);
            m_context.Startup();

            // The main window already exists on the shell; give it a RenderWindow.
            if (m_shell != nullptr && m_graphics != nullptr && m_shell->WindowManager() != nullptr)
            {
                if (IWindow* main = m_shell->WindowManager()->MainWindow())
                {
                    const RenderWindowDesc mainDesc =
                        app.MainRenderWindow(); // app's main-window render config
                    auto rw = m_graphics->CreateRenderWindow(*main, mainDesc);
                    if (rw.HasValue())
                    {
                        m_windows.PushBack(
                            static_cast<core::UniquePtr<RenderWindow>&&>(rw.Value()));
                    }
                }
            }

            m_app->OnStartup(*this);
            m_app->OnLaunch(*this); // standalone enters play immediately

            m_started = true;
            m_running = true;
        }

        // Advance exactly one frame with an explicit delta. The shell runner
        // passes wall-clock time; call directly for deterministic stepping.
        void Tick(core::f32 deltaTime)
        {
            PROFILE_FRAME_BEGIN();
            m_context.BeginFrame(deltaTime);

            {
                PROFILE_SCOPE("Update");
                // Simulation lanes run on SCALED time (slow-mo/pause); the app hook and
                // frame bookkeeping keep the raw dt.
                const core::f32 scaledDelta = deltaTime * m_context.TimeScale();
                // The fixed step is CONFIG on the context (the scene bridge seeds per-scene
                // steppers from it); the context-level fixed EXECUTION lane is gone (FrameTime
                // cutover - nothing overrode it). The host stepper survives for the APP-level
                // fixed hook (networking: DriveNetwork per instance).
                m_context.SetFixedTimeStep(m_settings.fixedTimeStep);
                m_stepper.step = m_settings.fixedTimeStep;
                m_stepper.maxSteps = m_settings.maxFixedStepsPerFrame;
                const core::u32 fixedSteps = m_stepper.Advance(scaledDelta);
                for (core::u32 i = 0; i < fixedSteps; ++i)
                {
                    m_app->OnFixedUpdate(*this, m_settings.fixedTimeStep);
                }

                m_context.Update(scaledDelta);
                m_app->OnUpdate(*this, deltaTime);
                m_context.PostUpdate(scaledDelta);
            }

            // Render every window uniformly (main == windows[0]).
            if (m_graphics != nullptr)
            {
                PROFILE_SCOPE("Render");
                for (auto& rw : m_windows)
                {
                    rw->SyncSize();
                    // Acquire BLOCKS the CPU until a swapchain image is free - under vsync (or a
                    // GPU-bound frame) this is where the CPU waits for the display/GPU, so scope it
                    // separately to tell a healthy present-wait from a real stall.
                    FrameContext frame{};
                    {
                        PROFILE_SCOPE("Render.Acquire");
                        frame = rw->BeginFrame();
                    }
                    if (!frame.valid)
                    {
                        continue;
                    }
                    m_app->OnRenderWindow(*this, frame);
                    {
                        PROFILE_SCOPE("Render.Present"); // record submit + queue present
                        rw->EndFrame(frame);
                    }
                }
                {
                    PROFILE_SCOPE(
                        "Render.Advance"); // ring step; may wait on the frame fence
                    m_graphics->AdvanceFrame();
                }
            }

            m_context.EndFrame();
            FlushPendingCloses();
            PROFILE_FRAME_END();
        }

        // Tear the application down: leave play, stop the Context, destroy windows.
        // Idempotent.
        void Stop()
        {
            if (!m_started)
            {
                return;
            }
            m_app->OnExit(*this);
            m_context.Shutdown();
            m_app->OnShutdown(*this);

            m_pendingClose.Clear();
            m_windows.Clear(); // RenderWindow dtors WaitIdle + free GPU resources

            // Tear down the engine-wide JobSystem last - after every subsystem (Context.Shutdown)
            // and all GPU resource frees (window dtors), so nothing references it afterward.
            core::ShutdownGlobalJobSystem();

            m_started = false;
            m_running = false;
        }

        // --- IApplicationHost ---
        void RequestExit(int code = 0) noexcept override
        {
            m_running = false;
            m_exitCode = code;
        }

        [[nodiscard]] Context& Ctx() noexcept override { return m_context; }
        [[nodiscard]] IShell* Shell() noexcept override { return m_shell; }
        [[nodiscard]] GraphicsDevice* Graphics() noexcept override { return m_graphics; }

        [[nodiscard]] RenderWindow* MainRenderWindow() noexcept override
        {
            return m_windows.IsEmpty() ? nullptr : m_windows[0].Get();
        }

        RenderWindow* OpenWindow(const WindowSettings& windowSettings,
                                 const RenderWindowDesc& renderDesc) override
        {
            if (m_shell == nullptr || m_graphics == nullptr)
            {
                return nullptr;
            }
            IWindowManager* wm = m_shell->WindowManager();
            if (wm == nullptr)
            {
                return nullptr;
            }

            auto osWindow = wm->CreateWindow(windowSettings);
            if (!osWindow.HasValue())
            {
                return nullptr;
            }

            auto rw = m_graphics->CreateRenderWindow(*osWindow.Value(), renderDesc);
            if (!rw.HasValue())
            {
                wm->DestroyWindow(osWindow.Value());
                wm->FlushDestroyed();
                return nullptr;
            }

            RenderWindow* ptr = rw.Value().Get();
            m_windows.PushBack(static_cast<core::UniquePtr<RenderWindow>&&>(rw.Value()));
            return ptr;
        }

        void CloseWindow(RenderWindow* window) override
        {
            if (window == nullptr)
            {
                return;
            }
            for (RenderWindow* p : m_pendingClose)
            {
                if (p == window)
                {
                    return;
                }
            } // already queued
            m_pendingClose.PushBack(window);
        }

        [[nodiscard]] const ApplicationSettings& Settings() const noexcept { return m_settings; }
        [[nodiscard]] bool IsRunning() const noexcept { return m_running; }
        [[nodiscard]] int ExitCode() const noexcept { return m_exitCode; }
        [[nodiscard]] core::Span<const core::UniquePtr<RenderWindow>> Windows() const noexcept
        {
            return core::Span<const core::UniquePtr<RenderWindow>>(m_windows.Data(),
                                                                   m_windows.Size());
        }

    private:
        // Destroy windows queued by CloseWindow: free the RenderWindow (GPU) then
        // the OS window. Runs at frame end, after the GPU finished the frame.
        void FlushPendingCloses()
        {
            if (m_pendingClose.IsEmpty())
            {
                return;
            }
            IWindowManager* wm = (m_shell != nullptr) ? m_shell->WindowManager() : nullptr;

            for (RenderWindow* dead : m_pendingClose)
            {
                IWindow* osWindow = &dead->Window();
                for (core::usize i = 0; i < m_windows.Size(); ++i)
                {
                    if (m_windows[i].Get() == dead)
                    {
                        m_windows.RemoveAt(i);
                        break;
                    } // dtor frees GPU resources
                }
                if (wm != nullptr)
                {
                    wm->DestroyWindow(osWindow);
                }
            }
            m_pendingClose.Clear();
            if (wm != nullptr)
            {
                wm->FlushDestroyed();
            }
        }

        Context m_context;
        ApplicationSettings m_settings;
        IApplication* m_app = nullptr;                        // borrowed; owned by the entry point
        IShell* m_shell = nullptr;                            // borrowed; owned by the entry point
        GraphicsDevice* m_graphics = nullptr;                 // borrowed; owned by the entry point
        core::Array<core::UniquePtr<RenderWindow>> m_windows; // [0] == main
        core::Array<RenderWindow*> m_pendingClose;            // deferred destroy
        bool m_started = false;
        bool m_running = false;
        int m_exitCode = 0;
        FixedStepper m_stepper;
    };
}
