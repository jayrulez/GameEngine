// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :component_gizmos partition.
//
// IGizmoRenderer and its registry: per-component-type viewport gizmos drawn through debug-draw. The
// reflection probe gizmo draws a wire box from halfExtents, as probes are boxes. DrawWhenUnselected
// defaults to false, so unselected lights do not all draw their range spheres; entity markers
// already anchor unselected entities. The interface is type-erased on reflection Instances (there
// is no Component base class), so a renderer looks its data up from the manager's
// GetComponentInstance.

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
        // The viewport's full camera (view and projection), which the LOD overlay uses to compute
        // the per-view pick. Null when the caller has no camera to offer.
        const render::ViewCamera* viewCamera = nullptr;
        // The page's LOD-overlay toolbar toggle: the overlay renderer draws nothing
        // without it (DrawWhenUnselected renderers run for every entity every frame).
        bool lodOverlay = false;
        // The page's "Show Colliders" toolbar toggle: when on, the physics collider gizmo draws
        // every entity's collider wireframe; when off, nothing. This is editor-only (drawn into the
        // viewport's DebugView) and independent of the runtime PhysicsSceneSettings.debugDraw.
        bool showColliders = false;
        // Set by DrawEntity per entity: false when the entity is effectively inactive (itself or an
        // ancestor disabled). The simulation never builds bodies, joints or characters for such an
        // entity, so the physics renderers draw it dimmed to show that the simulation ignores it.
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

    /// Light wireframes: a directional light draws a sun cross and a direction arrow, a point light
    /// its range sphere, and a spot light a cone (a circle at range from the outer half-angle, and
    /// four rays from the apex).
    class LightGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Reflection probe: a wire influence box from halfExtents (entity-oriented) and a center
    /// cross.
    class ReflectionProbeGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Decal projection volume: the oriented box the decal clips to (local [-size/2, size/2],
    /// entity-oriented) and an arrow along local +Z, the projection direction. Decals land only on
    /// surfaces inside this box that face against the arrow (within the angle fade), so the gizmo
    /// makes placement mistakes visible when a decal does not show.
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

    /// The navigation zone's extents box (local, entity-oriented): the region the bake samples
    /// geometry from. Read-only, as the extents are edited in the inspector; drawn when the zone is
    /// selected (DrawWhenUnselected defaults to false).
    class NavMeshZoneGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;

        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// LOD debug overlay: with the toolbar toggle on, every mesh carrying a LOD chain draws its
    /// bounds tinted by the level this viewport's camera selects (green 0, yellow 1, orange 2, red
    /// 3+). It recomputes the raw pick through the exported pure selection functions, without
    /// hysteresis, since it visualizes the selection rather than the renderer's state.
    class LodOverlayGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// Edit-time physics collider wireframe, drawn from the RigidBodyComponent's shape and the
    /// entity transform with no physics world. Drawn for every entity when the "Show Colliders"
    /// toggle is on (ctx.showColliders), nothing when off. This is an editor concern, distinct from
    /// the runtime PhysicsSceneSettings.debugDraw, which draws live Jolt bodies during simulation.
    class PhysicsColliderGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// Edit-time compound-child collider wireframe (ColliderComponent). The runtime folds every
    /// descendant ColliderComponent into the nearest ancestor body, so "Show Colliders" draws them
    /// too; a rig that showed only the root shape would suggest the child colliders are not
    /// registered. Drawn at the child's own entity transform, in a distinct colour (compound teal),
    /// under the same toggle as the body renderer.
    class ChildColliderGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// Edit-time character-controller capsule (CharacterComponent), from radius and halfHeight and
    /// the entity transform. The companion of PhysicsColliderGizmoRenderer, under the same "Show
    /// Colliders" toggle.
    class CharacterColliderGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// Edit-time joint constraint gizmo (JointComponent): the anchor cross, a link line to the
    /// connected target entity, and the hinge or slider axis arrow, from component data and
    /// transforms. Same "Show Colliders" toggle and DebugView as the collider gizmos.
    class JointGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
        [[nodiscard]] bool DrawWhenUnselected() const override { return true; }
    };

    /// The authored spline curve and its control points, drawn whenever the owning entity is
    /// selected. The spline tool adds interaction; this keeps the curve visible under every tool.
    class SplineGizmoRenderer final : public IGizmoRenderer
    {
    public:
        [[nodiscard]] const TypeInfo* ComponentType() const override;
        void Draw(const Instance& component, scene::EntityHandle owner, GizmoContext& ctx) override;
    };

    /// Inverse kinematics on the selected entity: the solve itself while the scene runs, otherwise
    /// the chain where the bind pose puts it, with its target and its pole (or up), so a chain can
    /// be seen before it runs.
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

    /// Registers the built-in component gizmos (called from RegisterSceneEditor).
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
