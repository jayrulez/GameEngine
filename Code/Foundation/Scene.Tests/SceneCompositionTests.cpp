// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.scene :composition - declarative assembly (SceneModule/SceneComposition), ordered
// observation (ISceneObserver/SceneLifecycleStage), the FrameTime time-scale chain, and the pure
// SceneRegistry. No SubsystemManager is needed anywhere here: the registry is testable
// standalone.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;

using namespace foundation::core;
using namespace foundation::scene;

namespace
{
    // Install/reflection hooks record shared state (function pointers carry no capture, so a
    // file-local counter and order string carry what they saw).
    String g_installOrder;
    String g_obsOrder;
    int g_reflections = 0;

    void ResetProbes()
    {
        g_installOrder = String{};
        g_obsOrder = String{};
        g_reflections = 0;
    }

    void InstallA(Scene&) { g_installOrder += u8"a"; }
    void InstallB(Scene&) { g_installOrder += u8"b"; }
    void ReflectA() { ++g_reflections; }
    void ReflectB() { ++g_reflections; }

    const SceneModule kModuleA{u8"a", &InstallA, &ReflectA};
    const SceneModule kModuleB{u8"b", &InstallB, &ReflectB};
    const SceneModule* kDepsFromBToA[] = {&kModuleA};
    const SceneModule kModuleBDependsOnA{u8"b", &InstallB, &ReflectB, kDepsFromBToA};

    struct RecordingObserver final : public ISceneObserver
    {
        int composing = 0, ready = 0, destroying = 0;
        i32 order = 0;
        utf8char tag = u8'?';
        RecordingObserver(utf8char t, i32 o = 0) : order(o), tag(t) {}

        i32 Order() const noexcept override { return order; }

        void OnComposing(Scene&) override { ++composing; }
        void OnSystemsReady(Scene&) override { ++ready; }
        void OnDestroying(Scene&) override
        {
            ++destroying;
            g_obsOrder.PushBack(tag);
        }
    };

    // A probe system that records the dt its variable lane saw (for the registry lane-fan-out test).
    struct DeltaProbeSystem : SceneSystem
    {
        f32 lastUpdate = 0.0f;
        u32 fixedSteps = 0;
        void OnUpdate(ScenePhase phase, f32 dt) override
        {
            if (phase == ScenePhase::Update)
            {
                lastUpdate = dt;
            }
        }
        void OnFixedUpdate(f32) override { ++fixedSteps; }
    };
}

TEST_CASE("composition: FrameTime folds the host x context x group x scene chain")
{
    const FrameTime identity(0.016f);
    CHECK(identity.rawDt == doctest::Approx(0.016f));
    CHECK(identity.contextScale == doctest::Approx(1.0f));
    CHECK(identity.groupScale == doctest::Approx(1.0f));
    CHECK(identity.SceneDt() == doctest::Approx(0.016f));

    const FrameTime ft(0.016f, 2.0f, 0.5f, 0.25f);
    CHECK(ft.ContextDt() == doctest::Approx(0.032f)); // host x context
    CHECK(ft.SceneDt() == doctest::Approx(0.004f));   // host x context x group x scene
}

TEST_CASE("composition: Build topologically sorts modules by dependsOn")
{
    ResetProbes();

    // Input order is reversed on purpose: B depends on A, so A must come first.
    const SceneModule* modules[] = {&kModuleBDependsOnA, &kModuleA};
    SceneComposition comp = SceneComposition::Build(modules);
    REQUIRE(comp.ModuleCount() == 2u);

    Scene scene(DefaultAllocator(), u8"probe");
    comp.Instantiate(scene);
    CHECK(g_installOrder == u8"ab");

    comp.RegisterReflection();
    CHECK(g_reflections == 2);
}

TEST_CASE("composition: a dependency absent from the module set is treated as satisfied")
{
    // B declares a dependency on A, but only B is composed: Build still emits B (out-of-scope dep).
    const SceneModule* modules[] = {&kModuleBDependsOnA};
    SceneComposition comp = SceneComposition::Build(modules);
    CHECK(comp.ModuleCount() == 1u);

    ResetProbes();
    Scene scene(DefaultAllocator(), u8"probe");
    comp.Instantiate(scene);
    CHECK(g_installOrder == u8"b");
}

TEST_CASE("composition: a dependency cycle does not hang Build")
{
    // A true 2-cycle: x depends on y, y depends on x. Built with indirection so each module's span can
    // point at the other (which does not exist yet at x's construction).
    const SceneModule* xDeps[1] = {nullptr};
    const SceneModule* yDeps[1] = {nullptr};
    SceneModule x(u8"x", &InstallA, nullptr, Span<const SceneModule*>{xDeps, 1});
    SceneModule y(u8"y", &InstallB, nullptr, Span<const SceneModule*>{yDeps, 1});
    xDeps[0] = &y;
    yDeps[0] = &x;

    const SceneModule* modules[] = {&x, &y};
    SceneComposition comp = SceneComposition::Build(modules);
    CHECK(comp.ModuleCount() == 2u);

    ResetProbes();
    Scene scene(DefaultAllocator(), u8"probe");
    comp.Instantiate(scene);
    // Both installs ran (Build broke the cycle and kept every module exactly once).
    CHECK(g_installOrder.Size() == 2u);
}

TEST_CASE("composition: a null install is skipped safely")
{
    const SceneModule noInstall{u8"meta", nullptr, &ReflectA};
    const SceneModule* modules[] = {&noInstall};
    SceneComposition comp = SceneComposition::Build(modules);

    Scene scene(DefaultAllocator(), u8"probe");
    comp.Instantiate(scene); // must not fault

    ResetProbes();
    comp.RegisterReflection();
    CHECK(g_reflections == 1);
}

TEST_CASE("scene-registry: observers fire at their stage, lower Order() first")
{
    SceneRegistry registry;
    RecordingObserver a(u8'a', 10);
    RecordingObserver b(u8'b', -10);

    registry.AddObserver(&a, SceneLifecycleStage::Destroying);
    registry.AddObserver(&b, SceneLifecycleStage::Destroying);
    registry.AddObserver(&a, SceneLifecycleStage::Destroying); // idempotent

    Scene scene(DefaultAllocator(), u8"s");

    g_obsOrder = String{};
    registry.Notify(SceneLifecycleStage::Destroying, scene);
    CHECK(a.destroying == 1);          // notified once (dedup held)
    CHECK(b.destroying == 1);
    CHECK(g_obsOrder == u8"ba"); // lower Order (-10) ran before higher Order (10)

    // Other stages did not fire for Destroying-registered observers.
    registry.Notify(SceneLifecycleStage::SystemsReady, scene);
    CHECK(a.ready == 0);
    CHECK(b.ready == 0);

    registry.RemoveObserver(&a);
    g_obsOrder = String{};
    registry.Notify(SceneLifecycleStage::Destroying, scene);
    CHECK(a.destroying == 1); // unregistered
    CHECK(b.destroying == 2);
    CHECK(g_obsOrder == u8"b");
}

TEST_CASE("scene-registry: manager registry dedups and sweeps scenes")
{
    SceneRegistry registry;
    SceneManager m1{DefaultAllocator()};
    SceneManager m2{DefaultAllocator()};

    registry.RegisterManager(&m1);
    registry.RegisterManager(&m1); // idempotent
    registry.RegisterManager(&m2);
    CHECK(registry.ManagerCount() == 2u);

    (void)m1.CreateScene(u8"A");
    (void)m1.CreateScene(u8"B");
    (void)m2.CreateScene(u8"C");

    u32 sweeps = 0;
    registry.ForEachScene([&](Scene&) { ++sweeps; });
    CHECK(sweeps == 3u);

    registry.UnregisterManager(&m1);
    CHECK(registry.ManagerCount() == 1u);

    sweeps = 0;
    registry.ForEachScene([&](Scene&) { ++sweeps; });
    CHECK(sweeps == 1u);
}

TEST_CASE("scene-registry: lane fan-out ticks every registered manager's scenes")
{
    SceneRegistry registry;
    SceneManager m1{DefaultAllocator()};
    SceneManager m2{DefaultAllocator()};
    registry.RegisterManager(&m1);
    registry.RegisterManager(&m2);

    Scene* a = m1.CreateScene(u8"A");
    Scene* b = m2.CreateScene(u8"B");
    a->SetFixedTiming(1.0f / 60.0f, 4);
    b->SetFixedTiming(1.0f / 60.0f, 4);
    DeltaProbeSystem* pa = a->AddSystem<DeltaProbeSystem>();
    DeltaProbeSystem* pb = b->AddSystem<DeltaProbeSystem>();

    registry.BeginFrame(FrameTime(1.0f / 60.0f, 1.0f, 1.0f, 1.0f, 1.0f / 60.0f));
    registry.Update(FrameTime(1.0f / 60.0f));

    CHECK(pa->fixedSteps == 1u);
    CHECK(pb->fixedSteps == 1u);
    CHECK(pa->lastUpdate == doctest::Approx(1.0f / 60.0f));
    CHECK(pb->lastUpdate == doctest::Approx(1.0f / 60.0f));
}
TEST_CASE("composition: OWNS its module copies - source modules may die after Build")
{
    // A composition that kept the caller's SceneModule* would dereference a dead stack
    // module at Instantiate time. Build must copy by value so block-scoped or temporary
    // modules are legal callers.
    ResetProbes();
    SceneComposition comp;
    {
        const SceneModule localA{u8"a", &InstallA, &ReflectA};
        const SceneModule* deps[] = {&localA};
        const SceneModule localB{u8"b", &InstallB, &ReflectB, deps};
        const SceneModule* modules[] = {&localB, &localA};
        comp = SceneComposition::Build(modules);
    } // both modules and the deps array are dead here

    CHECK(comp.ModuleCount() == 2u);
    Scene scene(DefaultAllocator(), u8"lifetime");
    comp.Instantiate(scene);            // dereferences the stored copies, not the corpses
    CHECK(g_installOrder == u8"ab");    // dependency order also survived the copy
    comp.RegisterReflection();
    CHECK(g_reflections == 2);
}

TEST_CASE("frame-time cutover: the manager lanes fold the FULL chain from one FrameTime")
{
    // The bridge builds one FrameTime (host x context, and the step); the manager
    // contributes the group term, the scene its own term. Assert the composed
    // dt reaching a scene's variable lane equals raw x context x group x scene, and the
    // fixed lane steps at the configured step under the same chain.
    SceneManager mgr{DefaultAllocator()};
    mgr.SetTimeScale(0.5f); // the group term
    Scene* s = mgr.CreateScene(u8"chain");
    s->Start();
    s->SetSimulationEnabled(true);
    s->SetTimeScale(0.5f); // the scene term
    DeltaProbeSystem* probe = s->AddSystem<DeltaProbeSystem>();

    const FrameTime time(/*raw*/ 0.032f, /*context*/ 0.5f, 1.0f, 1.0f, /*step*/ 1.0f / 250.0f);
    mgr.BeginFrame(time); // seeds the scene stepper to the lane step and advances fixed time
    mgr.Update(time);

    // 0.032 x 0.5 (context) x 0.5 (group) x 0.5 (scene) = 0.004
    CHECK(probe->lastUpdate == doctest::Approx(0.004f));
    // Fixed lane: 0.004s of scene time at a 4ms step = exactly one step.
    CHECK(probe->fixedSteps == 1u);
    CHECK(s->FixedTimeStep() == doctest::Approx(1.0f / 250.0f)); // seeded from the lane config
}

// --- Runtime contributions: a plugin's managers/systems ----------------------------------------
namespace
{
    // A contributed system with a settings block (managers and plain systems alike).
    struct ContribSettings
    {
        i32 value = 7;
    };
    class ContribSystem final : public SceneSystem
    {
    public:
        ContribSettings settings;
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<ContribSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &settings; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"contrib"; }
    };
    void InstallContrib(Scene& scene) { scene.AddSystem<ContribSystem>(); }

    // A test live-scene sink over a plain list (what the SceneSubsystem does over its registry).
    Array<Scene*> g_liveScenes;
    void VisitLive(void*, void (*fn)(void*, Scene&), void* fnContext)
    {
        for (Scene* s : g_liveScenes)
        {
            fn(fnContext, *s);
        }
    }
    SceneModuleContributions::Contribution MakeContrib()
    {
        SceneModuleContributions::Contribution c;
        c.id = u8"contrib";
        c.install = &InstallContrib;
        c.systemType = TypeOf<ContribSystem>().id;
        return c;
    }
}

TEST_CASE("contributions: Instantiate appends contributed systems; Remove withdraws them")
{
    SceneModuleContributions& reg = SceneModuleContributions::Global();
    reg.Remove(TypeOf<ContribSystem>().id); // clean slate (shared process state)

    Array<TypeId> recorded;
    reg.SetRegistrationObserver(
        [](void* ctx, TypeId id) { static_cast<Array<TypeId>*>(ctx)->PushBack(id); }, &recorded);
    reg.Add(MakeContrib());
    reg.Add(MakeContrib()); // idempotent by system type: no second insert, no second record
    reg.SetRegistrationObserver(nullptr, nullptr);
    REQUIRE(recorded.Size() == 1u);
    CHECK(recorded[0] == TypeOf<ContribSystem>().id);
    CHECK(reg.Contains(TypeOf<ContribSystem>().id));

    // A scene composed after the contribution gets the system through Instantiate's tail.
    const SceneModule* none[] = {&kModuleA};
    SceneComposition comp = SceneComposition::Build(Span<const SceneModule*>{none, 1});
    Scene scene(DefaultAllocator(), u8"contrib-new");
    comp.Instantiate(scene);
    CHECK(scene.HasSystem<ContribSystem>());
    CHECK(scene.GetSystem<ContribSystem>()->settings.value == 7);

    reg.Remove(TypeOf<ContribSystem>().id);
    CHECK_FALSE(reg.Contains(TypeOf<ContribSystem>().id));
    Scene later(DefaultAllocator(), u8"contrib-after-remove");
    comp.Instantiate(later);
    CHECK_FALSE(later.HasSystem<ContribSystem>());
}

TEST_CASE("contributions: live scenes gain the system on Add and lose it on Remove")
{
    SceneModuleContributions& reg = SceneModuleContributions::Global();
    reg.Remove(TypeOf<ContribSystem>().id);

    Scene live(DefaultAllocator(), u8"contrib-live");
    g_liveScenes.Clear();
    g_liveScenes.PushBack(&live);
    reg.SetLiveSceneSink(&VisitLive, nullptr);

    reg.Add(MakeContrib());
    CHECK(live.HasSystem<ContribSystem>()); // applied to the already-alive scene

    reg.Remove(TypeOf<ContribSystem>().id);
    CHECK_FALSE(live.HasSystem<ContribSystem>()); // withdrawn (RemoveSystem) while its code lives

    reg.SetLiveSceneSink(nullptr, nullptr);
    g_liveScenes.Clear();
}

TEST_CASE("scene: RemoveSystem destroys the system and every lookup forgets it")
{
    Scene scene(DefaultAllocator(), u8"remove-system");
    ContribSystem* sys = scene.AddSystem<ContribSystem>();
    REQUIRE(sys != nullptr);
    usize count = 0;
    scene.ForEachSystem([&](SceneSystem&) { ++count; });
    CHECK(count == 1u);

    CHECK(scene.RemoveSystem(TypeOf<ContribSystem>().id));
    CHECK_FALSE(scene.HasSystem<ContribSystem>());
    CHECK(scene.GetSystem<ContribSystem>() == nullptr);
    count = 0;
    scene.ForEachSystem([&](SceneSystem&) { ++count; });
    CHECK(count == 0u);
    CHECK_FALSE(scene.RemoveSystem(TypeOf<ContribSystem>().id)); // idempotent
}
