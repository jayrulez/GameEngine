// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Phase 1 - the entity table: generational handles, free-list slot reuse + stale-handle
// detection, persistent-Guid <-> handle mapping, active/name state. These encode the
// entity-lifecycle/validity behaviors pinned from the Sedulous test suite.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;

using namespace foundation::core;
using namespace foundation::scene;

// The renderer tells a view's scenes apart by this (a pooled snapshot or a reused address
// cannot): every scene instance has its own, never zero, and a later scene never repeats one.
TEST_CASE("scene serial: every scene instance has its own")
{
    Scene first(DefaultAllocator(), u8"first");
    Scene second(DefaultAllocator(), u8"second");
    CHECK(first.Serial() != 0u);
    CHECK(second.Serial() != 0u);
    CHECK(first.Serial() != second.Serial());
    u64 earlier = 0;
    {
        Scene gone(DefaultAllocator());
        earlier = gone.Serial();
    }
    Scene later(DefaultAllocator()); // may reuse the address; not the serial
    CHECK(later.Serial() != earlier);
    CHECK(later.Serial() > second.Serial());
}

TEST_CASE("entity create: unique valid handles + count")
{
    Scene scene(DefaultAllocator(), u8"world");
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
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
    CHECK_FALSE(scene.IsValid(EntityHandle::Invalid()));
    CHECK_FALSE(scene.IsValid(EntityHandle{999u, 1u})); // out of range
    CHECK(scene.GetEntityName(EntityHandle::Invalid()) == StringView{});
    scene.SetActive(EntityHandle::Invalid(), false);     // no crash
    scene.SetEntityName(EntityHandle::Invalid(), u8"x"); // no crash
}

TEST_CASE("persistent Guid <-> handle: find resolves, survives a specific-id create")
{
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
    EntityHandle e = scene.CreateEntity(u8"orig");
    scene.SetActive(e, false);
    CHECK_FALSE(scene.IsActive(e));
    scene.SetEntityName(e, u8"renamed");
    CHECK(scene.GetEntityName(e) == u8"renamed");
}

TEST_CASE("ForEachEntity visits exactly the live entities")
{
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
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
    Scene scene{DefaultAllocator()};
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
    Scene scene(DefaultAllocator(), u8"world");
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

// === effective active: own flag AND every ancestor's flag,
// cached O(1), resettled at the SetActive / reparent / creation choke points ===

TEST_CASE("effective active: deep chain - a mid-ancestor's flag darks the whole subtree")
{
    Scene scene(DefaultAllocator(), u8"eff");
    EntityHandle a = scene.CreateEntity(u8"a");
    EntityHandle b = scene.CreateEntity(u8"b");
    EntityHandle c = scene.CreateEntity(u8"c");
    EntityHandle d = scene.CreateEntity(u8"d");
    scene.SetParent(b, a);
    scene.SetParent(c, b);
    scene.SetParent(d, c);

    CHECK(scene.IsEffectivelyActive(a));
    CHECK(scene.IsEffectivelyActive(d));

    scene.SetActive(b, false); // the MIDDLE of the chain
    CHECK(scene.IsEffectivelyActive(a));
    CHECK_FALSE(scene.IsEffectivelyActive(b));
    CHECK_FALSE(scene.IsEffectivelyActive(c));
    CHECK_FALSE(scene.IsEffectivelyActive(d));
    // Own flags below the toggle are untouched.
    CHECK(scene.IsActive(c));
    CHECK(scene.IsActive(d));

    scene.SetActive(b, true); // exactly the previously-active descendants come back
    CHECK(scene.IsEffectivelyActive(c));
    CHECK(scene.IsEffectivelyActive(d));
}

TEST_CASE("effective active: child's own flag survives a parent toggle (restore matrix)")
{
    Scene scene(DefaultAllocator(), u8"eff2");
    EntityHandle parent = scene.CreateEntity(u8"p");
    EntityHandle onChild = scene.CreateEntity(u8"on");
    EntityHandle offChild = scene.CreateEntity(u8"off");
    scene.SetParent(onChild, parent);
    scene.SetParent(offChild, parent);
    scene.SetActive(offChild, false);

    scene.SetActive(parent, false);
    CHECK_FALSE(scene.IsEffectivelyActive(onChild));
    CHECK_FALSE(scene.IsEffectivelyActive(offChild));

    scene.SetActive(parent, true);
    CHECK(scene.IsEffectivelyActive(onChild));       // was on -> back on
    CHECK_FALSE(scene.IsEffectivelyActive(offChild)); // own flag off stays off
    CHECK_FALSE(scene.IsActive(offChild));
}

TEST_CASE("effective active: reparent under an inactive parent and back out")
{
    Scene scene(DefaultAllocator(), u8"eff3");
    EntityHandle deadHost = scene.CreateEntity(u8"host");
    EntityHandle mover = scene.CreateEntity(u8"mover");
    EntityHandle moverChild = scene.CreateEntity(u8"mc");
    scene.SetParent(moverChild, mover);
    scene.SetActive(deadHost, false);

    scene.SetParent(mover, deadHost); // into the dark subtree
    CHECK_FALSE(scene.IsEffectivelyActive(mover));
    CHECK_FALSE(scene.IsEffectivelyActive(moverChild));
    CHECK(scene.IsActive(mover)); // own flag untouched by the move

    scene.SetParent(mover, EntityHandle::Invalid()); // back to root
    CHECK(scene.IsEffectivelyActive(mover));
    CHECK(scene.IsEffectivelyActive(moverChild));
}

TEST_CASE("effective active: MoveBefore across parents resettles the subtree")
{
    Scene scene(DefaultAllocator(), u8"eff4");
    EntityHandle activeParent = scene.CreateEntity(u8"ap");
    EntityHandle inactiveParent = scene.CreateEntity(u8"ip");
    EntityHandle anchor = scene.CreateEntity(u8"anchor");
    EntityHandle mover = scene.CreateEntity(u8"mover");
    scene.SetParent(anchor, inactiveParent);
    scene.SetParent(mover, activeParent);
    scene.SetActive(inactiveParent, false);

    scene.MoveBefore(mover, anchor); // splice into the INACTIVE parent's child list
    CHECK(scene.GetParent(mover) == inactiveParent);
    CHECK_FALSE(scene.IsEffectivelyActive(mover));
}

TEST_CASE("effective active: created under nothing = active; invalid handle answers false")
{
    Scene scene(DefaultAllocator(), u8"eff5");
    EntityHandle e = scene.CreateEntity(u8"e");
    CHECK(scene.IsEffectivelyActive(e));
    CHECK_FALSE(scene.IsEffectivelyActive(EntityHandle::Invalid()));
    scene.DestroyEntity(e);
    CHECK_FALSE(scene.IsEffectivelyActive(e));
}
