// Phase 4 - the Context-level scene driver: an owner registers its own SceneManager, the subsystem
// assembles every scene from a SceneComposition, fans the SystemsReady/Destroying observer stages, and
// ticks each group on the Context lane.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.scene;
import engine.scene;

using namespace foundation::core;
using namespace engine::scene;
using namespace foundation::scene;
namespace runtime = foundation::runtime;

namespace
{
    // A per-scene system the "render" module installs into every scene (the composition's install).
    struct RenderSceneSystem : SceneSystem
    {
        int ticks = 0;
        void OnUpdate(ScenePhase p, f32) override
        {
            if (p == ScenePhase::PostTransform)
            {
                ++ticks;
            }
        }
    };

    void InstallRenderManagers(Scene& scene) { scene.AddSystem<RenderSceneSystem>(); }

    const SceneModule kRenderModule{u8"render", &InstallRenderManagers, nullptr};

    // A Context-level subsystem that reacts to scene lifecycle (the ISceneObserver role): it observes
    // SystemsReady + Destroying, but leaves ASSEMBLY to the composition. This mirrors a real domain's
    // split between declarative assembly and reactive wiring.
    class FakeRenderSubsystem : public runtime::Subsystem, public ISceneObserver
    {
    public:
        int ready = 0, destroyed = 0;

        void OnReady() override
        {
            if (SceneSubsystem* ss = GetContext()->GetSubsystem<SceneSubsystem>())
            {
                ss->RegisterObserver(this, SceneLifecycleStage::SystemsReady);
                ss->RegisterObserver(this, SceneLifecycleStage::Destroying);
            }
        }
        void OnSystemsReady(Scene&) override { ++ready; }
        void OnDestroying(Scene&) override { ++destroyed; }
    };

    // Counts fixed steps + captures the variable dt a scene's systems actually see.
    struct TimeProbeSystem : SceneSystem
    {
        u32 fixedSteps = 0;
        f32 lastUpdateDelta = 0.0f;
        f32 accumulatedUpdate = 0.0f;
        void OnFixedUpdate(f32) override { ++fixedSteps; }
        void OnUpdate(ScenePhase phase, f32 deltaTime) override
        {
            if (phase != ScenePhase::Update)
            {
                return;
            }
            lastUpdateDelta = deltaTime;
            accumulatedUpdate += deltaTime;
        }
    };
}

TEST_CASE("the composition assembles a scene; SystemsReady observers then wire reactive state")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    FakeRenderSubsystem* render = ctx.AddSubsystem<FakeRenderSubsystem>();
    ctx.Startup(); // OnReady -> FakeRenderSubsystem registers as a scene observer

    const SceneModule* modules[] = {&kRenderModule};
    scenes->SetComposition(SceneComposition::Build(modules));

    // An owner registers its own SceneManager (no shared default manager).
    SceneManager sm;
    scenes->RegisterManager(&sm);

    Scene* level = sm.CreateScene(u8"level");
    REQUIRE(level != nullptr);
    CHECK(level->GetSystem<RenderSceneSystem>() != nullptr); // composition installed the system
    CHECK(render->ready == 1);                              // SystemsReady fired after assembly
    CHECK(render->destroyed == 0);
    CHECK(sm.ActiveScenes().Size() == 1);

    ctx.Shutdown();
}

TEST_CASE("the subsystem ticks its scenes each Context update")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    FakeRenderSubsystem* render = ctx.AddSubsystem<FakeRenderSubsystem>();
    ctx.Startup();

    const SceneModule* modules[] = {&kRenderModule};
    scenes->SetComposition(SceneComposition::Build(modules));

    SceneManager sm;
    scenes->RegisterManager(&sm);

    Scene* level = sm.CreateScene();
    RenderSceneSystem* sys = level->GetSystem<RenderSceneSystem>();
    REQUIRE(sys != nullptr);

    ctx.Update(0.016f); // Context -> SceneSubsystem.Update -> scene.Update
    ctx.Update(0.016f);
    CHECK(sys->ticks == 2);

    ctx.Shutdown();
    (void)render;
}

TEST_CASE("destroying a scene fires Destroying observers + drops it from the active list")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    FakeRenderSubsystem* render = ctx.AddSubsystem<FakeRenderSubsystem>();
    ctx.Startup();

    const SceneModule* modules[] = {&kRenderModule};
    scenes->SetComposition(SceneComposition::Build(modules));

    SceneManager sm;
    scenes->RegisterManager(&sm);

    Scene* a = sm.CreateScene(u8"a");
    Scene* b = sm.CreateScene(u8"b");
    CHECK(sm.ActiveScenes().Size() == 2);
    CHECK(render->ready == 2);

    sm.DestroyScene(a);
    CHECK(render->destroyed == 1);
    CHECK(sm.ActiveScenes().Size() == 1);
    CHECK(sm.GetScene(u8"a") == nullptr);
    CHECK(sm.GetScene(u8"b") == b);

    ctx.Shutdown();
}

TEST_CASE("per-scene time: scales isolate scenes; pause stops one without the other")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    ctx.Startup();
    SceneManager sm;
    scenes->RegisterManager(&sm);

    Scene* normal = sm.CreateScene(u8"normal");
    Scene* slow = sm.CreateScene(u8"slow");
    TimeProbeSystem* normalProbe = normal->AddSystem<TimeProbeSystem>();
    TimeProbeSystem* slowProbe = slow->AddSystem<TimeProbeSystem>();
    slow->SetTimeScale(0.5f);

    // Drive 60 frames of 1/60s through the REAL lanes (BeginFrame steps, Update ticks).
    for (int i = 0; i < 60; ++i)
    {
        ctx.BeginFrame(1.0f / 60.0f);
        ctx.Update(1.0f / 60.0f);
    }
    CHECK(normalProbe->fixedSteps == 60);
    CHECK(slowProbe->fixedSteps == 30); // half speed
    CHECK(normalProbe->accumulatedUpdate == doctest::Approx(1.0f));
    CHECK(slowProbe->accumulatedUpdate == doctest::Approx(0.5f));

    // Pause ONE scene via its time scale: the other keeps stepping.
    slow->SetTimeScale(0.0f);
    const u32 slowBefore = slowProbe->fixedSteps;
    for (int i = 0; i < 30; ++i)
    {
        ctx.BeginFrame(1.0f / 60.0f);
        ctx.Update(1.0f / 60.0f);
    }
    CHECK(slowProbe->fixedSteps == slowBefore); // frozen
    CHECK(normalProbe->fixedSteps == 90);       // unaffected
    CHECK(slowProbe->lastUpdateDelta == doctest::Approx(0.0f));

    // Context scale still multiplies on top of scene scales.
    slow->SetTimeScale(1.0f);
    ctx.SetTimeScale(2.0f);
    for (int i = 0; i < 30; ++i)
    {
        ctx.BeginFrame(1.0f / 60.0f);
        ctx.Update((1.0f / 60.0f) * ctx.TimeScale()); // host pre-scales Update's dt
    }
    CHECK(normalProbe->fixedSteps == 150); // 30 frames x 2 steps
    CHECK(slowProbe->fixedSteps == slowBefore + 60);

    ctx.Shutdown();
}

TEST_CASE("per-scene time: fixed alpha is the scene's own leftover fraction")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    ctx.Startup();
    SceneManager sm;
    scenes->RegisterManager(&sm);
    Scene* scene = sm.CreateScene(u8"alpha");
    scene->SetFixedTiming(1.0f / 60.0f, 4);

    // Feed 1.5 steps: one step fires, half a step remains -> alpha 0.5.
    ctx.BeginFrame(1.5f / 60.0f);
    CHECK(scene->FixedAlpha() == doctest::Approx(0.5f).epsilon(0.01));

    // 0.75 more steps: the second step fires, a quarter step remains -> alpha 0.25.
    ctx.BeginFrame(0.75f / 60.0f);
    CHECK(scene->FixedAlpha() == doctest::Approx(0.25f).epsilon(0.05));

    ctx.Shutdown();
}