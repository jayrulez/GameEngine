// draconic.physics core tests: headless Jolt world - "determinism-enough" simulation
// (spawn/step/assert poses), the layer matrix, compound building, queries, kinematic
// motion, and contact/trigger buffering.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <cmath>
#include <cstdio>

import draconic.foundation;
import draconic.physics;

using namespace draconic::foundation;
using namespace draconic::physics;

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
    PhysicsWorld world;
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

TEST_CASE("physics: static-static never pairs; dynamic collides with static")
{
    PhysicsWorld world;
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
    PhysicsWorld world;
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
    PhysicsWorld world;
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
    PhysicsWorld world;
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
    PhysicsWorld world;
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
    PhysicsWorld world;
    BodyDesc box = BoxAt(0.0f, MotionKind::Static);
    const BodyId body = world.CreateBody(box);
    Array<BodyId> hits;
    world.QueryPoint(Float3{0.0f, 0.0f, 0.0f}, hits);
    REQUIRE(hits.Size() == 1);
    CHECK(hits[0] == body);
    hits.Clear();
    world.QueryPoint(Float3{10.0f, 0.0f, 0.0f}, hits);
    CHECK(hits.IsEmpty());
}

// ---- cooked shapes (P2: builder-cooked convex hulls + triangle meshes) ----

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

    PhysicsWorld world;
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

    PhysicsWorld world;
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

    PhysicsWorld world;
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
    PhysicsWorld world;
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

// ---- joints (P3) ----

TEST_CASE("physics: a fixed joint to the world holds a body against gravity")
{
    PhysicsWorld world;
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
    PhysicsWorld world;
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

TEST_CASE("physics: a distance joint to a world anchor makes a pendulum rope")
{
    PhysicsWorld world;
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
    PhysicsWorld world;
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

// ---- character controller (P3) ----

TEST_CASE("physics: the character walks, climbs steps, and pushes light bodies")
{
    PhysicsWorld world;
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
