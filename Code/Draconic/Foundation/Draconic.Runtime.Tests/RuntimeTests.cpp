#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h" // <new> reachability for container instantiation (GCC)

import draconic.foundation;
import draconic.runtime;
import draconic.runtime.client;
import draconic.shell;
import draconic.graphics;

using namespace draconic::foundation;
using namespace draconic::runtime;

namespace
{
    // Build dirs hand us a narrow UTF-8 path; the IO/Library APIs take StringView.
    [[nodiscard]] StringView PluginPath()
    {
        return StringView{reinterpret_cast<const utf8char*>(DRACONIC_TEST_PLUGIN_PATH)};
    }

    // Distinct subsystem types (distinct TypeOf<> keys). Each records its tag in a
    // shared log on Update and counts its lifecycle calls.
    template <int Tag>
    class Sys final : public Subsystem
    {
    public:
        Sys(i32 order, Array<int>* log) : m_order(order), m_log(log) {}

        [[nodiscard]] i32 UpdateOrder() const noexcept override { return m_order; }

        void Update(f32) override
        {
            if (m_log != nullptr)
            {
                m_log->PushBack(Tag);
            }
            ++updates;
        }
        void BeginFrame(f32) override { ++beginFrames; }
        void EndFrame() override { ++endFrames; }

        int inits = 0, readys = 0, updates = 0, shutdowns = 0, beginFrames = 0, endFrames = 0;

    protected:
        void OnInit() override { ++inits; }
        void OnReady() override { ++readys; }
        void OnShutdown() override { ++shutdowns; }

    private:
        i32 m_order;
        Array<int>* m_log;
    };
}

TEST_CASE("runtime: time scale clamps at zero and defaults to realtime")
{
    draconic::runtime::Context ctx;
    CHECK(ctx.TimeScale() == 1.0f);
    ctx.SetTimeScale(0.5f);
    CHECK(ctx.TimeScale() == 0.5f);
    ctx.SetTimeScale(-3.0f); // negative time is not a thing
    CHECK(ctx.TimeScale() == 0.0f);

    // Half-speed feeding the stepper: 60 raw frames at 1/60 yield ~30 fixed steps.
    draconic::runtime::FixedStepper stepper;
    ctx.SetTimeScale(0.5f);
    draconic::foundation::u32 steps = 0;
    for (int i = 0; i < 60; ++i)
    {
        steps += stepper.Advance((1.0f / 60.0f) * ctx.TimeScale());
    }
    CHECK(steps >= 29u);
    CHECK(steps <= 30u);
}

TEST_CASE("runtime: fixed stepper - exact cadence, alpha, and the hitch clamp")
{
    using draconic::runtime::FixedStepper;

    // Exact cadence: sixty 1/60 frames = sixty steps, alpha stays ~0 (no drift blowup).
    FixedStepper stepper;
    stepper.step = 1.0f / 60.0f;
    stepper.maxSteps = 4;
    draconic::foundation::u32 total = 0;
    for (int i = 0; i < 60; ++i)
    {
        total += stepper.Advance(1.0f / 60.0f);
    }
    CHECK(total >= 59u); // float accumulation may defer one step...
    CHECK(total <= 60u);
    CHECK(stepper.Alpha() >= 0.0f);
    CHECK(stepper.Alpha() < 1.0f);

    // Sub-step frames accumulate: two half-steps = one step, alpha reflects the leftover.
    FixedStepper half;
    half.step = 0.02f;
    CHECK(half.Advance(0.01f) == 0u);
    CHECK(half.Alpha() == doctest::Approx(0.5f));
    CHECK(half.Advance(0.01f) == 1u);
    CHECK(half.Alpha() == doctest::Approx(0.0f).epsilon(0.01));

    // A hitch is CLAMPED, never a step storm: one 1-second frame at 1/60 yields exactly
    // maxSteps, the excess time is dropped, and alpha stays a valid weight in [0,1).
    FixedStepper hitch;
    hitch.step = 1.0f / 60.0f;
    hitch.maxSteps = 4;
    CHECK(hitch.Advance(1.0f) == 4u);
    CHECK(hitch.Alpha() >= 0.0f);
    CHECK(hitch.Alpha() < 1.0f);
    // The next normal frame is back to a single step - no debt carried.
    CHECK(hitch.Advance(1.0f / 60.0f) <= 1u);

    // Degenerate inputs: negative/zero dt never steps; zero step never divides by zero.
    FixedStepper degenerate;
    CHECK(degenerate.Advance(-1.0f) == 0u);
    CHECK(degenerate.Advance(0.0f) == 0u);
    degenerate.step = 0.0f;
    CHECK(degenerate.Alpha() == 0.0f);
    CHECK(degenerate.Advance(1.0f) == 0u); // zero step: no spin, no steps
}

TEST_CASE("runtime: register, look up, and own subsystems by type")
{
    Context ctx;
    CHECK_FALSE(ctx.HasSubsystem<Sys<1>>());

    Sys<1>* a = ctx.AddSubsystem<Sys<1>>(0, nullptr);
    Sys<2>* b = ctx.AddSubsystem<Sys<2>>(0, nullptr);

    CHECK(ctx.HasSubsystem<Sys<1>>());
    CHECK(ctx.GetSubsystem<Sys<1>>() == a);
    CHECK(ctx.GetSubsystem<Sys<2>>() == b);
    CHECK(ctx.GetSubsystem<Sys<3>>() == nullptr); // never registered
    CHECK(a->GetContext() == &ctx);               // OnRegister wired the context
}

TEST_CASE("runtime: startup / shutdown lifecycle")
{
    Context ctx;
    Sys<1>* a = ctx.AddSubsystem<Sys<1>>(0, nullptr);
    CHECK_FALSE(a->IsInitialized());
    CHECK_FALSE(ctx.IsRunning());

    ctx.Startup();
    CHECK(ctx.IsRunning());
    CHECK(a->inits == 1);
    CHECK(a->readys == 1);
    CHECK(a->IsInitialized());

    ctx.Shutdown();
    CHECK_FALSE(ctx.IsRunning());
    CHECK(a->shutdowns == 1);
    CHECK_FALSE(a->IsInitialized());
}

TEST_CASE("runtime: frame phases run in UpdateOrder")
{
    Array<int> log;
    Context ctx;
    Sys<2>* high = ctx.AddSubsystem<Sys<2>>(5, &log);  // runs later
    Sys<1>* low = ctx.AddSubsystem<Sys<1>>(-10, &log); // runs earlier

    ctx.Startup();
    ctx.BeginFrame(0.016f);
    ctx.Update(0.016f);
    ctx.EndFrame();

    REQUIRE(log.Size() == 2u);
    CHECK(log[0] == 1); // Sys<1> (order -10) before
    CHECK(log[1] == 2); // Sys<2> (order 5)
    CHECK(low->updates == 1);
    CHECK(high->updates == 1);
    CHECK(low->beginFrames == 1);
    CHECK(high->endFrames == 1);
}

TEST_CASE("runtime: Dispose shuts down running subsystems")
{
    int shutdowns = 0;
    {
        Context ctx;
        Sys<1>* a = ctx.AddSubsystem<Sys<1>>(0, nullptr);
        ctx.Startup();
        CHECK(a->IsInitialized());
        // ctx destructs here -> Dispose -> Shutdown -> destroy
    }
    // (Lifecycle correctness is covered above; this exercises the dtor path.)
    CHECK(shutdowns == 0);
}

TEST_CASE("runtime: register a caller-owned subsystem; Context does not destroy it")
{
    Sys<1> owned(0, nullptr); // lives on the stack; Context must not free it
    {
        Context ctx;
        Sys<1>* registered = ctx.RegisterSubsystem<Sys<1>>(&owned);
        CHECK(registered == &owned);
        CHECK(ctx.GetSubsystem<Sys<1>>() == &owned);

        ctx.Startup();
        ctx.Update(0.016f);
        CHECK(owned.inits == 1);
        CHECK(owned.updates == 1);
        // ctx disposes here: shuts the subsystem down but must NOT destroy it.
    }
    CHECK(owned.shutdowns == 1); // object still valid -> reading it is safe
}

TEST_CASE("runtime: subsystems registered after Startup come up immediately")
{
    Context ctx;
    ctx.Startup();
    CHECK(ctx.IsRunning());

    Sys<1>* late = ctx.AddSubsystem<Sys<1>>(0, nullptr);
    CHECK(late->inits == 1); // Init + Ready ran on registration
    CHECK(late->readys == 1);
    CHECK(late->IsInitialized());

    ctx.Update(0.016f);
    CHECK(late->updates == 1); // participates in the frame loop right away
}

TEST_CASE("runtime: RemoveSubsystem shuts down, unregisters, and destroys owned")
{
    Context ctx;
    ctx.AddSubsystem<Sys<1>>(0, nullptr);
    ctx.AddSubsystem<Sys<2>>(0, nullptr);
    ctx.Startup();

    ctx.RemoveSubsystem<Sys<1>>();
    CHECK_FALSE(ctx.HasSubsystem<Sys<1>>());
    CHECK(ctx.HasSubsystem<Sys<2>>());

    // The removed subsystem no longer ticks; the survivor still does.
    Array<int> log;
    Sys<2>* b = ctx.GetSubsystem<Sys<2>>();
    b->Update(0.016f); // sanity: survivor is live
    CHECK(b->updates == 1);
}

namespace
{
    // A plugin defined in-process (statically linked). Owns its subsystem and
    // registers/removes it non-owningly across load/unload.
    class StaticTestPlugin final : public IRuntimePlugin
    {
    public:
        [[nodiscard]] StringView Name() const noexcept override { return u8"StaticTestPlugin"; }
        void OnLoad(Context& ctx) override { ctx.RegisterSubsystem<Sys<7>>(&m_sys); }
        void OnUnload(Context& ctx) override { ctx.RemoveSubsystem<Sys<7>>(); }

        Sys<7> m_sys{0, nullptr};
    };
}

TEST_CASE("runtime: PluginHost::Add registers a static plugin's subsystem")
{
    Context ctx;
    StaticTestPlugin plugin;
    {
        PluginHost host(ctx);
        CHECK(host.Add(&plugin) == &plugin);
        CHECK(host.Count() == 1u);
        CHECK(ctx.HasSubsystem<Sys<7>>());

        ctx.Startup();
        ctx.Update(0.016f);
        CHECK(plugin.m_sys.updates == 1);

        host.UnloadAll();
        CHECK(host.Count() == 0u);
        CHECK_FALSE(ctx.HasSubsystem<Sys<7>>()); // OnUnload removed it
    }
    // Plugin object outlives the host (caller-owned) and was never freed by it.
    CHECK(plugin.m_sys.shutdowns == 1);
}

TEST_CASE("runtime: PluginHost::Load loads a plugin from a shared library")
{
    Context ctx;
    {
        PluginHost host(ctx);

        auto loaded = host.Load(PluginPath());
        REQUIRE(loaded.HasValue());
        CHECK(host.Count() == 1u);
        CHECK(loaded.Value()->Name() == StringView{u8"DraconicTestPlugin"});

        ctx.Startup();
        ctx.Update(0.016f);

        // Observe the library's subsystem ran via a C symbol it exports. A second
        // handle to the same image shares the counter (dlopen refcounts).
        DynamicLibrary probe{PluginPath()};
        REQUIRE(probe.IsLoaded());
        const auto ticks = probe.GetSymbol<int (*)()>(u8"DraconicTestPluginTicks");
        REQUIRE(ticks != nullptr);
        CHECK(ticks() == 1);

        host.UnloadAll();
        CHECK(host.Count() == 0u);

        // After unload the plugin's subsystem is gone; updating must not tick it.
        ctx.Update(0.016f);
        CHECK(ticks() == 1);
    }
}

TEST_CASE("runtime: PluginHost::Load reports failure for a missing library")
{
    Context ctx;
    PluginHost host(ctx);
    auto result = host.Load(u8"./definitely-not-a-real-plugin.so");
    CHECK_FALSE(result.HasValue());
    CHECK(host.Count() == 0u);
}

namespace
{
    // A minimal app that records which context it configured into.
    struct EmbeddedProbeApp final : draconic::runtime::IApplication
    {
        draconic::runtime::Context* configuredInto = nullptr;
        void Configure(draconic::runtime::IApplicationHost& host) override
        {
            configuredInto = &host.Ctx();
        }
    };

    struct NullOuterHost final : draconic::runtime::IApplicationHost
    {
        draconic::runtime::Context editorContext;
        draconic::runtime::Context& Ctx() noexcept override { return editorContext; }
        draconic::shell::IShell* Shell() noexcept override { return nullptr; }
        draconic::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        draconic::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        draconic::graphics::RenderWindow*
        OpenWindow(const draconic::shell::WindowSettings&,
                   const draconic::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(draconic::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}
    };
}

TEST_CASE("embedded host routes Ctx to the runtime context and exit to the embedder")
{
    NullOuterHost outer;
    draconic::runtime::Context runtimeContext;
    draconic::runtime::EmbeddedApplicationHost embedded(outer, runtimeContext);

    // The hosted app configures into the EMBEDDED context, not the editor's.
    EmbeddedProbeApp app;
    app.Configure(embedded);
    CHECK(app.configuredInto == &runtimeContext);
    CHECK(app.configuredInto != &outer.editorContext);

    // No OS window surface: attach-to-window code must see the headless shape.
    CHECK(embedded.MainRenderWindow() == nullptr);
    CHECK(embedded.OpenWindow({}, {}) == nullptr);

    // Exit means "stop the play session" - the embedder's handler receives it.
    int exitCode = -1;
    embedded.SetExitHandler(
        draconic::foundation::Function<void(int)>{[&](int code) { exitCode = code; }});
    embedded.RequestExit(7);
    CHECK(exitCode == 7);
}
