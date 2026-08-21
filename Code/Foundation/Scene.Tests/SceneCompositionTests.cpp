// foundation.scene :composition - declarative assembly (SceneModule/SceneComposition), ordered
// observation (ISceneObserver/SceneLifecycleStage), the FrameTime time-scale chain, and the pure
// SceneRegistry (scene-composition.md). No Context needed anywhere here - the registry is testable
// standalone, which is the point of the design.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;

using namespace foundation::core;
using namespace foundation::scene;

namespace
{
    // Install/reflection hooks record shared state (function pointers carry no capture, so a file-
    // local counter + order string are the canonical test vehicles).
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
        int composing = 0, ready = 0, started = 0, stopped = 0, destroying = 0;
        i32 order = 0;
        utf8char tag = u8'?';
        RecordingObserver(utf8char t, i32 o = 0) : order(o), tag(t) {}

        i32 Order() const noexcept override { return order; }

        void OnComposing(Scene&) override { ++composing; }
        void OnSystemsReady(Scene&) override { ++ready; }
        void OnStarted(Scene&) override { ++started; }
        void OnStopped(Scene&) override { ++stopped; }
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

    // Input order is deliberately reversed: B depends on A, so A must come first.
    const SceneModule* modules[] = {&kModuleBDependsOnA, &kModuleA};
    SceneComposition comp = SceneComposition::Build(modules);
    REQUIRE(comp.ModuleCount() == 2u);

    Scene scene(u8"probe");
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
    Scene scene(u8"probe");
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
    Scene scene(u8"probe");
    comp.Instantiate(scene);
    // Both installs ran (Build broke the cycle and kept every module exactly once).
    CHECK(g_installOrder.Size() == 2u);
}

TEST_CASE("composition: a null install is skipped safely")
{
    const SceneModule noInstall{u8"meta", nullptr, &ReflectA};
    const SceneModule* modules[] = {&noInstall};
    SceneComposition comp = SceneComposition::Build(modules);

    Scene scene(u8"probe");
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

    Scene scene(u8"s");

    g_obsOrder = String{};
    registry.Notify(SceneLifecycleStage::Destroying, scene);
    CHECK(a.destroying == 1);          // notified once (dedup held)
    CHECK(b.destroying == 1);
    CHECK(g_obsOrder == u8"ba"); // lower Order (-10) ran before higher Order (10)

    // Other stages did not fire for Destroying-registered observers.
    registry.Notify(SceneLifecycleStage::Started, scene);
    CHECK(a.started == 0);
    CHECK(b.started == 0);

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
    SceneManager m1;
    SceneManager m2;

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
    SceneManager m1;
    SceneManager m2;
    registry.RegisterManager(&m1);
    registry.RegisterManager(&m2);

    Scene* a = m1.CreateScene(u8"A");
    Scene* b = m2.CreateScene(u8"B");
    a->SetFixedTiming(1.0f / 60.0f, 4);
    b->SetFixedTiming(1.0f / 60.0f, 4);
    DeltaProbeSystem* pa = a->AddSystem<DeltaProbeSystem>();
    DeltaProbeSystem* pb = b->AddSystem<DeltaProbeSystem>();

    registry.BeginFrame(1.0f / 60.0f, 1.0f, 1.0f / 60.0f);
    registry.Update(1.0f / 60.0f);

    CHECK(pa->fixedSteps == 1u);
    CHECK(pb->fixedSteps == 1u);
    CHECK(pa->lastUpdate == doctest::Approx(1.0f / 60.0f));
    CHECK(pb->lastUpdate == doctest::Approx(1.0f / 60.0f));
}