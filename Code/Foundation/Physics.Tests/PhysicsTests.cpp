// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.physics core tests: headless Jolt world - "determinism-enough" simulation
// (spawn/step/assert poses), the layer matrix, compound building, queries, kinematic
// motion, and contact/trigger buffering.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

import foundation.core;
import foundation.physics;

using namespace foundation::core;
using namespace foundation::physics;

namespace
{
    [[nodiscard]] BodyDesc FloorDesc()
    {
        BodyDesc floor;
        floor.motion = MotionKind::Static;
        floor.layer = PhysicsLayer::Static;
        ShapeDesc slab;
        slab.kind = ShapeKind::Box;
        slab.halfExtents = Float3{50.0f, 0.5f, 50.0f};
        floor.shapes.PushBack(slab);
        floor.position = Float3{0.0f, -0.5f, 0.0f};
        return floor;
    }

    [[nodiscard]] BodyDesc BoxAt(f32 y, MotionKind motion = MotionKind::Dynamic)
    {
        BodyDesc box;
        box.motion = motion;
        box.layer = motion == MotionKind::Static ? PhysicsLayer::Static : PhysicsLayer::Dynamic;
        ShapeDesc cube;
        cube.halfExtents = Float3{0.5f, 0.5f, 0.5f};
        box.shapes.PushBack(cube);
        box.position = Float3{0.0f, y, 0.0f};
        return box;
    }
}

TEST_CASE("physics: a dynamic box falls under gravity and comes to rest on the floor")
{
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());
    BodyDesc drop = BoxAt(5.0f);
    drop.userData = 42;
    const BodyId box = world.CreateBody(drop);
    REQUIRE(box.IsValid());
    CHECK(world.UserData(box) == 42u);

    for (int i = 0; i < 240; ++i)
    {
        world.Step(1.0f / 60.0f);
    } // 4 seconds

    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(box, position, rotation);
    CHECK(position.y == doctest::Approx(0.5f).epsilon(0.05)); // resting: half extent above floor
    CHECK(std::fabs(position.x) < 0.01f);
    CHECK(world.LinearVelocity(box).y == doctest::Approx(0.0f).epsilon(0.05));
}

TEST_CASE("physics: a heightfield collider - a sphere rests on it, off-footprint falls through")
{
    PhysicsWorld world(DefaultAllocator());

    // A flat 65x65 heightfield at world Y = 2, 64x64 footprint centred on origin, static.
    constexpr u32 n = 65;
    Array<f32> samples;
    samples.Resize(static_cast<usize>(n) * n, 2.0f);
    BodyDesc ground;
    ground.motion = MotionKind::Static;
    ground.layer = PhysicsLayer::Static;
    ShapeDesc hf;
    hf.kind = ShapeKind::Heightfield;
    hf.heightSamples = Span<const f32>(samples.Data(), samples.Size());
    hf.heightSampleCount = n;
    hf.heightWorldSize = Float2{64.0f, 64.0f};
    ground.shapes.PushBack(hf);
    REQUIRE(world.CreateBody(ground).IsValid());

    // A sphere dropped over the centre rests on the surface (2 + radius).
    BodyDesc drop;
    ShapeDesc sphere;
    sphere.kind = ShapeKind::Sphere;
    sphere.radius = 0.5f;
    drop.shapes.PushBack(sphere);
    drop.position = Float3{0.0f, 10.0f, 0.0f};
    const BodyId ball = world.CreateBody(drop);
    REQUIRE(ball.IsValid());

    // A sphere far OUTSIDE the footprint (x = 100) meets no ground and falls through - pins Jolt's
    // no-collision padding: collision never widens past the authored extent.
    BodyDesc off;
    ShapeDesc sphere2;
    sphere2.kind = ShapeKind::Sphere;
    sphere2.radius = 0.5f;
    off.shapes.PushBack(sphere2);
    off.position = Float3{100.0f, 10.0f, 0.0f};
    const BodyId offBall = world.CreateBody(off);
    REQUIRE(offBall.IsValid());

    for (int i = 0; i < 240; ++i)
    {
        world.Step(1.0f / 60.0f);
    }

    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(ball, position, rotation);
    CHECK(position.y == doctest::Approx(2.5f).epsilon(0.1)); // rests on the heightfield surface
    CHECK(std::fabs(position.x) < 0.1f);

    world.GetBodyTransform(offBall, position, rotation);
    CHECK(position.y < 0.0f); // fell past the surface level: no collision off-footprint
}

TEST_CASE("physics: a heightfield hole - a sphere over the cut falls through, one beside it rests")
{
    // The terrain holes rule (Specs/terrain-holes.md): a sample at kNoCollisionHeight removes
    // every triangle touching it, exactly as the renderer removes them.
    PhysicsWorld world(DefaultAllocator());
    const u32 n = 65;
    Array<f32> samples;
    samples.Resize(static_cast<usize>(n) * n, 2.0f);
    for (u32 z = 30; z <= 34; ++z)
    {
        for (u32 x = 30; x <= 34; ++x)
        {
            samples[static_cast<usize>(z) * n + x] = ShapeDesc::kNoCollisionHeight; // a 5x5 cut at the centre
        }
    }
    BodyDesc ground;
    ground.motion = MotionKind::Static;
    ground.layer = PhysicsLayer::Static;
    ShapeDesc hf;
    hf.kind = ShapeKind::Heightfield;
    hf.heightSamples = Span<const f32>(samples.Data(), samples.Size());
    hf.heightSampleCount = n;
    hf.heightWorldSize = Float2{64.0f, 64.0f};
    ground.shapes.PushBack(hf);
    REQUIRE(world.CreateBody(ground).IsValid());

    const auto drop = [&](Float3 at)
    {
        BodyDesc body;
        ShapeDesc sphere;
        sphere.kind = ShapeKind::Sphere;
        sphere.radius = 0.5f;
        body.shapes.PushBack(sphere);
        body.position = at;
        return world.CreateBody(body);
    };
    const BodyId through = drop(Float3{0.0f, 10.0f, 0.0f});   // over the cut (sample 32,32)
    const BodyId beside = drop(Float3{20.0f, 10.0f, 0.0f});   // solid ground
    REQUIRE(through.IsValid());
    REQUIRE(beside.IsValid());
    for (int i = 0; i < 240; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(beside, position, rotation);
    CHECK(position.y == doctest::Approx(2.5f).epsilon(0.1)); // rests on the surface
    world.GetBodyTransform(through, position, rotation);
    CHECK(position.y < 0.0f); // fell through the hole
}

TEST_CASE("physics: static-static never pairs; dynamic collides with static")
{
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());
    // A static box INSIDE the floor: no contact events from the overlapping statics.
    BodyDesc buried = BoxAt(0.0f, MotionKind::Static);
    (void)world.CreateBody(buried);
    world.Step(1.0f / 60.0f);
    Array<ContactEvent> events;
    world.DrainContacts(events);
    CHECK(events.IsEmpty());

    // A dynamic box dropped from just above: contact Begin against the floor arrives.
    const BodyId box = world.CreateBody(BoxAt(1.2f));
    for (int i = 0; i < 60; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    events.Clear();
    world.DrainContacts(events);
    bool sawBegin = false;
    for (const ContactEvent& e : events)
    {
        if (e.kind == ContactKind::Begin)
        {
            sawBegin = true;
        }
    }
    CHECK(sawBegin);
    (void)box;
}

TEST_CASE("physics: triggers sense without colliding")
{
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());

    BodyDesc sensor;
    sensor.motion = MotionKind::Static; // static sensors don't pair with statics...
    sensor.isTrigger = true;
    ShapeDesc volume;
    volume.halfExtents = Float3{1.0f, 1.0f, 1.0f};
    sensor.shapes.PushBack(volume);
    sensor.position = Float3{0.0f, 2.0f, 0.0f};
    sensor.userData = 7;
    // ...so make it kinematic (a trigger VOLUME that can also move).
    sensor.motion = MotionKind::Kinematic;
    const BodyId trigger = world.CreateBody(sensor);
    REQUIRE(trigger.IsValid());

    BodyDesc drop = BoxAt(5.0f);
    drop.userData = 42;
    const BodyId box = world.CreateBody(drop);

    bool entered = false;
    Float3 position;
    Quaternion rotation;
    for (int i = 0; i < 240; ++i)
    {
        world.Step(1.0f / 60.0f);
        Array<ContactEvent> events;
        world.DrainContacts(events);
        for (const ContactEvent& e : events)
        {
            if (e.kind == ContactKind::TriggerEnter && (e.userA == 7u || e.userB == 7u) &&
                (e.userA == 42u || e.userB == 42u))
            {
                entered = true;
            }
        }
    }
    CHECK(entered);
    // The sensor produced no collision RESPONSE: the box fell straight through to the floor.
    world.GetBodyTransform(box, position, rotation);
    CHECK(position.y == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("physics: compound bodies build and simulate")
{
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());

    // A dumbbell: two spheres offset on x - lands and rests HIGHER than a bare sphere
    // would if it were a point, proving the compound extent matters.
    BodyDesc dumbbell;
    ShapeDesc left;
    left.kind = ShapeKind::Sphere;
    left.radius = 0.5f;
    left.localPosition = Float3{-1.0f, 0.0f, 0.0f};
    ShapeDesc right = left;
    right.localPosition = Float3{1.0f, 0.0f, 0.0f};
    dumbbell.shapes.PushBack(left);
    dumbbell.shapes.PushBack(right);
    dumbbell.position = Float3{0.0f, 4.0f, 0.0f};
    const BodyId body = world.CreateBody(dumbbell);
    REQUIRE(body.IsValid());

    for (int i = 0; i < 240; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    CHECK(position.y == doctest::Approx(0.5f).epsilon(0.1)); // resting on the sphere radius
}

TEST_CASE("physics: ray casts hit the nearest body with user data + normal")
{
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());
    BodyDesc target = BoxAt(0.5f, MotionKind::Static);
    target.userData = 99;
    (void)world.CreateBody(target);

    RayHit hit;
    REQUIRE(world.RayCast(Float3{0.0f, 10.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.userData == 99u);                                   // the box, not the floor
    CHECK(hit.position.y == doctest::Approx(1.0f).epsilon(0.02)); // its top face
    CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.02));   // pointing up

    // A miss stays a miss.
    RayHit miss;
    CHECK_FALSE(world.RayCast(Float3{500.0f, 10.0f, 0.0f}, Float3{0.0f, 1.0f, 0.0f}, 10.0f, miss));
}

TEST_CASE("physics: kinematic bodies follow MoveKinematic with velocity")
{
    PhysicsWorld world(DefaultAllocator());
    BodyDesc platform = BoxAt(0.0f, MotionKind::Kinematic);
    platform.layer = PhysicsLayer::Kinematic;
    const BodyId body = world.CreateBody(platform);

    // March it +x at 1 unit per step-second.
    Float3 position{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60; ++i)
    {
        position.x += 1.0f / 60.0f;
        world.MoveKinematic(body, position, Quaternion::Identity, 1.0f / 60.0f);
        world.Step(1.0f / 60.0f);
    }
    Float3 result;
    Quaternion rotation;
    world.GetBodyTransform(body, result, rotation);
    CHECK(result.x == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(world.LinearVelocity(body).x == doctest::Approx(1.0f).epsilon(0.1));
}

TEST_CASE("physics: point query finds containing bodies")
{
    PhysicsWorld world(DefaultAllocator());
    BodyDesc box = BoxAt(0.0f, MotionKind::Static);
    const BodyId body = world.CreateBody(box);
    Array<BodyId> hits;
    world.QueryPoint(Float3{0.0f, 0.0f, 0.0f}, hits);
    REQUIRE(hits.Size() == 1);
    CHECK(hits[0] == body);
    // FILL semantics: the reused array is cleared by the query, never appended to.
    world.QueryPoint(Float3{10.0f, 0.0f, 0.0f}, hits);
    CHECK(hits.IsEmpty());
    world.QueryPoint(Float3{0.0f, 0.0f, 0.0f}, hits);
    world.QueryPoint(Float3{0.0f, 0.0f, 0.0f}, hits);
    CHECK(hits.Size() == 1);
}

TEST_CASE("physics: a lone shape keeps its local rotation (built as a one-child compound)")
{
    PhysicsWorld world(DefaultAllocator());

    // A 2x0.2x0.2 STATIC bar stood on end by a 90-degree local rotation about Z. A ray from
    // above must meet its LONG half extent (top at y = 1.0); dropping the rotation would lay
    // it flat and the ray would land on y = 0.1.
    BodyDesc bar;
    bar.motion = MotionKind::Static;
    bar.layer = PhysicsLayer::Static;
    ShapeDesc box;
    box.halfExtents = Float3{1.0f, 0.1f, 0.1f};
    box.localRotation = Quaternion::FromAxisAngle(Float3{0.0f, 0.0f, 1.0f}, 1.5707963f);
    bar.shapes.PushBack(box);
    const BodyId body = world.CreateBody(bar);
    REQUIRE(body.IsValid());

    RayHit hit;
    REQUIRE(world.RayCast(Float3{0.0f, 5.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
    CHECK(hit.body == body);
    CHECK(hit.position.y == doctest::Approx(1.0f).epsilon(0.01));
    // And nothing where the flat bar would have been.
    CHECK_FALSE(world.RayCast(Float3{0.8f, 5.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, 10.0f, hit));
}

TEST_CASE("physics: sphere overlap finds intersecting bodies (deduped) + honors the group mask")
{
    PhysicsWorld world(DefaultAllocator());
    BodyDesc a = BoxAt(0.0f, MotionKind::Static); // at the origin, group 0
    const BodyId bodyA = world.CreateBody(a);
    BodyDesc b = BoxAt(0.0f, MotionKind::Static);
    b.position = Float3{5.0f, 0.0f, 0.0f};
    b.group = 1;
    const BodyId bodyB = world.CreateBody(b);

    QueryShape sphere;
    sphere.kind = ShapeKind::Sphere;
    sphere.radius = 1.0f;

    Array<BodyId> hits;
    world.ShapeOverlap(sphere, Float3{0.0f, 0.0f, 0.0f}, Quaternion::Identity, hits);
    REQUIRE(hits.Size() == 1);
    CHECK(hits[0] == bodyA);

    hits.Clear();
    world.ShapeOverlap(sphere, Float3{5.0f, 0.0f, 0.0f}, Quaternion::Identity, hits);
    REQUIRE(hits.Size() == 1);
    CHECK(hits[0] == bodyB);

    // The gap between them -> nothing.
    hits.Clear();
    world.ShapeOverlap(sphere, Float3{2.5f, 0.0f, 0.0f}, Quaternion::Identity, hits);
    CHECK(hits.IsEmpty());

    // A big sphere spanning both -> both, EACH ONCE (de-dup across sub-shape hits).
    QueryShape big;
    big.kind = ShapeKind::Sphere;
    big.radius = 3.0f;
    hits.Clear();
    world.ShapeOverlap(big, Float3{2.5f, 0.0f, 0.0f}, Quaternion::Identity, hits);
    CHECK(hits.Size() == 2);

    // Group mask excluding group 1 -> only A, even with the big sphere.
    hits.Clear();
    world.ShapeOverlap(big, Float3{2.5f, 0.0f, 0.0f}, Quaternion::Identity, hits, ~(1u << 1));
    REQUIRE(hits.Size() == 1);
    CHECK(hits[0] == bodyA);
}

// A box thinner than Jolt's default convex radius (0.05) builds with a smaller radius instead of
// tripping Jolt's assert: an imported model's prefab gives its body a 0.01 placeholder box beside
// the real collider, so a level of such models used to stop a debug build at play.
TEST_CASE("physics: a box thinner than the convex radius builds, alone and in a compound")
{
    PhysicsWorld world(DefaultAllocator());
    BodyDesc thin;
    thin.motion = MotionKind::Static;
    thin.layer = PhysicsLayer::Static;
    ShapeDesc sliver;
    sliver.kind = ShapeKind::Box;
    sliver.halfExtents = Float3{0.01f, 0.01f, 0.01f};
    thin.shapes.PushBack(sliver);
    CHECK(world.CreateBody(thin).IsValid());

    // The prefab's shape: the placeholder at the origin and the real (here a half-metre box)
    // beside it, compounded. A ray finds the real one.
    BodyDesc model;
    model.motion = MotionKind::Static;
    model.layer = PhysicsLayer::Static;
    model.userData = 7;
    model.position = Float3{5.0f, 0.0f, 0.0f};
    model.shapes.PushBack(sliver);
    ShapeDesc solid;
    solid.kind = ShapeKind::Box;
    solid.halfExtents = Float3{0.5f, 0.5f, 0.5f};
    solid.localPosition = Float3{0.0f, 0.5f, 0.0f};
    model.shapes.PushBack(solid);
    REQUIRE(world.CreateBody(model).IsValid());
    RayHit hit;
    REQUIRE(world.RayCast(Float3{5.0f, 10.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.userData == 7u);
    CHECK(hit.position.y == doctest::Approx(1.0f).epsilon(0.02)); // the real box's top face

    // A box with no extent is no shape: the body is refused, not built degenerate.
    BodyDesc flat = thin;
    flat.shapes[0].halfExtents = Float3{0.5f, 0.0f, 0.5f};
    CHECK_FALSE(world.CreateBody(flat).IsValid());
}

TEST_CASE("physics: a BOX query shape overlaps too (not only spheres)")
{
    PhysicsWorld world(DefaultAllocator());
    const BodyId body = world.CreateBody(BoxAt(0.0f, MotionKind::Static)); // 0.5-half box at origin
    QueryShape box;
    box.kind = ShapeKind::Box;
    box.halfExtents = Float3{0.4f, 0.4f, 0.4f};

    Array<BodyId> hits;
    world.ShapeOverlap(box, Float3{0.7f, 0.0f, 0.0f}, Quaternion::Identity, hits); // 0.3 into the box
    REQUIRE(hits.Size() == 1);
    CHECK(hits[0] == body);

    hits.Clear();
    world.ShapeOverlap(box, Float3{2.0f, 0.0f, 0.0f}, Quaternion::Identity, hits); // clear
    CHECK(hits.IsEmpty());
}

TEST_CASE("physics: shape cast sweeps a sphere onto the nearest body (earlier than a ray)")
{
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());
    BodyDesc target = BoxAt(0.5f, MotionKind::Static); // spans y in [0,1], top face at y=1.0
    target.userData = 42;
    (void)world.CreateBody(target);

    QueryShape sphere;
    sphere.kind = ShapeKind::Sphere;
    sphere.radius = 0.5f;
    RayHit hit;
    REQUIRE(world.ShapeCast(sphere, Float3{0.0f, 10.0f, 0.0f}, Quaternion::Identity,
                            Float3{0.0f, -1.0f, 0.0f}, 100.0f, hit));
    CHECK(hit.userData == 42u); // the box, not the floor below it
    // Sphere (r=0.5) touches the box top (y=1.0) when its centre reaches y=1.5 -> travelled 8.5/100.
    CHECK(hit.fraction == doctest::Approx(0.085f).epsilon(0.05));
    CHECK(hit.position.y == doctest::Approx(1.0f).epsilon(0.05)); // contact on the box top

    // The VOLUME makes it hit EARLIER than a point ray (which needs y=1.0 -> 9.0/100).
    RayHit rayHit;
    REQUIRE(world.RayCast(Float3{0.0f, 10.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f}, 100.0f, rayHit));
    CHECK(hit.fraction < rayHit.fraction);

    // A clear sweep misses.
    RayHit miss;
    CHECK_FALSE(world.ShapeCast(sphere, Float3{500.0f, 10.0f, 0.0f}, Quaternion::Identity,
                                Float3{0.0f, 1.0f, 0.0f}, 10.0f, miss));
}

// ---- cooked shapes (builder-cooked convex hulls + triangle meshes) ----

namespace
{
    // Unit-cube corner cloud (half extent 0.5) for hull cooking.
    [[nodiscard]] Array<Float3> CubeCorners(f32 half)
    {
        Array<Float3> points;
        const f32 ends[2] = {-half, half};
        for (f32 x : ends)
            for (f32 y : ends)
                for (f32 z : ends)
                    points.PushBack(Float3{x, y, z});
        return points;
    }
}

TEST_CASE("physics: cooked convex hull simulates like a box")
{
    Array<byte> blob;
    const Array<Float3> corners = CubeCorners(0.5f);
    REQUIRE(CookConvexHull(Span<const Float3>(corners.Data(), corners.Size()), blob));
    REQUIRE(!blob.IsEmpty());

    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());
    BodyDesc drop;
    drop.position = Float3{0.0f, 5.0f, 0.0f};
    ShapeDesc shape;
    shape.kind = ShapeKind::Cooked;
    shape.cooked = Span<const byte>(blob.Data(), blob.Size());
    drop.shapes.PushBack(shape);
    const BodyId body = world.CreateBody(drop);
    REQUIRE(body.IsValid());

    for (int i = 0; i < 300; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    // Rests with its half extent above the floor top (convex radius slop allowed).
    CHECK(position.y == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("physics: cooked triangle mesh carries per-face material slots to ray hits")
{
    // Two-triangle ground quad: -x triangle slot 7, +x triangle slot 3.
    const Float3 positions[] = {
        {-2.0f, 0.0f, -2.0f},
        {-2.0f, 0.0f, 2.0f},
        {2.0f, 0.0f, 2.0f},
        {2.0f, 0.0f, -2.0f},
    };
    const u32 indices[] = {0, 1, 3, 1, 2, 3}; // left tri (uses corner 0), right tri (corner 2)
    const u32 slots[] = {7, 3};
    Array<byte> blob;
    REQUIRE(CookTriangleMesh(Span<const Float3>(positions, 4), Span<const u32>(indices, 6),
                             Span<const u32>(slots, 2), blob));

    PhysicsWorld world(DefaultAllocator());
    BodyDesc ground;
    ground.motion = MotionKind::Static;
    ground.layer = PhysicsLayer::Static;
    ShapeDesc shape;
    shape.kind = ShapeKind::Cooked;
    shape.cooked = Span<const byte>(blob.Data(), blob.Size());
    ground.shapes.PushBack(shape);
    REQUIRE(world.CreateBody(ground).IsValid());

    RayHit hit;
    REQUIRE(world.RayCast(Float3{-1.5f, 1.0f, -1.5f}, Float3{0.0f, -1.0f, 0.0f}, 5.0f, hit));
    CHECK(hit.surface == 7);
    CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.01));
    REQUIRE(world.RayCast(Float3{1.5f, 1.0f, 1.5f}, Float3{0.0f, -1.0f, 0.0f}, 5.0f, hit));
    CHECK(hit.surface == 3);

    // A dynamic box rests ON the mesh (mesh collides, not just queries).
    BodyDesc drop = BoxAt(3.0f);
    const BodyId box = world.CreateBody(drop);
    for (int i = 0; i < 300; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(box, position, rotation);
    CHECK(position.y == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("physics: cooked shapes scale; garbage blobs fail gracefully")
{
    Array<byte> blob;
    const Array<Float3> corners = CubeCorners(0.5f);
    REQUIRE(CookConvexHull(Span<const Float3>(corners.Data(), corners.Size()), blob));

    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());
    BodyDesc drop;
    drop.position = Float3{0.0f, 5.0f, 0.0f};
    ShapeDesc shape;
    shape.kind = ShapeKind::Cooked;
    shape.cooked = Span<const byte>(blob.Data(), blob.Size());
    shape.scale = Float3{2.0f, 2.0f, 2.0f};
    drop.shapes.PushBack(shape);
    const BodyId body = world.CreateBody(drop);
    REQUIRE(body.IsValid());
    for (int i = 0; i < 300; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    CHECK(position.y == doctest::Approx(1.0f).epsilon(0.05)); // doubled half extent

    // Garbage blob -> invalid body, no crash.
    const byte garbage[] = {byte{0xde}, byte{0xad}, byte{0xbe}, byte{0xef}};
    BodyDesc bad;
    ShapeDesc badShape;
    badShape.kind = ShapeKind::Cooked;
    badShape.cooked = Span<const byte>(garbage, 4);
    bad.shapes.PushBack(badShape);
    CHECK_FALSE(world.CreateBody(bad).IsValid());

    // Debug outline geometry extracts from a good blob.
    Array<Float3> triangles;
    CHECK(ExtractShapeTriangles(Span<const byte>(blob.Data(), blob.Size()), triangles));
    CHECK(triangles.Size() % 3 == 0);
    CHECK(!triangles.IsEmpty());
    Array<Float3> none;
    CHECK_FALSE(ExtractShapeTriangles(Span<const byte>(garbage, 4), none));
}

TEST_CASE("physics: an infinite plane catches bodies anywhere within its half extent")
{
    PhysicsWorld world(DefaultAllocator());
    BodyDesc ground;
    ground.motion = MotionKind::Static;
    ground.layer = PhysicsLayer::Static;
    ShapeDesc plane;
    plane.kind = ShapeKind::Plane; // +Y normal through origin
    ground.shapes.PushBack(plane);
    REQUIRE(world.CreateBody(ground).IsValid());

    // Far outside any box-sized floor, still well inside the plane's half extent.
    BodyDesc drop = BoxAt(5.0f);
    drop.position = Float3{800.0f, 5.0f, -650.0f};
    const BodyId box = world.CreateBody(drop);
    for (int i = 0; i < 300; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(box, position, rotation);
    CHECK(position.y == doctest::Approx(0.5f).epsilon(0.05));

    // Rays see it too, with an up normal.
    RayHit hit;
    REQUIRE(world.RayCast(Float3{-300.0f, 2.0f, 40.0f}, Float3{0.0f, -1.0f, 0.0f}, 5.0f, hit));
    CHECK(hit.normal.y == doctest::Approx(1.0f).epsilon(0.01));
}

// ---- joints ----

TEST_CASE("physics: a fixed joint to the world holds a body against gravity")
{
    PhysicsWorld world(DefaultAllocator());
    BodyDesc drop = BoxAt(3.0f);
    const BodyId body = world.CreateBody(drop);
    JointDesc joint;
    joint.kind = JointKind::Fixed;
    joint.bodyA = body;
    const JointId id = world.CreateJoint(joint);
    REQUIRE(id.IsValid());

    for (int i = 0; i < 120; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    CHECK(position.y == doctest::Approx(3.0f).epsilon(0.01));

    // Released, it falls.
    world.DestroyJoint(id);
    for (int i = 0; i < 60; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    world.GetBodyTransform(body, position, rotation);
    CHECK(position.y < 2.0f);
}

TEST_CASE("physics: a motorized hinge spins its body at the target velocity")
{
    PhysicsWorld world(DefaultAllocator());
    world.SetGravity(Float3{0.0f, 0.0f, 0.0f});
    BodyDesc blade = BoxAt(2.0f);
    blade.shapes[0].halfExtents = Float3{1.5f, 0.1f, 0.1f};
    const BodyId body = world.CreateBody(blade);

    JointDesc joint;
    joint.kind = JointKind::Hinge;
    joint.bodyA = body;
    joint.anchor = Float3{0.0f, 2.0f, 0.0f};
    joint.axis = Float3{0.0f, 1.0f, 0.0f};
    joint.motorEnabled = true;
    joint.motorTargetVelocity = 2.0f; // rad/s
    joint.motorLimit = 1.0e6f;
    const JointId id = world.CreateJoint(joint);
    REQUIRE(id.IsValid());

    for (int i = 0; i < 120; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    // After spin-up the blade should have rotated well away from identity but stayed put.
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    CHECK(position.y == doctest::Approx(2.0f).epsilon(0.01));
    const f32 identityDot = rotation.w > 0 ? rotation.w : -rotation.w;
    CHECK(identityDot < 0.99f); // meaningfully rotated

    // Motor off: it coasts (no snap-back); motor reversed via SetJointMotor spins back.
    world.SetJointMotor(id, true, -2.0f);
    for (int i = 0; i < 10; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    CHECK(true); // exercised the runtime motor path without asserts
}

TEST_CASE("physics: a live motor sync survives its connected body being destroyed first")
{
    // The subsystem calls SetJointMotor EVERY fixed step for every joint component; a body
    // destroyed ahead of its joint (entity-active reconcile) must not be dereferenced through
    // the constraint's stale Body pointers (heap-use-after-free under ASAN).
    PhysicsWorld world(DefaultAllocator());
    world.SetGravity(Float3{0.0f, 0.0f, 0.0f});
    const BodyId a = world.CreateBody(BoxAt(2.0f));
    BodyDesc other = BoxAt(2.0f);
    other.position = Float3{3.0f, 2.0f, 0.0f};
    const BodyId b = world.CreateBody(other);

    JointDesc joint;
    joint.kind = JointKind::Hinge;
    joint.bodyA = a;
    joint.bodyB = b;
    joint.anchor = Float3{1.5f, 2.0f, 0.0f};
    joint.axis = Float3{0.0f, 1.0f, 0.0f};
    const JointId id = world.CreateJoint(joint);
    REQUIRE(id.IsValid());
    world.Step(1.0f / 60.0f);

    world.DestroyBody(b);
    world.SetJointMotor(id, true, 2.0f); // would read the corpse via GetBody2()
    world.SetJointMotor(id, false, 0.0f);
    world.DestroyJoint(id);
    world.Step(1.0f / 60.0f);
    CHECK(world.BodyCount() == 1u);
}

TEST_CASE("physics: a distance joint to a world anchor makes a pendulum rope")
{
    PhysicsWorld world(DefaultAllocator());
    BodyDesc bob;
    ShapeDesc bobShape;
    bobShape.kind = ShapeKind::Sphere;
    bobShape.radius = 0.25f;
    bob.shapes.PushBack(bobShape);
    bob.position = Float3{0.0f, 3.0f, 0.0f};
    const BodyId body = world.CreateBody(bob);

    JointDesc joint;
    joint.kind = JointKind::Distance;
    joint.bodyA = body;
    joint.anchor = Float3{0.0f, 5.0f, 0.0f};
    joint.minDistance = 0.0f;
    joint.maxDistance = 2.0f;
    REQUIRE(world.CreateJoint(joint).IsValid());

    for (int i = 0; i < 300; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    // Hangs on the rope: 2 below the anchor, not on the (absent) floor.
    CHECK(position.y == doctest::Approx(3.0f).epsilon(0.03));
}

TEST_CASE("physics: a slider joint constrains travel to its axis and limits")
{
    PhysicsWorld world(DefaultAllocator());
    world.SetGravity(Float3{0.0f, 0.0f, 0.0f});
    BodyDesc cart = BoxAt(1.0f);
    const BodyId body = world.CreateBody(cart);

    JointDesc joint;
    joint.kind = JointKind::Slider;
    joint.bodyA = body;
    joint.axis = Float3{1.0f, 0.0f, 0.0f};
    joint.limitMin = -1.5f;
    joint.limitMax = 1.5f;
    REQUIRE(world.CreateJoint(joint).IsValid());

    world.AddImpulse(body, Float3{4000.0f, 3000.0f, 3000.0f}); // shove in all axes
    for (int i = 0; i < 180; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    CHECK(position.x <= 1.55f);                               // clamped by the limit
    CHECK(position.y == doctest::Approx(1.0f).epsilon(0.01)); // off-axis locked
    CHECK(position.z == doctest::Approx(0.0f).epsilon(0.01).scale(1.0));
}

// ---- character controller ----

TEST_CASE("physics: the character walks, climbs steps, and pushes light bodies")
{
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());

    // A 0.3-high ledge ahead (within stepUp 0.4), running x in [2, 10].
    BodyDesc ledge;
    ledge.motion = MotionKind::Static;
    ledge.layer = PhysicsLayer::Static;
    ShapeDesc ledgeShape;
    ledgeShape.halfExtents = Float3{4.0f, 0.15f, 2.0f};
    ledge.shapes.PushBack(ledgeShape);
    ledge.position = Float3{6.0f, 0.15f, 0.0f};
    REQUIRE(world.CreateBody(ledge).IsValid());

    // A light crate ON the ledge: its face is 0.5 above the ledge - too tall to stair
    // over (stepUp 0.4), so the walking character must PUSH it.
    BodyDesc crate = BoxAt(0.8f);
    crate.shapes[0].halfExtents = Float3{0.25f, 0.25f, 0.25f};
    crate.position = Float3{5.2f, 0.56f, 0.0f};
    crate.density = 100.0f;
    const BodyId box = world.CreateBody(crate);

    CharacterDesc desc;
    desc.position = Float3{0.0f, 0.9f, 0.0f}; // capsule CENTER (feet at 0)
    desc.maxStrength = 800.0f;                // default 100N barely beats crate friction
    const CharacterId character = world.CreateCharacter(desc);
    REQUIRE(character.IsValid());

    // Settle, then confirm grounded on the floor.
    for (int i = 0; i < 30; ++i)
    {
        world.SetCharacterVelocity(character, Float3{0.0f, -1.0f, 0.0f});
        world.Step(1.0f / 60.0f);
        world.UpdateCharacter(character, 1.0f / 60.0f);
    }
    CHECK(world.GetCharacterGround(character) == CharacterGround::OnGround);
    CHECK(world.CharacterPosition(character).y == doctest::Approx(0.9f).epsilon(0.02));

    // Walk +x for 1.2s: climbs onto the ledge and keeps walking (center now at 1.2).
    for (int i = 0; i < 72; ++i)
    {
        world.SetCharacterVelocity(character, Float3{3.0f, 0.0f, 0.0f});
        world.Step(1.0f / 60.0f);
        world.UpdateCharacter(character, 1.0f / 60.0f);
    }
    Float3 position = world.CharacterPosition(character);
    CHECK(position.x == doctest::Approx(3.6f).epsilon(0.15));
    CHECK(position.y == doctest::Approx(1.2f).epsilon(0.03)); // ON the ledge
    CHECK(world.GetCharacterGround(character) == CharacterGround::OnGround);

    // Keep walking into the crate: it gets SHOVED forward, not climbed.
    for (int i = 0; i < 90; ++i)
    {
        world.SetCharacterVelocity(character, Float3{3.0f, 0.0f, 0.0f});
        world.Step(1.0f / 60.0f);
        world.UpdateCharacter(character, 1.0f / 60.0f);
    }
    Float3 cratePosition;
    Quaternion crateRotation;
    world.GetBodyTransform(box, cratePosition, crateRotation);
    CHECK(cratePosition.x > 5.5f);
    position = world.CharacterPosition(character);
    CHECK(position.y == doctest::Approx(1.2f).epsilon(0.05)); // still walking the ledge
}

TEST_CASE("physics: a character reads the normal of the slope it stands on, and up in the air")
{
    PhysicsWorld world(DefaultAllocator());

    // A slab tilted 20 degrees about Z: it rises toward +x, so its top faces (-sin 20, cos 20, 0).
    const f32 tilt = 20.0f * 3.14159265f / 180.0f;
    BodyDesc slope;
    slope.motion = MotionKind::Static;
    slope.layer = PhysicsLayer::Static;
    ShapeDesc slab;
    slab.kind = ShapeKind::Box;
    slab.halfExtents = Float3{20.0f, 0.5f, 5.0f};
    slope.shapes.PushBack(slab);
    slope.rotation = Quaternion::FromAxisAngle(Float3{0.0f, 0.0f, 1.0f}, tilt);
    REQUIRE(world.CreateBody(slope).IsValid());

    CharacterDesc desc;
    desc.position = Float3{0.0f, 2.5f, 0.0f};
    const CharacterId character = world.CreateCharacter(desc);
    REQUIRE(character.IsValid());

    // Falling: in the air, the normal is straight up.
    world.SetCharacterVelocity(character, Float3{0.0f, -1.0f, 0.0f});
    world.Step(1.0f / 60.0f);
    world.UpdateCharacter(character, 1.0f / 60.0f);
    CHECK(world.GetCharacterGround(character) == CharacterGround::InAir);
    CHECK(world.CharacterGroundNormal(character).y == doctest::Approx(1.0f));

    // Landed: the slope's own normal (20 degrees is well under the default 50 degree limit).
    for (int i = 0; i < 90; ++i)
    {
        world.SetCharacterVelocity(character, Float3{0.0f, -3.0f, 0.0f});
        world.Step(1.0f / 60.0f);
        world.UpdateCharacter(character, 1.0f / 60.0f);
    }
    REQUIRE(world.GetCharacterGround(character) == CharacterGround::OnGround);
    const Float3 normal = world.CharacterGroundNormal(character);
    CHECK(normal.x == doctest::Approx(-std::sin(tilt)).epsilon(0.02));
    CHECK(normal.y == doctest::Approx(std::cos(tilt)).epsilon(0.02));
    CHECK(normal.z == doctest::Approx(0.0f).scale(1.0).epsilon(0.02));

    // An unknown character reads up rather than a zero vector.
    CHECK(world.CharacterGroundNormal(CharacterId{}).y == doctest::Approx(1.0f));
}

TEST_CASE("physics: continuous collision stops a fast body a discrete body tunnels through")
{
    // A thin static wall + a small fast sphere fired straight at it: with discrete stepping
    // the sphere jumps clean across the wall in one step; LinearCast sweeps and stops.
    const auto fire = [](bool continuous) -> f32
    {
        PhysicsWorld world(DefaultAllocator());
        BodyDesc wall;
        wall.motion = MotionKind::Static;
        wall.layer = PhysicsLayer::Static;
        ShapeDesc slab;
        slab.kind = ShapeKind::Box;
        // Thin, but >= Jolt's default box convex radius (0.05) or BoxShape asserts.
        slab.halfExtents = Float3{0.1f, 5.0f, 5.0f}; // 20cm thick vs 3.3 units per step
        wall.shapes.PushBack(slab);
        wall.position = Float3{5.0f, 0.0f, 0.0f};
        (void)world.CreateBody(wall);

        BodyDesc bullet;
        bullet.motion = MotionKind::Dynamic;
        bullet.layer = PhysicsLayer::Dynamic;
        bullet.continuousCollision = continuous;
        ShapeDesc ball;
        ball.kind = ShapeKind::Sphere;
        ball.radius = 0.05f;
        bullet.shapes.PushBack(ball);
        bullet.position = Float3{0.0f, 0.0f, 0.0f};
        const BodyId id = world.CreateBody(bullet);
        world.SetLinearVelocity(id, Float3{200.0f, 0.0f, 0.0f}); // 3.3 units per 1/60 step

        for (int i = 0; i < 30; ++i)
        {
            world.Step(1.0f / 60.0f);
        }
        Float3 position;
        Quaternion rotation;
        world.GetBodyTransform(id, position, rotation);
        return position.x;
    };

    CHECK(fire(false) > 6.0f); // discrete: sailed through the wall
    CHECK(fire(true) < 5.0f);  // LinearCast: stopped at (or bounced off) the wall
}

TEST_CASE("physics: an explicit mass override wins over the density-derived mass")
{
    PhysicsWorld world(DefaultAllocator());
    // Density path: a unit-ish box at 1000 kg/m^3 has a known mass (volume * density).
    BodyDesc byDensity = BoxAt(1.0f);
    const BodyId dense = world.CreateBody(byDensity);
    const f32 derived = world.BodyMass(dense);
    CHECK(derived == doctest::Approx(1.0f * 1000.0f).epsilon(0.01)); // 1m^3 box

    BodyDesc overridden = BoxAt(3.0f);
    overridden.massOverride = 5.0f;
    const BodyId light = world.CreateBody(overridden);
    CHECK(world.BodyMass(light) == doctest::Approx(5.0f).epsilon(0.001));

    // Unset (0) keeps the density path byte-identical.
    BodyDesc unset = BoxAt(5.0f);
    CHECK(world.BodyMass(world.CreateBody(unset)) == doctest::Approx(derived).epsilon(0.001));

    // Static bodies report no mass.
    CHECK(world.BodyMass(world.CreateBody(FloorDesc())) == 0.0f);
}

TEST_CASE("physics: a DYNAMIC body over a mesh/plane shape simulates as static, not an assert")
{
    // Jolt: triangle meshes, planes and heightfields derive no mass ("Invalid mass" assert at
    // creation) and have no collision path against each other ("Unsupported shape pair" at the
    // first contact). The editor's Simulate crashed on a dynamic body with a cooked mesh shape
    // (2026-09-21). The world demotes such a body to static with an error, and it still exists.
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());

    const Float3 positions[4] = {Float3{-1, 0, -1}, Float3{1, 0, -1}, Float3{1, 0, 1},
                                 Float3{-1, 0, 1}};
    const u32 indices[6] = {0, 2, 1, 0, 3, 2};
    const u32 slots[2] = {0, 0};
    Array<byte> blob;
    REQUIRE(CookTriangleMesh(Span<const Float3>(positions, 4), Span<const u32>(indices, 6),
                             Span<const u32>(slots, 2), blob));

    BodyDesc meshBody;
    meshBody.motion = MotionKind::Dynamic;
    meshBody.position = Float3{0.0f, 5.0f, 0.0f};
    {
        ShapeDesc shape;
        shape.kind = ShapeKind::Cooked;
        shape.cooked = Span<const byte>(blob.Data(), blob.Size());
        meshBody.shapes.PushBack(shape);
    }
    const BodyId mesh = world.CreateBody(meshBody);
    REQUIRE(mesh.IsValid());
    CHECK(world.BodyMass(mesh) == 0.0f); // static: no mass

    BodyDesc planeBody;
    planeBody.motion = MotionKind::Dynamic;
    planeBody.position = Float3{20.0f, 5.0f, 0.0f};
    {
        ShapeDesc shape;
        shape.kind = ShapeKind::Plane;
        shape.planeHalfExtent = 2.0f;
        planeBody.shapes.PushBack(shape);
    }
    const BodyId plane = world.CreateBody(planeBody);
    REQUIRE(plane.IsValid());

    // A compound with a mesh part is non-convex too.
    BodyDesc compound = meshBody;
    compound.position = Float3{-20.0f, 5.0f, 0.0f};
    {
        ShapeDesc box;
        box.kind = ShapeKind::Box;
        compound.shapes.PushBack(box);
    }
    const BodyId compoundId = world.CreateBody(compound);
    REQUIRE(compoundId.IsValid());
    CHECK(world.BodyMass(compoundId) == 0.0f);

    // A KINEMATIC mesh: Jolt sets mass properties for every non-static body, so it is the same
    // assert - demoted the same way.
    BodyDesc kinematic = meshBody;
    kinematic.motion = MotionKind::Kinematic;
    kinematic.position = Float3{0.0f, 5.0f, 20.0f};
    const BodyId kinematicId = world.CreateBody(kinematic);
    REQUIRE(kinematicId.IsValid());
    CHECK(world.BodyMass(kinematicId) == 0.0f);

    // A dynamic BOX still falls through the same world, and the static-demoted bodies stay put.
    BodyDesc drop;
    drop.motion = MotionKind::Dynamic;
    drop.position = Float3{0.0f, 8.0f, 0.0f};
    {
        ShapeDesc box;
        box.kind = ShapeKind::Box;
        drop.shapes.PushBack(box);
    }
    const BodyId dropped = world.CreateBody(drop);
    REQUIRE(dropped.IsValid());
    for (int i = 0; i < 60; ++i)
    {
        world.Step(1.0f / 60.0f); // mesh vs box contacts are supported; nothing asserts
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(mesh, position, rotation);
    CHECK(position.y == doctest::Approx(5.0f));
    world.GetBodyTransform(dropped, position, rotation);
    CHECK(position.y < 7.9f);
}

TEST_CASE("physics: a heightfield asked to be dynamic is static - it stays put and a ball lands on it")
{
    PhysicsWorld world(DefaultAllocator());
    constexpr u32 kN = 17;
    Array<f32> samples;
    samples.Resize(static_cast<usize>(kN) * kN, 2.0f); // a flat field 2 units up
    BodyDesc ground;
    ground.motion = MotionKind::Dynamic; // the authored default, left as it is
    {
        ShapeDesc field;
        field.kind = ShapeKind::Heightfield;
        field.heightSamples = Span<const f32>(samples.Data(), samples.Size());
        field.heightSampleCount = kN;
        field.heightWorldSize = Float2{16.0f, 16.0f};
        ground.shapes.PushBack(field);
    }
    const BodyId terrain = world.CreateBody(ground);
    REQUIRE(terrain.IsValid());
    CHECK(world.BodyMass(terrain) == 0.0f);

    BodyDesc drop;
    drop.motion = MotionKind::Dynamic;
    drop.position = Float3{0.0f, 10.0f, 0.0f};
    {
        ShapeDesc sphere;
        sphere.kind = ShapeKind::Sphere;
        sphere.radius = 0.5f;
        drop.shapes.PushBack(sphere);
    }
    const BodyId ball = world.CreateBody(drop);
    REQUIRE(ball.IsValid());
    for (int i = 0; i < 240; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(terrain, position, rotation);
    CHECK(position.y == doctest::Approx(0.0f).epsilon(0.001)); // the ground never fell
    world.GetBodyTransform(ball, position, rotation);
    CHECK(position.y == doctest::Approx(2.5f).epsilon(0.05)); // and the ball landed on it
}

TEST_CASE("physics: a dynamic body over a FLAT convex hull gets a solid-box mass and simulates")
{
    // The one reachable zero-volume convex: a hull cooked from a flat quad. Jolt derives no
    // mass for it; the world gives it a solid-box mass over its bounds (a centimetre thick).
    PhysicsWorld world(DefaultAllocator());
    (void)world.CreateBody(FloorDesc());
    const Float3 quad[4] = {Float3{-1, 0, -1}, Float3{1, 0, -1}, Float3{1, 0, 1}, Float3{-1, 0, 1}};
    Array<byte> blob;
    if (!CookConvexHull(Span<const Float3>(quad, 4), blob))
    {
        MESSAGE("this Jolt refuses a coplanar hull; the degenerate path is unreachable here");
        return;
    }
    BodyDesc flat;
    flat.motion = MotionKind::Dynamic;
    flat.position = Float3{0.0f, 5.0f, 0.0f};
    {
        ShapeDesc shape;
        shape.kind = ShapeKind::Cooked;
        shape.cooked = Span<const byte>(blob.Data(), blob.Size());
        flat.shapes.PushBack(shape);
    }
    const BodyId body = world.CreateBody(flat);
    REQUIRE(body.IsValid());
    CHECK(world.BodyMass(body) > 0.0f);
    BodyDesc heavy = flat;
    heavy.massOverride = 3.0f;
    heavy.position = Float3{20.0f, 5.0f, 0.0f};
    const BodyId heavyId = world.CreateBody(heavy);
    REQUIRE(heavyId.IsValid());
    CHECK(world.BodyMass(heavyId) == doctest::Approx(3.0f));
    for (int i = 0; i < 60; ++i)
    {
        world.Step(1.0f / 60.0f);
    }
    Float3 position;
    Quaternion rotation;
    world.GetBodyTransform(body, position, rotation);
    CHECK(position.y < 4.9f); // it falls: a dynamic body, mass invented, collision real
}

// Jolt's process-wide bring-up is shared by every world and every cook. The cook driver cooks
// collision shapes on parallel job workers with no world alive: each cook acquires Jolt, and
// none may use it before the first has finished registering it (a shared build lost that race
// and cooked with a null allocator - Sky Hopper's platforms, in Integration.Mcp).
TEST_CASE("physics: triangle meshes cook in parallel with no world keeping Jolt up")
{
    const Float3 positions[] = {
        {-2.0f, 0.0f, -2.0f},
        {-2.0f, 0.0f, 2.0f},
        {2.0f, 0.0f, 2.0f},
        {2.0f, 0.0f, -2.0f},
    };
    const u32 indices[] = {0, 1, 3, 1, 2, 3};
    std::atomic<int> cooked{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back(
            [&]()
            {
                while (!go.load()) // all at once: the bring-up is where they collide
                {
                }
                for (int i = 0; i < 25; ++i)
                {
                    Array<byte> blob;
                    if (CookTriangleMesh(Span<const Float3>(positions, 4), Span<const u32>(indices, 6),
                                         Span<const u32>{}, blob) &&
                        !blob.IsEmpty())
                    {
                        cooked.fetch_add(1);
                    }
                }
            });
    }
    go.store(true);
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK(cooked.load() == 8 * 25);
}

// The navigation bake reads level geometry from the bodies themselves: the triangles come out in
// world space, facing out, a compound through its leaves, only what touches the box asked for,
// and a body whose shape cannot build is counted rather than dropped silently. No world.
TEST_CASE("physics: bodies give their world triangles touching a box, compounds and planes included")
{
    const auto normalY = [](const Float3* t)
    {
        const Float3 a = t[1] - t[0];
        const Float3 b = t[2] - t[0];
        return a.z * b.x - a.x * b.z; // the y of cross(a, b)
    };
    const AABB everywhere{Float3{-50.0f, -50.0f, -50.0f}, Float3{50.0f, 50.0f, 50.0f}};

    // A 2 m cube raised 1 m: twelve triangles between y 0 and 2, its top facing up.
    BodyDesc box;
    box.motion = MotionKind::Static;
    box.layer = PhysicsLayer::Static;
    box.position = Float3{3.0f, 1.0f, 0.0f};
    box.shapes.PushBack(ShapeDesc{});
    box.shapes[0].halfExtents = Float3{1.0f, 1.0f, 1.0f};
    Array<Float3> triangles;
    CHECK(AppendBodyTriangles(Span<const BodyDesc>(&box, 1), everywhere, triangles) == 0);
    REQUIRE(triangles.Size() == 36);
    usize upward = 0;
    for (usize i = 0; i < triangles.Size(); i += 3)
    {
        for (usize v = 0; v < 3; ++v)
        {
            CHECK(triangles[i + v].x >= 1.999f);
            CHECK(triangles[i + v].x <= 4.001f);
            CHECK(triangles[i + v].y >= -0.001f);
            CHECK(triangles[i + v].y <= 2.001f);
        }
        if (triangles[i].y > 1.99f && triangles[i + 1].y > 1.99f && triangles[i + 2].y > 1.99f)
        {
            CHECK(normalY(&triangles[i]) > 0.0f);
            ++upward;
        }
    }
    CHECK(upward == 2);

    // Two boxes as one compound body: both leaves give their triangles.
    BodyDesc pair = box;
    pair.shapes.PushBack(box.shapes[0]);
    pair.shapes[0].localPosition = Float3{-2.0f, 0.0f, 0.0f};
    pair.shapes[1].localPosition = Float3{2.0f, 0.0f, 0.0f};
    triangles.Clear();
    CHECK(AppendBodyTriangles(Span<const BodyDesc>(&pair, 1), everywhere, triangles) == 0);
    CHECK(triangles.Size() == 72);

    // A ground plane, as wide as a level allows: its triangles touch the box asked for and face up.
    BodyDesc ground;
    ground.motion = MotionKind::Static;
    ground.layer = PhysicsLayer::Static;
    ground.shapes.PushBack(ShapeDesc{});
    ground.shapes[0].kind = ShapeKind::Plane;
    ground.shapes[0].planeHalfExtent = 1000.0f;
    const AABB patch{Float3{-5.0f, -1.0f, -5.0f}, Float3{5.0f, 1.0f, 5.0f}};
    triangles.Clear();
    CHECK(AppendBodyTriangles(Span<const BodyDesc>(&ground, 1), patch, triangles) == 0);
    REQUIRE(!triangles.IsEmpty());
    for (usize i = 0; i < triangles.Size(); i += 3)
    {
        CHECK(normalY(&triangles[i]) > 0.0f);
    }
    for (const Float3& p : triangles)
    {
        CHECK(Abs(p.y) < 0.001f);
    }

    // Nothing of a body outside the box; a shape that does not build is counted.
    BodyDesc broken = box;
    broken.shapes[0].halfExtents = Float3{0.0f, 1.0f, 1.0f};
    const BodyDesc both[] = {box, broken};
    triangles.Clear();
    const AABB elsewhere{Float3{-5.0f, -1.0f, 25.0f}, Float3{5.0f, 1.0f, 35.0f}};
    CHECK(AppendBodyTriangles(Span<const BodyDesc>(both, 2), elsewhere, triangles) == 1);
    CHECK(triangles.IsEmpty());
}
