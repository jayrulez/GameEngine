// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Headless tests of TransformGizmo and GizmoController: the pick math (priorities, disabling
// grazing handles, half-ring culling), the drag math (translate, rotate and scale with snap
// quantization), and scripted drag sessions against a live Scene. A drag is one undo entry
// (bracketed as a group), undo restores the exact start transform, deltas convert into parent
// space, and the mode and space keys switch the gizmo.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.render;   // debug::DebugDraw (collider gizmo test)
import foundation.physics;  // ShapeKind
import engine.render;
import engine.navigation;
import engine.physics;      // RigidBodyComponent and its manager
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

    // Ring in the Z=0 plane, radius 0.8: a ray through a point on the circumference gives 0.
    Float3 hit;
    CHECK(TransformGizmo::RayRingDistance(RayThrough(Float3{0.8f, 0.0f, 0.0f}), Float3{},
                                          Float3{0, 0, 1}, 0.8f,
                                          &hit) == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(hit.x == doctest::Approx(0.8f).epsilon(0.01f));

    // A ring plane behind the ray is unreachable.
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
    // beats the axis handles by priority even though the axes pass nearby.
    CHECK(g.UpdateHover(RayThrough(Float3{0.4f, 0.4f, 0.0f}), GizmoMode::Translate) ==
          GizmoAxis::PlaneZ);

    // Grazing: the Z axis points straight at the camera, so it is disabled and unpickable. The
    // plane quads containing Z (PlaneX = YZ, PlaneY = XZ) are edge-on and also disabled.
    CHECK_FALSE(g.IsAxisEnabled(GizmoAxis::Z, GizmoMode::Translate));
    CHECK(g.IsAxisEnabled(GizmoAxis::X, GizmoMode::Translate));
    CHECK_FALSE(g.IsAxisEnabled(GizmoAxis::PlaneX, GizmoMode::Translate));
    CHECK_FALSE(g.IsAxisEnabled(GizmoAxis::PlaneY, GizmoMode::Translate));
    CHECK(g.IsAxisEnabled(GizmoAxis::PlaneZ, GizmoMode::Translate));

    // Empty space hovers nothing.
    CHECK(g.UpdateHover(RayThrough(Float3{3.0f, 3.0f, 0.0f}), GizmoMode::Translate) ==
          GizmoAxis::None);

    // Rotate mode: a 45-degree point on the Z ring circumference (radius 0.8). On-axis points
    // like (0.8, 0, 0) lie on two rings, so the fixture avoids them.
    CHECK(g.UpdateHover(RayThrough(Float3{0.566f, 0.566f, 0.0f}), GizmoMode::Rotate) ==
          GizmoAxis::Z);
    // The outer screen-space ring (radius 1.0, camera-facing).
    CHECK(g.UpdateHover(RayThrough(Float3{0.0f, 1.0f, 0.0f}), GizmoMode::Rotate) ==
          GizmoAxis::View);
}

TEST_CASE("gizmo: translate drag along an axis, on a plane, and snapped")
{
    TransformGizmo g = MakeGizmo();

    // Axis drag: grab the X shaft at x=0.7 and pull to x=1.9 for a delta of (1.2, 0, 0).
    REQUIRE(g.UpdateHover(RayThrough(Float3{0.7f, 0.0f, 0.0f}), GizmoMode::Translate) ==
            GizmoAxis::X);
    REQUIRE(g.BeginDrag(RayThrough(Float3{0.7f, 0.0f, 0.0f}), GizmoMode::Translate));
    Float3 delta = g.UpdateTranslateDrag(RayThrough(Float3{1.9f, 0.0f, 0.0f}));
    CHECK(delta.x == doctest::Approx(1.2f).epsilon(0.01f));
    CHECK(delta.y == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(delta.z == doctest::Approx(0.0f).epsilon(0.01f));

    // Snap quantizes the delta (translateSnap = 1.0): 1.2 becomes 1.0.
    delta = g.UpdateTranslateDrag(RayThrough(Float3{1.9f, 0.0f, 0.0f}), true);
    CHECK(delta.x == doctest::Approx(1.0f).epsilon(0.01f));
    g.EndDrag();

    // Plane drag: grab the XY quad and move diagonally; both components move.
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

    // Grab the Z ring at 45 degrees and sweep to 135 degrees for 90 degrees of rotation.
    const Float3 at45{0.566f, 0.566f, 0.0f};
    REQUIRE(g.UpdateHover(RayThrough(at45), GizmoMode::Rotate) == GizmoAxis::Z);
    REQUIRE(g.BeginDrag(RayThrough(at45), GizmoMode::Rotate));

    TransformGizmo::RotateDelta d = g.UpdateRotateDrag(RayThrough(Float3{-0.566f, 0.566f, 0.0f}));
    CHECK(Abs(d.axis.z) == doctest::Approx(1.0f).epsilon(0.01f));
    CHECK(Abs(d.angle) == doctest::Approx(kPi * 0.5f).epsilon(0.01f));

    // Sweeping to 170 degrees, then to -170 (190) degrees unwraps rather than jumping by 2pi.
    const f32 a170 = DegreesToRadians(170.0f);
    d = g.UpdateRotateDrag(RayThrough(Float3{0.8f * Cos(a170), 0.8f * Sin(a170), 0.0f}));
    const f32 before = d.angle;
    const f32 a190 = DegreesToRadians(190.0f);
    d = g.UpdateRotateDrag(RayThrough(Float3{0.8f * Cos(a190), 0.8f * Sin(a190), 0.0f}));
    CHECK(Abs(Abs(d.angle) - Abs(before)) < DegreesToRadians(45.0f)); // continuous across +-180

    // Snap quantizes to rotateSnapDegrees (15 degrees by default): about 100 degrees becomes 105
    // or 90.
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

    // The gizmo scales with view depth: size = tan(fov/2) * 10 * 0.3, about 1.73. Grab the X shaft.
    const f32 size = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{});
    const Float3 grab{size * 0.6f, 0.0f, 0.0f};

    // Hover only: the input is consumed (the handle is hot), but no command is pushed yet.
    CHECK(ctl.Update(Frame(grab, false, false, false)));
    CHECK(ctl.Gizmo().Hovered() == GizmoAxis::X);
    CHECK_FALSE(commands.CanUndo());

    // Press, drag over two moves, release: about 2.0 along world +X in total.
    CHECK(ctl.Update(Frame(grab, true, true, false)));
    CHECK(ctl.Gizmo().IsDragging());
    CHECK(ctl.Update(Frame(grab + Float3{1.0f, 0, 0}, false, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{2.0f, 0, 0}, false, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{2.0f, 0, 0}, false, false, true)));
    CHECK_FALSE(ctl.Gizmo().IsDragging());

    const foundation::scene::EntityHandle e = edit.Resolve(id);
    CHECK(scene.GetLocalTransform(e).position.x == doctest::Approx(2.0f).epsilon(0.05f));

    // Exactly one undo entry; undo restores the exact start.
    REQUIRE(commands.CanUndo());
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x == doctest::Approx(0.0f));
    CHECK_FALSE(commands.CanUndo());
    commands.Redo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x ==
          doctest::Approx(2.0f).epsilon(0.05f));

    // A second drag is its own undo entry (LockGroup stops group coalescing).
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

TEST_CASE("gizmo-controller: Select hides the handles until a mode; Snap snaps every drag as Ctrl does one")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);
    const Guid id = edit.CreateEntity(u8"Box");
    edit.EntitySelection().Set(id);
    const f32 size = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{});
    const Float3 grab{size * 0.6f, 0.0f, 0.0f};

    // Q: no handles, so the shaft is not hot and a click goes to picking.
    GizmoFrameInput in = Frame(grab, false, false, false);
    in.keySelect = true;
    CHECK_FALSE(ctl.Update(in));
    CHECK_FALSE(ctl.HandlesShown());
    CHECK_FALSE(ctl.IsActive());
    CHECK(ctl.Mode() == GizmoMode::Translate); // the mode is kept for when they show again
    CHECK_FALSE(ctl.Update(Frame(grab, true, true, false)));
    CHECK_FALSE(ctl.Gizmo().IsDragging());
    (void)ctl.Update(Frame(grab, false, false, true));

    // W brings them back.
    in = Frame(grab, false, false, false);
    in.keyTranslate = true;
    CHECK(ctl.Update(in));
    CHECK(ctl.HandlesShown());
    ctl.SetHandlesShown(false);
    ctl.SetMode(GizmoMode::Translate); // the bar's Move does the same
    CHECK(ctl.HandlesShown());

    // Snap on: a drag of 1.3 lands on the nearest 0.5 step, with no Ctrl.
    ctl.SetSnapMode(true);
    ctl.Gizmo().translateSnap = 0.5f;
    CHECK(ctl.Update(Frame(grab, true, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{1.3f, 0, 0}, false, true, false)));
    CHECK(ctl.Update(Frame(grab + Float3{1.3f, 0, 0}, false, false, true)));
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x == doctest::Approx(1.5f).epsilon(0.05f));
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

    // The child's world position moved along world X even though its local axes are rotated.
    // ComposeWorldMatrix is used because the cached GetWorldMatrix only updates on the scene tick.
    const Float4x4 world = scene.ComposeWorldMatrix(edit.Resolve(childId));
    CHECK(world.m[3][0] == doctest::Approx(1.0f).epsilon(0.05f));
    CHECK(world.m[3][1] == doctest::Approx(0.0f).epsilon(0.01f));
    CHECK(world.m[3][2] == doctest::Approx(0.0f).epsilon(0.05f));

    // So the local delta was conjugated into the parent's frame and is not along x.
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

    // Scale always uses the entity's orientation, even in World space.
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

    // With no selection the controller is inactive and never consumes the mouse.
    edit.EntitySelection().Clear();
    CHECK_FALSE(ctl.Update(Frame(Float3{}, false, false, false)));
    CHECK_FALSE(ctl.IsActive());
}

TEST_CASE("gizmo-registry: renderers resolve by component type; unselected entities gated")
{
    GizmoRendererRegistry registry;
    RegisterBuiltinGizmoRenderers(registry);
    CHECK(registry.Count() == 14u); // including the four physics and three IK gizmos
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

    // Built-ins draw only when selected; wireframes on every unselected entity would be noise.
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
    CHECK_FALSE(ctl.Update(away)); // only syncs the pose; never consumes the mouse
    CHECK(ctl.IsActive());
    CHECK(ctl.Gizmo().position.x == doctest::Approx(0.0f));

    // Switch to B: the gizmo follows immediately, without waiting for the mouse to re-enter
    // the viewport.
    edit.EntitySelection().Set(b);
    (void)ctl.Update(away);
    CHECK(ctl.IsActive());
    CHECK(ctl.Gizmo().position.x == doctest::Approx(5.0f));

    // Hovering a handle, then leaving the viewport, clears the highlight.
    const f32 size = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{5, 0, 0});
    // From this camera the gizmo sits far off-axis, so the ray toward the X shaft also grazes
    // the foreshortened Z shaft (coplanar, almost zero 3D distance). The foreshortening penalty
    // gives the pick to X, the perpendicular axis the user can control.
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
    // Simulate mode drives the gizmo with pointerValid=false every frame (read-only), so the
    // gizmo follows a selected box that physics moves instead of staying at the pre-play pose.
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

    // Stand in for physics moving the entity (a runtime transform write, no command).
    core::Transform t;
    t.position = Float3{0.0f, -3.0f, 2.0f};
    scene.SetLocalTransform(edit.Resolve(box), t);

    (void)ctl.Update(away);
    CHECK(ctl.IsActive());
    CHECK(ctl.Gizmo().position.y == doctest::Approx(-3.0f));
    CHECK(ctl.Gizmo().position.z == doctest::Approx(2.0f));
}

// The edit-time physics collider gizmo draws the shape from component data and the entity
// transform, with no physics world. It is gated on the editor's "Show Colliders" toggle
// (ctx.showColliders), which is separate from the runtime PhysicsSceneSettings.debugDraw.
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

    // Off: nothing is drawn (no world is needed either way; this is pure shape data).
    ctx.showColliders = false;
    renderer.Draw(bodies->GetComponentInstance(e), e, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On: the box wireframe lands as line segments (12 edges, 24 vertices).
    ctx.showColliders = true;
    renderer.Draw(bodies->GetComponentInstance(e), e, ctx);
    CHECK(dd.LineVertices().Size() > 0);
}

// The edit-time joint gizmo draws from component data and transforms, with no physics world. The
// anchor cross always draws; a link line draws only for a resolved explicit target (a nil target
// means the ancestor or the world and draws nothing); the axis arrow draws only for the hinge and
// slider kinds. It shares the "Show Colliders" gate with the collider gizmos.
TEST_CASE("component-gizmo: joint gizmo - gated, anchor-only for a nil target, hinge adds the axis")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* joints = scene.AddSystem<engine::physics::JointComponentManager>();
    const auto e = scene.CreateEntity(u8"Jointed");
    engine::physics::JointComponent& joint = joints->Add(e);
    // Defaults: kind = Fixed and a nil targetEntity, so only the anchor draws (no link, no axis).

    foundation::render::debug::DebugDraw dd;
    JointGizmoRenderer renderer;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;

    // Off: nothing is drawn.
    ctx.showColliders = false;
    renderer.Draw(joints->GetComponentInstance(e), e, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On, nil target, Fixed kind: the anchor cross alone lands (non-empty).
    ctx.showColliders = true;
    renderer.Draw(joints->GetComponentInstance(e), e, ctx);
    CHECK(dd.HasAnyDraws());
    const usize anchorOnly = dd.LineVertices().Size();
    CHECK(anchorOnly > 0u);

    // Hinge kind: the rotation-axis arrow draws in addition to the cross (strictly more line
    // vertices).
    dd.Clear();
    joint.kind = foundation::physics::JointKind::Hinge;
    renderer.Draw(joints->GetComponentInstance(e), e, ctx);
    CHECK(dd.LineVertices().Size() > anchorOnly);
}

// The edit-time character-controller capsule draws from radius, halfHeight and the entity
// transform, with no live CharacterVirtual. It shares the "Show Colliders" gate with the collider
// gizmos.
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

    // Off: nothing is drawn.
    ctx.showColliders = false;
    renderer.Draw(characters->GetComponentInstance(e), e, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On: the capsule wireframe lands as line segments.
    ctx.showColliders = true;
    renderer.Draw(characters->GetComponentInstance(e), e, ctx);
    CHECK(dd.HasAnyDraws());
    CHECK(dd.LineVertices().Size() > 0);
}

// The edit-time compound-child collider (ColliderComponent). The runtime folds descendant
// colliders into the ancestor body, so "Show Colliders" draws them at each child's own transform
// too; a rig that showed only the root shape would look as if its children were not registered.
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

    // Off: nothing is drawn.
    ctx.showColliders = false;
    renderer.Draw(colliders->GetComponentInstance(child), child, ctx);
    CHECK_FALSE(dd.HasAnyDraws());

    // On: the child's box wireframe lands as line segments.
    ctx.showColliders = true;
    renderer.Draw(colliders->GetComponentInstance(child), child, ctx);
    CHECK(dd.HasAnyDraws());
    CHECK(dd.LineVertices().Size() > 0);
}

// The physics gizmos of an effectively inactive entity draw dimmed (uniform gray) rather than
// being skipped, because knowing that the simulation will ignore the entity is useful. The
// registry's DrawEntity sets ctx.entityEffectivelyActive from the scene.
TEST_CASE("component-gizmo: an inactive entity's collider draws dimmed gray, not skipped")
{
    foundation::scene::Scene scene{DefaultAllocator()};
    auto* bodies = scene.AddSystem<engine::physics::RigidBodyComponentManager>();
    const auto e = scene.CreateEntity(u8"Box");
    engine::physics::RigidBodyComponent& body = bodies->Add(e);
    body.shape = foundation::physics::ShapeKind::Box;
    body.motion = foundation::physics::MotionKind::Dynamic; // active color is green (R != G)

    GizmoRendererRegistry registry;
    RegisterBuiltinGizmoRenderers(registry);
    foundation::render::debug::DebugDraw dd;
    GizmoContext ctx;
    ctx.scene = &scene;
    ctx.debug = &dd;
    ctx.showColliders = true;

    scene.SetActive(e, false);
    registry.DrawEntity(e, /*selected*/ true, ctx);
    REQUIRE(dd.LineVertices().Size() > 0); // dimmed, not skipped
    const u32 packed = dd.LineVertices()[0].color;
    const u32 r = packed & 0xFFu;
    const u32 g = (packed >> 8) & 0xFFu;
    const u32 b = (packed >> 16) & 0xFFu;
    CHECK(r == g); // uniform gray is the dim color, not the dynamic green
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

// The capsule gizmo draws as a capsule (two cap spheres and four side lines), not as a sphere
// inside a bounding box.
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

    // Two wire spheres are far more segments than a 12-edge box (24 vertices). The four side
    // lines are vertical: at least 8 vertices sit at exactly +-halfHeight on Y with |x| == radius
    // or |z| == radius.
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
    // The dragged box has a mesh of its own: once moved in front of the wall it does not snap
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

TEST_CASE("gizmo: under an orthographic view it keeps one size at every depth")
{
    // An orthographic view 12 m tall: half its height times the ratio, near or far, and hidden
    // behind the camera as in perspective.
    const f32 near = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{0, 0, 8}, 0.3f, 12.0f);
    const f32 far = TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{0, 0, -90}, 0.3f, 12.0f);
    CHECK(near == doctest::Approx(0.5f * 12.0f * 0.3f));
    CHECK(far == doctest::Approx(near));
    CHECK(TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{0, 0, 11}, 0.3f, 12.0f) <= 0.0f);
    // In perspective it grows with depth.
    CHECK(TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{0, 0, -90}) >
          TransformGizmo::ScreenScale(kCamPos, kCamFwd, 1.0472f, Float3{0, 0, 8}));
}
