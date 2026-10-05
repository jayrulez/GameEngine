// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :component_gizmos partition.
//
// IGizmoRenderer + registry: per-component-type viewport gizmos drawn through debug-draw.
// Ported from Sedulous.Editor (IGizmoRenderer/GizmoContext + the light and
// reflection-probe renderers) with fixes for our components:
//   - the probe gizmo draws a wire BOX from halfExtents (our probes are boxes; Sedulous drew an
//     influence sphere);
//   - DrawWhenUnselected defaults to false (Sedulous drew every light's range sphere always -
//     noisy; entity markers already anchor unselected entities).
// The interface is type-erased on reflection Instances (no Component base class here), so a
// renderer looks its data up from the manager's GetComponentInstance.

module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.render;
import foundation.geometry;
import engine.render;
import engine.navigation;
import engine.spline;
import engine.animation;
import foundation.animation; // kMaxAimBones
import foundation.spline;
import engine.physics;          // RigidBodyComponent (edit-time collider gizmo)
import foundation.physics;      // ShapeKind / MotionKind
import foundation.heightfield;  // Heightfield (ShapeKind::Heightfield bounds)
import foundation.physics.resource; // CollisionShape (ShapeKind::Cooked outline)

using namespace foundation::core;
namespace geometry = foundation::geometry;
namespace render = foundation::render;
namespace scene = foundation::scene;

namespace editor
{
    const TypeInfo* CameraGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::render::CameraComponent>();
    }

    void CameraGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                   GizmoContext& ctx)
    {
        const auto* camera = component.TryGet<engine::render::CameraComponent>();
        if (camera == nullptr)
        {
            return;
        }

        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        const Float3 position = detail::WorldPosition(world);
        const Float3 forward = detail::WorldForward(world);
        const Float3 up = (Abs(forward.y) < 0.99f) ? Float3{0, 1, 0} : Float3{0, 0, 1};

        // Short preview frustum (clamped far) so scene cameras stay readable; an orthographic
        // camera draws as the box it sees.
        engine::render::CameraComponent preview = *camera;
        preview.nearZ = Max(camera->nearZ, 0.01f);
        preview.farZ = Min(camera->farZ, 8.0f);
        const Float4x4 view = Float4x4::LookAtRH(position, position + forward, up);
        const Float4x4 proj = engine::render::MakeCameraProjection(preview, camera->aspect);
        ctx.debug->DrawFrustum(Inverse(view * proj), Color{0.9f, 0.9f, 0.9f, 1.0f});
    }
    const TypeInfo* DecalGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::render::DecalComponent>();
    }

    void DecalGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                  GizmoContext& ctx)
    {
        const auto* decal = component.TryGet<engine::render::DecalComponent>();
        if (decal == nullptr)
        {
            return;
        }

        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        const Float3 position = detail::WorldPosition(world);
        render::debug::DebugDraw& dd = *ctx.debug;
        const Color color{1.0f, 0.75f, 0.2f, 1.0f};

        const Float3 he = decal->size * 0.5f;
        dd.DrawTransformedBox(Float3{} - he, he, world, color);
        // Projection direction: local +Z (the opposite of the camera-style forward).
        const Float3 projDir = Float3{} - detail::WorldForward(world);
        dd.DrawArrow(position, position + projDir * (he.z + 0.35f), color, 0.12f);
    }

    const TypeInfo* NavMeshZoneGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::navigation::NavMeshZoneComponent>();
    }
    void NavMeshZoneGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                        GizmoContext& ctx)
    {
        const auto* zone = component.TryGet<engine::navigation::NavMeshZoneComponent>();
        if (zone == nullptr)
        {
            return;
        }
        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        // The bake region: the entity-oriented box of the component's half-extents.
        ctx.debug->DrawTransformedBox(Float3{} - zone->extents, zone->extents, world,
                                      Color{0.20f, 0.85f, 1.0f, 1.0f});
    }
    const TypeInfo* SplineGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::spline::SplineComponent>();
    }
    void SplineGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                   GizmoContext& ctx)
    {
        const auto* spline = component.TryGet<engine::spline::SplineComponent>();
        if (spline == nullptr)
        {
            return;
        }
        const auto& curve = spline->curve;
        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        const Color curveColor{0.35f, 0.85f, 1.0f, 1.0f};
        const u32 segments = curve.SegmentCount();
        if (segments > 0)
        {
            const u32 steps = segments * foundation::spline::SplineCurve::kSamplesPerSegment;
            Float3 previous = TransformPoint(curve.Evaluate(0.0f), world);
            for (u32 i = 1; i <= steps; ++i)
            {
                const f32 t =
                    curve.MaxT() * static_cast<f32>(i) / static_cast<f32>(steps);
                const Float3 position = TransformPoint(curve.Evaluate(t), world);
                ctx.debug->DrawLine(previous, position, curveColor);
                previous = position;
            }
        }
        for (const foundation::spline::SplinePoint& point : curve.points)
        {
            ctx.debug->DrawWireSphere(TransformPoint(point.position, world), 0.1f,
                                      Color{1.0f, 0.85f, 0.2f, 1.0f}, 12);
        }
    }

    void GizmoRendererRegistry::Register(UniquePtr<IGizmoRenderer> renderer)
    {
        if (renderer)
        {
            m_renderers.PushBack(Move(renderer));
        }
    }

    IGizmoRenderer* GizmoRendererRegistry::Find(const TypeInfo* componentType) const
    {
        for (const UniquePtr<IGizmoRenderer>& r : m_renderers)
        {
            if (r->ComponentType() == componentType)
            {
                return r.Get();
            }
        }
        return nullptr;
    }

    void GizmoRendererRegistry::DrawEntity(scene::EntityHandle entity, bool selected,
                                           GizmoContext& ctx) const
    {
        if (ctx.scene == nullptr || ctx.debug == nullptr || !entity.IsAssigned())
        {
            return;
        }
        ctx.entityEffectivelyActive = ctx.scene->IsEffectivelyActive(entity);
        ctx.scene->ForEachManager(
            [&](scene::ComponentManagerBase& mgr)
            {
                if (!mgr.HasComponent(entity))
                {
                    return;
                }
                IGizmoRenderer* renderer = Find(mgr.ComponentType());
                if (renderer == nullptr)
                {
                    return;
                }
                if (!selected && !renderer->DrawWhenUnselected())
                {
                    return;
                }
                const Instance component = mgr.GetComponentInstance(entity);
                if (!component.IsEmpty())
                {
                    renderer->Draw(component, entity, ctx);
                }
            });
    }
    const TypeInfo* LightGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::render::LightComponent>();
    }

    void LightGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                  GizmoContext& ctx)
    {
        const auto* light = component.TryGet<engine::render::LightComponent>();
        if (light == nullptr)
        {
            return;
        }

        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        const Float3 position = detail::WorldPosition(world);
        const Float3 forward = detail::WorldForward(world);
        render::debug::DebugDraw& dd = *ctx.debug;
        const Color color{Clamp(light->color.r, 0.0f, 1.0f), Clamp(light->color.g, 0.0f, 1.0f),
                          Clamp(light->color.b, 0.0f, 1.0f), 1.0f};

        switch (light->type)
        {
        case engine::render::LightType::Directional:
        {
            detail::DrawCenterCross(dd, position, 0.3f, color);
            const Float3 tip = position + forward * 1.5f;
            dd.DrawArrow(position, tip, color, 0.2f);
            break;
        }
        case engine::render::LightType::Point:
        {
            dd.DrawWireSphere(position, light->range, color, 24);
            detail::DrawCenterCross(dd, position, 0.15f, color);
            break;
        }
        case engine::render::LightType::Spot:
        {
            const f32 tipDist = Max(light->range, 0.1f);
            const Float3 tipCenter = position + forward * tipDist;
            const f32 tipRadius = tipDist * Tan(light->outerAngle);

            const Float3 up = (Abs(forward.y) < 0.99f) ? Float3{0, 1, 0} : Float3{0, 0, 1};
            const Float3 right = Normalized(Cross(forward, up));
            const Float3 trueUp = Cross(right, forward);

            dd.DrawCircle(tipCenter, right, trueUp, tipRadius, color, 24);
            dd.DrawLine(position, tipCenter + right * tipRadius, color);
            dd.DrawLine(position, tipCenter - right * tipRadius, color);
            dd.DrawLine(position, tipCenter + trueUp * tipRadius, color);
            dd.DrawLine(position, tipCenter - trueUp * tipRadius, color);
            break;
        }
        }
    }
    const TypeInfo* ReflectionProbeGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::render::ReflectionProbeComponent>();
    }

    void ReflectionProbeGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                            GizmoContext& ctx)
    {
        const auto* probe = component.TryGet<engine::render::ReflectionProbeComponent>();
        if (probe == nullptr)
        {
            return;
        }

        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        const Float3 position = detail::WorldPosition(world);
        render::debug::DebugDraw& dd = *ctx.debug;
        const Color color{0.4f, 0.8f, 1.0f, 1.0f};

        dd.DrawTransformedBox(Float3{} - probe->halfExtents, probe->halfExtents, world, color);
        detail::DrawCenterCross(dd, position, 0.2f, color);
    }

    const TypeInfo* LodOverlayGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::render::MeshComponent>();
    }

    void LodOverlayGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                       GizmoContext& ctx)
    {
        if (!ctx.lodOverlay || ctx.viewCamera == nullptr)
        {
            return;
        }
        const auto* mc = component.TryGet<engine::render::MeshComponent>();
        if (mc == nullptr)
        {
            return;
        }
        const geometry::StaticMesh* mesh = mc->mesh.Get();
        if (mesh == nullptr || mesh->lodCount <= 1)
        {
            return;
        }
        // World bounds the way extraction sees them: local AABB center through the entity
        // world; radius = half the transformed box diagonal (conservative, close enough
        // for the same pick the renderer makes).
        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        const Float3 center = TransformPoint(mesh->bounds.Center(), world);
        Float3 minCorner = center;
        Float3 maxCorner = center;
        const Float3 lo = mesh->bounds.min;
        const Float3 hi = mesh->bounds.max;
        for (u32 corner = 0; corner < 8; ++corner)
        {
            const Float3 local{(corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                               (corner & 4) ? hi.z : lo.z};
            const Float3 p = TransformPoint(local, world);
            minCorner = Float3{Min(minCorner.x, p.x), Min(minCorner.y, p.y), Min(minCorner.z, p.z)};
            maxCorner = Float3{Max(maxCorner.x, p.x), Max(maxCorner.y, p.y), Max(maxCorner.z, p.z)};
        }
        const f32 radius = 0.5f * Length(maxCorner - minCorner);
        const f32 coverage =
            render::LodCoverageFor(*ctx.viewCamera, center, radius, mc->lodBias);
        const u32 maxLod = mesh->lodCount - 1;
        const u32 level =
            (mc->forceLod >= 0)
                ? ((static_cast<u32>(mc->forceLod) < maxLod) ? static_cast<u32>(mc->forceLod)
                                                             : maxLod)
                : render::PickLodLevel(*mesh, coverage);
        static const Color kLevelColors[] = {
            Color{0.3f, 0.9f, 0.3f, 1.0f}, // 0 = green (finest)
            Color{0.95f, 0.9f, 0.2f, 1.0f}, // 1 = yellow
            Color{1.0f, 0.6f, 0.15f, 1.0f}, // 2 = orange
            Color{1.0f, 0.25f, 0.2f, 1.0f}, // 3+ = red
        };
        const Color color = kLevelColors[(level < 3u) ? level : 3u];
        ctx.debug->DrawTransformedBox(mesh->bounds.min, mesh->bounds.max, world, color);
    }

    namespace
    {
        // Shared shape-wireframe drawing for the body + child-collider gizmos: one shape at the
        // entity's world transform (rigid frame for primitives, scaled for cooked outlines -
        // matching how the runtime builds the Jolt shapes).
        struct ColliderShapeDraw
        {
            foundation::physics::ShapeKind shape = foundation::physics::ShapeKind::Box;
            Float3 halfExtents{0.5f, 0.5f, 0.5f};
            f32 radius = 0.5f;
            f32 halfHeight = 0.5f;
            f32 planeHalfExtent = 1000.0f;
            const foundation::physics::CollisionShape* cooked = nullptr;
            const foundation::heightfield::Heightfield* heightfield = nullptr;
        };

        // A capsule wireframe that READS as a capsule: cap spheres at +-halfHeight plus four
        // side lines. (The old sphere-inside-a-bounding-box drawing read as a box collider with
        // a sphere in it.)
        void DrawWireCapsule(render::debug::DebugDraw& dd, const Float4x4& frame, f32 radius,
                             f32 halfHeight, const Color& color)
        {
            const Float3 top = TransformPoint(Float3{0, halfHeight, 0}, frame);
            const Float3 bottom = TransformPoint(Float3{0, -halfHeight, 0}, frame);
            dd.DrawWireSphere(top, radius, color);
            dd.DrawWireSphere(bottom, radius, color);
            const Float3 offsets[4] = {
                {radius, 0, 0}, {-radius, 0, 0}, {0, 0, radius}, {0, 0, -radius}};
            for (const Float3& o : offsets)
            {
                dd.DrawLine(TransformPoint(Float3{o.x, -halfHeight, o.z}, frame),
                            TransformPoint(Float3{o.x, halfHeight, o.z}, frame), color);
            }
        }

        void DrawColliderShape(const ColliderShapeDraw& s, const Float4x4& world,
                               const Color& color, render::debug::DebugDraw& dd)
        {
            using foundation::physics::ShapeKind;
            Float3 position, scale;
            Quaternion rotation;
            if (!Decompose(world, position, rotation, scale))
            {
                return;
            }
            // The rigid (unscaled) frame - primitive sizes are absolute half-extents/radii, like
            // the runtime.
            const Float4x4 rigid = Transform{position, rotation, Float3{1, 1, 1}}.ToMatrix();
            switch (s.shape)
            {
            case ShapeKind::Box:
                dd.DrawTransformedBox(Float3{} - s.halfExtents, s.halfExtents, rigid, color);
                break;
            case ShapeKind::Sphere:
                dd.DrawWireSphere(position, s.radius, color);
                break;
            case ShapeKind::Capsule:
                DrawWireCapsule(dd, rigid, s.radius, s.halfHeight, color);
                break;
            case ShapeKind::Plane:
            {
                const f32 extent = s.planeHalfExtent < 25.0f ? s.planeHalfExtent : 25.0f;
                const i32 kCells = 10;
                for (i32 g = -kCells; g <= kCells; ++g)
                {
                    const f32 off = extent * static_cast<f32>(g) / kCells;
                    dd.DrawLine(TransformPoint(Float3{off, 0, -extent}, rigid),
                                TransformPoint(Float3{off, 0, extent}, rigid), color);
                    dd.DrawLine(TransformPoint(Float3{-extent, 0, off}, rigid),
                                TransformPoint(Float3{extent, 0, off}, rigid), color);
                }
                break;
            }
            case ShapeKind::Heightfield:
                if (s.heightfield != nullptr)
                {
                    const Float2 ws = s.heightfield->WorldSize();
                    dd.DrawTransformedBox(
                        Float3{-ws.x * 0.5f, s.heightfield->MinY(), -ws.y * 0.5f},
                        Float3{ws.x * 0.5f, s.heightfield->MaxY(), ws.y * 0.5f}, rigid, color);
                }
                break;
            case ShapeKind::Cooked:
                if (s.cooked != nullptr)
                {
                    // Outline is authored unit-scale; re-apply the entity's scale.
                    const Float4x4 shapeMatrix = Transform{position, rotation, scale}.ToMatrix();
                    const Array<Float3>& outline = s.cooked->outline;
                    for (usize t = 0; t + 2 < outline.Size(); t += 3)
                    {
                        const Float3 a = TransformPoint(outline[t + 0], shapeMatrix);
                        const Float3 b = TransformPoint(outline[t + 1], shapeMatrix);
                        const Float3 d = TransformPoint(outline[t + 2], shapeMatrix);
                        dd.DrawLine(a, b, color);
                        dd.DrawLine(b, d, color);
                        dd.DrawLine(d, a, color);
                    }
                }
                break;
            }
        }
    }

    const TypeInfo* PhysicsColliderGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::physics::RigidBodyComponent>();
    }

    // Draw the collider's shape wireframe from the component data + the entity's world transform -
    // no physics world, no Jolt body (edit time). Colours are STATIC (dynamic=green, static/
    // kinematic=blue, trigger=yellow) since there is no live body to query awake/sleeping.
    void PhysicsColliderGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                            GizmoContext& ctx)
    {
        if (!ctx.showColliders) // the editor "Show Colliders" toggle (runtime debug draw is separate)
        {
            return;
        }
        const auto* body = component.TryGet<engine::physics::RigidBodyComponent>();
        if (body == nullptr)
        {
            return;
        }
        using foundation::physics::MotionKind;

        const Color color = !ctx.entityEffectivelyActive
                                ? Color{0.45f, 0.45f, 0.45f, 1.0f} // dimmed: sim will skip it
                            : body->isTrigger ? Color{1.0f, 0.8f, 0.2f, 1.0f}
                            : body->motion == MotionKind::Dynamic ? Color{0.3f, 1.0f, 0.4f, 1.0f}
                                                                  : Color{0.4f, 0.6f, 1.0f, 1.0f};
        ColliderShapeDraw s;
        s.shape = body->shape;
        s.halfExtents = body->halfExtents;
        s.radius = body->radius;
        s.halfHeight = body->halfHeight;
        s.planeHalfExtent = body->planeHalfExtent;
        s.cooked = body->collisionShape.Get();
        s.heightfield = body->heightfield.Get();
        DrawColliderShape(s, ctx.scene->GetWorldMatrix(owner), color, *ctx.debug);
    }

    const TypeInfo* ChildColliderGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::physics::ColliderComponent>();
    }

    void ChildColliderGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                          GizmoContext& ctx)
    {
        if (!ctx.showColliders)
        {
            return;
        }
        const auto* collider = component.TryGet<engine::physics::ColliderComponent>();
        if (collider == nullptr)
        {
            return;
        }
        // Compound teal: visually distinct from the body's own shape, so a rig reads as
        // "one body + its folded children", not one anonymous pile of wireframes. Dimmed when
        // the entity is effectively inactive (the sim never folds it in).
        const Color color = ctx.entityEffectivelyActive ? Color{0.25f, 0.85f, 0.8f, 1.0f}
                                                        : Color{0.45f, 0.45f, 0.45f, 1.0f};
        ColliderShapeDraw s;
        s.shape = collider->shape;
        s.halfExtents = collider->halfExtents;
        s.radius = collider->radius;
        s.halfHeight = collider->halfHeight;
        s.planeHalfExtent = collider->planeHalfExtent;
        s.cooked = collider->collisionShape.Get();
        s.heightfield = collider->heightfield.Get();
        DrawColliderShape(s, ctx.scene->GetWorldMatrix(owner), color, *ctx.debug);
    }

    const TypeInfo* CharacterColliderGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::physics::CharacterComponent>();
    }

    // The character controller's capsule from radius/halfHeight at the entity position. A fixed
    // colour (edit time has no live controller to query the on-ground state).
    void CharacterColliderGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                              GizmoContext& ctx)
    {
        if (!ctx.showColliders)
        {
            return;
        }
        const auto* ch = component.TryGet<engine::physics::CharacterComponent>();
        if (ch == nullptr)
        {
            return;
        }
        const Float3 position = detail::WorldPosition(ctx.scene->GetWorldMatrix(owner));
        const Color color = ctx.entityEffectivelyActive
                                ? Color{0.2f, 0.9f, 0.9f, 1.0f}  // cyan
                                : Color{0.45f, 0.45f, 0.45f, 1.0f}; // dimmed: sim will skip it
        const Float4x4 frame =
            Transform{position, Quaternion::Identity, Float3::One}.ToMatrix();
        DrawWireCapsule(*ctx.debug, frame, ch->radius, ch->halfHeight, color);
    }

    const TypeInfo* JointGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::physics::JointComponent>();
    }

    // The joint anchor (cross), a link line to the connected target (nil target = ancestor/world,
    // not drawn), and the hinge/slider axis (arrow) - all from JointComponent data + the transform.
    void JointGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner,
                                  GizmoContext& ctx)
    {
        if (!ctx.showColliders)
        {
            return;
        }
        const auto* joint = component.TryGet<engine::physics::JointComponent>();
        if (joint == nullptr)
        {
            return;
        }
        using foundation::physics::JointKind;
        const Float4x4 world = ctx.scene->GetWorldMatrix(owner);
        const Float3 anchor = TransformPoint(joint->localAnchor, world);
        const Color color = ctx.entityEffectivelyActive
                                ? Color{1.0f, 0.5f, 0.1f, 1.0f}     // orange
                                : Color{0.45f, 0.45f, 0.45f, 1.0f}; // dimmed: sim will skip it
        render::debug::DebugDraw& dd = *ctx.debug;
        dd.DrawCross(anchor, 0.25f, color);
        // Link to the connected body (nil target = nearest ancestor / world; only drawn if resolved).
        if (!joint->targetEntity.id.IsNil())
        {
            const scene::EntityHandle target = ctx.scene->FindEntity(joint->targetEntity.id);
            if (target.IsAssigned())
            {
                dd.DrawLine(anchor, detail::WorldPosition(ctx.scene->GetWorldMatrix(target)), color);
            }
        }
        // Hinge = rotation axis; Slider = slide direction. A short arrow either side of the anchor.
        if (joint->kind == JointKind::Hinge || joint->kind == JointKind::Slider)
        {
            const Float3 axisEnd = TransformPoint(joint->localAnchor + joint->localAxis, world);
            Float3 dir = axisEnd - anchor;
            const f32 len = Length(dir);
            if (len > 1e-4f)
            {
                dir = dir * (1.0f / len);
                dd.DrawArrow(anchor - dir * 0.5f, anchor + dir * 0.5f, color, 0.1f);
            }
        }
    }
}

namespace editor
{
    namespace
    {
        const Color kIkChain{0.3f, 0.7f, 1.0f, 1.0f};
        const Color kIkTarget{0.2f, 0.9f, 0.3f, 1.0f};
        const Color kIkPole{0.8f, 0.4f, 1.0f, 1.0f};
        const Color kIkMissing{1.0f, 0.55f, 0.1f, 1.0f};

        // A chain of named bones where the bind pose stands, joint spheres and links; a name the
        // skeleton lacks breaks the line there and is marked orange at the entity.
        void DrawBindChain(GizmoContext& ctx, scene::EntityHandle owner,
                           const engine::animation::IkAuthoringAnimator& animator, Span<const StringView> bones,
                           Float3* outLast = nullptr)
        {
            bool havePrevious = false;
            Float3 previous{};
            for (const StringView bone : bones)
            {
                Float3 at;
                if (!engine::animation::IkBindBoneWorld(*ctx.scene, animator, bone, at))
                {
                    ctx.debug->DrawWireSphere(ctx.scene->GetWorldPosition(owner), 0.08f, kIkMissing, 10, true);
                    havePrevious = false;
                    continue;
                }
                if (havePrevious)
                {
                    ctx.debug->DrawLine(previous, at, kIkChain, true);
                }
                ctx.debug->DrawWireSphere(at, 0.02f, kIkChain, 8, true);
                previous = at;
                havePrevious = true;
                if (outLast != nullptr)
                {
                    *outLast = at;
                }
            }
        }

        bool EntityAt(GizmoContext& ctx, const foundation::scene::EntityRef& ref, Float3& out)
        {
            if (ref.IsNil())
            {
                return false;
            }
            const scene::EntityHandle e = ctx.scene->FindEntity(ref.id);
            if (!ctx.scene->IsValid(e))
            {
                return false;
            }
            out = ctx.scene->GetWorldPosition(e);
            return true;
        }

        // While the scene runs the component draws its own solve; true when it did.
        bool DrawSolving(GizmoContext& ctx, const engine::animation::IkRuntime& runtime)
        {
            if (runtime.status != engine::animation::IkStatus::Solving || runtime.modifier.Get() == nullptr ||
                !runtime.modifier->solved)
            {
                return false;
            }
            engine::animation::DrawIk(*ctx.debug, runtime);
            return true;
        }
    }

    const TypeInfo* TwoBoneIkGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::animation::TwoBoneIkComponent>();
    }
    void TwoBoneIkGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx)
    {
        const auto* c = component.TryGet<engine::animation::TwoBoneIkComponent>();
        if (c == nullptr || DrawSolving(ctx, c->runtime))
        {
            return;
        }
        engine::animation::IkAuthoringAnimator animator;
        if (!engine::animation::FindIkAuthoringAnimator(*ctx.scene, owner, animator))
        {
            ctx.debug->DrawWireSphere(ctx.scene->GetWorldPosition(owner), 0.08f, kIkMissing, 10, true);
            return;
        }
        const StringView bones[] = {c->startBone.AsView(), c->midBone.AsView(), c->endBone.AsView()};
        DrawBindChain(ctx, owner, animator, bones);
        Float3 target;
        if (!EntityAt(ctx, c->target, target))
        {
            target = ctx.scene->GetWorldPosition(owner);
        }
        ctx.debug->DrawWireSphere(target, 0.05f, kIkTarget, 12, true);
        Float3 pole;
        Float3 mid;
        if (EntityAt(ctx, c->pole, pole) &&
            engine::animation::IkBindBoneWorld(*ctx.scene, animator, c->midBone.AsView(), mid))
        {
            ctx.debug->DrawLine(mid, pole, kIkPole, true);
        }
    }

    const TypeInfo* AimIkGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::animation::AimIkComponent>();
    }
    void AimIkGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx)
    {
        const auto* c = component.TryGet<engine::animation::AimIkComponent>();
        if (c == nullptr || DrawSolving(ctx, c->runtime))
        {
            return;
        }
        engine::animation::IkAuthoringAnimator animator;
        if (!engine::animation::FindIkAuthoringAnimator(*ctx.scene, owner, animator))
        {
            ctx.debug->DrawWireSphere(ctx.scene->GetWorldPosition(owner), 0.08f, kIkMissing, 10, true);
            return;
        }
        StringView bones[foundation::animation::kMaxAimBones];
        usize count = 0;
        for (const engine::animation::AimIkBone& b : c->bones)
        {
            if (count < foundation::animation::kMaxAimBones)
            {
                bones[count++] = b.bone.AsView();
            }
        }
        Float3 last{};
        DrawBindChain(ctx, owner, animator, Span<const StringView>{bones, count}, &last);
        Float3 target;
        if (!EntityAt(ctx, c->target, target))
        {
            target = ctx.scene->GetWorldPosition(owner);
        }
        ctx.debug->DrawWireSphere(target, 0.05f, kIkTarget, 12, true);
        if (count > 0)
        {
            ctx.debug->DrawLine(last, target, kIkTarget, true);
        }
        Float3 up;
        if (count > 0 && EntityAt(ctx, c->up, up))
        {
            ctx.debug->DrawLine(last, up, kIkPole, true);
        }
    }

    const TypeInfo* FootIkGizmoRenderer::ComponentType() const
    {
        return &TypeOf<engine::animation::FootIkComponent>();
    }
    void FootIkGizmoRenderer::Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx)
    {
        const auto* c = component.TryGet<engine::animation::FootIkComponent>();
        if (c == nullptr || DrawSolving(ctx, c->runtime))
        {
            return;
        }
        engine::animation::IkAuthoringAnimator animator;
        if (!engine::animation::FindIkAuthoringAnimator(*ctx.scene, owner, animator))
        {
            ctx.debug->DrawWireSphere(ctx.scene->GetWorldPosition(owner), 0.08f, kIkMissing, 10, true);
            return;
        }
        // Each leg, and the reach of its ground probe (above and below the foot, along the
        // model's up).
        const Float3 up = Normalized(TransformDirection(Float3{0, 1, 0},
                                                        ctx.scene->GetWorldMatrix(animator.modelEntity)));
        for (const engine::animation::FootIkLegBones& leg : c->legs)
        {
            const StringView bones[] = {leg.startBone.AsView(), leg.midBone.AsView(), leg.endBone.AsView()};
            Float3 foot{};
            DrawBindChain(ctx, owner, animator, bones, &foot);
            if (engine::animation::IkBindBoneWorld(*ctx.scene, animator, leg.endBone.AsView(), foot))
            {
                ctx.debug->DrawLine(foot + up * c->rayUp, foot - up * c->rayDown, kIkTarget, true);
            }
        }
    }
}
