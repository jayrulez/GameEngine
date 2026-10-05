// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Spline - the `engine.spline` module.
///
/// The authorable 3D spline as scene DATA: SplineComponent wraps a foundation.spline
/// SplineCurve (cubic-Bezier points + closed flag) with no per-frame work - consumers
/// (path-follow, scatter, road extrusion, particles) evaluate it; the editor's spline tool
/// authors it. Data-only on purpose: no subsystem, no tick - the manager exists so the
/// component serializes, reflects into the component menu, and rides scene composition.

module;
#include "Core/Prelude.h"

export module engine.spline;

import foundation.core;
import engine.domain;
import foundation.scene;
import foundation.spline;
import foundation.script.facades;

using namespace foundation::core;

export namespace engine::spline
{
    namespace fspline = foundation::spline;

    struct SplineComponent
    {
        fspline::SplineCurve curve;

        // The inspector's rows (points are authored in the viewport by the spline tool): the
        // loop flag, through the curve so its arc-length table follows, and the point count.
        [[nodiscard]] bool IsClosed() const noexcept { return curve.closed; }
        void SetClosed(bool closed)
        {
            if (curve.closed != closed)
            {
                curve.closed = closed;
                curve.RebuildArcLength();
            }
        }
        [[nodiscard]] u32 PointCount() const noexcept
        {
            return static_cast<u32>(curve.points.Size());
        }
    };

    /// What a spline added with no points starts as: a short segment along the entity's local
    /// X, so the viewport tool has points to grab and the gizmo shows a curve at once. Also the
    /// runtime's answer for a script that adds the component bare.
    inline void SeedDefaultSpline(fspline::SplineCurve& curve)
    {
        fspline::SplinePoint a;
        a.position = Float3{-1.0f, 0.0f, 0.0f};
        fspline::SplinePoint b;
        b.position = Float3{1.0f, 0.0f, 0.0f};
        curve.points.PushBack(a);
        curve.points.PushBack(b);
        curve.UpdateAutoHandles();
        curve.RebuildArcLength();
    }

    inline void Serialize(ISerializer& ar, SplineComponent& c)
    {
        foundation::core::Serialize(ar, "points", c.curve.points);
        foundation::core::Serialize(ar, "closed", c.curve.closed);
        if (ar.Mode() == SerializeMode::Read)
        {
            // Stored handles are authoritative; only the derived caches rebuild on load.
            c.curve.RebuildArcLength();
        }
    }

    class SplineComponentManager final
        : public foundation::scene::SerializableComponentManager<SplineComponent>
    {
    public:
        SplineComponentManager()
            : foundation::scene::SerializableComponentManager<SplineComponent>(u8"spline")
        {
        }

        /// A component that reaches its first Initialize phase with NO points was added bare
        /// (the editor's Add Component, a script): it is seeded. A loaded or spawned one has its
        /// points by then and is left alone.
        void OnComponentInitialized(SplineComponent& component,
                                    foundation::scene::EntityHandle) override
        {
            if (component.curve.points.IsEmpty())
            {
                SeedDefaultSpline(component.curve);
            }
        }
    };

    /// Moves its entity along another entity's spline at a constant speed (the first spline
    /// consumer). `distance` is the position along the curve (serialized, so an authored
    /// start offset survives); non-loop follows stop at the end (closed curves always wrap).
    struct PathFollowComponent
    {
        foundation::scene::EntityRef spline; // the entity carrying the SplineComponent
        f32 speed = 1.0f;                    // world units per second along the curve
        f32 distance = 0.0f;                 // current position along the curve
        bool playing = true;
        bool loop = true;
        bool alignToTangent = true; // face -Z along the curve direction
    };

    inline void Serialize(ISerializer& ar, PathFollowComponent& c)
    {
        foundation::core::Serialize(ar, "spline", c.spline.id);
        foundation::core::Serialize(ar, "speed", c.speed);
        foundation::core::Serialize(ar, "distance", c.distance);
        foundation::core::Serialize(ar, "playing", c.playing);
        foundation::core::Serialize(ar, "loop", c.loop);
        foundation::core::Serialize(ar, "alignToTangent", c.alignToTangent);
    }

    /// Simulation-only: advances every follower during Update (edit mode never moves them).
    class PathFollowComponentManager final
        : public foundation::scene::SerializableComponentManager<PathFollowComponent>
    {
    public:
        PathFollowComponentManager()
            : foundation::scene::SerializableComponentManager<PathFollowComponent>(
                  u8"path_follow")
        {
        }

        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        void OnSceneCreate(foundation::scene::Scene& scene) override { m_scene = &scene; }
        void OnUpdate(foundation::scene::ScenePhase phase, f32 deltaTime) override;

    private:
        foundation::scene::Scene* m_scene = nullptr;
    };

    /// Scene-composition install (the domain module entry; see Engine.Composition).
    void AddSplineSceneManagers(foundation::scene::Scene& scene);
    /// Component reflection (component menu + inspector + data-version gate). Idempotent.
    void RegisterSplineComponentReflection();

    /// A sampled spline result for script - WORLD-space position + unit tangent, plus the
    /// curve parameter and the distance along the curve the sample answers for. `valid` is
    /// false when the entity has no spline (fields zeroed) - the RayCastHit convention.
    struct SplineHit
    {
        bool valid = false;
        f32 t = 0.0f;        // the curve parameter: segment index + the way along it
        f32 distance = 0.0f; // how far along the curve that is (m, curve-local, as length())
        Float3 position{};
        Float3 tangent{};
    };

    /// The scene-bound spline facade (SceneSplines.of(scene), the ScenePhysics idiom): all
    /// queries take the entity CARRYING the SplineComponent and answer in WORLD space (the
    /// entity transform places the curve; points are stored entity-local).
    struct SceneSplines
    {
        foundation::scene::Scene* scene = nullptr;

        [[nodiscard]] f32 length(foundation::script::Entity entity) const;
        [[nodiscard]] i32 pointCount(foundation::script::Entity entity) const;
        [[nodiscard]] bool isClosed(foundation::script::Entity entity) const;
        /// Sample at the curve parameter t (0..segment count; wraps on closed loops).
        [[nodiscard]] SplineHit sampleAt(foundation::script::Entity entity, f32 t) const;
        /// Sample at a distance along the curve (even spacing via the arc-length table).
        [[nodiscard]] SplineHit sampleAtDistance(foundation::script::Entity entity,
                                                 f32 distance) const;
        /// The closest point on the curve to a world position.
        [[nodiscard]] SplineHit closestPoint(foundation::script::Entity entity, f32 x, f32 y,
                                             f32 z) const;
        /// The same by a Float3 (Sedulous's ClosestPoint).
        [[nodiscard]] SplineHit closestPoint(foundation::script::Entity entity, Float3 world) const
        {
            return closestPoint(entity, world.x, world.y, world.z);
        }

        [[nodiscard]] static SceneSplines of(foundation::script::Scene sceneHandle)
        {
            return SceneSplines{sceneHandle.scene};
        }

    private:
        [[nodiscard]] const SplineComponent* Component(foundation::script::Entity entity) const;
    };

    /// Script surface registration (called from RegisterAllScriptFacades). Idempotent.
    void RegisterSplineScriptFacade();
}

export namespace engine::spline
{
    /// This domain's declaration (engine-composition.md D4): what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& SplineDomain() noexcept;
}
