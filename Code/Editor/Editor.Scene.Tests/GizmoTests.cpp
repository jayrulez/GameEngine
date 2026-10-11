// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// TransformGizmo + GizmoController headless tests: pick math (priorities, grazing disable,
// half-ring culling), drag math (translate/rotate/scale + snap quantization), and full scripted
// drag sessions against a live Scene (one undo entry per drag via group bracketing, exact
// start-transform restore, parent-space conversion, mode/space keys).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.render;   // debug::DebugDraw (collider gizmo test)
import foundation.physics;  // ShapeKind
import engine.render;
import engine.navigation;
import engine.physics;      // RigidBodyComponent + manager
import engine.animation;    // the IK components and their animator
import foundation.animation; // Skeleton (the IK gizmo's bind chain)
import foundation.geometry;  // Primitives (the vertex snap test's cubes)
import editor.core;
import editor.scene;

using namespace foundation::core;
using namespace editor;
namespace core = foundation::core;

namespace
{
    // Camera at +Z looking down -Z at the origin (the fixture for all pick tests).
    constexpr Float3 kCamPos{0.0f, 0.0f, 10.0f};
    constexpr Float3 kCamFwd{0.0f, 0.0f, -1.0f};

    // Ray from the fixture camera through a world point.
    GizmoRay RayThrough(Float3 point) { return GizmoRay{kCamPos, Normalized(point - kCamPos)}; }

    TransformGizmo MakeGizmo()
    {
        TransformGizmo g;
        g.position = Float3{};
        g.size = 1.0f;
        g.SetCamera(kCamPos, kCamFwd);
        return g;
    }
}

TEST_CASE("gizmo: ray-axis / ray-ring / ray-point distances")
{
    // Ray straight through the X-axis segment midpoint: distance ~0.
    const GizmoRay onAxis = RayThrough(Float3{0.5f, 0.0f, 0.0f});
    CHECK(TransformGizmo::RayAxisDistance(onAxis, Float3{}, Float3{1, 0, 0}, 1.0f) ==
          doctest::Approx(0.0f).epsilon(0.01f));

    // Ray passing 0.5 above the segment.
    const GizmoRay offAxis = RayThrough(Float3{0.5f, 0.5f, 0.0f});
    CHECK(TransformGizmo::RayAxisDistance(offAxis, Float3{}, Float3{1, 0, 0}, 1.0f) ==
          doctest::Approx(0.5f).epsilon(0.05f));

    // Beyond the segment end the distance is to the endpoint, not the infinite line.
    const GizmoRay past = RayThrough(Float3{2.0f, 0.0f, 0.0f});
    CHECK(TransformGizmo::RayAxisDistance(past, Float3{}, Float3{1, 0, 0}, 1.0f) ==
          doctest::Approx(1.0f).epsilon(0.05f));

    // Ring in the Z=0 plane, radius 0.8: a ray through a circumference point -> 0.
    Float3 hit;
    CHECK(TransformGizmo::RayRingDistance(RayThrough(Float3{0.8f, 0.0f, 0.0f}), Float3{},
                                          Float3{0, 0, 1}, 0.8f,
                                          &hit) == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(hit.x == doctest::Approx(0.8f).epsilon(0.01f));

    // Ring plane behind the ray -> unreachable.
    const GizmoRay away{kCamPos, Float3{0.0f, 0.0f, 1.0f}};
    CHECK(TransformGizmo::RayRingDistance(away, Float3{}, Float3{0, 0, 1}, 0.8f, nullptr) ==
          kFloatMax);

    CHECK(TransformGizmo::RayPointDistance(RayThrough(Float3{}), Float3{}) ==
          doctest::Approx(0.0f).epsilon(0.01f));
}

TEST_CASE("gizmo: hover picks with priorities and grazing disables")
{
    TransformGizmo g = MakeGizmo();

    // Center of the gizmo: the View handle (priority 2) wins over everything.
    CHECK(g.UpdateHover(RayThrough(Float3{}), GizmoMode::Translate) == GizmoAxis::View);

    // Over the X axis shaft.
    CHECK(g.UpdateHover(RayThrough(Float3{0.7f, 0.0f, 0.0f}), GizmoMode::Translate) ==
          GizmoAxis::X);
    CHECK(g.UpdateHover(RayThrough(Float3{0.0f, 0.7f, 0.0f}), GizmoMode::Translate) ==
          GizmoAxis::Y);

    // The XY plane quad (normal Z, facing the camera) sits at [0.25, 0.55] on both axes and
    // BEATS the axis handles by priority even though the axes pass nearby.
    CHECK(g.UpdateHover(RayThrough(Float3{0.4f, 0.4f, 0.0f}), GizmoMode::Translate) ==
          GizmoAxis::PlaneZ);

    // Grazing: the Z axis points straight at the camera -> disabled + unpickable; the plane
    // quads containing Z (PlaneX = YZ, PlaneY = XZ) are edge-on -> disabled.
    CHECK_FALSE(g.IsAxisEnabled(GizmoAxis::Z, GizmoMode::Translate));
    CHECK(g.IsAxisEnabled(GizmoAxis::X, GizmoMode::Translate));
    CHECK_FALSE(g.IsAxisEnabled(GizmoAxis::PlaneX, GizmoMode::Translate));
    CHECK_FALSE(g.IsAxisEnabled(GizmoAxis::PlaneY, GizmoMode::Translate));
    CHECK(g.IsAxisEnabled(GizmoAxis::PlaneZ, GizmoMode::Translate));

    // Empty space hovers nothing.
    CHECK(g.UpdateHover(RayThrough(Float3{3.0f, 3.0f, 0.0f}), GizmoMode::Translate) ==
          GizmoAxis::None);

    // Rotate mode: a 45-degree point on the Z ring circumference (radius 0.8). (On-axis points
    // like (0.8, 0, 0) genuinely lie on TWO rings, so the fixture avoids them.)
    CHECK(g.UpdateHover(RayThrough(Float3{0.566f, 0.566f, 0.0f}), GizmoMode::Rotate) ==
          GizmoAxis::Z);
    // The outer screen-space ring (radius 1.0, camera-facing).
    CHECK(g.UpdateHover(RayThrough(Float3{0.0f, 1.0f, 0.0f}), GizmoMode::Rotate) ==
          GizmoAxis::View);
}

TEST_CASE("gizmo: translate drag along an axis, on a plane, and snapped")
{
    TransformGizmo g = MakeGizmo();

    // Axis drag: grab the X shaft at x=0.7, pull to x=1.9 -> delta (1.2, 0, 0).
    REQUIRE(g.UpdateHover(RayThrough(Float3{0.7f, 0.0f, 0.0f}), GizmoMode::Translate) ==
            GizmoAxis::X);
    REQUIRE(g.BeginDrag(RayThrough(Float3{0.7f, 0.0f, 0.0f}), GizmoMode::Translate));
    Float3 delta = g.UpdateTranslateDrag(RayThrough(Float3{1.9f, 0.0f, 0.0f}));
    CHECK(delta.x == doctest::Approx(1.2f).epsilon(0.01f));
    CHECK(delta.y == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(delta.z == doctest::Approx(0.0f).epsilon(0.01f));

    // Snap quantizes the delta (translateSnap = 1.0): 1.2 -> 1.0.
    delta = g.UpdateTranslateDrag(RayThrough(Float3{1.9f, 0.0f, 0.0f}), true);
    CHECK(delta.x == doctest::Approx(1.0f).epsilon(0.01f));
    g.EndDrag();

    // Plane drag: grab the XY quad, move diagonally -> both components move.
    REQUIRE(g.UpdateHover(RayThrough(Float3{0.4f, 0.4f, 0.0f}), GizmoMode::Translate) ==
            GizmoAxis::PlaneZ);
    REQUIRE(g.BeginDrag(RayThrough(Float3{0.4f, 0.4f, 0.0f}), GizmoMode::Translate));
    delta = g.UpdateTranslateDrag(RayThrough(Float3{1.4f, 0.9f, 0.0f}));
    CHECK(delta.x == doctest::Approx(1.0f).epsilon(0.02f));
    CHECK(delta.y == doctest::Approx(0.5f).epsilon(0.02f));
    g.EndDrag();
}

TEST_CASE("gizmo: rotate drag measures the angle on the captured plane and unwraps the seam")
{
    TransformGizmo g = MakeGizmo();

    // Grab the Z ring at 45deg, sweep to 135deg -> 90 degrees of rotation.
    const Float3 at45{0.566f, 0.566f, 0.0f};
    REQUIRE(g.UpdateHover(RayThrough(at45), GizmoMode::Rotate) == GizmoAxis::Z);
    REQUIRE(g.BeginDrag(RayThrough(at45), GizmoMode::Rotate));

    TransformGizmo::RotateDelta d = g.UpdateRotateDrag(RayThrough(Float3{-0.566f, 0.566f, 0.0f}));
    CHECK(Abs(d.axis.z) == doctest::Approx(1.0f).epsilon(0.01f));
    CHECK(Abs(d.angle) == doctest::Approx(kPi * 0.5f).epsilon(0.01f));

    // Sweeping to 170deg then to -170deg (= 190deg) must unwrap, not jump by ~2pi.
    const f32 a170 = DegreesToRadians(170.0f);
    d = g.UpdateRotateDrag(RayThrough(Float3{0.8f * Cos(a170), 0.8f * Sin(a170), 0.0f}));
    const f32 before = d.angle;
    const f32 a190 = DegreesToRadians(190.0f);
    d = g.UpdateRotateDrag(RayThrough(Float3{0.8f * Cos(a190), 0.8f * Sin(a190), 0.0f}));
    CHECK(Abs(Abs(d.angle) - Abs(before)) < DegreesToRadians(45.0f)); // continuous across the seam

    // Snap quantizes to rotateSnapDegrees (15 deg default): ~100deg -> 105 or 90.
    const f32 a100 = DegreesToRadians(100.0f);
    d = g.UpdateRotateDrag(RayThrough(Float3{0.8f * Cos(a100), 0.8f * Sin(a100), 0.0f}), true);
    const f32 snapped = Abs(RadiansToDegrees(d.angle));
    const f32 remainder = snapped - Round(snapped / 15.0f) * 15.0f;
    CHECK(Abs(remainder) == doctest::Approx(0.0f).epsilon(0.1f));
    g.EndDrag();
}

TEST_CASE("gizmo: scale drag returns per-axis and uniform deltas")
{
    TransformGizmo g = MakeGizmo();

    REQUIRE(g.UpdateHover(RayThrough(Float3{0.7f, 0.0f, 0.0f}), GizmoMode::Scale) == GizmoAxis::X);
    REQUIRE(g.BeginDrag(RayThrough(Float3{0.7f, 0.0f, 0.0f}), GizmoMode::Scale));
    Float3 d = g.UpdateScaleDrag(RayThrough(Float3{1.7f, 0.0f, 0.0f}));
    CHECK(d.x == doctest::Approx(1.0f).epsilon(0.02f)); // delta / size(=1)
    CHECK(d.y == doctest::Approx(0.0f).epsilon(0.01f));
    g.EndDrag();

    // Center handle: uniform scale (all three components equal).
    REQUIRE(g.UpdateHover(RayThrough(Float3{}), GizmoMode::Scale) == GizmoAxis::View);
    REQUIRE(g.BeginDrag(RayThrough(Float3{}), GizmoMode::Scale));
    d = g.UpdateScaleDrag(RayThrough(Float3{0.5f, 0.5f, 0.0f}));
    CHECK(d.x == doctest::Approx(d.y).epsilon(0.001f));
    CHECK(d.y == doctest::Approx(d.z).epsilon(0.001f));
    CHECK(d.x > 0.1f);
    g.EndDrag();
}

namespace
{
    // A scripted controller frame against the fixture camera.
    GizmoFrameInput Frame(Float3 through, bool pressed, bool down, bool released, bool snap = false)
    {
        GizmoFrameInput in;
        in.ray = RayThrough(through);
        in.cameraPosition = kCamPos;
        in.cameraForward = kCamFwd;
        in.leftPressed = pressed;
        in.leftDown = down;
        in.leftReleased = released;
        in.snap = snap;
        return in;
    }
}

TEST_CASE("gizmo-controller: a drag session is exactly one undo entry restoring the start")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    const Guid id = edit.CreateEntity(u8"Box");
    edit.EntitySelection().Set(id);
    commands.Clear(); // forget the create so undo counts below are the drags only

    // The gizmo scales with view depth: size = tan(fov/2) * 10 * 0.3 ~ 1.73. Grab the X shaft.
    const f32 size = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{});
    const Float3 grab{size * 0.6f, 0.0f, 0.0f};

    // Hover only: consumed (hot handle), no command yet.
    CHECK(ctl.Update(Frame(grab, false, false, false)));
    CHECK(ctl.Gizmo().Hovered() == GizmoAxis::X);
    CHECK_FALSE(commands.CanUndo());

    // Press, drag two moves, release: world +X by ~2.0 total.
    CHECK(ctl.Update(Frame(grab, true, true, false)));
    CHECK(ctl.Gizmo().IsDragging());
    CHECK(ctl.Update(Frame(grab + Float3{1.0f, 0, 0}, false, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{2.0f, 0, 0}, false, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{2.0f, 0, 0}, false, false, true)));
    CHECK_FALSE(ctl.Gizmo().IsDragging());

    const foundation::scene::EntityHandle e = edit.Resolve(id);
    CHECK(scene.GetLocalTransform(e).position.x == doctest::Approx(2.0f).epsilon(0.05f));

    // Exactly ONE undo entry; undo restores the exact start.
    REQUIRE(commands.CanUndo());
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x == doctest::Approx(0.0f));
    CHECK_FALSE(commands.CanUndo());
    commands.Redo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x ==
          doctest::Approx(2.0f).epsilon(0.05f));

    // A second drag is its OWN undo entry (LockGroup stops group coalescing).
    CHECK(ctl.Update(Frame(grab + Float3{2.0f, 0, 0}, true, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{3.0f, 0, 0}, false, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{3.0f, 0, 0}, false, false, true)));
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x ==
          doctest::Approx(3.0f).epsilon(0.05f));
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x ==
          doctest::Approx(2.0f).epsilon(0.05f));
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x == doctest::Approx(0.0f));
}

TEST_CASE("gizmo-controller: world drags convert into a rotated parent's space")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    // Parent rotated 90 degrees about Y; child at the origin.
    const Guid parentId = edit.CreateEntity(u8"Parent");
    const Guid childId = edit.CreateEntity(u8"Child", parentId);
    {
        core::Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, kPi * 0.5f);
        scene.SetLocalTransform(edit.Resolve(parentId), t);
    }
    edit.EntitySelection().Set(childId);

    const f32 size = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{});
    const Float3 grab{size * 0.6f, 0.0f, 0.0f};

    // World-space gizmo (default): drag along world +X by ~1.
    CHECK(ctl.Update(Frame(grab, false, false, false)));
    REQUIRE(ctl.Gizmo().Hovered() == GizmoAxis::X);
    CHECK(ctl.Update(Frame(grab, true, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{1.0f, 0, 0}, false, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{1.0f, 0, 0}, false, false, true)));

    // The child's WORLD position moved along world X even though its local axes are rotated.
    // (ComposeWorldMatrix: the cached GetWorldMatrix only updates on the scene tick.)
    const Float4x4 world = scene.ComposeWorldMatrix(edit.Resolve(childId));
    CHECK(world.m[3][0] == doctest::Approx(1.0f).epsilon(0.05f));
    CHECK(world.m[3][1] == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(world.m[3][2] == doctest::Approx(0.0f).epsilon(0.05f));

    // ... which means the LOCAL delta was conjugated into the parent's frame (not x).
    const core::Transform local = scene.GetLocalTransform(edit.Resolve(childId));
    CHECK(Abs(local.position.x) < 0.05f);
    CHECK(Abs(local.position.z) == doctest::Approx(1.0f).epsilon(0.05f));
}

TEST_CASE("gizmo-controller: mode keys, space toggle, and scale forcing local")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    const Guid id = edit.CreateEntity(u8"Thing");
    {
        core::Transform t;
        t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.7f);
        scene.SetLocalTransform(edit.Resolve(id), t);
    }
    edit.EntitySelection().Set(id);

    CHECK(ctl.Mode() == GizmoMode::Translate);
    CHECK(ctl.Space() == GizmoSpace::World);

    GizmoFrameInput in = Frame(Float3{5, 5, 0}, false, false, false);
    in.keyRotate = true;
    (void)ctl.Update(in);
    CHECK(ctl.Mode() == GizmoMode::Rotate);

    in = Frame(Float3{5, 5, 0}, false, false, false);
    in.keyScale = true;
    (void)ctl.Update(in);
    CHECK(ctl.Mode() == GizmoMode::Scale);

    // Scale ALWAYS uses the entity's orientation, even in World space.
    const Quaternion q = ctl.Gizmo().orientation;
    CHECK(Abs(q.y) > 0.1f);

    in = Frame(Float3{5, 5, 0}, false, false, false);
    in.keyTranslate = true;
    (void)ctl.Update(in);
    CHECK(ctl.Mode() == GizmoMode::Translate);
    // Back in World space, translate handles are world-aligned again.
    CHECK(ctl.Gizmo().orientation.y == doctest::Approx(0.0f).epsilon(0.001f));

    in = Frame(Float3{5, 5, 0}, false, false, false);
    in.keyToggleSpace = true;
    (void)ctl.Update(in);
    CHECK(ctl.Space() == GizmoSpace::Local);
    CHECK(Abs(ctl.Gizmo().orientation.y) > 0.1f); // local: entity orientation

    // No selection -> inactive, never consumes the mouse.
    edit.EntitySelection().Clear();
    CHECK_FALSE(ctl.Update(Frame(Float3{}, false, false, false)));
    CHECK_FALSE(ctl.IsActive());
}

TEST_CASE("gizmo-registry: renderers resolve by component type; unselected entities gated")
{
    GizmoRendererRegistry registry;
    RegisterBuiltinGizmoRenderers(registry);
    CHECK(registry.Count() == 14u); // +PhysicsCollider/ChildCollider/Character/Joint, +the three IK gizmos
    CHECK(registry.Find(&TypeOf<engine::animation::TwoBoneIkComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::animation::AimIkComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::animation::FootIkComponent>()) != nullptr);

    CHECK(registry.Find(&TypeOf<engine::render::LightComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::physics::RigidBodyComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::physics::ColliderComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::physics::CharacterComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::physics::JointComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::render::ReflectionProbeComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::render::CameraComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::render::DecalComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<engine::navigation::NavMeshZoneComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<f32>()) == nullptr);

    // Built-ins draw only when selected (design: unselected wireframes everywhere are noise).
    CHECK_FALSE(registry.Find(&TypeOf<engine::render::LightComponent>())->DrawWhenUnselected());
    CHECK_FALSE(
        registry.Find(&TypeOf<engine::navigation::NavMeshZoneComponent>())->DrawWhenUnselected());
}

TEST_CASE("gizmo-controller: pose tracks selection even while the pointer is off the viewport")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    const Guid a = edit.CreateEntity(u8"A");
    const Guid b = edit.CreateEntity(u8"B");
    {
        core::Transform t;
        t.position = Float3{5.0f, 0.0f, 0.0f};
        scene.SetLocalTransform(edit.Resolve(b), t);
    }

    GizmoFrameInput away = Frame(Float3{}, false, false, false);
    away.pointerValid = false;

    // Select A from the tree (no mouse anywhere near the viewport): the gizmo lands on A.
    edit.EntitySelection().Set(a);
    CHECK_FALSE(ctl.Update(away)); // pose-sync only, never consumes the mouse
    CHECK(ctl.IsActive());
    CHECK(ctl.Gizmo().position.x == doctest::Approx(0.0f));

    // Switch to B: the gizmo must follow immediately (the reported bug: it stayed on A
    // until the mouse re-entered the viewport).
    edit.EntitySelection().Set(b);
    (void)ctl.Update(away);
    CHECK(ctl.IsActive());
    CHECK(ctl.Gizmo().position.x == doctest::Approx(5.0f));

    // Hovering a handle, then leaving the viewport, clears the highlight.
    const f32 size = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{5, 0, 0});
    // Note: from this camera the gizmo sits far off-axis, so the ray toward the X shaft ALSO
    // grazes the foreshortened Z shaft (coplanar, ~zero 3D distance). The foreshortening
    // penalty must give the pick to X - the perpendicular, controllable axis.
    (void)ctl.Update(Frame(Float3{5.0f + size * 0.6f, 0, 0}, false, false, false));
    CHECK(ctl.Gizmo().Hovered() == GizmoAxis::X);
    (void)ctl.Update(away);
    CHECK(ctl.Gizmo().Hovered() == GizmoAxis::None);

    // Losing the pointer mid-drag finishes the drag cleanly: still exactly one undo entry.
    commands.Clear();
    CHECK(ctl.Update(Frame(Float3{5.0f + size * 0.6f, 0, 0}, true, true, false)));
    CHECK(ctl.Update(Frame(Float3{6.0f + size * 0.6f, 0, 0}, false, true, false)));
    REQUIRE(ctl.Gizmo().IsDragging());
    CHECK(ctl.Update(away)); // consumed: the drag is being closed out
    CHECK_FALSE(ctl.Gizmo().IsDragging());
    CHECK(scene.GetLocalTransform(edit.Resolve(b)).position.x ==
          doctest::Approx(6.0f).epsilon(0.05f));
    REQUIRE(commands.CanUndo());
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(b)).position.x ==
          doctest::Approx(5.0f).epsilon(0.01f));
    CHECK_FALSE(commands.CanUndo());
}

TEST_CASE("gizmo-controller: a pointer-less update follows an entity the simulation moves")
{
    // Simulate mode drives the gizmo with pointerValid=false every frame (read-only).
    // The reported bug: physics moved the selected box and the gizmo stayed at the
    // pre-play pose - the page skipped Update entirely while simulating.
    foundation::scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    const Guid box = edit.CreateEntity(u8"box");
    edit.EntitySelection().Set(box);

    GizmoFrameInput away = Frame(Float3{}, false, false, false);
    away.pointerValid = false;
    (void)ctl.Update(away);
    CHECK(ctl.Gizmo().position.y == doctest::Approx(0.0f));

    // "Physics" moves the entity (runtime transform write, no command).
    core::Transform t;
    t.position = Float3{0.0f, -3.0f, 2.0f};
    scene.SetLocalTransform(edit.Resolve(box), t);

    (void)ctl.Update(away);
    CHECK(ctl.IsActive());
    CHECK(ctl.Gizmo().position.y == doctest::Approx(-3.0f));
    CHECK(ctl.Gizmo().position.z == doctest::Approx(2.0f));
}

// The EDIT-TIME physics collider gizmo (approach b: draw the shape from component data + the entity
// transform, no physics world). Gated on the editor "Show Colliders" toggle (ctx.showColliders) -
// distinct from the runtime PhysicsSceneSettings.debugDraw.
TEST_CASE("component-gizmo: physics collider gizmo draws a box only when Show Colliders is on")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* bodies = scene.AddSystem<engine::physics::RigidBodyComponentManager>();
    const auto e = scene.CreateEntity(u8"Box");
    engine::physics::RigidBodyComponent& body = bodies->Add(e);
    body.shape = foundation::physics::ShapeKind::Box;
    body.halfExtents = Float3{1.0f, 2.0f, 0.5f};

    foundation::render::debug::DebugDraw dd;
    PhysicsColliderGizmoRenderer renderer;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;

    // Off -> nothing drawn (no world needed either way; this is pure shape data).
    ctx.showColliders = false;
    renderer.Draw(bodies->GetComponentInstance(e), e, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On -> the box wireframe lands as line segments (12 edges = 24 verts).
    ctx.showColliders = true;
    renderer.Draw(bodies->GetComponentInstance(e), e, ctx);
    CHECK(dd.LineVertices().Size() > 0);
}

// The EDIT-TIME joint gizmo (component data + transforms, no physics world): the anchor cross
// always; a link line only for a RESOLVED explicit target (nil = ancestor/world, not drawn); the
// axis arrow only for hinge/slider kinds. Same "Show Colliders" gate as the collider gizmos.
TEST_CASE("component-gizmo: joint gizmo - gated, anchor-only for a nil target, hinge adds the axis")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* joints = scene.AddSystem<engine::physics::JointComponentManager>();
    const auto e = scene.CreateEntity(u8"Jointed");
    engine::physics::JointComponent& joint = joints->Add(e);
    // Defaults: kind = Fixed, targetEntity nil -> the anchor-only branch (no link, no axis).

    foundation::render::debug::DebugDraw dd;
    JointGizmoRenderer renderer;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;

    // Off -> nothing drawn.
    ctx.showColliders = false;
    renderer.Draw(joints->GetComponentInstance(e), e, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On, nil target, Fixed kind: the anchor cross alone lands (non-empty).
    ctx.showColliders = true;
    renderer.Draw(joints->GetComponentInstance(e), e, ctx);
    CHECK(dd.HasAnyDraws());
    const usize anchorOnly = dd.LineVertices().Size();
    CHECK(anchorOnly > 0u);

    // Hinge kind: the rotation-axis arrow draws ON TOP of the cross (strictly more line verts).
    dd.Clear();
    joint.kind = foundation::physics::JointKind::Hinge;
    renderer.Draw(joints->GetComponentInstance(e), e, ctx);
    CHECK(dd.LineVertices().Size() > anchorOnly);
}

// The EDIT-TIME character-controller capsule (radius/halfHeight + entity transform, no live
// CharacterVirtual) - same "Show Colliders" gate as the collider gizmos.
TEST_CASE("component-gizmo: character capsule draws only when Show Colliders is on")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* characters = scene.AddSystem<engine::physics::CharacterComponentManager>();
    const auto e = scene.CreateEntity(u8"Hero");
    (void)characters->Add(e); // defaults: radius 0.35, halfHeight 0.55

    foundation::render::debug::DebugDraw dd;
    CharacterColliderGizmoRenderer renderer;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;

    // Off -> nothing drawn.
    ctx.showColliders = false;
    renderer.Draw(characters->GetComponentInstance(e), e, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On -> the capsule wireframe (cap spheres + bounds box) lands as line segments.
    ctx.showColliders = true;
    renderer.Draw(characters->GetComponentInstance(e), e, ctx);
    CHECK(dd.HasAnyDraws());
    CHECK(dd.LineVertices().Size() > 0);
}

// The EDIT-TIME compound-child collider (ColliderComponent): the runtime folds descendant colliders
// into the ancestor body, so "Show Colliders" must draw them at the CHILD's own transform too - a
// body-plus-children rig showing only the root shape reads as "the children are not registered".
TEST_CASE("component-gizmo: child collider draws its wireframe only when Show Colliders is on")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* colliders = scene.AddSystem<engine::physics::ColliderComponentManager>();
    const auto parent = scene.CreateEntity(u8"Body");
    const auto child = scene.CreateEntity(u8"Fist");
    scene.SetParent(child, parent);
    engine::physics::ColliderComponent& collider = colliders->Add(child);
    collider.shape = foundation::physics::ShapeKind::Box;
    collider.halfExtents = Float3{0.25f, 0.25f, 0.25f};

    foundation::render::debug::DebugDraw dd;
    ChildColliderGizmoRenderer renderer;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;

    // Off -> nothing drawn.
    ctx.showColliders = false;
    renderer.Draw(colliders->GetComponentInstance(child), child, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On -> the child's box wireframe lands as line segments.
    ctx.showColliders = true;
    renderer.Draw(colliders->GetComponentInstance(child), child, ctx);
    CHECK(dd.HasAnyDraws());
    CHECK(dd.LineVertices().Size() > 0);
}

// An EFFECTIVELY-inactive entity's physics gizmos draw DIMMED (uniform gray),
// never skipped - "the sim will ignore this" is the interesting information. The registry's
// DrawEntity sets ctx.entityEffectivelyActive from the scene.
TEST_CASE("component-gizmo: an inactive entity's collider draws dimmed gray, not skipped")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* bodies = scene.AddSystem<engine::physics::RigidBodyComponentManager>();
    const auto e = scene.CreateEntity(u8"Box");
    engine::physics::RigidBodyComponent& body = bodies->Add(e);
    body.shape = foundation::physics::ShapeKind::Box;
    body.motion = foundation::physics::MotionKind::Dynamic; // active color = green (R != G)

    GizmoRendererRegistry registry;
    RegisterBuiltinGizmoRenderers(registry);
    foundation::render::debug::DebugDraw dd;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;
    ctx.showColliders = true;

    scene.SetActive(e, false);
    registry.DrawEntity(e, /*selected*/ true, ctx);
    REQUIRE(dd.LineVertices().Size() > 0); // dimmed, NOT skipped
    const u32 packed = dd.LineVertices()[0].color;
    const u32 r = packed & 0xFFu;
    const u32 g = (packed >> 8) & 0xFFu;
    const u32 b = (packed >> 16) & 0xFFu;
    CHECK(r == g); // uniform gray = the dim color, not the dynamic green
    CHECK(g == b);

    // Re-activated: back to the normal (non-gray) palette.
    scene.SetActive(e, true);
    foundation::render::debug::DebugDraw dd2;
    ctx.debug = &dd2;
    registry.DrawEntity(e, /*selected*/ true, ctx);
    REQUIRE(dd2.LineVertices().Size() > 0);
    const u32 active = dd2.LineVertices()[0].color;
    CHECK(((active & 0xFFu) != ((active >> 8) & 0xFFu))); // green: R != G
}

// The capsule gizmo reads as a CAPSULE (two cap spheres + four side lines), not
// a sphere inside a bounding box.
TEST_CASE("component-gizmo: the capsule collider draws cap spheres + side lines, not a box")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* bodies = scene.AddSystem<engine::physics::RigidBodyComponentManager>();
    const auto e = scene.CreateEntity(u8"Cap");
    engine::physics::RigidBodyComponent& body = bodies->Add(e);
    body.shape = foundation::physics::ShapeKind::Capsule;
    body.radius = 0.5f;
    body.halfHeight = 1.0f;

    foundation::render::debug::DebugDraw dd;
    PhysicsColliderGizmoRenderer renderer;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;
    ctx.showColliders = true;
    renderer.Draw(bodies->GetComponentInstance(e), e, ctx);

    // Two wire spheres are far more segments than a 12-edge box (24 verts); the old drawing was
    // one sphere + one box. Also pin the FOUR side lines' verticality: at least 8 vertices sit
    // at exactly +-halfHeight on Y with |x|==radius or |z|==radius.
    CHECK(dd.LineVertices().Size() > 100u);
    usize sideVerts = 0;
    for (const auto& v : dd.LineVertices())
    {
        const bool atCapY = v.position.y == doctest::Approx(1.0f).epsilon(0.001) ||
                            v.position.y == doctest::Approx(-1.0f).epsilon(0.001);
        const bool onRim = Abs(Abs(v.position.x) - 0.5f) < 0.001f ||
                           Abs(Abs(v.position.z) - 0.5f) < 0.001f;
        if (atCapY && onRim)
        {
            ++sideVerts;
        }
    }
    CHECK(sideVerts >= 8u); // the four side lines' endpoints (plus any coincident ring verts)
}

// An IK component is seen before it runs: its chain where the bind pose stands (through the
// animator's model entity), its target, and an orange mark when a bone name is not in the skeleton.
TEST_CASE("component-gizmo: a two-bone IK chain draws in its bind pose, with its target")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    scene.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(scene);
    RefPtr<foundation::animation::Skeleton> skeleton = MakeRef<foundation::animation::Skeleton>(DefaultAllocator(), 3);
    {
        Array<foundation::animation::Bone>& bones = skeleton->Bones();
        const char8_t* names[] = {u8"Thigh", u8"Shin", u8"Foot"};
        const f32 ys[] = {1.0f, -0.5f, -0.5f};
        for (i32 i = 0; i < 3; ++i)
        {
            bones[static_cast<usize>(i)].index = i;
            bones[static_cast<usize>(i)].name = String(names[i]);
            bones[static_cast<usize>(i)].parentIndex = i - 1;
            bones[static_cast<usize>(i)].localBindPose.position = Float3{0, ys[i], 0};
        }
        skeleton->BuildNameMap();
        skeleton->FindRootBones();
        skeleton->BuildChildIndices();
        skeleton->ComputeInverseBindPoses();
    }
    const auto rider = scene.CreateEntity(u8"Rider");
    scene.SetLocalPosition(rider, Float3{10, 0, 0});
    scene.GetSystem<engine::animation::SkeletalAnimationComponentManager>()->Add(rider).skeleton.SetDirect(skeleton);
    const auto leg = scene.CreateEntity(u8"LegIk");
    scene.SetParent(leg, rider);
    auto* legs = scene.GetSystem<engine::animation::TwoBoneIkComponentManager>();
    engine::animation::TwoBoneIkComponent& c = legs->Add(leg);
    c.startBone = String(u8"Thigh");
    c.midBone = String(u8"Shin");
    c.endBone = String(u8"Foot");
    scene.UpdateTransforms();

    GizmoRendererRegistry registry;
    RegisterBuiltinGizmoRenderers(registry);
    foundation::render::debug::DebugDraw dd;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;
    registry.DrawEntity(leg, /*selected*/ true, ctx);
    // The chain's joints at x = 10, y 1, 0.5 and 0 (the bind pose through the rider).
    bool sawFoot = false;
    bool sawThigh = false;
    for (const auto& v : dd.OverlayLineVertices())
    {
        sawThigh = sawThigh || (Abs(v.position.x - 10.0f) < 1e-4f && Abs(v.position.y - 1.0f) < 1e-4f);
        sawFoot = sawFoot || (Abs(v.position.x - 10.0f) < 1e-4f && Abs(v.position.y) < 1e-4f);
    }
    CHECK(sawThigh);
    CHECK(sawFoot);

    // A bone the skeleton lacks: the chain breaks and the entity is marked orange (R > G).
    c.midBone = String(u8"Knee");
    foundation::render::debug::DebugDraw missing;
    ctx.debug = &missing;
    registry.DrawEntity(leg, /*selected*/ true, ctx);
    bool orange = false;
    for (const auto& v : missing.OverlayLineVertices())
    {
        orange = orange || ((v.color & 0xFFu) > ((v.color >> 8) & 0xFFu) + 40u);
    }
    CHECK(orange);
}

TEST_CASE("gizmo-controller: vertex snap puts the pivot on the vertex under the cursor")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);
    RefPtr<foundation::geometry::StaticMesh> cube = foundation::geometry::Primitives::Cube(DefaultAllocator(), 2.0f);

    // A cube to snap onto at (3, 0, 0): its face toward the camera at z = 1, corners (2..4, -1..1).
    const Guid targetId = edit.CreateEntity(u8"Wall");
    const foundation::scene::EntityHandle target = edit.Resolve(targetId);
    scene.SetLocalPosition(target, Float3{3, 0, 0});
    meshes->Add(target).mesh = cube;
    // The dragged box has a mesh of its own: once moved in front of the wall it must not snap
    // onto itself.
    const Guid id = edit.CreateEntity(u8"Box");
    meshes->Add(edit.Resolve(id)).mesh = cube;
    edit.EntitySelection().Set(id);

    const f32 size = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{});
    const Float3 grab{size * 0.6f, 0.0f, 0.0f}; // the X shaft
    const auto snapping = [](GizmoFrameInput in)
    {
        in.vertexSnap = true;
        return in;
    };

    REQUIRE(ctl.Update(Frame(grab, true, true, false)));
    // Over the wall's face near its (4, 1, 1) corner: the pivot exactly there, though the drag
    // holds the X shaft.
    CHECK(ctl.Update(snapping(Frame(Float3{3.8f, 0.8f, 1.0f}, false, true, false))));
    REQUIRE(ctl.SnapTarget().HasValue());
    Float3 at = scene.GetLocalTransform(edit.Resolve(id)).position;
    CHECK(at.x == doctest::Approx(4.0f));
    CHECK(at.y == doctest::Approx(1.0f));
    CHECK(at.z == doctest::Approx(1.0f));
    // Again with the box now in front of the wall: it never snaps onto itself.
    CHECK(ctl.Update(snapping(Frame(Float3{3.8f, 0.8f, 1.0f}, false, true, false))));
    at = scene.GetLocalTransform(edit.Resolve(id)).position;
    CHECK(at.x == doctest::Approx(4.0f));
    CHECK(at.y == doctest::Approx(1.0f));

    // Over no surface: the drag as it is (along X, y back to 0), and nothing marked.
    CHECK(ctl.Update(snapping(Frame(grab + Float3{0, 40.0f, 0}, false, true, false))));
    CHECK_FALSE(ctl.SnapTarget().HasValue());
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.y == doctest::Approx(0.0f));

    // Without vertex snap, over the wall: the plain axis drag.
    CHECK(ctl.Update(Frame(grab + Float3{1.0f, 0.5f, 0}, false, true, false)));
    CHECK_FALSE(ctl.SnapTarget().HasValue());
    at = scene.GetLocalTransform(edit.Resolve(id)).position;
    CHECK(at.x == doctest::Approx(1.0f).epsilon(0.05f));
    CHECK(at.y == doctest::Approx(0.0f));

    // One drag, one undo entry, back to the start.
    CHECK(ctl.Update(Frame(grab + Float3{1.0f, 0.5f, 0}, false, false, true)));
    CHECK_FALSE(ctl.SnapTarget().HasValue());
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x == doctest::Approx(0.0f));

    // The toolbar's mode snaps every drag, V or not.
    ctl.SetVertexSnapMode(true);
    REQUIRE(ctl.Update(Frame(grab, true, true, false)));
    CHECK(ctl.Update(Frame(Float3{3.8f, 0.8f, 1.0f}, false, true, false)));
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.y == doctest::Approx(1.0f));
    CHECK(ctl.Update(Frame(Float3{3.8f, 0.8f, 1.0f}, false, false, true)));
}
