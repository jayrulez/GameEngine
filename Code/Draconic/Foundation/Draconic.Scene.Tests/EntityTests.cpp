// Phase 1 - the entity table: generational handles, free-list slot reuse + stale-handle
// detection, persistent-Guid <-> handle mapping, active/name state. These encode the
// entity-lifecycle/validity behaviors pinned from the Sedulous test suite.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;

using namespace draconic::foundation;
using namespace draconic::scene;

TEST_CASE("entity create: unique valid handles + count")
{
    Scene scene(u8"world");
    CHECK(scene.Name() == u8"world");
    CHECK(scene.EntityCount() == 0);

    EntityHandle a = scene.CreateEntity(u8"a");
    EntityHandle b = scene.CreateEntity(u8"b");
    CHECK(scene.IsValid(a));
    CHECK(scene.IsValid(b));
    CHECK(a != b);
    CHECK(scene.EntityCount() == 2);
    CHECK(scene.GetEntityName(a) == u8"a");
    CHECK(scene.GetEntityName(b) == u8"b");
    CHECK(scene.IsActive(a)); // entities start active
}

TEST_CASE("entity destroy: invalidates the handle")
{
    Scene scene;
    EntityHandle e = scene.CreateEntity();
    REQUIRE(scene.IsValid(e));
    scene.DestroyEntity(e);
    CHECK_FALSE(scene.IsValid(e));
    CHECK(scene.EntityCount() == 0);
    scene.DestroyEntity(e); // double-destroy is a no-op
    CHECK(scene.EntityCount() == 0);
}

TEST_CASE("slot reuse bumps generation: a stale handle is detected, not confused with the new one")
{
    Scene scene;
    EntityHandle first = scene.CreateEntity();
    const u32 reusedIndex = first.index;
    scene.DestroyEntity(first);

    EntityHandle second = scene.CreateEntity();   // reuses the freed slot
    CHECK(second.index == reusedIndex);           // same slot...
    CHECK(second.generation != first.generation); // ...new generation
    CHECK(scene.IsValid(second));
    CHECK_FALSE(scene.IsValid(first)); // old handle stays invalid
}

TEST_CASE("invalid/unassigned handles never validate")
{
    Scene scene;
    CHECK_FALSE(scene.IsValid(EntityHandle::Invalid()));
    CHECK_FALSE(scene.IsValid(EntityHandle{999u, 1u})); // out of range
    CHECK(scene.GetEntityName(EntityHandle::Invalid()) == StringView{});
    scene.SetActive(EntityHandle::Invalid(), false);     // no crash
    scene.SetEntityName(EntityHandle::Invalid(), u8"x"); // no crash
}

TEST_CASE("persistent Guid <-> handle: find resolves, survives a specific-id create")
{
    Scene scene;
    EntityHandle e = scene.CreateEntity(u8"named");
    const Guid id = scene.GetEntityId(e);
    CHECK(id != Guid{});
    CHECK(scene.FindEntity(id) == e);

    // a deterministic id (e.g. from a loaded scene) round-trips through Find
    const Guid fixed{0x0123456789abcdefull, 0xfedcba9876543210ull};
    EntityHandle loaded = scene.CreateEntity(fixed, u8"loaded");
    CHECK(scene.GetEntityId(loaded) == fixed);
    CHECK(scene.FindEntity(fixed) == loaded);

    // after destroy, Find prunes the stale entry and reports Invalid
    scene.DestroyEntity(loaded);
    CHECK(scene.FindEntity(fixed) == EntityHandle::Invalid());
}

TEST_CASE("active + name are mutable on live entities")
{
    Scene scene;
    EntityHandle e = scene.CreateEntity(u8"orig");
    scene.SetActive(e, false);
    CHECK_FALSE(scene.IsActive(e));
    scene.SetEntityName(e, u8"renamed");
    CHECK(scene.GetEntityName(e) == u8"renamed");
}

TEST_CASE("ForEachEntity visits exactly the live entities")
{
    Scene scene;
    EntityHandle a = scene.CreateEntity();
    EntityHandle b = scene.CreateEntity();
    EntityHandle c = scene.CreateEntity();
    scene.DestroyEntity(b);

    u32 visited = 0;
    bool sawA = false, sawC = false, sawB = false;
    scene.ForEachEntity(
        [&](EntityHandle h)
        {
            ++visited;
            if (h == a)
            {
                sawA = true;
            }
            if (h == c)
            {
                sawC = true;
            }
            if (h == b)
            {
                sawB = true;
            }
        });
    CHECK(visited == 2);
    CHECK(sawA);
    CHECK(sawC);
    CHECK_FALSE(sawB);
}

TEST_CASE("revision advances on structural change")
{
    Scene scene;
    const u64 r0 = scene.Revision();
    EntityHandle e = scene.CreateEntity();
    CHECK(scene.Revision() > r0);
    const u64 r1 = scene.Revision();
    scene.DestroyEntity(e);
    CHECK(scene.Revision() > r1);
}

TEST_CASE("revision advances on rename, reparent, and active toggle")
{
    // Observers (e.g. an editor hierarchy) rebuild off Revision(); every mutation that changes
    // what they display must advance it. Local transforms deliberately do NOT (they'd churn a
    // rebuild every frame of a gizmo drag).
    Scene scene;
    EntityHandle a = scene.CreateEntity(u8"a");
    EntityHandle b = scene.CreateEntity(u8"b");

    u64 r = scene.Revision();
    scene.SetEntityName(a, u8"renamed");
    CHECK(scene.Revision() > r);

    r = scene.Revision();
    scene.SetParent(b, a);
    CHECK(scene.Revision() > r);

    r = scene.Revision();
    scene.SetActive(a, false);
    CHECK(scene.Revision() > r);

    r = scene.Revision();
    scene.SetActive(a, false); // no-op: unchanged active state doesn't advance
    CHECK(scene.Revision() == r);

    r = scene.Revision();
    Transform t;
    t.position = Float3{1, 2, 3};
    scene.SetLocalTransform(a, t);
    CHECK(scene.Revision() == r);
}

TEST_CASE("MoveBefore reorders siblings and roots")
{
    Scene scene;
    EntityHandle a = scene.CreateEntity(u8"a");
    EntityHandle b = scene.CreateEntity(u8"b");
    EntityHandle c = scene.CreateEntity(u8"c");

    // Root list starts in creation order: a, b, c.
    CHECK(scene.GetFirstRoot() == a);
    CHECK(scene.GetNextSibling(a) == b);
    CHECK(scene.GetNextSibling(b) == c);

    // Move c before a: c, a, b (head update path).
    u64 r = scene.Revision();
    scene.MoveBefore(c, a);
    CHECK(scene.Revision() > r);
    CHECK(scene.GetFirstRoot() == c);
    CHECK(scene.GetNextSibling(c) == a);
    CHECK(scene.GetNextSibling(a) == b);

    // Move c before b (middle): a, c, b.
    scene.MoveBefore(c, b);
    CHECK(scene.GetFirstRoot() == a);
    CHECK(scene.GetNextSibling(a) == c);
    CHECK(scene.GetNextSibling(c) == b);

    // Already in place: no revision churn.
    r = scene.Revision();
    scene.MoveBefore(c, b);
    CHECK(scene.Revision() == r);

    // SetParent to invalid = move to END of the root list: c, b, ... a.
    scene.SetParent(a, EntityHandle::Invalid());
    CHECK(scene.GetFirstRoot() == c);
    CHECK(scene.GetNextSibling(b) == a);

    // Reorder INTO a child list: parent p with child b -> move a before b under p.
    EntityHandle p = scene.CreateEntity(u8"p");
    scene.SetParent(b, p);
    scene.MoveBefore(a, b);
    CHECK(scene.GetParent(a) == p);
    CHECK(scene.GetFirstChild(p) == a);
    CHECK(scene.GetNextSibling(a) == b);

    // Cycle guard: moving p before its own grandchild's sibling slot is refused.
    scene.SetParent(c, b); // p / [a, b / [c]]
    r = scene.Revision();
    scene.MoveBefore(p, c);
    CHECK(scene.Revision() == r);
    CHECK(scene.GetParent(p) == EntityHandle::Invalid());
}

TEST_CASE("entity find: by name (first match) and by hierarchy path")
{
    Scene scene(u8"world");
    EntityHandle player = scene.CreateEntity(u8"Player");
    EntityHandle weapon = scene.CreateEntity(u8"Weapon");
    EntityHandle muzzle = scene.CreateEntity(u8"Muzzle");
    EntityHandle enemy = scene.CreateEntity(u8"Enemy");
    scene.SetParent(weapon, player);
    scene.SetParent(muzzle, weapon);

    // Find by name.
    CHECK(scene.FindEntityByName(u8"Player") == player);
    CHECK(scene.FindEntityByName(u8"Muzzle") == muzzle);
    CHECK_FALSE(scene.FindEntityByName(u8"Missing").IsAssigned());

    // First-match on duplicate names.
    EntityHandle dupA = scene.CreateEntity(u8"Dup");
    (void)scene.CreateEntity(u8"Dup");
    CHECK(scene.FindEntityByName(u8"Dup") == dupA);

    // Find by hierarchy path (roots downward).
    CHECK(scene.FindEntityByPath(u8"Player") == player);
    CHECK(scene.FindEntityByPath(u8"Player/Weapon") == weapon);
    CHECK(scene.FindEntityByPath(u8"Player/Weapon/Muzzle") == muzzle);
    // Tolerate leading/trailing/double slashes.
    CHECK(scene.FindEntityByPath(u8"/Player//Weapon/") == weapon);
    // A miss at any depth is invalid; Enemy is a root, not under Player.
    CHECK_FALSE(scene.FindEntityByPath(u8"Player/Muzzle").IsAssigned());
    CHECK_FALSE(scene.FindEntityByPath(u8"Player/Weapon/Enemy").IsAssigned());
    CHECK_FALSE(scene.FindEntityByPath(u8"").IsAssigned());
    CHECK(scene.FindEntityByPath(u8"Enemy") == enemy);

    // FindChildByName: invalid parent = search roots.
    CHECK(scene.FindChildByName(EntityHandle::Invalid(), u8"Player") == player);
    CHECK(scene.FindChildByName(player, u8"Weapon") == weapon);
    CHECK_FALSE(scene.FindChildByName(player, u8"Muzzle").IsAssigned()); // grandchild
}
