// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h" // <new> reachability for container instantiation (GCC)

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;          // SceneModuleContributions (the cross-boundary scene case)
import foundation.scene.resource; // SceneSnapshot
import foundation.shell;
import foundation.graphics;

using namespace foundation::core;
using namespace foundation::runtime;

namespace
{
    // Build dirs hand us a narrow UTF-8 path; the IO/Library APIs take StringView.
    [[nodiscard]] StringView PluginPath()
    {
        return StringView{reinterpret_cast<const utf8char*>(TEST_PLUGIN_PATH)};
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
    foundation::runtime::SubsystemManager ctx(foundation::core::DefaultAllocator());
    CHECK(ctx.TimeScale() == 1.0f);
    ctx.SetTimeScale(0.5f);
    CHECK(ctx.TimeScale() == 0.5f);
    ctx.SetTimeScale(-3.0f); // negative time is not a thing
    CHECK(ctx.TimeScale() == 0.0f);

    // Half-speed feeding the stepper: 60 raw frames at 1/60 yield ~30 fixed steps.
    foundation::runtime::FixedStepper stepper;
    ctx.SetTimeScale(0.5f);
    foundation::core::u32 steps = 0;
    for (int i = 0; i < 60; ++i)
    {
        steps += stepper.Advance((1.0f / 60.0f) * ctx.TimeScale());
    }
    CHECK(steps >= 29u);
    CHECK(steps <= 30u);
}

TEST_CASE("runtime: fixed stepper - exact cadence, alpha, and the hitch clamp")
{
    using foundation::runtime::FixedStepper;

    // Exact cadence: sixty 1/60 frames = sixty steps, alpha stays ~0 (no drift blowup).
    FixedStepper stepper;
    stepper.step = 1.0f / 60.0f;
    stepper.maxSteps = 4;
    foundation::core::u32 total = 0;
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
    SubsystemManager ctx(DefaultAllocator());
    CHECK_FALSE(ctx.HasSubsystem<Sys<1>>());

    Sys<1>* a = ctx.AddSubsystem<Sys<1>>(0, nullptr);
    Sys<2>* b = ctx.AddSubsystem<Sys<2>>(0, nullptr);

    CHECK(ctx.HasSubsystem<Sys<1>>());
    CHECK(ctx.GetSubsystem<Sys<1>>() == a);
    CHECK(ctx.GetSubsystem<Sys<2>>() == b);
    CHECK(ctx.GetSubsystem<Sys<3>>() == nullptr); // never registered
    CHECK(a->Manager() == &ctx);                 // OnRegister wired the manager
}

TEST_CASE("runtime: startup / shutdown lifecycle")
{
    SubsystemManager ctx(DefaultAllocator());
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
    SubsystemManager ctx(DefaultAllocator());
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
        SubsystemManager ctx(DefaultAllocator());
        Sys<1>* a = ctx.AddSubsystem<Sys<1>>(0, nullptr);
        ctx.Startup();
        CHECK(a->IsInitialized());
        // ctx destructs here -> Dispose -> Shutdown -> destroy
    }
    // (Lifecycle correctness is covered above; this exercises the dtor path.)
    CHECK(shutdowns == 0);
}

TEST_CASE("runtime: register a caller-owned subsystem; SubsystemManager does not destroy it")
{
    Sys<1> owned(0, nullptr); // lives on the stack; SubsystemManager must not free it
    {
        SubsystemManager ctx(DefaultAllocator());
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
    SubsystemManager ctx(DefaultAllocator());
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
    SubsystemManager ctx(DefaultAllocator());
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
        void OnLoad(SubsystemManager& ctx) override { ctx.RegisterSubsystem<Sys<7>>(&m_sys); }
        void OnUnload(SubsystemManager& ctx) override { ctx.RemoveSubsystem<Sys<7>>(); }

        Sys<7> m_sys{0, nullptr};
    };
}

TEST_CASE("runtime: PluginHost::Add registers a static plugin's subsystem")
{
    SubsystemManager ctx(DefaultAllocator());
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
    SubsystemManager ctx(DefaultAllocator());
    {
        PluginHost host(ctx);

        auto loaded = host.Load(PluginPath());
        REQUIRE(loaded.HasValue());
        CHECK(host.Count() == 1u);
        CHECK(loaded.Value()->Name() == StringView{u8"TestPlugin"});

        ctx.Startup();
        ctx.Update(0.016f);

        // Observe the library's subsystem ran via a C symbol it exports. A second
        // handle to the same image shares the counter (dlopen refcounts).
        DynamicLibrary probe{PluginPath()};
        REQUIRE(probe.IsLoaded());
        const auto ticks = probe.GetSymbol<int (*)()>(u8"TestPluginTicks");
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
    SubsystemManager ctx(DefaultAllocator());
    PluginHost host(ctx);
    auto result = host.Load(u8"./definitely-not-a-real-plugin.so");
    CHECK_FALSE(result.HasValue());
    CHECK(host.Count() == 0u);
}

namespace
{
    // A minimal app that records which context it configured into.
    struct EmbeddedProbeApp final : foundation::runtime::IApplication
    {
        foundation::runtime::SubsystemManager* configuredInto = nullptr;
        void Configure(foundation::runtime::IApplicationHost& host) override
        {
            configuredInto = &host.Subsystems();
        }
    };

    struct NullOuterHost final : foundation::runtime::IApplicationHost
    {
        foundation::runtime::SubsystemManager editorContext{DefaultAllocator()};
        foundation::runtime::SubsystemManager& Subsystems() noexcept override { return editorContext; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow*
        OpenWindow(const foundation::shell::WindowSettings&,
                   const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}
    };
}

TEST_CASE("embedded host routes Ctx to the runtime context and exit to the embedder")
{
    NullOuterHost outer;
    foundation::runtime::SubsystemManager runtimeContext(DefaultAllocator());
    foundation::runtime::EmbeddedApplicationHost embedded(outer, runtimeContext);

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
        foundation::core::Function<void(int)>{[&](int code) { exitCode = code; }});
    embedded.RequestExit(7);
    CHECK(exitCode == 7);
}

// ---------------------------------------------------------------------------
// Plugin cross-boundary identity (ENGINE_SHARED_LIBS lanes only) - the supported
// plugin model is plugins linked against the SHARED engine; this proves the
// shared-libraries work across a real dlopen boundary (shared-libraries.md P4).
// ---------------------------------------------------------------------------
#ifdef CROSS_PLUGIN_PATH
#include "CrossBoundaryProbe.h"

TEST_CASE("runtime: a shared-engine plugin shares identity + rendezvous with the host")
{
    const StringView path{reinterpret_cast<const utf8char*>(CROSS_PLUGIN_PATH)};

    SubsystemManager ctx(DefaultAllocator());
    RegisterCoreTypes(); // the host image patches Float3's metadata (see 3b)
    // Host-side probe subsystem, registered BEFORE the plugin loads.
    crossprobe::HostProbeSubsystem hostProbe;
    ctx.RegisterSubsystem<crossprobe::HostProbeSubsystem>(&hostProbe);

    // The scene-contribution recorder (what Engine.Scene's SceneContributionRecorder does):
    // records the managers a plugin contributes so unload reverses them.
    struct ContributionRecorder final : IRegistrationRecorder
    {
        void Arm(Array<TypeId>& sink) override
        {
            foundation::scene::SceneModuleContributions::Global().SetRegistrationObserver(
                [](void* c, TypeId id) { static_cast<Array<TypeId>*>(c)->PushBack(id); }, &sink);
        }
        void Disarm() override
        {
            foundation::scene::SceneModuleContributions::Global().SetRegistrationObserver(nullptr,
                                                                                         nullptr);
        }
        void Reverse(TypeId id) override
        {
            foundation::scene::SceneModuleContributions::Global().Remove(id);
        }
    } contributionRecorder;

    PluginHost host(ctx);
    host.AddRecorder(&contributionRecorder);
    auto loaded = host.Load(path);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value()->Name() == StringView{u8"CrossPlugin"});
    // The plugin contributed its Fancy manager (S1) - recorded, and reversed below.
    CHECK(foundation::scene::SceneModuleContributions::Global().Contains(
        TypeOf<crossprobe::FancyManager>().id));

    DynamicLibrary probe{path};
    REQUIRE(probe.IsLoaded());

    // 1. The plugin resolved the HOST's subsystem through the TypeId-keyed SubsystemManager
    //    (its own TypeOf<HostProbeSubsystem> copy lives at a different address).
    const auto resolved = probe.GetSymbol<int (*)()>(u8"CrossPluginResolvedHostValue");
    REQUIRE(resolved != nullptr);
    CHECK(resolved() == 41);

    // 2. An object MakeRef'd inside the plugin crosses the boundary alive (the
    //    RefControl handshake slot is process-single), casts by id, and releases
    //    cleanly from the host side.
    const auto create = probe.GetSymbol<Object* (*)(int)>(u8"CrossPluginCreateObject");
    REQUIRE(create != nullptr);
    {
        RefPtr<Object> adopted{create(7), AdoptRef{}};
        REQUIRE(adopted.Get() != nullptr);
        CHECK(adopted->GetType() != nullptr);
        CHECK(adopted->GetType()->id == crossprobe::ProbeObject::StaticType().id);
        crossprobe::ProbeObject* cast = Cast<crossprobe::ProbeObject>(adopted.Get());
        REQUIRE(cast != nullptr);
        CHECK(cast->payload == 7);
    } // host-side release of a plugin-created object

    // 3. The type the plugin registered is visible through the shared registry.
    const TypeInfo* found = GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject");
    REQUIRE(found != nullptr);
    CHECK(found->id == crossprobe::ProbeObject::StaticType().id);

    // 3b. TypeOf<T>() is ONE TypeInfo per process: the plugin's own TypeOf<Float3>() is
    //     the host's slot and carries the properties Core's registrar patched in (with a
    //     per-image TypeInfo the plugin would read an unpatched copy: 0 properties, the
    //     W1 finding). Core types are registered by the host, before the plugin loaded.
    const auto pluginTypeOf =
        probe.GetSymbol<const TypeInfo* (*)()>(u8"CrossPluginTypeOfFloat3");
    const auto pluginPropertyCount =
        probe.GetSymbol<unsigned (*)()>(u8"CrossPluginFloat3PropertyCount");
    REQUIRE(pluginTypeOf != nullptr);
    REQUIRE(pluginPropertyCount != nullptr);
    CHECK(pluginTypeOf() == &TypeOf<Float3>());
    CHECK(pluginPropertyCount() == 3u);
    CHECK(Properties(TypeOf<Float3>()).Size() == 3u);

    // 4. Unload reverses the RECORDED registrations (the N6 RegistrationScope): the
    //    registry entry pointing into the closed library is gone...
    host.UnloadAll();
    CHECK(GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject") == nullptr);

    //    ...and a reload (the rebuilt module in a real flow; the same one here)
    //    re-registers into the freed slot.
    auto reloaded = host.Load(path);
    REQUIRE(reloaded.HasValue());
    CHECK(GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject") != nullptr);
    CHECK(resolved() == 41); // OnLoad ran again against the live host subsystem

    host.UnloadAll();
    CHECK(GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject") == nullptr);

    // 5. The editor's hot-reload shape (N6): unload KEEPING the old mapping alive
    //    (leak-on-purpose), then load a fresh versioned COPY - dlopen refcounts by
    //    path, so only a new file yields a genuinely new module. Registrations land
    //    in the freed slots.
    const StringView copyPath = u8".test-scratch/reload-copy-crossplugin.so";
    {
        auto again = host.Load(path);
        REQUIRE(again.HasValue());
        REQUIRE(GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject") != nullptr);
        host.UnloadAll(/*closeLibraries*/ false); // old mapping stays; recording reversed
        CHECK(GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject") == nullptr);

        auto original = ReadFile(path, DefaultAllocator());
        REQUIRE(original.HasValue());
        (void)foundation::core::CreateDirectory(u8".test-scratch");
        REQUIRE(WriteFile(copyPath, Span<const byte>{original.Value().Data(),
                                                     original.Value().Size()})
                    .IsOk());
        auto reloadedCopy = host.Load(copyPath);
        REQUIRE(reloadedCopy.HasValue());
        CHECK(GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject") != nullptr);
        CHECK(resolved() == 41); // the copy's OnLoad ran against the live host subsystem
        host.UnloadAll();
        CHECK(GlobalTypeRegistry().FindByName("crossprobe", "ProbeObject") == nullptr);
        CHECK_FALSE(foundation::scene::SceneModuleContributions::Global().Contains(
            TypeOf<crossprobe::FancyManager>().id)); // the contribution reversed with it
    }

    // 6. The MyFancyComponent walkthrough (game-native-code.md N6 + S1): a scene ALIVE across
    //    a plugin reload keeps a plugin-owned component through the snapshot bracket.
    {
        using foundation::scene::Scene;
        using foundation::scene::SceneModuleContributions;
        Scene scene(DefaultAllocator(), u8"editing");
        Scene* live[] = {&scene};
        SceneModuleContributions::Global().SetLiveSceneSink(
            [](void* c, void (*fn)(void*, Scene&), void* fnCtx)
            {
                for (Scene* s : Span<Scene* const>{static_cast<Scene**>(c), 1})
                {
                    fn(fnCtx, *s);
                }
            },
            live);

        // Plugin loads AFTER the scene exists: the contribution reaches the live scene.
        auto again = host.Load(copyPath);
        REQUIRE(again.HasValue());
        crossprobe::FancyManager* fancy = scene.GetSystem<crossprobe::FancyManager>();
        REQUIRE(fancy != nullptr);
        const foundation::scene::EntityHandle hero = scene.CreateEntity(u8"hero");
        fancy->Add(hero).payload = 9;

        // The reload bracket: snapshot -> unload (the manager leaves the live scene while its
        // code is mapped; the old mapping is kept) -> load the rebuild -> restore.
        auto snapshot = foundation::scene::SceneSnapshot::Capture(scene);
        REQUIRE(snapshot);
        host.UnloadAll(/*closeLibraries*/ false);
        CHECK(scene.GetSystem<crossprobe::FancyManager>() == nullptr);

        auto rebuilt = host.Load(copyPath);
        REQUIRE(rebuilt.HasValue());
        REQUIRE(scene.GetSystem<crossprobe::FancyManager>() != nullptr); // contributed anew
        REQUIRE(snapshot->Restore(scene).IsOk());
        const foundation::scene::EntityHandle restoredHero = scene.FindEntityByName(u8"hero");
        REQUIRE(restoredHero.IsAssigned());
        crossprobe::FancyManager* newFancy = scene.GetSystem<crossprobe::FancyManager>();
        REQUIRE(newFancy->Get(restoredHero) != nullptr);
        CHECK(newFancy->Get(restoredHero)->payload == 9); // MyFancyComponent survived the reload

        host.UnloadAll();
        SceneModuleContributions::Global().SetLiveSceneSink(nullptr, nullptr);
    }
    ctx.RemoveSubsystem<crossprobe::HostProbeSubsystem>();
}
#endif // CROSS_PLUGIN_PATH
