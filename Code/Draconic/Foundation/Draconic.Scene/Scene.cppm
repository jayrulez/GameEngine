/// Draconic::Scene - the `:scene` partition.
///
/// Scene: an isolated world of entities with a transform hierarchy (and, in later
/// phases, per-scene systems). Two parallel pools indexed by entity slot:
///   * the entity table - generation-guarded, free-list reuse, persistent-Guid <->
///     handle map, active/name state.
///   * the transform hierarchy - local TRS + cached world matrix per entity, in a
///     doubly-linked parent/child/sibling tree with O(1) splice (head+tail+back
///     pointers), a dirty-flag cascade, and a two-pass UpdateTransforms that snapshots
///     the previous world matrix (for motion vectors) and recomputes only dirty
///     subtrees. Destruction is recursive (children first) and immediate for now;
///     deferred-during-update destroy lands with the update loop in a later phase.

module;
#include "Draconic.Foundation/Prelude.h"
#include <type_traits>

export module draconic.scene:scene;

import draconic.foundation;
import :entity;
import :phase;
import :system;
import :component;
import :events;

using namespace draconic::foundation;

export namespace draconic::scene
{

    class Scene
    {
    public:
        explicit Scene(StringView name = {}, IAllocator& allocator = DefaultAllocator())
            : m_allocator(&allocator), m_name(name, allocator)
        {
        }

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        void SetName(StringView name) { m_name = String(name); }

        [[nodiscard]] u32 EntityCount() const noexcept { return m_aliveCount; }

        // ---- entity lifecycle ----

        // Creates an entity with a fresh random Guid; starts as a root, active, identity transform.
        EntityHandle CreateEntity(StringView name = {});
        // Creates an entity with a specific Guid (used when loading a scene from disk).
        EntityHandle CreateEntity(const Guid& id, StringView name = {});

        // Destroys an entity and its whole subtree. No-op if the handle is stale. If called
        // during Update (a system destroying entities), destruction is deferred to the
        // frame's Cleanup so iteration stays stable.
        void DestroyEntity(EntityHandle entity);

        [[nodiscard]] bool IsValid(EntityHandle entity) const noexcept;

        [[nodiscard]] Guid GetEntityId(EntityHandle entity) const;
        [[nodiscard]] EntityHandle FindEntity(const Guid& id);

        [[nodiscard]] StringView GetEntityName(EntityHandle entity) const;
        void SetEntityName(EntityHandle entity, StringView name);

        [[nodiscard]] bool IsActive(EntityHandle entity) const noexcept;
        void SetActive(EntityHandle entity, bool active);

        template <typename Fn>
        void ForEachEntity(Fn&& fn) const
        {
            for (u32 i = 0; i < m_entities.Size(); ++i)
            {
                if (m_entities[i].alive)
                {
                    fn(EntityHandle{i, m_entities[i].generation});
                }
            }
        }

        [[nodiscard]] u64 Revision() const noexcept { return m_revision; }

        // ---- prefab instances (runtime-only bookkeeping; scene.resource drives it) ----
        //
        // One state record per spawned prefab instance. `sourceIds[i]` is the member's guid in
        // the PREFAB PAYLOAD (the stable delta key), `liveIds[i]` its guid in THIS scene.
        // Baselines capture the template state as of spawn (per-member local transform +
        // per-component serialized blob); the scene serializer diffs live state against them at
        // save time, so overrides are DERIVED - nothing tracks edits, and undo/redo can never
        // desynchronize the override set. Never serialized as-is: the scene serializer persists
        // instances as ref+deltas (or expanded, for snapshots) and rebuilds this state on load.
        struct PrefabComponentBaseline
        {
            Guid sourceEntity;
            String typeId;  // the owning manager's SerializationTypeId
            Array<u8> blob; // binary WriteComponent capture at spawn
        };
        struct PrefabInstanceState
        {
            Guid prefabId;         // the prefab asset/product instance guid
            Guid rootEntityId;     // live guid of the instance's root entity
            Array<Guid> sourceIds; // payload guids (parallel to liveIds)
            Array<Guid> liveIds;
            Array<Transform>
                baselineTransforms; // parallel to sourceIds (template local transforms)
            Array<PrefabComponentBaseline> componentBaselines;

            // NESTING (P4): an instance spawned BY another instance's payload record links to its
            // owner; nestedRootSourceId is this instance's stable identity in the owner's
            // namespace (the payload record's root id) - rebuilds match on it. Nil = top-level.
            Guid ownerRootEntityId{};
            Guid nestedRootSourceId{};
            // Top-level only, TRANSIENT (recomputed at spawn): every prefab id this instance's
            // payload consumed (own + nested records) - template edits to any of them rebuild
            // this instance through its owner payload.
            Array<Guid> referencedPrefabIds;
        };

        void AddPrefabInstance(UniquePtr<PrefabInstanceState> state);

        [[nodiscard]] PrefabInstanceState* FindPrefabInstanceByRoot(const Guid& rootEntityId);

        /// Removes the state whose root is `rootEntityId` (the entities are the caller's business).
        void RemovePrefabInstance(const Guid& rootEntityId);

        /// Visits every instance whose ROOT still resolves; states whose root entity was
        /// destroyed are pruned lazily here (no destroy-hook bookkeeping).
        template <typename Fn>
        void ForEachPrefabInstance(Fn&& fn)
        {
            usize w = 0;
            for (usize i = 0; i < m_prefabInstances.Size(); ++i)
            {
                if (!FindEntity(m_prefabInstances[i]->rootEntityId).IsAssigned())
                {
                    continue;
                } // prune
                if (w != i)
                {
                    m_prefabInstances[w] =
                        static_cast<UniquePtr<PrefabInstanceState>&&>(m_prefabInstances[i]);
                }
                fn(*m_prefabInstances[w]);
                ++w;
            }
            m_prefabInstances.Resize(w);
        }

        [[nodiscard]] usize PrefabInstanceCount() const noexcept;

        /// Drops ALL prefab-instance bookkeeping (snapshot restore repopulates from the stream).
        void ClearPrefabInstances() { m_prefabInstances.Clear(); }

        // A prefab instance READ from a scene stream, awaiting its payload: the scene serializer
        // can't resolve prefab assets itself (no DB access), so it parks descriptors here and
        // ResolveScenePrefabs (scene.resource) respawns them with a caller-supplied resolver -
        // the same two-phase shape as component resource Refs + ResolveSceneResources.
        struct PendingPrefabComponentOp
        {
            Guid sourceEntity;
            String typeId;
            u8 op = 0;      // 0 = modify, 1 = add, 2 = remove (blob empty)
            Array<u8> blob; // binary component payload for modify/add
        };
        struct PendingPrefabInstance
        {
            Guid prefabId;
            Guid parentEntityId; // nil = scene root
            Transform rootTransform;
            Array<Guid> sourceIds; // parallel member guid map
            Array<Guid> liveIds;
            Array<Guid> destroyedMembers;     // source ids the user deleted from the instance
            Array<Guid> overrideTransformIds; // source ids with transform overrides
            Array<Transform> overrideTransforms;
            Array<PendingPrefabComponentOp> componentOps;
            // NESTING (P4): mirror of PrefabInstanceState's links (see there). rootLiveId is the
            // instance root's live id (in a PAYLOAD record: the owner-namespace id nested scene
            // records match against).
            Guid rootLiveId{};
            Guid ownerRootEntityId{};
            Guid nestedRootSourceId{};
            // The sibling immediately AFTER the root at capture time (nil = it was the last
            // child): spawn appends records after the plain members, then restores list order
            // from this link. Namespace follows the record's (owner template / live scene).
            Guid nextSiblingId{};
            // False = the root transform matched its baseline at capture: the scene never moved
            // this NESTED instance, so on respawn the owner TEMPLATE's placement wins (a moved
            // root is a scene override and re-applies). Top-level placements always apply.
            // Revert forces it false. Serialized with the record since the order/placement wire
            // change (no pre-change compat).
            bool applyPlacement = true;
        };

        void AddPendingPrefabInstance(UniquePtr<PendingPrefabInstance> pending);
        [[nodiscard]] Array<UniquePtr<PendingPrefabInstance>> TakePendingPrefabInstances();
        [[nodiscard]] usize PendingPrefabInstanceCount() const noexcept;
        /// Non-consuming walk of the parked pendings (transcode re-emits them verbatim so a
        /// load->save cycle without ResolveScenePrefabs stays lossless).
        void ForEachPendingPrefabInstance(const Function<void(PendingPrefabInstance&)>& fn);

        // ---- transform hierarchy ----

        void SetLocalTransform(EntityHandle entity, const Transform& transform);
        [[nodiscard]] Transform GetLocalTransform(EntityHandle entity) const;
        void SetLocalPosition(EntityHandle entity, Float3 position);

        // World matrix from the most recent UpdateTransforms (Identity until first update).
        [[nodiscard]] Float4x4 GetWorldMatrix(EntityHandle entity) const;
        [[nodiscard]] Float4x4 GetPrevWorldMatrix(EntityHandle entity) const;
        // Translation row of the world matrix (row-vector convention).
        [[nodiscard]] Float3 GetWorldPosition(EntityHandle entity) const;
        // True iff the world matrix was recomputed in the most recent UpdateTransforms
        // (moved, reparented, or a dirty ancestor cascaded through it). Read in PostTransform.
        [[nodiscard]] bool IsTransformUpdatedThisFrame(EntityHandle entity) const;

        [[nodiscard]] EntityHandle GetParent(EntityHandle entity) const;
        /// First entity in the ROOT sibling list (walk with GetNextSibling for list order - the
        /// order the hierarchy displays and serialization preserves).
        [[nodiscard]] EntityHandle GetFirstRoot() const noexcept { return m_firstRoot; }
        [[nodiscard]] EntityHandle GetFirstChild(EntityHandle entity) const;
        [[nodiscard]] EntityHandle GetNextSibling(EntityHandle entity) const;
        [[nodiscard]] EntityHandle FirstRoot() const noexcept { return m_firstRoot; }

        [[nodiscard]] u32 GetChildCount(EntityHandle entity) const;

        /// First alive entity with this exact name (names are NOT unique - first in slot
        /// order wins), invalid if none. Linear scan; prefer FindEntity(Guid) for identity.
        [[nodiscard]] EntityHandle FindEntityByName(StringView name) const;

        /// Direct child of `parent` (or a scene ROOT when `parent` is invalid) with this
        /// name, in sibling order; invalid if none.
        [[nodiscard]] EntityHandle FindChildByName(EntityHandle parent, StringView name) const;

        /// Resolve a '/'-separated hierarchy path from the scene roots, e.g.
        /// "Player/Weapon/Muzzle". Empty/leading/trailing/double slashes are tolerated. Each
        /// segment matches a child name at that depth; invalid if any segment misses.
        [[nodiscard]] EntityHandle FindEntityByPath(StringView path) const;

        // Reparents `child` under `parent` (Invalid() = make a root). Rejects a reparent
        // that would form a cycle (parent is `child` or a descendant of it).
        void SetParent(EntityHandle child, EntityHandle parent);

        // World-preserving reparent (editor semantics: the entity stays put in the world; its LOCAL
        // transform is recomputed relative to the new parent via TRS decompose). World matrices are
        // composed fresh from the local chain, so this is correct even with dirty cached transforms.
        // No-op when the underlying move is refused (cycle etc.).
        void SetParent(EntityHandle child, EntityHandle parent, bool keepWorldTransform);

        void MoveBefore(EntityHandle child, EntityHandle sibling, bool keepWorldTransform);

        // Fresh world matrix composed up the parent chain (independent of the cached worldMatrix,
        // which is only current after UpdateTransforms).
        [[nodiscard]] Float4x4 ComposeWorldMatrix(EntityHandle entity) const;

        // Sibling ORDERING: moves `child` under `sibling`'s parent, immediately BEFORE `sibling`
        // (into the root list when `sibling` is a root). Same guards as SetParent: both must be
        // valid, and moving an entity below its own subtree (cycle) is refused. O(1).
        // (SetParent(child, parent) is the companion "append at END of parent" operation - it
        // re-appends even when the parent is unchanged, i.e. it doubles as move-to-end.)
        void MoveBefore(EntityHandle child, EntityHandle sibling);

        // Two-pass world-matrix update: clear last frame's "updated" flags + snapshot the
        // previous world matrix for entities that stopped moving, then recompute only the
        // dirty roots and their subtrees (depth-first, parent before child).
        void UpdateTransforms();

        // Entity indices whose world matrix was recomputed in the most recent UpdateTransforms.
        // Lifetime hazard: an index here may belong to an entity destroyed afterwards - gate
        // reads on IsValid(handle). Rewritten each UpdateTransforms.
        [[nodiscard]] Span<const u32> TransformsUpdatedThisFrame() const noexcept;

        // ---- per-scene systems ----

        // Constructs + adds a system of type T (one per type); the Scene owns it. Returns a
        // borrowed pointer. Runs OnSceneCreate immediately.
        template <typename T, typename... Args>
        T* AddSystem(Args&&... args)
        {
            static_assert(std::is_base_of_v<SceneSystem, T>, "T must derive from SceneSystem");
            T* system = m_allocator->New<T>(Forward<Args>(args)...);
            m_systems.PushBack(
                UniquePtr<SceneSystem>(static_cast<SceneSystem*>(system), *m_allocator));
            m_systemsByType.InsertOrAssign(&TypeOf<T>(), static_cast<SceneSystem*>(system));
            InsertSortedSystem(static_cast<SceneSystem*>(system));
            system->OnSceneCreate(*this);
            return system;
        }

        template <typename T>
        [[nodiscard]] T* GetSystem() noexcept
        {
            SceneSystem* const* found = m_systemsByType.Find(&TypeOf<T>());
            return (found != nullptr) ? static_cast<T*>(*found) : nullptr;
        }
        template <typename T>
        [[nodiscard]] bool HasSystem() const noexcept
        {
            return m_systemsByType.Contains(&TypeOf<T>());
        }

        // Visits every component manager (systems that are managers), in UpdateOrder.
        template <typename Fn>
        void ForEachManager(Fn&& fn)
        {
            for (SceneSystem* s : m_sortedSystems)
            {
                if (ComponentManagerBase* m = s->AsComponentManager())
                {
                    fn(*m);
                }
            }
        }
        // Visits every system (managers AND plain systems), in UpdateOrder - the editor's
        // scene-settings inspector and SerializeScene walk systems with settings blocks.
        template <typename Fn>
        void ForEachSystem(Fn&& fn)
        {
            for (SceneSystem* s : m_sortedSystems)
            {
                fn(*s);
            }
        }
        // The serializable manager with this on-disk type id, or null (used on scene load
        // to route a component record to its pool).
        [[nodiscard]] ComponentManagerBase* FindManagerBySerializationId(StringView typeId);

        // The component manager whose component type is `componentType`, or null. This is the
        // scene-side resolve for `entity.get(Type)`: callers hold {scene, entity, manager} and
        // re-resolve the LIVE component via manager->GetComponentInstance(entity) on every access
        // - never a stashed component pointer (the sparse-set pool swap-removes, so an address can
        // move or, worse, point at a different entity's component). The manager pointer itself is
        // scene-owned and stable for the scene's lifetime, so this lookup runs once per get().
        [[nodiscard]] ComponentManagerBase* FindManagerByComponentType(const TypeInfo& componentType)
        {
            ComponentManagerBase* found = nullptr;
            ForEachManager(
                [&](ComponentManagerBase& m)
                {
                    if (found == nullptr && m.ComponentType() == &componentType)
                    {
                        found = &m;
                    }
                });
            return found;
        }

        // The re-resolving component handle for `entity.get(Type)`: a RESOLVE-mode Variant that
        // recomputes the LIVE component address on every access (FindManagerByComponentType, then
        // GetComponentInstance per deref). Empty when no manager handles `componentType`. A dead
        // entity or removed component resolves to null, so a script get/set/call over it is a clean
        // no-op - never a stale/cross-entity pointer (Fable Correction 1). Defined in SceneImpl.cpp.
        [[nodiscard]] Variant MakeComponentRef(EntityHandle entity, const TypeInfo& componentType);

        // ---- events ----
        // This scene's native event bus: C++ systems Publish/Subscribe directly (a C++-only game is
        // first-class); script reaches it through a bridge. Drained at the scene tick's top level
        // (Scene::Update), so a handler runs with no VM call active. See :events.
        [[nodiscard]] EventBus& Events() noexcept { return m_events; }

        // ---- play / edit state ----

        [[nodiscard]] bool IsStarted() const noexcept { return m_started; }
        [[nodiscard]] bool SimulationEnabled() const noexcept { return m_simulationEnabled; }
        void SetSimulationEnabled(bool enabled) noexcept { m_simulationEnabled = enabled; }

        // ---- per-scene time (the scene IS the simulatable unit) ----
        // Each scene owns its time scale and fixed-step accumulator, so pausing/slowing one
        // scene (the Game tab) never affects another (a Simulate page beside it). The
        // effective frame dt a scene sees = host dt x context TimeScale x scene TimeScale
        // (the SceneSubsystem applies the scene factor; the context factor is already in
        // the dt it receives).

        void SetTimeScale(f32 scale) noexcept { m_timeScale = scale < 0.0f ? 0.0f : scale; }
        [[nodiscard]] f32 TimeScale() const noexcept { return m_timeScale; }

        /// Fixed-lane configuration (step seconds + spiral-of-death clamp).
        void SetFixedTiming(f32 step, u32 maxSteps) noexcept;
        [[nodiscard]] f32 FixedTimeStep() const noexcept { return m_stepper.step; }
        /// Interpolation weight for fixed-rate consumers (physics pose smoothing);
        /// published by AdvanceTime after its steps.
        [[nodiscard]] f32 FixedAlpha() const noexcept { return m_fixedAlpha; }

        /// The frame drive (SceneSubsystem::BeginFrame): accumulates `scaledDelta` (already
        /// context- AND scene-scaled), runs FixedUpdate for each whole step, publishes the
        /// leftover as FixedAlpha. Tests wanting exact-step control call FixedUpdate directly.
        u32 AdvanceTime(f32 scaledDelta);

        // Enters play mode: simulation on, notify systems. (Editor "stop" calls Stop.)
        void Start();
        void Stop();

        // ---- update loop ----

        // Runs one frame: initialize pending components, the gameplay phases, the transform
        // recompute, render/spatial extraction, then deferred destruction. Phases run in
        // ScenePhase order; within a phase, systems run in UpdateOrder. Simulation-only
        // systems are skipped while SimulationEnabled is false.
        void Update(f32 deltaTime);

        void FixedUpdate(f32 fixedDeltaTime);

        // Initializes components added since the last call (deferred init), across all managers.
        void InitializePendingComponents();

    protected:
        struct EntitySlot
        {
            u32 generation = 0;
            bool active = false;
            bool alive = false;
            Guid persistentId;
            String name;
        };

        // Per-entity transform node: local TRS + cached world/prev-world matrices, and the
        // doubly-linked parent/child/sibling pointers (head+tail+back for O(1) splice).
        struct TransformData
        {
            Transform local;
            Float4x4 worldMatrix = Float4x4::Identity();
            Float4x4 prevWorldMatrix = Float4x4::Identity();
            EntityHandle parent = EntityHandle::Invalid();
            EntityHandle firstChild = EntityHandle::Invalid();
            EntityHandle lastChild = EntityHandle::Invalid();
            EntityHandle nextSibling = EntityHandle::Invalid();
            EntityHandle prevSibling = EntityHandle::Invalid();
            bool dirty = false;
            bool updatedThisFrame = false;
        };

        EntityHandle CreateEntityInternal(const Guid& id, StringView name);

        void DestroyEntityImmediate(EntityHandle entity);

        // Marks an entity dirty, cascading to its whole subtree AND up to its ancestors
        // (UpdateTransforms only walks dirty *roots*, so a dirty child needs a dirty root).
        void MarkDirty(EntityHandle entity);

        // Rewrite `child`'s LOCAL transform so its world matrix equals `childWorld` under its
        // CURRENT parent (the keep-world half of a reparent). Row-vector: local = world * parent⁻¹.
        void ApplyWorldAsLocal(EntityHandle child, const Float4x4& childWorld);

        void UpdateTransformRecursive(u32 index, const Float4x4& parentWorld);

        // O(1) append to a (head, tail) sibling list - tail pointer avoids an O(n) walk.
        void AppendToList(EntityHandle entity, EntityHandle& head, EntityHandle& tail);

        // O(1) splice-out using the back pointer (no walk to find the predecessor).
        void RemoveFromParent(EntityHandle child);

        // Whether `entity` is `ancestor` or somewhere below it (walks up from entity).
        [[nodiscard]] bool IsDescendantOf(EntityHandle entity, EntityHandle ancestor) const;

        // Runs one gameplay phase across systems in UpdateOrder, honoring sim gating.
        void RunPhase(ScenePhase phase, f32 deltaTime);

        // Insertion sort into m_sortedSystems, ascending by UpdateOrder (stable).
        void InsertSortedSystem(SceneSystem* system);

        // Destroys entities queued during Update (snapshot, since destroying a subtree can
        // be re-entrant). Stale/duplicate entries are skipped by the IsValid guard.
        void ProcessPendingDestroys();

        IAllocator* m_allocator;
        String m_name;
        Array<EntitySlot> m_entities;        // entity pool (index = slot)
        Array<TransformData> m_transforms;   // parallel transform pool (same index)
        Array<u32> m_freeList;               // free slot indices for reuse
        HashMap<Guid, EntityHandle> m_idMap; // persistent Guid -> live handle
        Array<u32> m_transformsUpdatedThisFrame;
        EntityHandle m_firstRoot = EntityHandle::Invalid();
        EntityHandle m_lastRoot = EntityHandle::Invalid();
        Random m_rng;
        Array<UniquePtr<PrefabInstanceState>> m_prefabInstances;
        Array<UniquePtr<PendingPrefabInstance>> m_pendingPrefabs;
        u32 m_aliveCount = 0;
        u64 m_revision = 0;

        // per-scene systems
        EventBus m_events;                                      // this scene's native event bus
        Array<UniquePtr<SceneSystem>> m_systems;                // ownership
        HashMap<const TypeInfo*, SceneSystem*> m_systemsByType; // lookup by type
        Array<SceneSystem*> m_sortedSystems;                    // non-owning, UpdateOrder-sorted
        Array<EntityHandle> m_pendingDestroys;
        bool m_isUpdating = false;
        bool m_started = false;
        bool m_simulationEnabled = true;
        f32 m_timeScale = 1.0f;
        foundation::FixedStepper m_stepper;
        f32 m_fixedAlpha = 0.0f;
    };

} // namespace draconic::scene
