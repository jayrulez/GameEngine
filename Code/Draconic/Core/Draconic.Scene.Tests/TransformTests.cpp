// Phase 2 - the transform hierarchy: parent/child/sibling links, world-matrix
// composition, the dirty-flag cascade + two-pass UpdateTransforms, motion-vector
// previous-matrix snapshotting, recursive destroy, and the reparent cycle guard.
// Encodes the transform-hierarchy behaviors pinned from the Sedulous test suite.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;

using namespace draconic::foundation;
using namespace draconic::scene;

namespace
{
    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-4f; }
}

TEST_CASE("parenting links the sibling list + child queries")
{
    Scene scene;
    EntityHandle parent = scene.CreateEntity(u8"p");
    EntityHandle a = scene.CreateEntity(u8"a");
    EntityHandle b = scene.CreateEntity(u8"b");

    CHECK(scene.GetParent(a) == EntityHandle::Invalid()); // roots
    scene.SetParent(a, parent);
    scene.SetParent(b, parent);

    CHECK(scene.GetParent(a) == parent);
    CHECK(scene.GetParent(b) == parent);
    CHECK(scene.GetChildCount(parent) == 2);
    CHECK(scene.GetFirstChild(parent) == a);
    CHECK(scene.GetNextSibling(a) == b);
    CHECK(scene.GetNextSibling(b) == EntityHandle::Invalid());

    // unparent b back to root
    scene.SetParent(b, EntityHandle::Invalid());
    CHECK(scene.GetParent(b) == EntityHandle::Invalid());
    CHECK(scene.GetChildCount(parent) == 1);
    CHECK(scene.GetNextSibling(a) == EntityHandle::Invalid());
}

TEST_CASE("world matrix composes local with parent (after UpdateTransforms)")
{
    Scene scene;
    EntityHandle parent = scene.CreateEntity();
    EntityHandle child = scene.CreateEntity();
    scene.SetLocalPosition(parent, Float3{10, 0, 0});
    scene.SetLocalPosition(child, Float3{5, 0, 0});
    scene.SetParent(child, parent);

    scene.UpdateTransforms();

    const Float3 pw = scene.GetWorldPosition(parent);
    const Float3 cw = scene.GetWorldPosition(child);
    CHECK(Near(pw.x, 10.0f));
    CHECK(Near(cw.x, 15.0f)); // child = parent(10) + local(5)
    CHECK(scene.IsTransformUpdatedThisFrame(child));
    CHECK(scene.TransformsUpdatedThisFrame().Size() == 2);
}

TEST_CASE("dirty cascade: moving a parent recomputes its descendants; nothing else")
{
    Scene scene;
    EntityHandle parent = scene.CreateEntity();
    EntityHandle child = scene.CreateEntity();
    EntityHandle other = scene.CreateEntity();
    scene.SetParent(child, parent);
    scene.UpdateTransforms(); // settle everything

    // a frame where nothing moved -> no recomputes
    scene.UpdateTransforms();
    CHECK(scene.TransformsUpdatedThisFrame().Size() == 0);

    // move the parent: parent + child recompute, `other` does not
    scene.SetLocalPosition(parent, Float3{0, 7, 0});
    scene.UpdateTransforms();
    CHECK(scene.IsTransformUpdatedThisFrame(parent));
    CHECK(scene.IsTransformUpdatedThisFrame(child));
    CHECK_FALSE(scene.IsTransformUpdatedThisFrame(other));
    CHECK(Near(scene.GetWorldPosition(child).y, 7.0f));
}

TEST_CASE("motion vectors: previous world matrix snapshots the prior frame")
{
    Scene scene;
    EntityHandle e = scene.CreateEntity();
    scene.SetLocalPosition(e, Float3{1, 0, 0});
    scene.UpdateTransforms();
    CHECK(Near(scene.GetWorldPosition(e).x, 1.0f));

    scene.SetLocalPosition(e, Float3{4, 0, 0});
    scene.UpdateTransforms();
    CHECK(Near(scene.GetWorldPosition(e).x, 4.0f));         // current
    CHECK(Near(scene.GetPrevWorldMatrix(e).m[3][0], 1.0f)); // previous frame's position

    // a frame where it stops moving: prev should catch up to current (the "just stopped" snapshot)
    scene.UpdateTransforms();
    CHECK(Near(scene.GetPrevWorldMatrix(e).m[3][0], 4.0f));
}

TEST_CASE("destroy is recursive: destroying a parent destroys its whole subtree")
{
    Scene scene;
    EntityHandle parent = scene.CreateEntity();
    EntityHandle child = scene.CreateEntity();
    EntityHandle grandchild = scene.CreateEntity();
    EntityHandle bystander = scene.CreateEntity();
    scene.SetParent(child, parent);
    scene.SetParent(grandchild, child);
    CHECK(scene.EntityCount() == 4);

    scene.DestroyEntity(parent);
    CHECK_FALSE(scene.IsValid(parent));
    CHECK_FALSE(scene.IsValid(child));
    CHECK_FALSE(scene.IsValid(grandchild));
    CHECK(scene.IsValid(bystander));
    CHECK(scene.EntityCount() == 1);
}

TEST_CASE("reparent cycle guard: cannot parent an entity under its own descendant")
{
    Scene scene;
    EntityHandle a = scene.CreateEntity();
    EntityHandle b = scene.CreateEntity();
    EntityHandle c = scene.CreateEntity();
    scene.SetParent(b, a); // a > b
    scene.SetParent(c, b); // a > b > c

    scene.SetParent(a, c);                                // would form a cycle a>b>c>a -> rejected
    CHECK(scene.GetParent(a) == EntityHandle::Invalid()); // a stays a root
    CHECK(scene.GetParent(c) == b);                       // tree intact

    // a normal reparent still works (and UpdateTransforms doesn't loop forever)
    scene.SetParent(a, EntityHandle::Invalid());
    scene.UpdateTransforms();
    CHECK(scene.GetChildCount(a) == 1);
}

TEST_CASE("destroying a child unlinks it from the parent's sibling list")
{
    Scene scene;
    EntityHandle parent = scene.CreateEntity();
    EntityHandle a = scene.CreateEntity();
    EntityHandle b = scene.CreateEntity();
    EntityHandle c = scene.CreateEntity();
    scene.SetParent(a, parent);
    scene.SetParent(b, parent);
    scene.SetParent(c, parent);
    CHECK(scene.GetChildCount(parent) == 3);

    scene.DestroyEntity(b); // remove the middle child
    CHECK(scene.GetChildCount(parent) == 2);
    CHECK(scene.GetFirstChild(parent) == a);
    CHECK(scene.GetNextSibling(a) == c); // list spliced: a -> c
}

TEST_CASE("keep-world reparent: the entity stays put in the world")
{
    Scene scene;
    EntityHandle parentA = scene.CreateEntity(u8"A");
    EntityHandle parentB = scene.CreateEntity(u8"B");
    EntityHandle child = scene.CreateEntity(u8"child");

    Transform ta;
    ta.position = Float3{10.0f, 0.0f, 0.0f};
    ta.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.7f);
    scene.SetLocalTransform(parentA, ta);
    Transform tb;
    tb.position = Float3{-5.0f, 2.0f, 1.0f};
    tb.scale = Float3{2.0f, 2.0f, 2.0f};
    scene.SetLocalTransform(parentB, tb);
    Transform tc;
    tc.position = Float3{1.0f, 2.0f, 3.0f};
    scene.SetLocalTransform(child, tc);
    scene.SetParent(child, parentA);

    const Float4x4 before = scene.ComposeWorldMatrix(child);

    // keep-world reparent A -> B: world matrix unchanged (local recomputed).
    scene.SetParent(child, parentB, /*keepWorldTransform*/ true);
    CHECK(scene.GetParent(child) == parentB);
    const Float4x4 after = scene.ComposeWorldMatrix(child);
    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            CHECK(after.m[r][c] == doctest::Approx(before.m[r][c]).epsilon(0.001f));

    // ...and the LOCAL transform did change (relative to a different parent now).
    CHECK(scene.GetLocalTransform(child).position.x != doctest::Approx(1.0f).epsilon(0.0001f));

    // keep-world to root: world unchanged, local == world.
    scene.SetParent(child, EntityHandle::Invalid(), true);
    const Float4x4 asRoot = scene.ComposeWorldMatrix(child);
    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            CHECK(asRoot.m[r][c] == doctest::Approx(before.m[r][c]).epsilon(0.001f));

    // Refused move (cycle) leaves the local untouched.
    scene.SetParent(parentB, parentB, true);
    CHECK(scene.GetParent(parentB) == EntityHandle::Invalid());
}

// Regression (user-reported): a child REPARENTED under a clean (non-dirty) parent kept its
// stale world matrix until something moved the parent - pasted/duplicated child entities
// rendered at the origin until the scene reloaded. UpdateTransforms must recurse from every
// dirty-subtree TOP (dirty node with a clean parent), not only from dirty roots.
TEST_CASE("transforms: reparent under a clean parent recomputes the child's world matrix")
{
    Scene scene;
    EntityHandle parent = scene.CreateEntity(u8"parent");
    scene.SetLocalPosition(parent, Float3{10, 0, 0});
    scene.UpdateTransforms(); // parent world settled + CLEAN

    // The paste/duplicate sequence: create at root, set the authored LOCAL, then parent.
    EntityHandle child = scene.CreateEntity(u8"child");
    scene.SetLocalPosition(child, Float3{0, 5, 0});
    scene.SetParent(child, parent);
    scene.UpdateTransforms();

    const Float4x4 world = scene.GetWorldMatrix(child);
    CHECK(world.m[3][0] == doctest::Approx(10.0f)); // parent's offset composed in
    CHECK(world.m[3][1] == doctest::Approx(5.0f));  // the authored local, relative to it
}
