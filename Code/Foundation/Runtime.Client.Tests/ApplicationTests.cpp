#include <doctest/doctest.h>

#include "Core/Prelude.h" // <new> reachability for container instantiation (GCC)

import foundation.core;
import foundation.runtime;
import foundation.shell;
import foundation.shell.null;
import foundation.graphics;
import foundation.graphics.null;
import foundation.runtime.client;

using namespace foundation::core;
using namespace foundation::runtime;
using namespace foundation::shell;
using namespace foundation::graphics; // GraphicsDevice/RenderWindow/FrameContext (moved from foundation::runtime)

namespace
{
    // A minimal in-process shell: counts ProcessEvents and can be made to quit.
    class MockShell final : public IShell
    {
    public:
        int processed = 0;
        bool running = true;
        IWindowManager* WindowManager() noexcept override { return nullptr; }
        IWindow* MainWindow() noexcept override { return nullptr; }
        IInputManager* Input() noexcept override { return nullptr; }
        IDialogService* Dialogs() noexcept override { return nullptr; }
        void ProcessEvents() override { ++processed; }
        bool IsRunning() const noexcept override { return running; }
        void RequestExit() override { running = false; }
        void SetClipboardText(StringView text) override { clipboard = String(text); }
        String GetClipboardText() const override { return clipboard; }
        bool HasClipboardText() const noexcept override { return clipboard.Size() > 0; }

    private:
        String clipboard;
    };

    // Counts the frame phases the host drives into the Context. NOTE no FixedUpdate: the
    // context-level fixed lane was deleted in the FrameTime cutover (zero overriders existed);
    // the host's fixed accumulator now drives ONLY the app-level OnFixedUpdate hook (counted
    // on the apps below) - fixed-rate engine work lives per scene.
    class CountingSys final : public Subsystem
    {
    public:
        int begin = 0, update = 0, post = 0, end = 0, inits = 0, shutdowns = 0;
        void BeginFrame(f32) override { ++begin; }
        void Update(f32) override { ++update; }
        void PostUpdate(f32) override { ++post; }
        void EndFrame() override { ++end; }

    protected:
        void OnInit() override { ++inits; }
        void OnShutdown() override { ++shutdowns; }
    };

    // Records the lifecycle hook order: 1=Configure 2=OnStartup 3=OnLaunch
    // 4=OnUpdate 5=OnExit 6=OnShutdown. Registers its subsystem in Configure (the
    // app owns subsystem registration). 0.5s fixed step for exact accumulator math.
    class LifecycleApp final : public IApplication
    {
    public:
        Array<int> order;
        CountingSys* sys = nullptr;
        int fixed = 0; // OnFixedUpdate count (the app-level fixed hook - see CountingSys note)
        bool sysLiveAtStartup = false;

        ApplicationSettings Settings() const override
        {
            ApplicationSettings s;
            s.fixedTimeStep = 0.5f;
            return s;
        }
        void Configure(IApplicationHost& host) override
        {
            order.PushBack(1);
            sys = host.Ctx().AddSubsystem<CountingSys>();
        }
        void OnStartup(IApplicationHost&) override
        {
            order.PushBack(2);
            sysLiveAtStartup = sys->IsInitialized();
        }
        void OnLaunch(IApplicationHost&) override { order.PushBack(3); }
        void OnFixedUpdate(IApplicationHost&, f32) override { ++fixed; }
        void OnUpdate(IApplicationHost&, f32) override { order.PushBack(4); }
        void OnExit(IApplicationHost&) override { order.PushBack(5); }
        void OnShutdown(IApplicationHost&) override { order.PushBack(6); }
    };
}

TEST_CASE("client: Start configures the app, starts subsystems, then launches")
{
    LifecycleApp app;
    ApplicationHost host;
    host.Start(app);

    REQUIRE(app.order.Size() == 3u);
    CHECK(app.order[0] == 1);    // Configure (app registers subsystems)
    CHECK(app.order[1] == 2);    // OnStartup
    CHECK(app.order[2] == 3);    // OnLaunch
    CHECK(app.sysLiveAtStartup); // Configure ran before Context.Startup, so Init happened
    CHECK(host.IsRunning());

    host.Stop();
    CHECK_FALSE(host.IsRunning());
    CHECK(app.sys->shutdowns == 1);
    CHECK(app.order[app.order.Size() - 1] == 6); // OnExit then OnShutdown last
}

TEST_CASE("client: Tick drives Context phases with a fixed-step accumulator")
{
    LifecycleApp app;
    ApplicationHost host;
    host.Start(app);

    host.Tick(0.25f); // accumulator 0.25 < 0.5 -> no fixed step
    CHECK(app.sys->begin == 1);
    CHECK(app.sys->update == 1);
    CHECK(app.sys->post == 1);
    CHECK(app.sys->end == 1);
    CHECK(app.fixed == 0);

    host.Tick(0.25f); // reaches 0.5 -> exactly one fixed step
    CHECK(app.fixed == 1);
    CHECK(app.sys->update == 2);

    host.Tick(0.5f); // another full step
    CHECK(app.fixed == 2);

    // OnUpdate fired once per Tick, after Context::Update each time.
    int updates = 0;
    for (usize i = 0; i < app.order.Size(); ++i)
    {
        if (app.order[i] == 4)
        {
            ++updates;
        }
    }
    CHECK(updates == 3);

    host.Stop();
}

namespace
{
    class ClampApp final : public IApplication
    {
    public:
        CountingSys* sys = nullptr;
        int fixed = 0;
        ApplicationSettings Settings() const override
        {
            ApplicationSettings s;
            s.fixedTimeStep = 0.1f;
            s.maxFrameTime = 0.25f;
            return s;
        }
        void Configure(IApplicationHost& host) override
        {
            sys = host.Ctx().AddSubsystem<CountingSys>();
        }
        void OnFixedUpdate(IApplicationHost&, f32) override { ++fixed; }
    };
}

TEST_CASE("client: maxFrameTime clamps a large delta")
{
    ClampApp app;
    ApplicationHost host;
    host.Start(app);

    // The runner clamps to Settings().maxFrameTime before calling Tick.
    f32 dt = 10.0f;
    if (dt > host.Settings().maxFrameTime)
    {
        dt = host.Settings().maxFrameTime;
    }
    host.Tick(dt); // 0.25 / 0.1 -> 2 fixed steps, not 100
    CHECK(app.fixed == 2);

    host.Stop();
}

TEST_CASE("client: RequestExit stops a manual run loop")
{
    LifecycleApp app;
    ApplicationHost host;
    host.Start(app);

    int frames = 0;
    while (host.IsRunning())
    {
        host.Tick(0.5f);
        if (++frames == 3)
        {
            host.RequestExit(7);
        }
    }
    host.Stop();

    CHECK(frames == 3);
    CHECK(host.ExitCode() == 7);
    CHECK(app.sys->update == 3);
}

namespace
{
    // Records the borrowed shell it sees during Configure.
    class PlatformApp final : public IApplication
    {
    public:
        IShell* seenShell = nullptr;
        void Configure(IApplicationHost& host) override { seenShell = host.Shell(); }
    };
}

TEST_CASE("client: the host borrows the shell and exposes it to the app")
{
    MockShell shell;
    PlatformApp app;
    ApplicationHost host;

    host.Start(app, &shell);
    CHECK(app.seenShell == &shell); // visible during Configure
    CHECK(host.Shell() == &shell);
    host.Stop();
}

namespace
{
    // Opens a second window at startup and counts per-window render calls.
    class RenderApp final : public IApplication
    {
    public:
        RenderWindow* second = nullptr;
        int renders = 0;
        void OnStartup(IApplicationHost& host) override
        {
            second = host.OpenWindow(WindowSettings{}, RenderWindowDesc{});
        }
        void OnRenderWindow(IApplicationHost&, FrameContext&) override { ++renders; }
    };
}

TEST_CASE("client: with a graphics device, every window renders each Tick")
{
    NullShell shell;
    auto created = CreateNullGraphicsDevice(2);
    REQUIRE(created.HasValue());
    UniquePtr<GraphicsDevice>& gd = created.Value();

    RenderApp app;
    ApplicationHost host;
    host.Start(app, &shell, gd.Get());

    CHECK(host.Windows().Size() == 2u); // main (from Start) + the one opened in OnStartup
    REQUIRE(app.second != nullptr);

    host.Tick(0.016f);
    CHECK(app.renders == 2); // one per window

    // Close is deferred to frame end: this Tick still renders BOTH (2 -> 4),
    // then flushes the window away.
    host.CloseWindow(app.second);
    host.Tick(0.016f);
    CHECK(host.Windows().Size() == 1u);
    CHECK(app.renders == 4);

    host.Tick(0.016f); // only the survivor renders now
    CHECK(app.renders == 5);

    host.Stop();
    CHECK(host.Windows().Size() == 0u);
}
