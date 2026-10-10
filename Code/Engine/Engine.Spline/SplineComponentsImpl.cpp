// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.spline implementation: reflection + the composition install (heavy REFLECT bodies
// out of the interface).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.spline;
import engine.domain;

import foundation.core;
import foundation.scene;
import foundation.spline;
import foundation.script.facades;

using namespace foundation::core;

namespace engine::spline
{
    REFLECT_VALUE(SplineComponent, "rtti::engine::spline")
    {
        builder.Attribute("displayName", String(u8"Spline"))
            .Attribute("category", String(u8"Utility"))
            .Attribute("description",
                       String(u8"Holds a curve of points, open or closed, for paths and "
                              u8"followers; shape it with the spline tool."))
            .DataVersion(1);
        // Points are authored by the viewport spline tool, not the inspector: the rows are the
        // loop flag (through the curve, so its caches follow) and the point count, read-only.
        builder.AccessorProperty<&SplineComponent::IsClosed, &SplineComponent::SetClosed>("closed")
            .ComputedProperty<&SplineComponent::PointCount>("pointCount");
    }

    void PathFollowComponentManager::OnUpdate(foundation::scene::ScenePhase phase,
                                              f32 deltaTime)
    {
        if (phase != foundation::scene::ScenePhase::Update || m_scene == nullptr ||
            deltaTime <= 0.0f)
        {
            return;
        }
        auto* splines = m_scene->GetSystem<SplineComponentManager>();
        if (splines == nullptr)
        {
            return;
        }
        ForEach(
            [&](PathFollowComponent& follow, foundation::scene::EntityHandle owner)
            {
                if (!follow.playing || follow.spline.IsNil() ||
                    !m_scene->IsEffectivelyActive(owner))
                {
                    return;
                }
                const foundation::scene::EntityHandle splineEntity =
                    m_scene->FindEntity(follow.spline.id);
                const SplineComponent* component = splines->Get(splineEntity);
                if (component == nullptr)
                {
                    return;
                }
                const foundation::spline::SplineCurve& curve = component->curve;
                const f32 length = curve.Length();
                if (length <= 0.0f)
                {
                    return;
                }

                follow.distance += follow.speed * deltaTime;
                if (curve.closed || follow.loop)
                {
                    follow.distance -= Floor(follow.distance / length) * length;
                }
                else if (follow.distance >= length)
                {
                    follow.distance = length;
                    follow.playing = false; // arrived
                }
                else if (follow.distance < 0.0f)
                {
                    follow.distance = 0.0f;
                    follow.playing = false;
                }

                const f32 t = curve.DistanceToT(follow.distance);
                const Float4x4 splineWorld = m_scene->GetWorldMatrix(splineEntity);
                const Float3 world = TransformPoint(curve.Evaluate(t), splineWorld);

                // NOTE: assumes an unparented follower (local == world), the NavAgent
                // moveEntity convention; a parented follower would need world->parent-local.
                Transform transform = m_scene->GetLocalTransform(owner);
                transform.position = world;
                if (follow.alignToTangent)
                {
                    const Float3 tangent =
                        Normalized(TransformDirection(curve.Tangent(t), splineWorld));
                    if (LengthSquared(tangent) > 0.5f)
                    {
                        // Yaw/pitch that turn -Z into the tangent (roll-free).
                        const f32 yaw = Atan2(-tangent.x, -tangent.z);
                        const f32 horizontal =
                            Sqrt(tangent.x * tangent.x + tangent.z * tangent.z);
                        const f32 pitch = Atan2(tangent.y, horizontal);
                        transform.rotation =
                            Quaternion::FromAxisAngle(Float3{0, 1, 0}, yaw) *
                            Quaternion::FromAxisAngle(Float3{1, 0, 0}, pitch);
                    }
                }
                m_scene->SetLocalTransform(owner, transform);
            });
    }

    REFLECT_VALUE(PathFollowComponent, "rtti::engine::spline")
    {
        builder.Attribute("displayName", String(u8"Path Follow"))
            .Attribute("category", String(u8"Utility"))
            .Attribute("description",
                       String(u8"Moves the entity along another entity's spline at a set speed, "
                              u8"optionally facing its direction."))
            .DataVersion(1)
            .Property<&PathFollowComponent::spline>("spline")
            .Property<&PathFollowComponent::speed>("speed")
            .Property<&PathFollowComponent::distance>("distance")
            .Property<&PathFollowComponent::playing>("playing")
            .Property<&PathFollowComponent::loop>("loop")
            .Property<&PathFollowComponent::alignToTangent>("alignToTangent");
        // PathFollowComponent.of(entity): a script drives its follower (a pace, a restart).
        builder.Method<&foundation::script::ComponentOf<PathFollowComponent>, PathFollowComponent>("of");
    }

    void AddSplineSceneManagers(foundation::scene::Scene& scene)
    {
        scene.AddSystem<SplineComponentManager>();
        scene.AddSystem<PathFollowComponentManager>();
    }

    void RegisterSplineComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_SplineComponent();
            RttiRegisterValue_PathFollowComponent();
            return true;
        }();
        (void)once;
    }

    const SplineComponent* SceneSplines::Component(foundation::script::Entity entity) const
    {
        if (scene == nullptr || entity.scene != scene)
        {
            return nullptr;
        }
        auto* manager = scene->GetSystem<SplineComponentManager>();
        return (manager != nullptr) ? manager->Get(entity.Handle()) : nullptr;
    }

    f32 SceneSplines::length(foundation::script::Entity entity) const
    {
        const SplineComponent* component = Component(entity);
        return (component != nullptr) ? component->curve.Length() : 0.0f;
    }

    i32 SceneSplines::pointCount(foundation::script::Entity entity) const
    {
        const SplineComponent* component = Component(entity);
        return (component != nullptr) ? static_cast<i32>(component->curve.points.Size()) : 0;
    }

    bool SceneSplines::isClosed(foundation::script::Entity entity) const
    {
        const SplineComponent* component = Component(entity);
        return component != nullptr && component->curve.closed;
    }

    namespace
    {
        [[nodiscard]] SplineHit MakeHit(const foundation::spline::SplineCurve& curve, f32 t,
                                        const Float4x4& world)
        {
            SplineHit hit;
            hit.valid = true;
            hit.t = t;
            hit.distance = curve.TToDistance(t);
            hit.position = TransformPoint(curve.Evaluate(t), world);
            hit.tangent = Normalized(TransformDirection(curve.Tangent(t), world));
            return hit;
        }
    }

    SplineHit SceneSplines::sampleAt(foundation::script::Entity entity, f32 t) const
    {
        const SplineComponent* component = Component(entity);
        if (component == nullptr)
        {
            return {};
        }
        return MakeHit(component->curve, t, scene->GetWorldMatrix(entity.Handle()));
    }

    SplineHit SceneSplines::sampleAtDistance(foundation::script::Entity entity,
                                             f32 distance) const
    {
        const SplineComponent* component = Component(entity);
        if (component == nullptr)
        {
            return {};
        }
        return MakeHit(component->curve, component->curve.DistanceToT(distance),
                       scene->GetWorldMatrix(entity.Handle()));
    }

    SplineHit SceneSplines::closestPoint(foundation::script::Entity entity, f32 x, f32 y,
                                         f32 z) const
    {
        const SplineComponent* component = Component(entity);
        if (component == nullptr)
        {
            return {};
        }
        const Float4x4 world = scene->GetWorldMatrix(entity.Handle());
        // The query point maps into curve-local space; the sample maps back out.
        const Float3 local = TransformPoint(Float3{x, y, z}, Inverse(world));
        const foundation::spline::SplineSample sample =
            component->curve.ClosestPoint(local);
        return MakeHit(component->curve, sample.t, world);
    }

    REFLECT_VALUE(SplineHit, "rtti::engine::spline")
    {
        builder.Property<&SplineHit::valid>("valid");
        builder.Property<&SplineHit::t>("t");
        builder.Property<&SplineHit::distance>("distance");
        builder.Property<&SplineHit::position>("position");
        builder.Property<&SplineHit::tangent>("tangent");
    }

    REFLECT_VALUE(SceneSplines, "rtti::engine::spline")
    {
        builder.Method<&SceneSplines::length>("length", {"entity"});
        builder.Method<&SceneSplines::pointCount>("pointCount", {"entity"});
        builder.Method<&SceneSplines::isClosed>("isClosed", {"entity"});
        builder.Method<&SceneSplines::sampleAt>("sampleAt", {"entity", "t"});
        builder.Method<&SceneSplines::sampleAtDistance>("sampleAtDistance",
                                                        {"entity", "distance"});
        builder.Method<static_cast<SplineHit (SceneSplines::*)(foundation::script::Entity, f32, f32, f32)
                                      const>(&SceneSplines::closestPoint)>("closestPoint",
                                                                           {"entity", "x", "y", "z"});
        builder.Method<static_cast<SplineHit (SceneSplines::*)(foundation::script::Entity, Float3) const>(
            &SceneSplines::closestPoint)>("closestPoint", {"entity", "world"});
        builder.Method<&SceneSplines::of>("of", {"scene"});
    }

    void RegisterSplineScriptFacade()
    {
        static const bool once = []()
        {
            RegisterSplineComponentReflection();
            RttiRegisterValue_SplineHit();
            GlobalTypeRegistry().Register(TypeOf<SplineHit>());
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SplineHit>());
            foundation::script::RegisterExtraFacadeName(u8"SplineHit");
            // The follower as a script class (PathFollowComponent.of(entity)): registered, an
            // emission root (no signature reaches it), its name visible in behaviour preludes.
            GlobalTypeRegistry().Register(TypeOf<PathFollowComponent>());
            foundation::script::RegisterExtraScriptRootType(&TypeOf<PathFollowComponent>());
            foundation::script::RegisterExtraFacadeName(u8"PathFollowComponent");
            RttiRegisterValue_SceneSplines();
            GlobalTypeRegistry().Register(TypeOf<SceneSplines>());
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SceneSplines>());
            foundation::script::RegisterExtraFacadeName(u8"SceneSplines");
            return true;
        }();
        (void)once;
    }
}

namespace engine::spline
{
    const engine::DomainModule& SplineDomain() noexcept
    {
        static const engine::DomainModule kModule{
            .id = u8"spline",
            .installScene = &AddSplineSceneManagers,
            .registerReflection = &RegisterSplineComponentReflection,
            .registerScriptFacade = &RegisterSplineScriptFacade};
        return kModule;
    }
}
