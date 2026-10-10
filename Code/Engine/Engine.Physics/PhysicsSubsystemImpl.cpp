// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Physics - implementation unit: the render-frame drive (interpolation
// + debug wireframes) and the component reflection bodies. Both live OUTSIDE the interface
// for GCC: heavy render imports stay out of the interface's module graph, and the
// REFLECT_* macros in the :components partition made GCC emit a gcm with an
// unreadable cluster (every -fno-module-lazy consumer then failed to import the module).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h"
#include "Core/Log/Log.h"
#include <cmath>

module engine.physics;
import engine.domain;

import foundation.core;
import foundation.profiler;
import foundation.runtime;
import foundation.scene;
import foundation.resource;
import foundation.physics;
import foundation.physics.resource;
import foundation.heightfield;
import foundation.render;
import engine.render;
import foundation.materials;
import foundation.script.facades; // RegisterExtraFacadeName (Physics into the behavior prelude)

using namespace foundation::core;
using namespace foundation::physics;
namespace core = foundation::core;
using foundation::heightfield::Heightfield;

namespace engine::physics
{
    namespace
    {
        // Body wireframes: green = awake dynamic, grey = sleeping, blue = static/kinematic,
        // yellow = trigger.
        void DrawPhysicsDebug(PhysicsSceneSystem& system, foundation::render::debug::DebugDraw& draw)
        {
            PhysicsWorld* world = system.World();
            foundation::scene::Scene* scene = system.ScenePtr();
            if (world == nullptr || scene == nullptr)
            {
                return;
            }
            auto* bodies = scene->GetSystem<RigidBodyComponentManager>();
            if (bodies == nullptr)
            {
                return;
            }
            bodies->ForEach(
                [&](RigidBodyComponent& c, foundation::scene::EntityHandle e)
                {
                    if (!c.body.IsValid())
                    {
                        return;
                    }
                    Float3 position;
                    Quaternion rotation;
                    world->GetBodyTransform(c.body, position, rotation);
                    const Color color =
                        c.isTrigger ? Color{1.0f, 0.8f, 0.2f, 1.0f}
                        : c.motion == MotionKind::Dynamic
                            ? (world->IsActive(c.body) ? Color{0.3f, 1.0f, 0.4f, 1.0f}
                                                       : Color{0.5f, 0.6f, 0.5f, 1.0f})
                            : Color{0.4f, 0.6f, 1.0f, 1.0f};
                    const Float4x4 worldMatrix =
                        Transform{position, rotation, Float3{1, 1, 1}}.ToMatrix();
                    switch (c.shape)
                    {
                    case ShapeKind::Box:
                        draw.DrawTransformedBox(
                            Float3{-c.halfExtents.x, -c.halfExtents.y, -c.halfExtents.z},
                            c.halfExtents, worldMatrix, color);
                        break;
                    case ShapeKind::Sphere:
                        draw.DrawWireSphere(position, c.radius, color);
                        break;
                    case ShapeKind::Capsule:
                        draw.DrawWireSphere(position, c.radius, color);
                        draw.DrawTransformedBox(
                            Float3{-c.radius, -(c.halfHeight + c.radius), -c.radius},
                            Float3{c.radius, c.halfHeight + c.radius, c.radius}, worldMatrix,
                            color);
                        break;
                    case ShapeKind::Plane:
                    {
                        // A bounded grid patch reads better than one huge quad.
                        const f32 extent = c.planeHalfExtent < 25.0f ? c.planeHalfExtent : 25.0f;
                        const i32 kCells = 10;
                        for (i32 g = -kCells; g <= kCells; ++g)
                        {
                            const f32 offset = extent * static_cast<f32>(g) / kCells;
                            draw.DrawLine(TransformPoint(Float3{offset, 0, -extent}, worldMatrix),
                                          TransformPoint(Float3{offset, 0, extent}, worldMatrix),
                                          color);
                            draw.DrawLine(TransformPoint(Float3{-extent, 0, offset}, worldMatrix),
                                          TransformPoint(Float3{extent, 0, offset}, worldMatrix),
                                          color);
                        }
                        break;
                    }
                    case ShapeKind::Heightfield:
                        // Bounding box of the footprint x [minY, maxY] - honest without walking
                        // every sample (per-cell wireframe is a phase-2 editor nicety).
                        if (const Heightfield* hf = c.heightfield.Get())
                        {
                            const Float2 ws = hf->WorldSize();
                            draw.DrawTransformedBox(
                                Float3{-ws.x * 0.5f, hf->MinY(), -ws.y * 0.5f},
                                Float3{ws.x * 0.5f, hf->MaxY(), ws.y * 0.5f}, worldMatrix, color);
                        }
                        break;
                    case ShapeKind::Cooked:
                        if (const CollisionShape* cooked = c.collisionShape.Get())
                        {
                            // Outline is authored unit-scale; re-apply the entity's scale.
                            Float3 sp, ss;
                            Quaternion sr;
                            const Float4x4 shapeMatrix =
                                Decompose(scene->GetWorldMatrix(e), sp, sr, ss)
                                    ? Transform{position, rotation, ss}.ToMatrix()
                                    : worldMatrix;
                            const Array<Float3>& outline = cooked->outline;
                            for (usize t = 0; t + 2 < outline.Size(); t += 3)
                            {
                                const Float3 a = TransformPoint(outline[t + 0], shapeMatrix);
                                const Float3 b = TransformPoint(outline[t + 1], shapeMatrix);
                                const Float3 d = TransformPoint(outline[t + 2], shapeMatrix);
                                draw.DrawLine(a, b, color);
                                draw.DrawLine(b, d, color);
                                draw.DrawLine(d, a, color);
                            }
                        }
                        break;
                    }
                });

            if (auto* characters = scene->GetSystem<CharacterComponentManager>())
            {
                characters->ForEach(
                    [&](CharacterComponent& c, foundation::scene::EntityHandle)
                    {
                        if (!c.character.IsValid())
                        {
                            return;
                        }
                        const Float3 position = world->CharacterPosition(c.character);
                        const Color color = c.ground == CharacterGround::OnGround
                                                ? Color{0.2f, 0.9f, 0.9f, 1.0f}
                                                : Color{0.9f, 0.5f, 0.9f, 1.0f};
                        draw.DrawWireSphere(
                            Float3{position.x, position.y + c.halfHeight, position.z}, c.radius,
                            color);
                        draw.DrawWireSphere(
                            Float3{position.x, position.y - c.halfHeight, position.z}, c.radius,
                            color);
                        draw.DrawWireBoxCenter(position, Float3{c.radius, c.halfHeight, c.radius},
                                               color);
                    });
            }
        }
    }

    bool DescribeBody(scene::Scene& scene, RigidBodyComponent& c, scene::EntityHandle e, BodyDesc& out,
                      Array<Array<f32>>& heightBuffers)
    {
        auto* colliders = scene.GetSystem<ColliderComponentManager>();
        out.motion = c.motion;
        out.layer = c.layer;
        out.friction = c.friction;
        out.restitution = c.restitution;
        out.linearDamping = c.linearDamping;
        out.angularDamping = c.angularDamping;
        out.isTrigger = c.isTrigger;
        out.continuousCollision = c.continuousCollision;
        out.massOverride = c.mass;
        out.group = c.collisionGroup;

        // Reverse map: the owning entity handle, packed losslessly into the body user
        // word (see PackEntity - unique by construction, unlike the guid's low bits).
        out.userData = PackEntity(e);

        Float3 position, scale;
        Quaternion rotation;
        if (!Decompose(scene.GetWorldMatrix(e), position, rotation, scale))
        {
            return false;
        }

        const auto fillHeightfield =
            [&](ShapeDesc& s, foundation::resource::Ref<Heightfield>& ref) -> bool {
            Heightfield* hf = ref.Get();
            if (hf == nullptr || hf->IsEmpty())
            {
                return false;
            }
            const i32 n = hf->Size();
            heightBuffers.PushBack(Array<f32>{});
            Array<f32>& buf = heightBuffers[heightBuffers.Size() - 1];
            const Span<const foundation::heightfield::Height> src = hf->Samples();
            const Span<const u8> holes = hf->Holes(); // a cut sample has no surface
            buf.Resize(src.Size());
            for (usize i = 0; i < src.Size(); ++i)
            {
                buf[i] = (i < holes.Size() && holes[i] != 0)
                             ? ShapeDesc::kNoCollisionHeight
                             : hf->SampleToWorldY(static_cast<f32>(src[i]));
            }
            s.heightSamples = Span<const f32>(buf.Data(), buf.Size());
            s.heightSampleCount = static_cast<u32>(n);
            s.heightWorldSize = hf->WorldSize();
            return true;
        };

        ShapeDesc own;
        own.kind = c.shape;
        own.halfExtents = c.halfExtents;
        own.radius = c.radius;
        own.halfHeight = c.halfHeight;
        own.planeHalfExtent = c.planeHalfExtent;
        if (c.shape == ShapeKind::Cooked)
        {
            CollisionShape* cooked = c.collisionShape.Get();
            if (cooked == nullptr)
            {
                LOG_WARNING(u8"Physics",
                                     u8"'{}': cooked shape has no collision-shape "
                                     u8"resource - body skipped",
                                     scene.GetEntityName(e));
                return false;
            }
            own.cooked = cooked->Blob();
            own.scale = scale; // cooked geometry is authored unit-scale
        }
        else if (c.shape == ShapeKind::Heightfield)
        {
            if (!fillHeightfield(own, c.heightfield))
            {
                LOG_WARNING(u8"Physics",
                                     u8"'{}': heightfield shape has no heightfield "
                                     u8"resource - body skipped",
                                     scene.GetEntityName(e));
                return false;
            }
        }
        // A shape that can only be static (Jolt's MustBeStatic: plane, heightfield,
        // a cooked triangle mesh) under a moving body: the world demotes it to
        // static rather than tripping Jolt's mass assert; named HERE, where the
        // entity is known, so the author can find the component.
        const bool staticOnly =
            c.shape == ShapeKind::Plane || c.shape == ShapeKind::Heightfield ||
            (c.shape == ShapeKind::Cooked && c.collisionShape.Get() != nullptr &&
             !c.collisionShape->convex);
        if (c.motion != MotionKind::Static && staticOnly)
        {
            LOG_ERROR(u8"Physics",
                      u8"'{}': a {} body cannot use a {} shape (static only: no mass, "
                      u8"no mesh-vs-mesh collision) - simulated as static",
                      scene.GetEntityName(e),
                      c.motion == MotionKind::Kinematic ? u8"kinematic" : u8"dynamic",
                      c.shape == ShapeKind::Plane         ? u8"plane"
                      : c.shape == ShapeKind::Heightfield ? u8"heightfield"
                                                          : u8"triangle-mesh");
        }
        out.shapes.PushBack(own);

        // Hierarchy compounding: descendant ColliderComponents fold in at their
        // offset relative to THIS entity (captured at start). The physics body has no scale
        // (only its position and rotation), so a child's place is measured in world units from
        // the body and a cooked child keeps its WORLD scale: relative to a scaled body (a
        // prefab scaled down) both would come out unscaled, the shape full size and off place.
        if (colliders != nullptr)
        {
            const Quaternion bodyInverse = Inverse(rotation);
            colliders->ForEach(
                [&](ColliderComponent& extra, scene::EntityHandle child)
                {
                    if (!IsDescendantOf(scene, child, e))
                    {
                        return;
                    }
                    Float3 wp, ls;
                    Quaternion wr;
                    if (!Decompose(scene.GetWorldMatrix(child), wp, wr, ls))
                    {
                        return;
                    }
                    const Float3 lp = RotateVector(bodyInverse, wp - position);
                    const Quaternion lr = bodyInverse * wr;
                    ShapeDesc shape;
                    shape.kind = extra.shape;
                    shape.halfExtents = extra.halfExtents;
                    shape.radius = extra.radius;
                    shape.halfHeight = extra.halfHeight;
                    shape.planeHalfExtent = extra.planeHalfExtent;
                    if (extra.shape == ShapeKind::Cooked)
                    {
                        CollisionShape* cooked = extra.collisionShape.Get();
                        if (cooked == nullptr)
                        {
                            return;
                        }
                        shape.cooked = cooked->Blob();
                        shape.scale = ls;
                    }
                    else if (extra.shape == ShapeKind::Heightfield)
                    {
                        if (!fillHeightfield(shape, extra.heightfield))
                        {
                            return;
                        }
                    }
                    shape.localPosition = lp;
                    shape.localRotation = lr;
                    out.shapes.PushBack(shape);
                });
        }

        out.position = position;
        out.rotation = rotation;

        // A referenced PhysicalMaterial wins over the inline surface fields.
        if (PhysicalMaterial* material = c.material.Get())
        {
            out.friction = material->friction;
            out.restitution = material->restitution;
            out.density = material->density;
        }

        return true;
    }

    void RigidBodyComponentManager::CollectStaticGeometry(scene::Scene& scene, const AABB& bounds, f32,
                                                          Array<Float3>& outTriangles)
    {
        // Static, solid bodies only: a dynamic or kinematic body moves and a trigger lets things
        // through, so neither is level geometry (a character is no rigid body at all). An inactive
        // entity has no body, as at scene start.
        Array<Array<f32>> heightBuffers; // the descs' height samples point in here
        Array<BodyDesc> bodies;
        ForEach(
            [&](RigidBodyComponent& c, scene::EntityHandle e)
            {
                if (c.motion != MotionKind::Static || c.isTrigger || !scene.IsEffectivelyActive(e))
                {
                    return;
                }
                BodyDesc desc;
                if (DescribeBody(scene, c, e, desc, heightBuffers))
                {
                    bodies.PushBack(static_cast<BodyDesc&&>(desc));
                }
            });
        const usize failed =
            AppendBodyTriangles(Span<const BodyDesc>(bodies.Data(), bodies.Size()), bounds, outTriangles);
        if (failed > 0)
        {
            LOG_WARNING(u8"Physics", u8"{} static bodies have a shape that does not build; they are left out of the static geometry",
                        failed);
        }
    }

    namespace
    {
        // The box a shape fills in its entity's own space, before the entity's world matrix: a
        // box by its half extents, a sphere by its radius, a capsule along y, a cooked shape by
        // its outline's points. A plane is endless and a heightfield is the terrain's to measure,
        // so neither answers.
        bool LocalShapeBounds(ShapeKind kind, Float3 halfExtents, f32 radius, f32 halfHeight,
                              CollisionShape* cooked, AABB& out)
        {
            switch (kind)
            {
            case ShapeKind::Box:
                out = AABB::FromCenterExtents(Float3{}, halfExtents);
                return true;
            case ShapeKind::Sphere:
                out = AABB::FromCenterExtents(Float3{}, Float3{radius, radius, radius});
                return true;
            case ShapeKind::Capsule:
                out = AABB::FromCenterExtents(Float3{}, Float3{radius, halfHeight + radius, radius});
                return true;
            case ShapeKind::Cooked:
            {
                if (cooked == nullptr || cooked->outline.IsEmpty())
                {
                    return false;
                }
                out = AABB::Empty();
                for (const Float3& p : cooked->outline)
                {
                    out.Expand(p);
                }
                return true;
            }
            default:
                return false;
            }
        }
    }

    bool RigidBodyComponentManager::EntityBounds(scene::Scene& scene, scene::EntityHandle entity, AABB& out)
    {
        RigidBodyComponent* c = Get(entity);
        AABB local;
        if (c == nullptr ||
            !LocalShapeBounds(c->shape, c->halfExtents, c->radius, c->halfHeight, c->collisionShape.Get(), local))
        {
            return false;
        }
        out = TransformAABB(local, scene.GetWorldMatrix(entity));
        return true;
    }

    bool ColliderComponentManager::EntityBounds(scene::Scene& scene, scene::EntityHandle entity, AABB& out)
    {
        ColliderComponent* c = Get(entity);
        AABB local;
        if (c == nullptr ||
            !LocalShapeBounds(c->shape, c->halfExtents, c->radius, c->halfHeight, c->collisionShape.Get(), local))
        {
            return false;
        }
        out = TransformAABB(local, scene.GetWorldMatrix(entity));
        return true;
    }

    void PhysicsSubsystem::Update(f32)
    {
        foundation::runtime::Context* context = GetContext();
        if (context == nullptr)
        {
            return;
        }
        auto* render = context->GetSubsystem<engine::render::RenderSubsystem>();
        PROFILE_SCOPE("Physics.Interpolate");
        for (const SceneEntry& entry : Systems())
        {
            // Per-scene alpha: each scene steps on its OWN accumulator/time scale.
            entry.system->ApplyInterpolation(entry.scene->FixedAlpha());
            if (render != nullptr && entry.system->Settings().debugDraw)
            {
                DrawPhysicsDebug(*entry.system, render->DebugScene(*entry.scene));
            }
        }
    }
}

// ---- reflection (see :components for why this lives here) ----
namespace engine::physics
{
    REFLECT_ENUM(MotionKind, "rtti::engine::physics")
    {
        builder.Value("Static", MotionKind::Static);
        builder.Value("Kinematic", MotionKind::Kinematic);
        builder.Value("Dynamic", MotionKind::Dynamic);
    }

    REFLECT_ENUM(PhysicsLayer, "rtti::engine::physics")
    {
        builder.Value("Static", PhysicsLayer::Static);
        builder.Value("Dynamic", PhysicsLayer::Dynamic);
        builder.Value("Kinematic", PhysicsLayer::Kinematic);
        builder.Value("Trigger", PhysicsLayer::Trigger);
    }

    REFLECT_ENUM(JointKind, "rtti::engine::physics")
    {
        builder.Value("Fixed", JointKind::Fixed);
        builder.Value("Point", JointKind::Point);
        builder.Value("Hinge", JointKind::Hinge);
        builder.Value("Slider", JointKind::Slider);
        builder.Value("Distance", JointKind::Distance);
    }

    REFLECT_ENUM(ShapeKind, "rtti::engine::physics")
    {
        builder.Value("Box", ShapeKind::Box);
        builder.Value("Sphere", ShapeKind::Sphere);
        builder.Value("Capsule", ShapeKind::Capsule);
        builder.Value("Cooked", ShapeKind::Cooked);
        builder.Value("Plane", ShapeKind::Plane);
        builder.Value("Heightfield", ShapeKind::Heightfield);
    }

    REFLECT_VALUE(RigidBodyComponent, "rtti::engine::physics")
    {
        builder.Attribute("displayName", String(u8"Rigid Body"))
            .Attribute("category", String(u8"Physics"))
            .Attribute("description",
                       String(u8"Makes the entity a physics body, static, kinematic or dynamic, "
                              u8"with a collision shape."))
            .DataVersion(3); // v3: continuous collision + explicit mass
        builder.Property<&RigidBodyComponent::motion>("motion");
        builder.Property<&RigidBodyComponent::layer>("layer");
        builder.Property<&RigidBodyComponent::shape>("shape");
        // Shape-conditional rows (the LightComponent pattern): the inspector shows a
        // dimension only for the ShapeKind that uses it (Box=0 Sphere=1 Capsule=2
        // Cooked=3 Plane=4).
        builder.Property<&RigidBodyComponent::halfExtents>("halfExtents")
            .PropAttribute("visibleWhen", String(u8"shape=0"));
        builder.Property<&RigidBodyComponent::radius>("radius")
            .PropAttribute("visibleWhen", String(u8"shape=1,2"));
        builder.Property<&RigidBodyComponent::halfHeight>("halfHeight")
            .PropAttribute("visibleWhen", String(u8"shape=2"));
        builder.Property<&RigidBodyComponent::planeHalfExtent>("planeHalfExtent")
            .PropAttribute("visibleWhen", String(u8"shape=4"));
        builder.Property<&RigidBodyComponent::heightfield>("heightfield")
            .PropAttribute("visibleWhen", String(u8"shape=5"));
        builder.Property<&RigidBodyComponent::friction>("friction");
        builder.Property<&RigidBodyComponent::restitution>("restitution");
        builder.Property<&RigidBodyComponent::linearDamping>("linearDamping");
        builder.Property<&RigidBodyComponent::angularDamping>("angularDamping");
        builder.Property<&RigidBodyComponent::isTrigger>("isTrigger");
        builder.Property<&RigidBodyComponent::continuousCollision>("continuousCollision");
        builder.Property<&RigidBodyComponent::mass>("mass");
        builder.Property<&RigidBodyComponent::collisionGroup>("collisionGroup");
        builder.Property<&RigidBodyComponent::collisionShape>("collisionShape");
        builder.Property<&RigidBodyComponent::material>("material");
        // OPTION 1 (spec Section 12): RigidBodyComponent.of(entity) -> a re-resolving handle.
        builder.Method<&foundation::script::ComponentOf<RigidBodyComponent>, RigidBodyComponent>("of");
    }

    REFLECT_VALUE(ColliderComponent, "rtti::engine::physics")
    {
        builder.Attribute("displayName", String(u8"Collider"))
            .Attribute("category", String(u8"Physics"))
            .Attribute("description",
                       String(u8"Adds an extra collision shape to the nearest ancestor's rigid "
                              u8"body."))
            .DataVersion(2); // v2: heightfield ref
        builder.Property<&ColliderComponent::shape>("shape");
        builder.Property<&ColliderComponent::halfExtents>("halfExtents")
            .PropAttribute("visibleWhen", String(u8"shape=0"));
        builder.Property<&ColliderComponent::radius>("radius")
            .PropAttribute("visibleWhen", String(u8"shape=1,2"));
        builder.Property<&ColliderComponent::halfHeight>("halfHeight")
            .PropAttribute("visibleWhen", String(u8"shape=2"));
        builder.Property<&ColliderComponent::planeHalfExtent>("planeHalfExtent")
            .PropAttribute("visibleWhen", String(u8"shape=4"));
        builder.Property<&ColliderComponent::collisionShape>("collisionShape")
            .PropAttribute("visibleWhen", String(u8"shape=3"));
        builder.Property<&ColliderComponent::heightfield>("heightfield")
            .PropAttribute("visibleWhen", String(u8"shape=5"));
    }

    REFLECT_VALUE(CharacterComponent, "rtti::engine::physics")
    {
        builder.Attribute("displayName", String(u8"Character"))
            .Attribute("category", String(u8"Physics"))
            .Attribute("description",
                       String(u8"Moves the entity as a capsule character that walks slopes and "
                              u8"steps, jumps and pushes bodies."))
            .DataVersion(1);
        builder.Property<&CharacterComponent::radius>("radius");
        builder.Property<&CharacterComponent::halfHeight>("halfHeight");
        builder.Property<&CharacterComponent::maxSlopeDegrees>("maxSlopeDegrees");
        builder.Property<&CharacterComponent::mass>("mass");
        builder.Property<&CharacterComponent::maxStrength>("maxStrength");
        builder.Property<&CharacterComponent::stepUp>("stepUp");
        builder.Property<&CharacterComponent::stepDown>("stepDown");
        // OPTION 1 (spec Section 12): CharacterComponent.of(entity) -> a re-resolving handle.
        builder.Method<&foundation::script::ComponentOf<CharacterComponent>, CharacterComponent>("of");
        // Per-entity character control (component-data ops - fixes the static facade's first-character
        // limitation): Character.of(entity).move(x, z) / .jump(speed) / .grounded() / .positionY().
        builder.Method<&CharacterComponent::move>("move", {"velocityX", "velocityZ"});
        builder.Method<static_cast<void (CharacterComponent::*)(f32, f32, f32)>(&CharacterComponent::drive)>(
            "drive", {"x", "y", "z"});
        builder.Method<static_cast<void (CharacterComponent::*)(Float3)>(&CharacterComponent::drive)>(
            "drive", {"velocity"});
        // Read-only: the motion over the last step and the ground under it (up in the air).
        builder.ComputedProperty<&CharacterComponent::currentVelocity>("velocity");
        builder.ComputedProperty<&CharacterComponent::currentGroundNormal>("groundNormal");
        builder.Method<&CharacterComponent::jump>("jump", {"speed"});
        builder.Method<&CharacterComponent::launch>("launch", {"speed"});
        builder.Method<static_cast<void (CharacterComponent::*)(f32, f32, f32)>(
            &CharacterComponent::setPosition)>("setPosition", {"x", "y", "z"});
        builder.Method<static_cast<void (CharacterComponent::*)(Float3)>(&CharacterComponent::setPosition)>(
            "setPosition", {"position"});
        builder.Method<&CharacterComponent::grounded>("grounded");
        builder.Method<&CharacterComponent::positionX>("positionX");
        builder.Method<&CharacterComponent::positionY>("positionY");
        builder.Method<&CharacterComponent::positionZ>("positionZ");
    }

    REFLECT_VALUE(JointComponent, "rtti::engine::physics")
    {
        // Kind-conditional rows (visibleWhen, evaluated live by InspectorView): JointKind is
        // Fixed=0, Point=1, Hinge=2, Slider=3, Distance=4. The axis + limits + motor are hinge/slider
        // (2,3) concepts; min/max separation is Distance (4); the motor drive params nest under the
        // motor checkbox. kind/targetEntity/localAnchor apply to every kind, so stay unconditional.
        builder.Attribute("displayName", String(u8"Joint"))
            .Attribute("category", String(u8"Physics"))
            .Attribute("description",
                       String(u8"Connects the entity's rigid body to another body or the world: "
                              u8"fixed, point, hinge, slider or distance."))
            .DataVersion(1)
            .Property<&JointComponent::kind>("kind")
            .Property<&JointComponent::targetEntity>("targetEntity")
            .Property<&JointComponent::localAnchor>("localAnchor")
            .Property<&JointComponent::localAxis>("localAxis")
            .PropAttribute("visibleWhen", String(u8"kind=2,3"))
            .Property<&JointComponent::limitMin>("limitMin")
            .PropAttribute("visibleWhen", String(u8"kind=2,3"))
            .Property<&JointComponent::limitMax>("limitMax")
            .PropAttribute("visibleWhen", String(u8"kind=2,3"))
            .Property<&JointComponent::minDistance>("minDistance")
            .PropAttribute("visibleWhen", String(u8"kind=4"))
            .Property<&JointComponent::maxDistance>("maxDistance")
            .PropAttribute("visibleWhen", String(u8"kind=4"))
            .Property<&JointComponent::motorEnabled>("motorEnabled")
            .PropAttribute("visibleWhen", String(u8"kind=2,3"))
            .Property<&JointComponent::motorTargetVelocity>("motorTargetVelocity")
            .PropAttribute("visibleWhen", String(u8"motorEnabled"))
            .Property<&JointComponent::motorLimit>("motorLimit")
            .PropAttribute("visibleWhen", String(u8"motorEnabled"));
    }

    REFLECT_VALUE(PhysicsSceneSettings, "rtti::engine::physics")
    {
        builder.DataVersion(1);
        builder.Property<&PhysicsSceneSettings::gravity>("gravity");
        builder.Property<&PhysicsSceneSettings::collisionSteps>("collisionSteps");
        builder.Property<&PhysicsSceneSettings::debugDraw>("debugDraw");
    }

    // The scene-bound physics handle: `ScenePhysics.of(scene).rayCast/gravityY/setGravity/...` on
    // THAT scene's world (the explicit-scene replacement for the retired static Physics facade).
    // Reflected with authored parameter names (A6). The `of` factory returns ScenePhysics by value
    // (concrete return type - cross-backend, no ReturnType-override needed).
    // The explicit ray-hit result: value handle carrying the whole
    // answer; entity()/impulse() resolve live state at call time.
    REFLECT_VALUE(RayCastHit, "rtti::engine::physics")
    {
        builder.Property<&RayCastHit::hit>("hit");
        builder.Property<&RayCastHit::distance>("distance");
        builder.Property<&RayCastHit::position>("position");
        builder.Property<&RayCastHit::normal>("normal");
        builder.Property<&RayCastHit::surface>("surface");
        builder.Method<&RayCastHit::entity>("entity");
        builder.Method<&RayCastHit::material>("material");
        builder.Method<&RayCastHit::impulse>("impulse", {"x", "y", "z"});
    }

    REFLECT_VALUE(ScenePhysics, "rtti::engine::physics")
    {
        using Entity = foundation::script::Entity;
        using Entities = Array<Entity>;
        // Each query an ARITY FAMILY: Sedulous's Float3 forms (with an optional group mask), and
        // ours by the numbers.
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(Float3, Float3, f32) const>(
            &ScenePhysics::rayCast)>("rayCast", {"from", "direction", "maxDistance"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(Float3, Float3, f32, u32) const>(
            &ScenePhysics::rayCast)>("rayCast", {"from", "direction", "maxDistance", "groupMask"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(f32, f32, f32, f32, f32, f32, f32) const>(
            &ScenePhysics::rayCast)>(
            "rayCast", {"fromX", "fromY", "fromZ", "dirX", "dirY", "dirZ", "maxDistance"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(Float3, Float3, f32, f32) const>(
            &ScenePhysics::sphereCast)>("sphereCast", {"from", "direction", "maxDistance", "radius"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(Float3, Float3, f32, f32, u32) const>(
            &ScenePhysics::sphereCast)>("sphereCast",
                                        {"from", "direction", "maxDistance", "radius", "groupMask"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(f32, f32, f32, f32, f32, f32, f32, f32)
                                       const>(&ScenePhysics::sphereCast)>(
            "sphereCast",
            {"fromX", "fromY", "fromZ", "dirX", "dirY", "dirZ", "maxDistance", "radius"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(Float3, f32) const>(
            &ScenePhysics::nearestOverlap)>("nearestOverlap", {"center", "radius"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(Float3, f32, u32) const>(
            &ScenePhysics::nearestOverlap)>("nearestOverlap", {"center", "radius", "groupMask"});
        builder.Method<static_cast<RayCastHit (ScenePhysics::*)(f32, f32, f32, f32, i32) const>(
            &ScenePhysics::nearestOverlap)>("nearestOverlap", {"x", "y", "z", "radius", "groupMask"});
        builder.Method<static_cast<Entities (ScenePhysics::*)(Float3, f32) const>(
            &ScenePhysics::overlapSphere)>("overlapSphere", {"center", "radius"});
        builder.Method<static_cast<Entities (ScenePhysics::*)(Float3, f32, u32) const>(
            &ScenePhysics::overlapSphere)>("overlapSphere", {"center", "radius", "groupMask"});
        builder.Method<static_cast<Entities (ScenePhysics::*)(f32, f32, f32, f32, i32) const>(
            &ScenePhysics::overlapSphere)>("overlapSphere", {"x", "y", "z", "radius", "groupMask"});
        builder.Method<static_cast<void (ScenePhysics::*)(Float3)>(&ScenePhysics::setGravity)>(
            "setGravity", {"gravity"});
        builder.Method<static_cast<void (ScenePhysics::*)(f32, f32, f32)>(&ScenePhysics::setGravity)>(
            "setGravity", {"x", "y", "z"});
        builder.Method<&ScenePhysics::gravity>("gravity");
        builder.Method<&ScenePhysics::gravityY>("gravityY");
        builder.Method<&ScenePhysics::bodyCount>("bodyCount");
        builder.Method<static_cast<void (ScenePhysics::*)(Entity, Float3)>(&ScenePhysics::applyImpulse)>(
            "applyImpulse", {"entity", "impulse"});
        builder.Method<static_cast<void (ScenePhysics::*)(Entity, f32, f32, f32)>(
            &ScenePhysics::applyImpulse)>("applyImpulse", {"entity", "x", "y", "z"});
        // The entity's character, as Sedulous's PhysicsFacade.
        builder.Method<&ScenePhysics::moveCharacter>("moveCharacter",
                                                     {"entity", "velocityX", "velocityZ"});
        builder.Method<&ScenePhysics::jumpCharacter>("jumpCharacter", {"entity", "speed"});
        builder.Method<&ScenePhysics::launchCharacter>("launchCharacter", {"entity", "speed"});
        builder.Method<&ScenePhysics::setCharacterPosition>("setCharacterPosition",
                                                            {"entity", "position"});
        builder.Method<&ScenePhysics::isCharacterGrounded>("isCharacterGrounded", {"entity"});
        builder.Method<&ScenePhysics::of>("of", {"scene"});
    }

    void RegisterPhysicsScriptFacade()
    {
        RegisterPhysicsComponentReflection(); // ensure component TypeData (incl `of`) is built first

        // Surface the physics COMPONENTS to script (OPTION 1: RigidBodyComponent.of(entity), ...):
        // register them (both backends emit registry types), seed emission roots (reachability),
        // and make their class names import-visible in behavior preludes.
        const core::TypeInfo* components[] = {&core::TypeOf<RigidBodyComponent>(),
                                              &core::TypeOf<CharacterComponent>()};
        for (const core::TypeInfo* component : components)
        {
            GlobalTypeRegistry().Register(*component);
            foundation::script::RegisterExtraScriptRootType(component);
        }
        foundation::script::RegisterExtraFacadeName(u8"RigidBodyComponent");
        foundation::script::RegisterExtraFacadeName(u8"CharacterComponent");

        // The scene-bound physics handle (ScenePhysics.of(scene)) + its explicit ray-hit
        // result: reflect, register, seed the emission roots, prelude-visible class names.
        RttiRegisterValue_RayCastHit();
        GlobalTypeRegistry().Register(core::TypeOf<RayCastHit>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<RayCastHit>());
        foundation::script::RegisterExtraFacadeName(u8"RayCastHit");
        // ScenePhysics.overlapSphere returns Array<Entity>, which crosses as a native array<Entity> /
        // Lua table. Patch the container TypeInfo so the backend renders it;
        // no script root / facade name - a native array is not a boxed handle type.
        core::RegisterArrayType<foundation::script::Entity>();
        RttiRegisterValue_ScenePhysics();
        GlobalTypeRegistry().Register(core::TypeOf<ScenePhysics>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<ScenePhysics>());
        foundation::script::RegisterExtraFacadeName(u8"ScenePhysics");
    }

    void RegisterPhysicsComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_MotionKind();
            RttiRegisterEnum_PhysicsLayer();
            RttiRegisterEnum_ShapeKind();
            RttiRegisterEnum_JointKind();
            RttiRegisterValue_RigidBodyComponent();
            RttiRegisterValue_ColliderComponent();
            RttiRegisterValue_JointComponent();
            RttiRegisterValue_CharacterComponent();
            RttiRegisterValue_PhysicsSceneSettings();
            return true;
        }();
        (void)once;
    }
}

namespace engine::physics
{
    const engine::DomainModule& PhysicsDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::physics::kPhysicsResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"physics",
            .installScene = &AddPhysicsSceneManagers,
            .registerReflection = &RegisterPhysicsComponentReflection,
            .registerScriptFacade = &RegisterPhysicsScriptFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
