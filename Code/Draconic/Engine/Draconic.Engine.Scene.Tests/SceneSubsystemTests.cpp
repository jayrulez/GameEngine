// Phase 4 - the Context-level scene driver + ISceneAware injection: a scene-aware
// subsystem registers with the broker and injects a per-scene system into each new
// scene; the SceneSubsystem owns scenes, ticks them, and notifies create/ready/destroy.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.runtime;
import draconic.scene;
import draconic.engine.scene;

using namespace draconic::foundation;
using namespace draconic::scene;
namespace runtime = draconic::runtime;

namespace
{
    // A per-scene system a "render" subsystem injects into every scene.
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

    // A Context-level subsystem that reacts to scene lifecycle (the ISceneAware role).
    class FakeRenderSubsystem : public runtime::Subsystem, public ISceneAware
    {
    public:
        int created = 0, ready = 0, destroyed = 0;

        void OnReady() override
        {
            if (SceneSubsystem* ss = GetContext()->GetSubsystem<SceneSubsystem>())
            {
                ss->RegisterSceneAware(this);
            }
        }
        void OnSceneCreated(Scene& scene) override
        {
            ++created;
            scene.AddSystem<RenderSceneSystem>();
        }
        void OnSceneReady(Scene&) override { ++ready; }
        void OnSceneDestroyed(Scene&) override { ++destroyed; }
    };
}

TEST_CASE("scene-aware subsystem injects a per-scene system on scene creation (two-pass)")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    FakeRenderSubsystem* render = ctx.AddSubsystem<FakeRenderSubsystem>();
    ctx.Startup(); // OnReady -> render registers with the broker

    // The subsystem owns no scenes; an owner registers its own SceneManager over the shared registry.
    SceneManager sm(&scenes->AwareRegistry());
    scenes->RegisterManager(&sm);

    Scene* level = sm.CreateScene(u8"level");
    REQUIRE(level != nullptr);
    CHECK(render->created == 1);
    CHECK(render->ready == 1);                               // both passes ran
    CHECK(level->GetSystem<RenderSceneSystem>() != nullptr); // injected
    CHECK(sm.GetScene(u8"level") == level);
    CHECK(sm.ActiveScenes().Size() == 1);

    ctx.Shutdown();
}

TEST_CASE("the subsystem ticks its scenes each Context update")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    FakeRenderSubsystem* render = ctx.AddSubsystem<FakeRenderSubsystem>();
    ctx.Startup();
    SceneManager sm(&scenes->AwareRegistry());
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

TEST_CASE("destroying a scene notifies aware subsystems + drops it from the active list")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    FakeRenderSubsystem* render = ctx.AddSubsystem<FakeRenderSubsystem>();
    ctx.Startup();
    SceneManager sm(&scenes->AwareRegistry());
    scenes->RegisterManager(&sm);

    Scene* a = sm.CreateScene(u8"a");
    Scene* b = sm.CreateScene(u8"b");
    CHECK(sm.ActiveScenes().Size() == 2);
    CHECK(render->created == 2);

    sm.DestroyScene(a);
    CHECK(render->destroyed == 1);
    CHECK(sm.ActiveScenes().Size() == 1);
    CHECK(sm.GetScene(u8"a") == nullptr);
    CHECK(sm.GetScene(u8"b") == b);

    ctx.Shutdown();
}

TEST_CASE("scene-aware registration is idempotent; unregister stops notifications")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    FakeRenderSubsystem* render = ctx.AddSubsystem<FakeRenderSubsystem>();
    ctx.Startup();
    SceneManager sm(&scenes->AwareRegistry());
    scenes->RegisterManager(&sm);

    scenes->RegisterSceneAware(render); // duplicate (already registered in OnReady)
    sm.CreateScene(u8"one");
    CHECK(render->created == 1); // notified once, not twice

    scenes->UnregisterSceneAware(render);
    sm.CreateScene(u8"two");
    CHECK(render->created == 1); // no longer notified

    ctx.Shutdown();
}

namespace
{
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

TEST_CASE("per-scene time: scales isolate scenes; pause stops one without the other")
{
    runtime::Context ctx;
    SceneSubsystem* scenes = ctx.AddSubsystem<SceneSubsystem>();
    ctx.Startup();
    SceneManager sm(&scenes->AwareRegistry());
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

    // Pause ONE scene via its time scale: the other keeps stepping (the collision the
    // old context-global scale could not express).
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
    SceneManager sm(&scenes->AwareRegistry());
    scenes->RegisterManager(&sm);
    Scene* scene = sm.CreateScene(u8"alpha");
    scene->SetFixedTiming(1.0f / 60.0f, 4);

    // Feed 1.5 steps: one step fires, half a step remains -> alpha 0.5.
    ctx.BeginFrame(1.5f / 60.0f);
    CHECK(scene->FixedAlpha() == doctest::Approx(0.5f).epsilon(0.01));

    // 0.75 more steps: the second step fires, a quarter step remains -> alpha 0.25.
    // (Not 0.5 more - landing EXACTLY on a step boundary is float-representation
    // dependent and can read as alpha ~1 or ~0.)
    ctx.BeginFrame(0.75f / 60.0f);
    CHECK(scene->FixedAlpha() == doctest::Approx(0.25f).epsilon(0.05));

    ctx.Shutdown();
}
