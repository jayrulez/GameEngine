// Phase 3b - systems wired into the Scene: ownership + lookup, the phase-ordered update
// loop, deferred destroy during update, entity-destroy freeing components, active-change
// + start/stop notification, UpdateOrder, and simulation gating.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;

using namespace draconic::foundation;
using namespace draconic::scene;

namespace
{
    struct Health
    {
        f32 value = 100.0f;
    };

    class HealthManager : public ComponentManager<Health>
    {
    public:
        int initialized = 0, destroyed = 0;

    protected:
        void OnComponentInitialized(Health&, EntityHandle) override { ++initialized; }
        void OnComponentDestroyed(Health&, EntityHandle) override { ++destroyed; }
    };

    // Records which phases/notifications it received.
    class Recorder : public SceneSystem
    {
    public:
        Array<ScenePhase> phases;
        int started = 0, stopped = 0, entityDestroyed = 0, activeChanged = 0, fixedUpdates = 0;
        void OnUpdate(ScenePhase p, f32) override { phases.PushBack(p); }
        void OnFixedUpdate(f32) override { ++fixedUpdates; }
        void OnSceneStarted() override { ++started; }
        void OnSceneStopped() override { ++stopped; }
        void OnEntityDestroyed(EntityHandle) override { ++entityDestroyed; }
        void OnEntityActiveChanged(EntityHandle, bool) override { ++activeChanged; }
    };

    class SimOnly : public SceneSystem
    {
    public:
        int updates = 0;
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        void OnUpdate(ScenePhase, f32) override { ++updates; }
    };
}

TEST_CASE("add/get systems by type; the Scene owns them")
{
    Scene scene;
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    REQUIRE(mgr != nullptr);
    CHECK(scene.GetSystem<HealthManager>() == mgr);
    CHECK(scene.HasSystem<HealthManager>());
    CHECK(scene.GetSystem<Recorder>() == nullptr);
}

TEST_CASE("update runs the gameplay phases in ScenePhase order")
{
    Scene scene;
    Recorder* rec = scene.AddSystem<Recorder>();
    scene.Update(0.016f);

    REQUIRE(rec->phases.Size() == 5);
    CHECK(rec->phases[0] == ScenePhase::PreUpdate);
    CHECK(rec->phases[1] == ScenePhase::Update);
    CHECK(rec->phases[2] == ScenePhase::AsyncUpdate);
    CHECK(rec->phases[3] == ScenePhase::PostUpdate);
    CHECK(rec->phases[4] == ScenePhase::PostTransform); // after the transform recompute
}

TEST_CASE("component init is deferred to the scene's Initialize phase")
{
    Scene scene;
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    EntityHandle e = scene.CreateEntity();
    mgr->Add(e).value = 50.0f;
    CHECK(mgr->initialized == 0); // not yet

    scene.Update(0.016f); // Initialize phase runs pending init
    CHECK(mgr->initialized == 1);
}

TEST_CASE("destroying an entity frees its component via the manager")
{
    Scene scene;
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    EntityHandle e = scene.CreateEntity();
    mgr->Add(e);
    CHECK(mgr->Has(e));

    scene.DestroyEntity(e); // not during update -> immediate
    CHECK_FALSE(mgr->Has(e));
    CHECK(mgr->destroyed == 1);
}

TEST_CASE("destroy during update is deferred to Cleanup")
{
    Scene scene;
    EntityHandle target = scene.CreateEntity();

    // a system that destroys `target` during the Update phase
    struct Destroyer : SceneSystem
    {
        Scene* scene = nullptr;
        EntityHandle target = EntityHandle::Invalid();
        bool validDuringUpdate = false;
        void OnSceneCreate(Scene& s) override { scene = &s; }
        void OnUpdate(ScenePhase p, f32) override
        {
            if (p == ScenePhase::Update)
            {
                validDuringUpdate = scene->IsValid(target);
                scene->DestroyEntity(target);
            }
        }
    };
    Destroyer* d = scene.AddSystem<Destroyer>();
    d->target = target;

    scene.Update(0.016f);
    CHECK(d->validDuringUpdate);        // still alive mid-update (deferred)
    CHECK_FALSE(scene.IsValid(target)); // destroyed in Cleanup
}

TEST_CASE("active change + start/stop notify systems; fixed update ticks")
{
    Scene scene;
    Recorder* rec = scene.AddSystem<Recorder>();
    EntityHandle e = scene.CreateEntity();

    scene.SetActive(e, false);
    CHECK(rec->activeChanged == 1);

    scene.Start();
    CHECK(rec->started == 1);
    CHECK(scene.IsStarted());
    scene.FixedUpdate(0.02f);
    CHECK(rec->fixedUpdates == 1);
    scene.Stop();
    CHECK(rec->stopped == 1);
    CHECK_FALSE(scene.IsStarted());
}

TEST_CASE("simulation gating: sim-only systems skip when simulation is disabled")
{
    Scene scene;
    SimOnly* sim = scene.AddSystem<SimOnly>();

    scene.Update(0.016f);     // enabled by default
    CHECK(sim->updates == 5); // 5 phases

    scene.SetSimulationEnabled(false);
    scene.Update(0.016f);
    CHECK(sim->updates == 5); // unchanged: skipped
    scene.FixedUpdate(0.02f); // also skipped
    CHECK(sim->updates == 5);
}

TEST_CASE("systems run within a phase in UpdateOrder")
{
    Array<i32> order;
    struct Ordered : SceneSystem
    {
        i32 ord;
        Array<i32>* log;
        Ordered(i32 o, Array<i32>* l) : ord(o), log(l) {}
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return ord; }
        void OnUpdate(ScenePhase p, f32) override
        {
            if (p == ScenePhase::Update)
            {
                log->PushBack(ord);
            }
        }
    };
    Scene scene;
    scene.AddSystem<Ordered>(10, &order); // added first, but higher order
    scene.AddSystem<Ordered>(-5, &order); // added second, lower order -> runs first
    scene.Update(0.016f);
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == -5);
    CHECK(order[1] == 10);
}
