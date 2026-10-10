// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Systems wired into the Scene: ownership + lookup, the phase-ordered update
// loop, deferred destroy during update, entity-destroy freeing components, active-change
// + start/stop notification, UpdateOrder, and simulation gating.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;

using namespace foundation::core;
using namespace foundation::scene;

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
    Scene scene{DefaultAllocator()};
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    REQUIRE(mgr != nullptr);
    CHECK(scene.GetSystem<HealthManager>() == mgr);
    CHECK(scene.HasSystem<HealthManager>());
    CHECK(scene.GetSystem<Recorder>() == nullptr);
}

TEST_CASE("update runs the gameplay phases in ScenePhase order")
{
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    EntityHandle e = scene.CreateEntity();
    mgr->Add(e).value = 50.0f;
    CHECK(mgr->initialized == 0); // not yet

    scene.Update(0.016f); // Initialize phase runs pending init
    CHECK(mgr->initialized == 1);
}

TEST_CASE("destroying an entity frees its component via the manager")
{
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
    scene.AddSystem<Ordered>(10, &order); // added first, but higher order
    scene.AddSystem<Ordered>(-5, &order); // added second, lower order -> runs first
    scene.Update(0.016f);
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == -5);
    CHECK(order[1] == 10);
}

TEST_CASE("scene: Events() is the BORROWED scope bus - null unwired, never drained by the scene")
{
    // No owned fallback: a scene holds only the scope's borrowed bus; unwired scenes have a
    // null Events() and never emit; the scene NEVER drains (only the owning scope does) -
    // fixtures inject a bus, exercising the exact runtime topology.
    namespace messaging = foundation::messaging;

    Scene scene{DefaultAllocator()};
    CHECK(scene.Events() == nullptr); // unwired: no bus, nothing to emit into

    messaging::EventBus scope;
    scene.SetEventBus(&scope);
    REQUIRE(scene.Events() == &scope);
    int fired = 0;
    (void)scope.Subscribe(StringHash(u8"Ping"), [&](const Variant&) { ++fired; });
    scene.Events()->Publish(StringHash(u8"Ping"), Variant{});
    scene.Update(0.016f);
    CHECK(fired == 0);                 // the scene did NOT drain - scopes drain
    CHECK(scope.PendingCount() == 1u); // still queued for the owning scope
    scope.Drain();
    CHECK(fired == 1);

    scene.SetEventBus(nullptr); // scope teardown detaches cleanly
    CHECK(scene.Events() == nullptr);
}

namespace
{
    // A system that owns level geometry: one floor quad at y 0, when the box reaches it.
    class FloorSource final : public SceneSystem, public IStaticGeometrySource
    {
    public:
        IStaticGeometrySource* AsStaticGeometrySource() noexcept override { return this; }
        void CollectStaticGeometry(Scene&, const AABB& bounds, f32, Array<Float3>& out) override
        {
            if (bounds.min.y > 0.0f || bounds.max.y < 0.0f)
            {
                return;
            }
            const Float3 quad[] = {{-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {-1, 0, -1}, {1, 0, 1}, {1, 0, -1}};
            for (const Float3& p : quad)
            {
                out.PushBack(p);
            }
        }
    };
}

// Navigation bakes from every system that answers AsStaticGeometrySource; the rest answer null.
TEST_CASE("a system with static geometry answers the capability, others answer null")
{
    Scene scene{DefaultAllocator()};
    scene.AddSystem<HealthManager>();
    scene.AddSystem<FloorSource>();
    Array<Float3> triangles;
    usize sources = 0;
    const AABB around{Float3{-5, -1, -5}, Float3{5, 1, 5}};
    scene.ForEachSystem(
        [&](SceneSystem& system)
        {
            if (IStaticGeometrySource* source = system.AsStaticGeometrySource())
            {
                ++sources;
                source->CollectStaticGeometry(scene, around, 0.3f, triangles);
            }
        });
    CHECK(sources == 1);
    CHECK(triangles.Size() == 6);
}

namespace
{
    // A system that measures one entity as a fixed box: what a render or physics domain answers.
    template <int Tag>
    class BoxMeasure final : public SceneSystem, public ISceneEntityBounds
    {
    public:
        EntityHandle entity = EntityHandle::Invalid();
        AABB box{};
        [[nodiscard]] ISceneEntityBounds* AsEntityBounds() noexcept override { return this; }
        [[nodiscard]] bool EntityBounds(Scene&, EntityHandle e, AABB& out) override
        {
            if (e != entity)
            {
                return false;
            }
            out = box;
            return true;
        }
    };
}

TEST_CASE("EntityWorldBounds merges what every measuring system answers, and says when none does")
{
    Scene scene{DefaultAllocator()};
    const EntityHandle crate = scene.CreateEntity(u8"Crate");
    const EntityHandle marker = scene.CreateEntity(u8"Marker");
    AABB out{Float3{7.0f, 7.0f, 7.0f}, Float3{7.0f, 7.0f, 7.0f}};

    // No system measures anything yet.
    CHECK_FALSE(EntityWorldBounds(scene, crate, out));
    CHECK(out.min.x == 7.0f); // untouched

    // One system: its box.
    auto* mesh = scene.AddSystem<BoxMeasure<0>>();
    mesh->entity = crate;
    mesh->box = AABB{Float3{0.0f, 0.0f, 0.0f}, Float3{1.0f, 2.0f, 1.0f}};
    REQUIRE(EntityWorldBounds(scene, crate, out));
    CHECK(out.max.y == 2.0f);

    // A second one (a collider reaching further): the box holding both.
    auto* collider = scene.AddSystem<BoxMeasure<1>>();
    collider->entity = crate;
    collider->box = AABB{Float3{-1.0f, 0.0f, 0.0f}, Float3{0.5f, 1.0f, 3.0f}};
    REQUIRE(EntityWorldBounds(scene, crate, out));
    CHECK(out.min.x == -1.0f);
    CHECK(out.max.y == 2.0f);
    CHECK(out.max.z == 3.0f);

    // An entity neither measures.
    CHECK_FALSE(EntityWorldBounds(scene, marker, out));
}
