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

export module editor.scene:component_gizmos;

import foundation.core;
import foundation.scene;
import foundation.render;
import engine.render;
import engine.navigation;
import engine.spline;
import engine.animation;

using namespace foundation::core;

export namespace editor
{
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    /// Drawing context passed to gizmo renderers.
    struct GizmoContext
    {
        render::debug::DebugDraw* debug = nullptr;
        scene::Scene* scene = nullptr;
        Float3 cameraPosition{};
        // The viewport's full camera (view + projection) - the LOD overlay computes the
        // per-view pick with it. Null when the caller has no camera to offer.
        const render::ViewCamera* viewCamera = nullptr;
        // The page's LOD-overlay toolbar toggle: the overlay renderer draws nothing
        // without it (DrawWhenUnselected renderers run for every entity every frame).
        bool lodOverlay = false;
        // The page's "Show Colliders" toolbar toggle: the physics collider gizmo draws every
        // entity's collider wireframe when on, nothing when off. EDITOR-only (drawn into the
        // viewport's DebugView) - independent of the RUNTIME PhysicsSceneSettings.debugDraw.
        bool showColliders = false;
        // Set by DrawEntity per entity: false when the entity is EFFECTIVELY inactive (itself or
        // an ancestor disabled). The simulation never builds bodies/joints/characters for such an
        // entity, so the physics renderers draw DIMMED - "the sim ignores this" is the
        // interesting information.
        bool entityEffectivelyActive = true;
    };

    /// A viewport gizmo for one component type. Registered per scene-editor module; the page
    /// draws the selected entity's components through the registry (and every entity's for
    /// renderers that opt into DrawWhenUnselected).
    class IGizmoRenderer
    {
    public:
        virtual ~IGizmoRenderer() = default;
        [[nodiscard]] virtual const TypeInfo* ComponentType() const = 0;
        virtual void Draw(const Instance& component, scene::EntityHandle owner,
                          GizmoContext& ctx) = 0;
        [[nodiscard]] virtual bool DrawWhenUnselected() const { return false; }
    };

    class GizmoRendererRegistry
    {
    public:
        void Register(UniquePtr<IGizmoRenderer> renderer);

        [[nodiscard]] IGizmoRenderer* Find(const TypeInfo* componentType) const;

        /// Draw gizmos for `entity`'s components; when `selected` is false only renderers with
        /// DrawWhenUnselected participate.
        void DrawEntity(scene::EntityHandle entity, bool selected, GizmoContext& ctx) const;

        [[nodiscard]] usize Count() const noexcept { return m_renderers.Size(); }

    private:
        Array<UniquePtr<IGizmoRenderer>> m_renderers;
    };

    namespace detail
    {
        inline Float3 WorldPosition(const Float4x4& world)
        {
            return Float3{world.m[3][0], world.m[3][1], world.m[3][2]};
        }

        inline Float3 WorldForward(const Float4x4& world) // -Z basis row, normalized
        {
            return Normalized(Float3{-world.m[2][0], -world.m[2][1], -world.m[2][2]});
        }

        inline void DrawCenterCross(render::debug::DebugDraw& dd, Float3 p, f32 r, Color color)
        {
            dd.DrawLine(p - Float3{r, 0, 0}, p + Float3{r, 0, 0}, color);
            dd.DrawLine(p - Float3{0, r, 0}, p + Float3{0, r, 0}, color);
            dd.DrawLine(p - Float3{0, 0, r}, p + Float3{0, 0, r}, color);
        }
    }

    /// Light wireframes: directional = sun cross + direction arrow; point = range sphere;
    /// spot = cone (tip circle at range with the outer half-angle + four apex rays).
    class LightGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Reflection probe: wire influence box from halfExtents (entity-oriented) + center cross.
    class ReflectionProbeGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Decal projection volume: the oriented box the decal clips to (local [-size/2, size/2],
    /// entity-oriented) + an arrow along local +Z, the projection direction. Decals only land on
    /// surfaces INSIDE this box facing (within the angle fade) against the arrow - the gizmo is
    /// what makes "why doesn't my decal show" placement mistakes visible.
    class DecalGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Camera frustum wireframe from the component's projection at the entity's pose.
    class CameraGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// The navigation zone's AABB extents box (local, entity-oriented) - the region the bake
    /// samples geometry from. Read-only (extents are inspector-edited); drawn when the zone is
    /// selected (DrawWhenUnselected defaults false).
    class NavMeshZoneGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// LOD debug overlay: with the toolbar toggle on, every mesh carrying a LOD
    /// chain draws its bounds tinted by the level THIS viewport's camera selects (green 0,
    /// yellow 1, orange 2, red 3+). Recomputes the RAW pick through the exported pure
    /// selection functions (no hysteresis - visualization, not the renderer's state).
    class LodOverlayGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// EDIT-TIME physics collider wireframe (approach: draw the shape geometry directly from the
    /// RigidBodyComponent + the entity transform, with NO physics world - matching Traktor's shape
    /// guides). Drawn for every entity when the "Show Colliders" toggle is on (ctx.showColliders),
    /// nothing when off. This is an EDITOR concern, distinct from the RUNTIME
    /// PhysicsSceneSettings.debugDraw (which draws from live Jolt bodies during simulation).
    class PhysicsColliderGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// EDIT-TIME compound-child collider wireframe (ColliderComponent): the runtime folds every
    /// descendant ColliderComponent into the nearest ancestor body, so "Show Colliders" must draw
    /// them too - a body-plus-three-children rig showing only the root shape reads as "the child
    /// colliders are not registered". Drawn at the child's OWN entity transform, in a distinct
    /// colour (compound teal), same gate as the body renderer.
    class ChildColliderGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// EDIT-TIME character-controller capsule (CharacterComponent), from radius/halfHeight + the
    /// entity transform - the companion of PhysicsColliderGizmoRenderer, same "Show Colliders" gate.
    class CharacterColliderGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// EDIT-TIME joint constraint gizmo (JointComponent): the anchor cross, a link line to the
    /// connected target entity, and the hinge/slider axis arrow - from component data + transforms.
    /// Same "Show Colliders" gate + DebugView as the collider gizmos.
    class JointGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// The authored spline curve + its control points, drawn whenever the owning entity is
    /// selected (the spline TOOL adds interaction; this keeps the curve visible under every
    /// tool).
    class SplineGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Inverse kinematics on the selected entity: the solve itself while the scene runs, else the
    /// chain where the bind pose puts it, its target and its pole (or up), so a chain is seen
    /// before it is run.
    class TwoBoneIkGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };
    class AimIkGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };
    class FootIkGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Register the built-in component gizmos (called from RegisterSceneEditor).
    inline void RegisterBuiltinGizmoRenderers(GizmoRendererRegistry& registry)
    {
        registry.Register(UniquePtr<IGizmoRenderer>(foundation::core::DefaultAllocator().New<LightGizmoRenderer>(),
                                                    foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<ReflectionProbeGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(foundation::core::DefaultAllocator().New<CameraGizmoRenderer>(),
                                                    foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(foundation::core::DefaultAllocator().New<DecalGizmoRenderer>(),
                                                    foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<NavMeshZoneGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<LodOverlayGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<PhysicsColliderGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<ChildColliderGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<CharacterColliderGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(foundation::core::DefaultAllocator().New<JointGizmoRenderer>(),
                                                    foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(foundation::core::DefaultAllocator().New<SplineGizmoRenderer>(),
                                                    foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<TwoBoneIkGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<AimIkGizmoRenderer>(), foundation::core::DefaultAllocator()));
        registry.Register(UniquePtr<IGizmoRenderer>(
            foundation::core::DefaultAllocator().New<FootIkGizmoRenderer>(), foundation::core::DefaultAllocator()));
    }
}
