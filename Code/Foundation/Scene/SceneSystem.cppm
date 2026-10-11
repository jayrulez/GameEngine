// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Scene - the `:system` partition.
///
/// SceneSystem: the base for a per-scene system - the unit a Scene owns, ticks per
/// phase, and notifies of entity lifecycle. A ComponentManager is the most common
/// SceneSystem (it stores + drives components), but a system need not own components
/// (a spatial index, a physics world, an audio listener could be plain systems).
///
/// Behavior lives here (and in scripts), not on the components themselves - which is
/// what lets components be plain value data in contiguous pools (see :component).
///
/// `Scene` is forward-declared (defined in :scene): a system only needs the incomplete
/// type for its reference-parameter hooks, so :system does not import :scene and the
/// partitions stay acyclic (:scene -> :component -> :system).

module;
#include "Core/Prelude.h"

export module foundation.scene:system;

import foundation.core;
import foundation.resource;
import :entity;
import :phase;

using namespace foundation::core;

export namespace foundation::scene
{

    class Scene;                // defined in :scene (same module)
    class ComponentManagerBase; // defined in :component (same module)

    // The scene's fixed, solid surfaces as one system knows them: the world-space triangles of
    // what it owns that does not move and that things stand on or are blocked by (physics: its
    // static, non-trigger bodies; terrain: its surface). Navigation bakes from every system that
    // has some, so what moves (agents, dynamic bodies, characters) never becomes level geometry.
    class IStaticGeometrySource
    {
    public:
        virtual ~IStaticGeometrySource() = default;

        // Appends the triangles touching `bounds` (3 positions each, counter-clockwise seen from
        // outside; a triangle reaching past `bounds` comes whole). `detail` is the finest spacing
        // worth producing: a sampled surface need not be finer.
        virtual void CollectStaticGeometry(Scene& scene, const AABB& bounds, f32 detail,
                                           Array<Float3>& outTriangles) = 0;
    };

    // An upright capsule of static content, in world space: from `foot` up `height` (the whole
    // capsule, foot to the top of its cap; the cylinder between the caps is height - 2 radius, at
    // least 0), `radius` round, in physics collision group `group`. A tree's trunk.
    struct StaticCapsule
    {
        Float3 foot{};
        f32 radius = 0.0f;
        f32 height = 0.0f;
        u8 group = 0;
    };

    // A system whose static content should be solid without an entity per piece (vegetation: a
    // forest's trunks): physics asks it for capsules when it builds its bodies, so neither domain
    // links the other. Specs/vegetation-colliders.md.
    class IStaticColliderSource
    {
    public:
        virtual ~IStaticColliderSource() = default;

        // Appends the capsules this system's static content stands on the world as. False when its
        // content is not ready yet (a resource still resolving): nothing is appended, and physics
        // asks again later.
        virtual bool CollectStaticCapsules(Scene& scene, Array<StaticCapsule>& outCapsules) = 0;
    };

    // A system that knows how much of the world an entity takes up (render: its meshes; physics:
    // its colliders), for one that only wants the space: the editor framing a selection. Neither
    // side names the other's components; EntityWorldBounds (in :scene) merges every answer.
    class ISceneEntityBounds
    {
    public:
        virtual ~ISceneEntityBounds() = default;

        // The world-space box this system's components on `entity` fill. False when it has
        // nothing on the entity to measure (or what it has is not loaded yet).
        [[nodiscard]] virtual bool EntityBounds(Scene& scene, EntityHandle entity, AABB& out) = 0;
    };

    // What a ray found on a solid surface (world space).
    struct SceneRayHit
    {
        f32 distance = 0.0f;
        Float3 position{};
        Float3 normal{0.0f, 1.0f, 0.0f};
    };

    // A system that can answer a ray against the scene's solid surfaces (physics: its bodies,
    // triggers never). Lets a system that does not depend on the one that owns the surfaces ask
    // what is there: foot IK finds the ground under a foot through it.
    class ISceneRayQuery
    {
    public:
        virtual ~ISceneRayQuery() = default;

        // The closest solid hit along unit `direction` from `origin` within `maxDistance`, among
        // the collision groups in `groupMask` (bit g = group g). False on a miss.
        [[nodiscard]] virtual bool CastRay(Float3 origin, Float3 direction, f32 maxDistance, u32 groupMask,
                                           SceneRayHit& out) = 0;
    };

    // What a ray found on an entity's drawn surface (world space): the entity, where, and the
    // corner of the triangle it met nearest the hit (a vertex to snap to).
    struct SceneSurfaceHit
    {
        EntityHandle entity{};
        f32 distance = 0.0f;
        Float3 position{};
        Float3 normal{0.0f, 1.0f, 0.0f};
        Float3 vertex{};
    };

    // A system that can answer a ray against what it draws (render: its meshes' triangles), for
    // one that wants the visible surface rather than the solid one: the editor snapping a dragged
    // entity onto a vertex. Neither names the other's components; RaycastSurface (in :scene) asks
    // every system that answers.
    class ISceneSurfaceQuery
    {
    public:
        virtual ~ISceneSurfaceQuery() = default;

        // The closest hit along unit `direction` from `origin` within `maxDistance`, on the
        // entities `accept` takes (an empty function takes all). False on a miss.
        [[nodiscard]] virtual bool RaycastSurface(Scene& scene, Float3 origin, Float3 direction, f32 maxDistance,
                                                  const Function<bool(EntityHandle)>& accept,
                                                  SceneSurfaceHit& out) = 0;
    };

    // A system that moves characters (physics: its character controllers), for a system that does
    // not depend on it: an animator walking its character by the clip's root motion.
    class ISceneCharacterMotion
    {
    public:
        virtual ~ISceneCharacterMotion() = default;

        // Whether `entity` has a character this system moves.
        [[nodiscard]] virtual bool HasCharacter(EntityHandle entity) const = 0;
        // Its walking velocity (world, m/s; the horizontal part) until the next call: it keeps
        // colliding and sliding, and gravity and jumps stay its own. Zero stops it.
        virtual void MoveCharacter(EntityHandle entity, Float3 velocity) = 0;
    };

    class SceneSystem
    {
    public:
        virtual ~SceneSystem() = default;

        // Capability query (the As*() idiom, since the engine is -fno-rtti): a system that is a
        // component manager returns itself, so the Scene can drive component-init / lookup
        // without a dynamic cast. Plain systems return null.
        [[nodiscard]] virtual ComponentManagerBase* AsComponentManager() noexcept
        {
            return nullptr;
        }
        // The same idiom for a system that owns static level geometry (IStaticGeometrySource).
        [[nodiscard]] virtual IStaticGeometrySource* AsStaticGeometrySource() noexcept
        {
            return nullptr;
        }
        // And for a system that answers rays against solid surfaces (ISceneRayQuery).
        [[nodiscard]] virtual ISceneRayQuery* AsRayQuery() noexcept { return nullptr; }
        // And for one that moves characters (ISceneCharacterMotion).
        [[nodiscard]] virtual ISceneCharacterMotion* AsCharacterMotion() noexcept { return nullptr; }
        // And for one with static content to make solid (IStaticColliderSource).
        [[nodiscard]] virtual IStaticColliderSource* AsStaticColliderSource() noexcept { return nullptr; }
        // And for one that measures entities (ISceneEntityBounds).
        [[nodiscard]] virtual ISceneEntityBounds* AsEntityBounds() noexcept { return nullptr; }
        // And for one that answers rays against what it draws (ISceneSurfaceQuery).
        [[nodiscard]] virtual ISceneSurfaceQuery* AsSurfaceQuery() noexcept { return nullptr; }

        // --- lifecycle (Scene calls these) ---
        virtual void OnSceneCreate(Scene& /*scene*/) {} // added to a scene
        virtual void OnSceneStarted() {}                // play mode began
        virtual void OnSceneStopped() {}                // play mode ended
        virtual void OnEntityDestroyed(EntityHandle /*entity*/) {}
        virtual void OnEntityActiveChanged(EntityHandle /*entity*/, bool /*active*/) {}

        // --- per-frame work ---
        // Called for each phase this system participates in (switch on `phase`). The Scene
        // runs phases in ScenePhase order; within a phase, systems run in UpdateOrder.
        virtual void OnUpdate(ScenePhase /*phase*/, f32 /*deltaTime*/) {}
        virtual void OnFixedUpdate(f32 /*fixedDeltaTime*/) {}

        // --- scene-level settings (the Sedulous scene-modules pattern) ---
        // A system with ONE per-scene settings block (not per-entity state) exposes it here:
        // the editor inspects it when no entity is selected, and SerializeScene persists it
        // with the scene. All four override together or not at all.
        //   SettingsType():      the settings struct's reflected TypeInfo (null = no settings)
        //   SettingsInstance():  the live struct (property edits write it directly)
        //   SettingsId():        stable on-disk id (like a manager's SerializationTypeId)
        //   SerializeSettings(): field serialization (the caller wraps it in the type's
        //                        versioned payload; a stale version is refused, not migrated)
        [[nodiscard]] virtual const TypeInfo* SettingsType() const noexcept { return nullptr; }
        [[nodiscard]] virtual void* SettingsInstance() noexcept { return nullptr; }
        [[nodiscard]] virtual StringView SettingsId() const noexcept { return {}; }
        virtual void SerializeSettings(ISerializer& /*ar*/) {}

        // A settings block that can take its values from a shared asset (a render profile) says so,
        // and answers which values are in effect and which asset gives them, so a tool edits what
        // is in effect without naming the block's type.
        //   SettingsProfileType():       the profile's product type (null = the block has none)
        //   EffectiveSettingsInstance(): the values in effect (SettingsType()'s layout)
        //   SettingsProfile():           the profile in use (nil = the block's own values)
        //   UseSettingsProfile(id):      take the values from that profile (nil = the block's own)
        //   CopySettingsProfileIntoScene(): the values in effect become the block's own, in use
        [[nodiscard]] virtual const TypeInfo* SettingsProfileType() const noexcept { return nullptr; }
        [[nodiscard]] virtual void* EffectiveSettingsInstance() noexcept { return SettingsInstance(); }
        [[nodiscard]] virtual Guid SettingsProfile() noexcept { return Guid{}; }
        virtual void UseSettingsProfile(const Guid& /*profile*/) {}
        virtual void CopySettingsProfileIntoScene() {}

        // Bind every resource::Ref this system holds (settings blocks included) through the
        // manager - the post-load resolve pass. ComponentManagerBase overrides it for component
        // pools; plain systems with resource-bearing settings override it too. Default: nothing.
        virtual void ResolveResources(foundation::resource::ResourceManager& /*manager*/) {}
        // Bind the resource::Refs ONE entity's data in this system holds - for a subtree that
        // arrived after the scene was resolved (a runtime prefab spawn), so the spawn binds what
        // it added rather than re-walking the scene. A system with no per-entity data does
        // nothing; SerializableComponentManager resolves that entity's component.
        virtual void ResolveEntityResources(EntityHandle /*entity*/,
                                            foundation::resource::ResourceManager& /*manager*/)
        {
        }

        // Lower runs earlier within a phase.
        [[nodiscard]] virtual i32 UpdateOrder() const noexcept { return 0; }
        // If true, OnUpdate/OnFixedUpdate are skipped while the scene's simulation is
        // paused (edit mode). Systems with mixed work can instead query Scene::SimulationEnabled.
        [[nodiscard]] virtual bool IsSimulationOnly() const noexcept { return false; }
    };

} // namespace foundation::scene
