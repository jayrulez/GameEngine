/// Draconic::Scene - the `:system` partition.
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
#include "Draconic.Foundation/Prelude.h"

export module draconic.scene:system;

import draconic.foundation;
import draconic.resource;
import :entity;
import :phase;

using namespace draconic::foundation;

export namespace draconic::scene
{

    class Scene;                // defined in :scene (same module)
    class ComponentManagerBase; // defined in :component (same module)

    class SceneSystem
    {
    public:
        virtual ~SceneSystem() = default;

        // Capability query (the As*() idiom, since Draconic is -fno-rtti): a system that is a
        // component manager returns itself, so the Scene can drive component-init / lookup
        // without a dynamic cast. Plain systems return null.
        [[nodiscard]] virtual ComponentManagerBase* AsComponentManager() noexcept
        {
            return nullptr;
        }

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
        //                        versioned payload, so bodies can gate on ar.Version())
        [[nodiscard]] virtual const TypeInfo* SettingsType() const noexcept { return nullptr; }
        [[nodiscard]] virtual void* SettingsInstance() noexcept { return nullptr; }
        [[nodiscard]] virtual StringView SettingsId() const noexcept { return {}; }
        virtual void SerializeSettings(ISerializer& /*ar*/) {}

        // Bind every resource::Ref this system holds (settings blocks included) through the
        // manager - the post-load resolve pass. ComponentManagerBase overrides it for component
        // pools; plain systems with resource-bearing settings override it too. Default: nothing.
        virtual void ResolveResources(draconic::resource::ResourceManager& /*manager*/) {}

        // Lower runs earlier within a phase.
        [[nodiscard]] virtual i32 UpdateOrder() const noexcept { return 0; }
        // If true, OnUpdate/OnFixedUpdate are skipped while the scene's simulation is
        // paused (edit mode). Systems with mixed work can instead query Scene::SimulationEnabled.
        [[nodiscard]] virtual bool IsSimulationOnly() const noexcept { return false; }
    };

} // namespace draconic::scene
