// draconic.engine.physics tests: the scene integration headless - component-driven
// body building (incl. hierarchy compounding), the fixed-step sync, render-frame
// interpolation between fixed poses, kinematic scene-follow, and play-cycle teardown.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <cmath>

import draconic.foundation;
import draconic.scene;
import draconic.physics;
import draconic.physics.resource;
import draconic.engine.physics;
import draconic.script;
import draconic.script.facades; // ExtraFacadeNames (the behavior-prelude facade list)
import draconic.script.wren;

using namespace draconic::foundation;
using namespace draconic::physics;
namespace scene = draconic::scene;

namespace
{
    struct PlayScene
    {
        scene::Scene scene{u8"physics-test"};
        PhysicsSceneSystem* physics = nullptr;

        PlayScene()
        {
            RegisterPhysicsComponentReflection();
            scene.AddSystem<RigidBodyComponentManager>();
            scene.AddSystem<ColliderComponentManager>();
            physics = scene.AddSystem<PhysicsSceneSystem>();
        }

        scene::EntityHandle AddFloor()
        {
            scene::EntityHandle e = scene.CreateEntity(u8"floor");
            scene.SetLocalPosition(e, Float3{0.0f, -0.5f, 0.0f});
            RigidBodyComponent& body = scene.GetSystem<RigidBodyComponentManager>()->Add(e);
            body.motion = MotionKind::Static;
            body.layer = PhysicsLayer::Static;
            body.halfExtents = Float3{50.0f, 0.5f, 50.0f};
            return e;
        }

        scene::EntityHandle AddBox(f32 y, MotionKind motion = MotionKind::Dynamic)
        {
            scene::EntityHandle e = scene.CreateEntity(u8"box");
            scene.SetLocalPosition(e, Float3{0.0f, y, 0.0f});
            RigidBodyComponent& body = scene.GetSystem<RigidBodyComponentManager>()->Add(e);
            body.motion = motion;
            body.layer = motion == MotionKind::Static      ? PhysicsLayer::Static
                         : motion == MotionKind::Kinematic ? PhysicsLayer::Kinematic
                                                           : PhysicsLayer::Dynamic;
            return e;
        }

        void Start()
        {
            scene.UpdateTransforms(); // world matrices current before body building
            scene.Start();
            scene.SetSimulationEnabled(true);
        }

        void Step(int steps = 1)
        {
            for (int i = 0; i < steps; ++i)
            {
                scene.FixedUpdate(1.0f / 60.0f);
            }
        }
    };
}

TEST_CASE("physics.scene: components build bodies at Start; dynamics fall and land")
{
    PlayScene play;
    (void)play.AddFloor();
    const scene::EntityHandle box = play.AddBox(5.0f);
    play.Start();
    REQUIRE(play.physics->World() != nullptr);
    CHECK(play.physics->World()->BodyCount() == 2u);

    play.Step(240);
    play.physics->ApplyInterpolation(1.0f); // write final poses to the scene
    play.scene.UpdateTransforms();
    const Float3 rest = play.scene.GetWorldPosition(box);
    CHECK(rest.y == doctest::Approx(0.5f).epsilon(0.05));

    // Stop tears the world down and clears handles.
    play.scene.Stop();
    CHECK(play.physics->World() == nullptr);
    CHECK_FALSE(play.scene.GetSystem<RigidBodyComponentManager>()->Get(box)->body.IsValid());
}

TEST_CASE("physics.scene: interpolation blends between the last two fixed poses")
{
    PlayScene play;
    const scene::EntityHandle box = play.AddBox(10.0f); // free fall, no floor
    play.Start();
    play.Step(30); // let it pick up speed

    RigidBodyComponent* body = play.scene.GetSystem<RigidBodyComponentManager>()->Get(box);
    REQUIRE(body != nullptr);
    const f32 prevY = body->prevPosition.y;
    const f32 currY = body->currPosition.y;
    REQUIRE(prevY > currY); // falling

    // Production order: interpolation (physics subsystem, -600) THEN Scene::Update's
    // UpdateTransforms - mirrored explicitly here.
    play.physics->ApplyInterpolation(0.0f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(box).y == doctest::Approx(prevY).epsilon(0.001));
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(box).y == doctest::Approx(currY).epsilon(0.001));
    play.physics->ApplyInterpolation(0.5f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(box).y ==
          doctest::Approx((prevY + currY) * 0.5f).epsilon(0.001));
}

TEST_CASE("physics.scene: kinematic bodies follow the scene; dynamics rest on them")
{
    PlayScene play;
    const scene::EntityHandle platform = play.AddBox(0.0f, MotionKind::Kinematic);
    const scene::EntityHandle rider = play.AddBox(1.5f);
    play.Start();

    // Land the rider on the platform, then slide the platform sideways: the rider rides.
    play.Step(120);
    for (int i = 0; i < 120; ++i)
    {
        Transform t = play.scene.GetLocalTransform(platform);
        t.position.x += 2.0f / 120.0f; // 2 units over 2 seconds
        play.scene.SetLocalTransform(platform, t);
        play.scene.UpdateTransforms();
        play.Step(1);
    }
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(rider).x > 1.0f); // dragged along by friction
    CHECK(play.scene.GetWorldPosition(rider).y ==
          doctest::Approx(1.0f).epsilon(0.1)); // platform top 0.5 + half extent
}

TEST_CASE("physics.scene: descendant colliders compound into the ancestor body")
{
    PlayScene play;
    (void)play.AddFloor();
    // An L-piece: the body box at origin + a child collider offset +x. Its compound
    // should topple/rest unlike a lone cube - assert the child shape EXISTS by ray.
    const scene::EntityHandle body = play.AddBox(0.5f, MotionKind::Static);
    scene::EntityHandle arm = play.scene.CreateEntity(u8"arm");
    play.scene.SetParent(arm, body);
    play.scene.SetLocalPosition(arm, Float3{2.0f, 0.0f, 0.0f});
    ColliderComponent& extra = play.scene.GetSystem<ColliderComponentManager>()->Add(arm);
    extra.halfExtents = Float3{0.5f, 0.5f, 0.5f};
    play.Start();

    RayHit hit;
    REQUIRE(play.physics->World()->RayCast(Float3{2.0f, 5.0f, 0.0f}, Float3{0.0f, -1.0f, 0.0f},
                                           10.0f, hit));
    CHECK(hit.position.y == doctest::Approx(1.0f).epsilon(0.05)); // the arm's top face
}

namespace
{
    // Records resolved contacts (the PhysicsSubsystem plays this role in a real run; the bare
    // scene-system harness wires it in directly via SetContactListeners).
    struct RecordingListener final : IContactListener
    {
        Array<EntityContact> contacts;
        void OnContact(const EntityContact& contact) override { contacts.PushBack(contact); }
    };
}

TEST_CASE("physics.scene: trigger components raise enter events resolved to entities")
{
    PlayScene play;
    (void)play.AddFloor();
    const scene::EntityHandle volume = play.AddBox(2.0f, MotionKind::Kinematic);
    RigidBodyComponent* sensor = play.scene.GetSystem<RigidBodyComponentManager>()->Get(volume);
    sensor->isTrigger = true;
    sensor->halfExtents = Float3{1.0f, 1.0f, 1.0f};
    const scene::EntityHandle faller = play.AddBox(6.0f);
    play.Start();

    RecordingListener recorder;
    Array<IContactListener*> listeners;
    listeners.PushBack(&recorder);
    play.physics->SetContactListeners(&listeners); // packed userData resolves to entities

    bool entered = false;
    for (int i = 0; i < 240 && !entered; ++i)
    {
        play.Step(1);
        for (const EntityContact& c : recorder.contacts)
        {
            if (c.kind == ContactKind::TriggerEnter &&
                ((c.a == volume && c.b == faller) || (c.a == faller && c.b == volume)))
            {
                entered = true;
            }
        }
    }
    CHECK(entered);
}

TEST_CASE("physics.scene: a real Jolt collision reaches a registered listener with resolved "
          "entities + geometry")
{
    PlayScene play;
    const scene::EntityHandle floor = play.AddFloor();
    const scene::EntityHandle box = play.AddBox(1.4f); // drops onto the floor
    play.Start();

    RecordingListener recorder;
    Array<IContactListener*> listeners;
    listeners.PushBack(&recorder);
    play.physics->SetContactListeners(&listeners);

    bool sawBegin = false;
    for (int i = 0; i < 120 && !sawBegin; ++i)
    {
        play.Step(1);
        for (const EntityContact& c : recorder.contacts)
        {
            if (c.kind != ContactKind::Begin)
            {
                continue;
            }
            if ((c.a == box && c.b == floor) || (c.a == floor && c.b == box))
            {
                sawBegin = true;
                CHECK(c.scene == &play.scene);
                CHECK(c.speed >= 0.0f); // approach speed, never negative
                const f32 normalLength = Length(c.normal);
                CHECK(normalLength == doctest::Approx(1.0f).epsilon(0.02)); // unit normal
            }
        }
    }
    CHECK(sawBegin);
}

TEST_CASE("physics.scene: cooked collision shape drives a body via the component ref")
{
    // Cook a unit-cube hull, wrap it in a CollisionShape product, and hand it to the
    // component DIRECTLY (Ref procedural override) - no content db in this harness.
    Array<byte> blob;
    Array<Float3> corners;
    const f32 ends[2] = {-0.5f, 0.5f};
    for (f32 x : ends)
        for (f32 y : ends)
            for (f32 z : ends)
            {
                corners.PushBack(Float3{x, y, z});
            }
    REQUIRE(CookConvexHull(Span<const Float3>(corners.Data(), corners.Size()), blob));
    RefPtr<CollisionShape> shape = MakeRef<CollisionShape>(DefaultAllocator());
    shape->blob.Resize(blob.Size());
    MemCopy(shape->blob.Data(), blob.Data(), blob.Size());

    PlayScene play;
    play.AddFloor();
    scene::EntityHandle crate = play.AddBox(3.0f);
    {
        RigidBodyComponent* body = play.scene.GetSystem<RigidBodyComponentManager>()->Get(crate);
        REQUIRE(body != nullptr);
        body->shape = ShapeKind::Cooked;
        body->collisionShape = shape; // Ref direct override
        // Entity scale doubles the cooked hull: rest height = scaled half extent.
        Transform t = play.scene.GetLocalTransform(crate);
        t.scale = Float3{2.0f, 2.0f, 2.0f};
        play.scene.SetLocalTransform(crate, t);
    }
    play.Start();
    play.Step(300);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(crate).y == doctest::Approx(1.0f).epsilon(0.08));
}

TEST_CASE("physics.scene: a referenced PhysicalMaterial overrides inline surface fields")
{
    RefPtr<PhysicalMaterial> bouncy = MakeRef<PhysicalMaterial>(DefaultAllocator());
    bouncy->friction = 0.1f;
    bouncy->restitution = 0.9f;

    PlayScene play;
    play.AddFloor();
    scene::EntityHandle ball = play.AddBox(3.0f);
    {
        RigidBodyComponent* body = play.scene.GetSystem<RigidBodyComponentManager>()->Get(ball);
        REQUIRE(body != nullptr);
        body->restitution = 0.0f; // inline says dead drop...
        body->material = bouncy;  // ...material says bounce
    }
    play.Start();

    // Track the maximum height AFTER the first impact; a dead drop stays ~at rest.
    bool impacted = false;
    f32 apex = 0.0f;
    for (int i = 0; i < 600; ++i)
    {
        play.Step();
        play.physics->ApplyInterpolation(1.0f);
        play.scene.UpdateTransforms();
        const f32 y = play.scene.GetWorldPosition(ball).y;
        if (!impacted && y < 0.6f)
        {
            impacted = true;
        }
        else if (impacted)
        {
            apex = y > apex ? y : apex;
        }
    }
    CHECK(apex > 1.0f); // bounced well above the rest height
}

TEST_CASE("physics.scene: a tilted plane entity makes boxes slide downhill")
{
    PlayScene play;
    scene::EntityHandle ground = play.scene.CreateEntity(u8"ramp");
    {
        RigidBodyComponent& body = play.scene.GetSystem<RigidBodyComponentManager>()->Add(ground);
        body.motion = MotionKind::Static;
        body.layer = PhysicsLayer::Static;
        body.shape = ShapeKind::Plane; // entity's local XZ plane; rotation tilts it
        body.friction = 0.0f;
        Transform t = play.scene.GetLocalTransform(ground);
        t.rotation = Quaternion::FromAxisAngle(Float3{0.0f, 0.0f, 1.0f}, 0.3f);
        play.scene.SetLocalTransform(ground, t);
    }
    scene::EntityHandle box = play.AddBox(3.0f);
    {
        RigidBodyComponent* body = play.scene.GetSystem<RigidBodyComponentManager>()->Get(box);
        body->friction = 0.0f;
    }
    play.Start();
    play.Step(240);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    // Frictionless on a plane tilted around +Z: the box slides toward -x downhill... the
    // tilt raises +x, so it slides to NEGATIVE x and keeps contact (no tunnel-through).
    const Float3 position = play.scene.GetWorldPosition(box);
    CHECK(position.x < -1.0f);
    CHECK(position.y > -30.0f);
}

TEST_CASE("physics.scene: the Wren Physics facade raycasts + pushes through the service")
{
    RegisterPhysicsScriptFacade();

    PlayScene play;
    play.AddFloor();
    scene::EntityHandle box = play.AddBox(0.5f); // resting on the floor at y=0.5
    play.Start();
    play.Step(10);

    PhysicsScriptBinding binding;
    binding.system = play.physics;

    RefPtr<draconic::script::IScriptManager> manager =
        draconic::script::wren::CreateScriptManager();
    draconic::script::RegisterReflectedTypes(*manager);
    RefPtr<draconic::script::IScriptContext> ctx = manager->CreateContext();
    REQUIRE(ctx.Get() != nullptr);
    ctx->SetService(kPhysicsScriptService, &binding);

    const StringView script =
        u8"var Distance = Physics.rayCast(0, 5, 0, 0, -1, 0, 20)\n"
        u8"var Top = Physics.hitY()\n"
        u8"var UpN = Physics.hitNormalY()\n"
        u8"var Bodies = Physics.bodyCount()\n"
        u8"Physics.impulseOnHit(8000, 0, 0)\n"; // the box weighs ~1000kg (default density)
    REQUIRE(ctx->Load(script, u8"main").IsOk());
    CHECK(ctx->GetGlobal(u8"Distance").Get<f64>() == doctest::Approx(4.0).epsilon(0.02));
    CHECK(ctx->GetGlobal(u8"Top").Get<f64>() == doctest::Approx(1.0).epsilon(0.02));
    CHECK(ctx->GetGlobal(u8"UpN").Get<f64>() == doctest::Approx(1.0).epsilon(0.01));
    CHECK(ctx->GetGlobal(u8"Bodies").Get<f64>() == doctest::Approx(2.0));

    // The scripted impulse actually moved the box.
    play.Step(30);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(box).x > 0.2f);

    // No service bound: released misses, never a crash.
    RefPtr<draconic::script::IScriptContext> bare = manager->CreateContext();
    REQUIRE(
        bare->Load(u8"var Distance = Physics.rayCast(0, 5, 0, 0, -1, 0, 20)\n", u8"main").IsOk());
    CHECK(bare->GetGlobal(u8"Distance").Get<f64>() == doctest::Approx(-1.0));
}

TEST_CASE("physics.scene: the Physics facade is in the Wren BEHAVIOR prelude (not just main)")
{
    RegisterPhysicsScriptFacade(); // registers the type AND the behavior-prelude facade name

    // The behavior/Level prelude is `import "main" for <built-ins + ExtraFacadeNames>`, so a
    // facade is reachable from a component behavior (or a Level) only if its name is in that
    // list. Before the fix Physics registered its TYPE but not its NAME, so it resolved only
    // from top-level `main`/Game scripts. Assert the name is now published to the prelude.
    bool inPrelude = false;
    for (const StringView facade : draconic::script::ExtraFacadeNames())
    {
        inPrelude = inPrelude || facade == StringView(u8"Physics");
    }
    CHECK(inPrelude);
}

TEST_CASE(
    "physics.scene: bodies build from authored positions even without a prior UpdateTransforms")
{
    // Regression: Scene::Start does NOT refresh world matrices; if the system builds from
    // never-updated (Identity) matrices, every body spawns at the origin interpenetrating
    // and depenetration blasts the stack apart (the PhysicsPlayground startup bug).
    PlayScene play;
    play.AddFloor();
    scene::EntityHandle left = play.AddBox(0.5f);
    scene::EntityHandle right = play.AddBox(0.5f);
    play.scene.SetLocalPosition(left, Float3{-3.0f, 0.5f, 0.0f});
    play.scene.SetLocalPosition(right, Float3{3.0f, 0.5f, 0.0f});

    // Deliberately NO UpdateTransforms before Start - the system must self-refresh.
    play.scene.Start();
    play.scene.SetSimulationEnabled(true);
    play.Step(60);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(left).x == doctest::Approx(-3.0f).epsilon(0.05));
    CHECK(play.scene.GetWorldPosition(right).x == doctest::Approx(3.0f).epsilon(0.05));
    CHECK(play.scene.GetWorldPosition(left).y == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("physics.scene: a motorized hinge joint spins a door to the world")
{
    PlayScene play;
    play.scene.AddSystem<JointComponentManager>();
    scene::EntityHandle door = play.scene.CreateEntity(u8"door");
    play.scene.SetLocalPosition(door, Float3{0.0f, 2.0f, 0.0f});
    {
        RigidBodyComponent& body = play.scene.GetSystem<RigidBodyComponentManager>()->Add(door);
        body.halfExtents = Float3{1.0f, 1.0f, 0.05f};
        JointComponent& joint = play.scene.GetSystem<JointComponentManager>()->Add(door);
        joint.kind = JointKind::Hinge; // no ancestor body -> world attachment
        joint.localAxis = Float3{0.0f, 1.0f, 0.0f};
        joint.motorEnabled = true;
        joint.motorTargetVelocity = 3.0f;
    }
    play.Start();
    play.Step(120);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();

    // Held at its pivot, meaningfully rotated by the motor.
    const Float3 position = play.scene.GetWorldPosition(door);
    CHECK(position.y == doctest::Approx(2.0f).epsilon(0.02));
    const Quaternion rotation = play.scene.GetLocalTransform(door).rotation;
    CHECK((rotation.w > 0 ? rotation.w : -rotation.w) < 0.99f);
}

TEST_CASE("physics.scene: nil-target joints attach to the nearest ancestor body; guid targets bind "
          "explicitly")
{
    PlayScene play;
    play.scene.AddSystem<JointComponentManager>();
    play.AddFloor();

    // anchor (static, elevated) > bob (dynamic child on a distance rope, nil target).
    scene::EntityHandle anchor = play.scene.CreateEntity(u8"anchor");
    play.scene.SetLocalPosition(anchor, Float3{0.0f, 6.0f, 0.0f});
    {
        RigidBodyComponent& body = play.scene.GetSystem<RigidBodyComponentManager>()->Add(anchor);
        body.motion = MotionKind::Static;
        body.layer = PhysicsLayer::Static;
        body.halfExtents = Float3{0.2f, 0.2f, 0.2f};
    }
    scene::EntityHandle bob = play.scene.CreateEntity(u8"bob");
    play.scene.SetParent(bob, anchor);
    play.scene.SetLocalPosition(bob, Float3{0.0f, -1.0f, 0.0f}); // world y = 5
    {
        RigidBodyComponent& body = play.scene.GetSystem<RigidBodyComponentManager>()->Add(bob);
        body.shape = ShapeKind::Sphere;
        body.radius = 0.25f;
        JointComponent& joint = play.scene.GetSystem<JointComponentManager>()->Add(bob);
        joint.kind = JointKind::Distance; // nil target -> ancestor body
        joint.maxDistance = 2.0f;
        joint.minDistance = 0.0f;
    }

    // Explicit-guid pair on the floor: two boxes fixed together side by side.
    scene::EntityHandle left = play.AddBox(0.5f);
    scene::EntityHandle right = play.AddBox(0.5f);
    play.scene.SetLocalPosition(left, Float3{4.0f, 0.5f, 0.0f});
    play.scene.SetLocalPosition(right, Float3{5.2f, 0.5f, 0.0f});
    {
        JointComponent& joint = play.scene.GetSystem<JointComponentManager>()->Add(right);
        joint.kind = JointKind::Fixed;
        joint.targetEntity = play.scene.GetEntityId(left);
    }

    play.Start();
    play.Step(300);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();

    // The bob hangs on the rope 2 below the anchor - it did NOT fall to the floor.
    CHECK(play.scene.GetWorldPosition(bob).y == doctest::Approx(4.0f).epsilon(0.05));

    // The fixed pair stays welded: push LEFT, RIGHT follows at the same offset.
    RigidBodyComponent* leftBody = play.scene.GetSystem<RigidBodyComponentManager>()->Get(left);
    play.physics->World()->AddImpulse(leftBody->body, Float3{0.0f, 0.0f, 4000.0f});
    play.Step(60);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    // The welded pair may rotate as one rigid unit from the off-center push - the
    // invariant is the CENTER DISTANCE, not per-axis offsets.
    const Float3 a = play.scene.GetWorldPosition(left);
    const Float3 b = play.scene.GetWorldPosition(right);
    const f32 distance = Length(Float3{b.x - a.x, b.y - a.y, b.z - a.z});
    CHECK(distance == doctest::Approx(1.2f).epsilon(0.02));
}

TEST_CASE("physics.scene: the character component walks, jumps, and lands (interpolated)")
{
    PlayScene play;
    play.scene.AddSystem<CharacterComponentManager>();
    play.AddFloor();
    scene::EntityHandle hero = play.scene.CreateEntity(u8"hero");
    play.scene.SetLocalPosition(hero, Float3{0.0f, 0.9f, 0.0f});
    CharacterComponent& character = play.scene.GetSystem<CharacterComponentManager>()->Add(hero);
    play.Start();

    // Settle to the ground.
    play.Step(30);
    CHECK(character.ground == CharacterGround::OnGround);

    // Walk +x for 1s.
    character.moveVelocity = Float3{3.0f, 0.0f, 0.0f};
    play.Step(60);
    play.physics->ApplyInterpolation(1.0f);
    play.scene.UpdateTransforms();
    CHECK(play.scene.GetWorldPosition(hero).x == doctest::Approx(3.0f).epsilon(0.1));
    CHECK(play.scene.GetWorldPosition(hero).y == doctest::Approx(0.9f).epsilon(0.03));

    // Jump: rises, then lands back at standing height.
    character.moveVelocity = Float3{0.0f, 0.0f, 0.0f};
    character.jumpSpeed = 5.0f;
    f32 apex = 0.0f;
    for (int i = 0; i < 120; ++i)
    {
        play.Step();
        play.physics->ApplyInterpolation(1.0f);
        play.scene.UpdateTransforms();
        const f32 y = play.scene.GetWorldPosition(hero).y;
        apex = y > apex ? y : apex;
    }
    CHECK(apex > 1.8f);                                   // cleared ~1m of air
    CHECK(character.ground == CharacterGround::OnGround); // landed
    CHECK(play.scene.GetWorldPosition(hero).y == doctest::Approx(0.9f).epsilon(0.03));
}

// ---- the editor Simulate cycle (regression: stop hung + OOMed the editor) ----
// Capture -> Start -> frames -> Stop -> Restore -> frames, twice, on the real
// subsystem stack (SceneSubsystem drives per-scene fixed stepping like the editor).

import draconic.runtime;
import draconic.engine.scene;
import draconic.scene.resource;

TEST_CASE("physics.scene: the editor simulate cycle (capture/start/stop/restore) terminates")
{
    namespace runtime = draconic::runtime;
    runtime::Context ctx;
    auto* scenes = ctx.AddSubsystem<scene::SceneSubsystem>();
    scene::SceneManager sm(&scenes->AwareRegistry());
    scenes->RegisterManager(&sm);
    ctx.AddSubsystem<PhysicsSubsystem>();
    ctx.Startup();

    scene::Scene* scene = sm.CreateScene(u8"level");
    {
        scene::EntityHandle floor = scene->CreateEntity(u8"floor");
        scene->SetLocalPosition(floor, Float3{0.0f, -0.5f, 0.0f});
        auto& body = scene->GetSystem<RigidBodyComponentManager>()->Add(floor);
        body.motion = MotionKind::Static;
        body.layer = PhysicsLayer::Static;
        body.halfExtents = Float3{50.0f, 0.5f, 50.0f};
    }
    for (int i = 0; i < 8; ++i)
    {
        scene::EntityHandle box = scene->CreateEntity(u8"box");
        scene->SetLocalPosition(
            box, Float3{static_cast<f32>(i) * 0.5f, 3.0f + static_cast<f32>(i), 0.0f});
        auto& body = scene->GetSystem<RigidBodyComponentManager>()->Add(box);
        body.motion = MotionKind::Dynamic;
        body.layer = PhysicsLayer::Dynamic;
    }
    scene->UpdateTransforms();

    for (int cycle = 0; cycle < 2; ++cycle)
    {
        MESSAGE("cycle ", cycle, ": capture");
        auto snapshot = scene::SceneSnapshot::Capture(*scene);
        REQUIRE(snapshot);
        MESSAGE("cycle ", cycle, ": start");
        scene->Start();
        scene->SetSimulationEnabled(true);
        MESSAGE("cycle ", cycle, ": simulate");
        for (int f = 0; f < 120; ++f)
        {
            ctx.BeginFrame(1.0f / 60.0f);
        }
        MESSAGE("cycle ", cycle, ": stop");
        scene->Stop();
        MESSAGE("cycle ", cycle, ": restore");
        CHECK(snapshot->Restore(*scene, nullptr).IsOk());
        scene->SetSimulationEnabled(false);
        MESSAGE("cycle ", cycle, ": post-stop frames");
        for (int f = 0; f < 60; ++f)
        {
            ctx.BeginFrame(1.0f / 60.0f);
        }
    }

    ctx.Shutdown();
}
