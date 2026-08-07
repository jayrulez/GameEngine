// TransformGizmo + GizmoController headless tests: pick math (priorities, grazing disable,
// half-ring culling), drag math (translate/rotate/scale + snap quantization), and full scripted
// drag sessions against a live Scene (one undo entry per drag via group bracketing, exact
// start-transform restore, parent-space conversion, mode/space keys).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;
import draconic.engine.render;
import draconic.editor.core;
import draconic.editor.scene;

using namespace draconic::foundation;
using namespace draconic::editor;
namespace foundation = draconic::foundation;

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
    draconic::scene::Scene scene;
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

    const draconic::scene::EntityHandle e = edit.Resolve(id);
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
    draconic::scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    // Parent rotated 90 degrees about Y; child at the origin.
    const Guid parentId = edit.CreateEntity(u8"Parent");
    const Guid childId = edit.CreateEntity(u8"Child", parentId);
    {
        foundation::Transform t;
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
    const foundation::Transform local = scene.GetLocalTransform(edit.Resolve(childId));
    CHECK(Abs(local.position.x) < 0.05f);
    CHECK(Abs(local.position.z) == doctest::Approx(1.0f).epsilon(0.05f));
}

TEST_CASE("gizmo-controller: mode keys, space toggle, and scale forcing local")
{
    draconic::scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    const Guid id = edit.CreateEntity(u8"Thing");
    {
        foundation::Transform t;
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
    CHECK(registry.Count() == 4u);

    CHECK(registry.Find(&TypeOf<draconic::render::LightComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<draconic::render::ReflectionProbeComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<draconic::render::CameraComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<draconic::render::DecalComponent>()) != nullptr);
    CHECK(registry.Find(&TypeOf<f32>()) == nullptr);

    // Built-ins draw only when selected (design: unselected wireframes everywhere are noise).
    CHECK_FALSE(registry.Find(&TypeOf<draconic::render::LightComponent>())->DrawWhenUnselected());
}

TEST_CASE("gizmo-controller: pose tracks selection even while the pointer is off the viewport")
{
    draconic::scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    GizmoController ctl(edit);

    const Guid a = edit.CreateEntity(u8"A");
    const Guid b = edit.CreateEntity(u8"B");
    {
        foundation::Transform t;
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
    draconic::scene::Scene scene;
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
    foundation::Transform t;
    t.position = Float3{0.0f, -3.0f, 2.0f};
    scene.SetLocalTransform(edit.Resolve(box), t);

    (void)ctl.Update(away);
    CHECK(ctl.IsActive());
    CHECK(ctl.Gizmo().position.y == doctest::Approx(-3.0f));
    CHECK(ctl.Gizmo().position.z == doctest::Approx(2.0f));
}
