// Phase 3a - the value-pool component manager (sparse set): add/get/has/remove by
// entity, dense contiguous iteration, swap-remove keeping the pack dense, generation
// staleness, the one-per-entity invariant, and manager-driven deferred lifecycle.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;

using namespace draconic::foundation;
using namespace draconic::scene;

namespace
{
    // A plain value component.
    struct Health
    {
        f32 value = 100.0f;
    };

    // A concrete manager that records lifecycle calls (manager-driven hooks).
    class HealthManager : public ComponentManager<Health>
    {
    public:
        int created = 0, initialized = 0, destroyed = 0;

    protected:
        void OnComponentCreated(Health&, EntityHandle) override { ++created; }
        void OnComponentInitialized(Health&, EntityHandle) override { ++initialized; }
        void OnComponentDestroyed(Health&, EntityHandle) override { ++destroyed; }
    };

    constexpr EntityHandle E(u32 i, u32 g = 1) { return EntityHandle{i, g}; }
}

TEST_CASE("add/get/has/remove a component by entity")
{
    HealthManager mgr;
    CHECK_FALSE(mgr.Has(E(0)));
    Health& h = mgr.Add(E(0));
    h.value = 42.0f;
    CHECK(mgr.Has(E(0)));
    CHECK(mgr.Count() == 1);
    CHECK(mgr.Get(E(0))->value == doctest::Approx(42.0f));
    CHECK(mgr.Get(E(1)) == nullptr); // different entity, no component

    mgr.RemoveComponent(E(0));
    CHECK_FALSE(mgr.Has(E(0)));
    CHECK(mgr.Get(E(0)) == nullptr);
    CHECK(mgr.Count() == 0);
}

TEST_CASE("dense storage stays packed across a middle remove (swap-with-last)")
{
    HealthManager mgr;
    mgr.Add(E(0)).value = 10.0f;
    mgr.Add(E(1)).value = 20.0f;
    mgr.Add(E(2)).value = 30.0f;
    CHECK(mgr.Dense().Size() == 3);

    mgr.RemoveComponent(E(1)); // remove the middle one
    CHECK(mgr.Count() == 2);
    CHECK(mgr.Dense().Size() == 2); // still contiguous, no hole
    // remaining components are still reachable by their entity (regardless of order)
    CHECK(mgr.Get(E(0))->value == doctest::Approx(10.0f));
    CHECK(mgr.Get(E(2))->value == doctest::Approx(30.0f));
    CHECK(mgr.Get(E(1)) == nullptr);

    // every dense slot has a live owner with a resolvable component
    u32 seen = 0;
    mgr.ForEach(
        [&](Health& c, EntityHandle owner)
        {
            ++seen;
            CHECK(mgr.Get(owner) == &c); // owner resolves back to this slot
        });
    CHECK(seen == 2);
}

TEST_CASE("generational staleness: a reused entity slot does not resolve an old handle")
{
    HealthManager mgr;
    mgr.Add(E(5, /*gen*/ 1)).value = 1.0f;
    mgr.RemoveComponent(E(5, 1));
    // slot index 5 reused by a new entity (generation 2) with its own component
    mgr.Add(E(5, 2)).value = 2.0f;

    CHECK(mgr.Get(E(5, 2))->value == doctest::Approx(2.0f)); // current occupant
    CHECK(mgr.Get(E(5, 1)) == nullptr);                      // stale handle: absent
    CHECK_FALSE(mgr.Has(E(5, 1)));
}

TEST_CASE("manager-driven lifecycle: create immediately, init deferred, destroy on remove")
{
    HealthManager mgr;
    mgr.Add(E(0));
    mgr.Add(E(1));
    CHECK(mgr.created == 2);
    CHECK(mgr.initialized == 0); // not yet - deferred

    mgr.InitializePendingComponents();
    CHECK(mgr.initialized == 2);

    // a component added then removed before init is never initialized
    mgr.Add(E(2));
    mgr.RemoveComponent(E(2));
    mgr.InitializePendingComponents();
    CHECK(mgr.initialized == 2); // still 2
    CHECK(mgr.destroyed == 1);

    // OnEntityDestroyed (the base hook the Scene calls) removes the component
    mgr.OnEntityDestroyed(E(0));
    CHECK_FALSE(mgr.Has(E(0)));
    CHECK(mgr.destroyed == 2);
}

TEST_CASE("component type id is stable + distinct per component type")
{
    HealthManager mgr;
    struct Mana
    {
        f32 v = 0;
    };
    ComponentManager<Mana> manaMgr;
    CHECK(mgr.ComponentType() == &TypeOf<Health>());
    CHECK(mgr.ComponentType() != manaMgr.ComponentType());
}

// The scene-side resolve for `entity.get(Type)` (game-ready-scripting P-A1): resolve the manager
// by the component's reflected type, then re-resolve the LIVE component on every access. This is
// the load-bearing safety property (Fable Correction 1): a re-resolving handle survives the pool's
// swap-remove, where a cached (borrow) component address would go stale or, worse, point at a
// different entity's component.
TEST_CASE("scene: FindManagerByComponentType + re-resolution survives swap-remove / growth "
          "(entity.get safety, Correction 1)")
{
    Scene scene(u8"resolve");
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    const EntityHandle e0 = scene.CreateEntity(u8"e0");
    const EntityHandle e1 = scene.CreateEntity(u8"e1");
    const EntityHandle e2 = scene.CreateEntity(u8"e2");
    mgr->Add(e0).value = 10.0f;
    mgr->Add(e1).value = 20.0f;
    mgr->Add(e2).value = 30.0f;

    // Resolve the manager by the component's reflected type (the entity.get type token).
    ComponentManagerBase* found = scene.FindManagerByComponentType(TypeOf<Health>());
    REQUIRE(found == static_cast<ComponentManagerBase*>(mgr));
    CHECK(scene.FindManagerByComponentType(TypeOf<int>()) == nullptr); // no manager -> null

    // Cache e2's CURRENT component address - what a borrow handle would keep.
    Instance before = found->GetComponentInstance(e2);
    REQUIRE(before.Pointer() != nullptr);
    void* staleAddr = before.Pointer();
    CHECK(static_cast<Health*>(before.Pointer())->value == doctest::Approx(30.0f));

    // Remove ANOTHER entity's component: swap-remove moves e2's component within the dense pool.
    mgr->RemoveComponent(e1);

    // Re-resolution (what entity.get does per access) still finds e2's REAL component...
    Instance after = found->GetComponentInstance(e2);
    REQUIRE(after.Pointer() != nullptr);
    CHECK(static_cast<Health*>(after.Pointer())->value == doctest::Approx(30.0f));
    CHECK(after.Pointer() != staleAddr); // ...and it moved - the cached borrow would be wrong

    // Pool growth (realloc) - re-resolution still correct.
    for (int i = 0; i < 32; ++i)
    {
        mgr->Add(scene.CreateEntity(u8"filler")).value = 1.0f;
    }
    Instance grown = found->GetComponentInstance(e2);
    REQUIRE(grown.Pointer() != nullptr);
    CHECK(static_cast<Health*>(grown.Pointer())->value == doctest::Approx(30.0f));

    // Component removed -> re-resolution is a clean empty Instance (null-op, no crash).
    mgr->RemoveComponent(e2);
    CHECK(found->GetComponentInstance(e2).Pointer() == nullptr);
}

// The full entity.get(Type) resolver: MakeComponentRef packages {scene, entity, manager} into a
// RESOLVE-mode Variant, so ToInstance() (what a script get/set/call goes through) recomputes the
// live component address every time - connecting FindManagerByComponentType + Variant RESOLVE mode.
TEST_CASE("scene: MakeComponentRef gives a RESOLVE Variant that re-resolves the live component")
{
    Scene scene(u8"ref");
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    const EntityHandle e0 = scene.CreateEntity(u8"e0");
    const EntityHandle e1 = scene.CreateEntity(u8"e1");
    mgr->Add(e0).value = 11.0f;
    mgr->Add(e1).value = 22.0f;

    CHECK(scene.MakeComponentRef(e1, TypeOf<int>()).IsEmpty()); // no manager for that type

    Variant ref = scene.MakeComponentRef(e1, TypeOf<Health>());
    CHECK(ref.IsResolving());
    CHECK(ref.Type() == &TypeOf<Health>());

    Instance i0 = ToInstance(ref);
    REQUIRE(i0.Pointer() != nullptr);
    CHECK(static_cast<Health*>(i0.Pointer())->value == doctest::Approx(22.0f));
    void* stale = i0.Pointer();

    // Swap-remove e0 moves e1's component; the SAME ref re-resolves to the moved component.
    mgr->RemoveComponent(e0);
    Instance i1 = ToInstance(ref);
    REQUIRE(i1.Pointer() != nullptr);
    CHECK(static_cast<Health*>(i1.Pointer())->value == doctest::Approx(22.0f));
    CHECK(i1.Pointer() != stale);

    // Component removed -> the ref resolves to empty (a script get/set/call no-ops).
    mgr->RemoveComponent(e1);
    CHECK(ToInstance(ref).Pointer() == nullptr);
}
