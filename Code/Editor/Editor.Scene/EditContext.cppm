// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :edit partition.
//
// SceneEditContext: the scene mutation mediator for one page. There is one per page, never a global
// one, so several scenes can be edited at once. Every mutation is an IEditorCommand on the page's
// stack; nothing edits the scene directly. Commands reference entities by persistent Guid, which is
// stable across destroy and undo (handles are generation-guarded pool slots and die with the
// entity), and resolve them through the scene at execute time.
//
// DestroyEntity's undo restores the full serialized subtree: names, transforms, active flags,
// hierarchy, and every serializable component (through the managers' WriteComponent and
// ReadComponent, the same per-entity routing SerializeScene uses).
//
// Reparenting preserves the world transform (Scene's keep-world overloads and a TRS decompose);
// undo restores the captured local transform exactly, with no drift from a decompose round trip.
// Reordering under the same parent leaves the local transform untouched.

module;
#include "Core/Prelude.h"

export module editor.scene:edit;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource; // ResolveSceneResources (pasted/restored refs bind immediately)
import editor.core;

using namespace foundation::core;

export namespace editor
{
    namespace scene = foundation::scene;

    /// Where a reflected property lives on a component: on the component itself (`container` null)
    /// or on element `index` of one of its reflected container properties (a struct inside an
    /// `Array<Struct>`, such as a vegetation layer's density). Every property command and inspector
    /// row builder takes one, so a struct element in a list edits just like a component field: by
    /// name, undoable, and merged per (entity, component, path, property).
    struct ComponentPropertyPath
    {
        const char* container = nullptr; // static string from PropertyInfo::name
        usize index = 0;

        [[nodiscard]] bool IsEmpty() const noexcept { return container == nullptr; }
        [[nodiscard]] bool operator==(const ComponentPropertyPath& other) const noexcept
        {
            if ((container == nullptr) != (other.container == nullptr))
            {
                return false;
            }
            if (container == nullptr)
            {
                return true;
            }
            usize i = 0;
            while (container[i] != 0 && container[i] == other.container[i])
            {
                ++i;
            }
            return container[i] == other.container[i] && index == other.index;
        }
    };

    class SceneEditContext
    {
    public:
        SceneEditContext(scene::Scene& scene, EditorCommandStack& commands)
            : m_scene(&scene), m_commands(&commands)
        {
        }

        SceneEditContext(const SceneEditContext&) = delete;
        SceneEditContext& operator=(const SceneEditContext&) = delete;

        [[nodiscard]] scene::Scene& Scene() noexcept { return *m_scene; }
        [[nodiscard]] EditorCommandStack& Commands() noexcept { return *m_commands; }

        /// The runtime resource manager (optional; wired by the page). When set, commands that
        /// materialize components from blobs (paste, duplicate, destroy-undo) resolve the scene's
        /// resource refs immediately. Without it, a pasted mesh ref stays unbound and nothing
        /// renders until the next scene load runs the resolve pass.
        void SetResources(foundation::resource::ResourceManager* resources) noexcept;
        void ResolveRestoredResources();
        /// Binds `entity`'s component of `componentType` to what its references name now, after an
        /// edit wrote a reference's id through reflection, so the mesh it names shows at once.
        void ResolveComponentResources(const Guid& entity, const TypeInfo* componentType);
        /// Binds a settings block's references to what they name now, likewise.
        void ResolveSettingsResources(const TypeInfo* settingsType);

        /// Payload resolver for nested prefab records (wired by the page from the source DB).
        /// Without it, spawning a prefab that contains other prefabs skips the nested parts.
        void SetPrefabResolver(scene::PrefabPayloadResolver resolver);
        [[nodiscard]] const scene::PrefabPayloadResolver* PrefabResolver() const noexcept;

        /// Per-page entity selection, by persistent Guid (survives destroy/undo round-trips).
        [[nodiscard]] Selection<Guid>& EntitySelection() noexcept { return m_selection; }

        // === Entity clipboard / duplication ===
        //
        // A captured subtree is a self-contained binary blob: pre-order entity records (the
        // original guid, for relinking parents within the subtree, and the name, transform and
        // active flag), each with its serializable components (typeId and payload). Pasting
        // instantiates the records with fresh guids (identity is per scene; commands stay routed by
        // guid), so the same blob pastes repeatedly and across scenes and pages. The blob's kind
        // for the editor clipboard is "entities".

        /// Capture `entity`'s subtree into a clipboard blob (empty on failure).
        [[nodiscard]] Array<byte> CopyEntity(const Guid& entity);

        /// Paste a captured subtree as a child of `parent` (at the root when nil or unresolved).
        /// Returns the new root entity's Guid (nil on failure); it becomes the selection.
        Guid PasteEntities(Span<const byte> blob, const Guid& parent = Guid{});

        /// Duplicate an entity subtree as a sibling (same parent). Returns the copy's root
        /// Guid (nil on failure); it becomes the selection. One undo step.
        Guid DuplicateEntity(const Guid& entity);

        [[nodiscard]] String UniqueSiblingName(StringView base, scene::EntityHandle parent);

        // === Component clipboard ===
        // The blob is the typeId string followed by the manager's WriteComponent payload; its kind
        // is "component".

        /// Capture one serializable component into a clipboard blob (empty on failure).
        [[nodiscard]] Array<byte> CopyComponent(const Guid& entity, const TypeInfo* componentType);

        /// The component type id a clipboard blob carries ("" when unreadable), for menu labels.
        [[nodiscard]] static String PeekComponentTypeId(Span<const byte> blob);

        /// Paste a copied component onto `entity` (adds it, or overwrites the existing one).
        /// Undo restores the prior state exactly. False when the blob or the manager does not apply.
        bool PasteComponent(const Guid& entity, Span<const byte> blob);

        /// Resolve a selected or stored Guid to a live handle (Invalid if the entity is gone).
        [[nodiscard]] scene::EntityHandle Resolve(const Guid& id);

        // === Mutations (each an undoable command; failed executes are dropped by the stack) ===

        /// Create an empty entity (child of `parent` when valid). Returns its Guid (empty on
        /// failure). Redo recreates the same Guid. The new entity becomes the selection.
        Guid CreateEntity(StringView name, const Guid& parent = Guid{});

        /// Destroy an entity and its subtree (undo restores everything, components included).
        void DestroyEntity(const Guid& entity);

        /// Rename an entity (consecutive renames of the same entity merge into one undo entry).
        void RenameEntity(const Guid& entity, StringView newName);

        /// Reparent an entity (an empty `newParent` makes it a root; it is appended at the end of
        /// the new parent's children). Cycles are refused (the command's Execute fails and is
        /// dropped).
        void ReparentEntity(const Guid& entity, const Guid& newParent);

        /// Sibling reorder: move an entity immediately before `sibling` (under sibling's parent)
        /// or, when `sibling` is empty, to the end of the root list. Undo restores the previous
        /// parent and position. Cycles and no-ops are refused.
        void MoveEntityBefore(const Guid& entity, const Guid& sibling);

        /// Set an entity's active flag (undoable).
        void SetEntityActive(const Guid& entity, bool active);

        /// Set an entity's local transform. Consecutive edits of the same entity merge into one
        /// undo entry (inspector field scrubs, gizmo drags).
        void SetLocalTransform(const Guid& entity, const Transform& transform);

        /// Set a reflected component property by Variant. Consecutive edits of the same entity,
        /// component and property merge.
        void SetComponentProperty(const Guid& entity, const TypeInfo* componentType,
                                  const char* property, const Variant& value);
        /// The same, on the property owner `path` addresses (a struct element in a list).
        void SetComponentProperty(const Guid& entity, const TypeInfo* componentType,
                                  const ComponentPropertyPath& path, const char* property,
                                  const Variant& value);

        /// Point a component's resource::Ref<T> property at a new asset (the inspector's picker):
        /// writes the Guid through PropertyInfo::address and rebinds through the ResourceManager
        /// (which may be null; the ref then resolves on the next scene open). Undo restores the
        /// previous target. Not merged: each pick is its own undo entry.
        template <typename T>
        void SetComponentResourceRef(const Guid& entity, const TypeInfo* componentType,
                                     const char* property, const Guid& value,
                                     foundation::resource::ResourceManager* resources)
        {
            SetComponentResourceRef<T>(entity, componentType, ComponentPropertyPath{}, property,
                                       value, resources);
        }
        template <typename T>
        void SetComponentResourceRef(const Guid& entity, const TypeInfo* componentType,
                                     const ComponentPropertyPath& path, const char* property,
                                     const Guid& value,
                                     foundation::resource::ResourceManager* resources)
        {
            (void)m_commands->Execute(UniquePtr<IEditorCommand>(
                editor::EditorRootAllocator().New<SetResourceRefCommand<T>>(
                    *this, entity, componentType, path, property, value, resources),
                editor::EditorRootAllocator()));
        }

        /// The same, for a reference-shaped property known only by its TypeInfo (an agent's
        /// component_set, a generic editor): the id is set through the type's ReferenceOps, the
        /// runtime binding dropped, and the entity's subtree resolved again so the new id binds
        /// (`resources` may be null; it then binds on the next scene open). Undo restores the old
        /// id the same way. Not merged.
        void SetComponentReference(const Guid& entity, const TypeInfo* componentType,
                                   const ComponentPropertyPath& path, const char* property,
                                   const Guid& value, foundation::resource::ResourceManager* resources)
        {
            // The command class is a member declared below: a member body sees the whole class.
            (void)m_commands->Execute(UniquePtr<IEditorCommand>(
                editor::EditorRootAllocator().New<SetReferenceCommand>(*this, entity, componentType,
                                                                      path, property, value, resources),
                editor::EditorRootAllocator()));
        }
        /// Point a component's EntityRef property at a new entity (the inspector's entity picker):
        /// the entity-reference counterpart of SetComponentResourceRef. Undoable; a nil `target`
        /// clears it.
        void SetComponentEntityRef(const Guid& entity, const TypeInfo* componentType,
                                   const char* property, const Guid& target)
        {
            SetComponentEntityRef(entity, componentType, ComponentPropertyPath{}, property, target);
        }
        void SetComponentEntityRef(const Guid& entity, const TypeInfo* componentType,
                                   const ComponentPropertyPath& path, const char* property,
                                   const Guid& target)
        {
            (void)m_commands->Execute(UniquePtr<IEditorCommand>(
                editor::EditorRootAllocator().New<SetEntityRefCommand>(*this, entity, componentType,
                                                                       path, property, target),
                editor::EditorRootAllocator()));
        }

        /// Set a reflected component property whose type a Variant cannot construct at runtime
        /// (enums known only by TypeInfo): writes the underlying integer through
        /// PropertyInfo::address. Merges like SetComponentProperty.
        void SetComponentPropertyRaw(const Guid& entity, const TypeInfo* componentType,
                                     const char* property, i64 value);
        void SetComponentPropertyRaw(const Guid& entity, const TypeInfo* componentType,
                                     const ComponentPropertyPath& path, const char* property,
                                     i64 value);

        /// Set a reflected property on a scene system's settings block (the scene inspector,
        /// shown when no entity is selected). Same merge semantics as SetComponentProperty
        /// (scrubs collapse into one undo entry); the settings instance is re-derived from
        /// the scene each apply.
        void SetSceneSettingProperty(const TypeInfo* settingsType, const char* property,
                                     const Variant& value);

        /// Any reference-shaped settings field (its type's reference traits), undoable.
        void SetSceneSettingReference(const TypeInfo* settingsType, const char* property,
                                      const Guid& value,
                                      foundation::resource::ResourceManager* resources)
        {
            (void)m_commands->Execute(UniquePtr<IEditorCommand>(
                editor::EditorRootAllocator().New<SetSceneSettingReferenceCommand>(
                    *this, settingsType, property, value, resources),
                editor::EditorRootAllocator()));
        }

        /// Whole-block scene-settings edit, for shapes reflection rows cannot express (such as the
        /// collision-group matrix). `newBlob` is the settings serialized through the system's own
        /// SerializeSettings; undo restores the prior serialized state. The caller builds the blob
        /// from an edited copy, so the live settings are touched only through the command and undo
        /// and redo replay cleanly.
        bool ApplySceneSettingsBlock(const TypeInfo* settingsType, Array<byte> newBlob);

        /// Enum flavor of SetSceneSettingProperty (writes the underlying integer through
        /// PropertyInfo::address, for the same reason as SetComponentPropertyRaw).
        void SetSceneSettingPropertyRaw(const TypeInfo* settingsType, const char* property,
                                        i64 value);

        /// The scene system whose SettingsType() is `settingsType` (null if none).
        [[nodiscard]] scene::SceneSystem* FindSystemBySettingsType(const TypeInfo* settingsType);

        // === A settings block whose source is a profile ===
        // Its values in effect are the profile's, so an edit of a value field lands there (every
        // scene sharing the profile sees it) and the page persists it to the profile's asset. The
        // block's own fields (those marked "sceneOnly": the source and the profile reference) stay
        // in the scene.

        /// The profile an edit of `property` lands in: nil when it lands in the scene's block.
        [[nodiscard]] Guid SettingsEditProfile(const TypeInfo* settingsType, const char* property);
        /// The values an edit for `profile` (from SettingsEditProfile) writes: the scene's block
        /// for nil, the profile's values while the block still uses it, else null.
        [[nodiscard]] void* SettingsEditTarget(const TypeInfo* settingsType, const Guid& profile);
        /// The values `property` shows and edits now: SettingsEditTarget of SettingsEditProfile.
        [[nodiscard]] void* SettingsValuesFor(const TypeInfo* settingsType, const char* property)
        {
            return SettingsEditTarget(settingsType, SettingsEditProfile(settingsType, property));
        }
        /// Called by the settings commands after they write a profile's values (nil = no-op).
        void NoteSettingsProfileEdited(const TypeInfo* settingsType, const Guid& profile);
        /// Set by the page to persist a profile edit (as an asset edit for the save flow).
        Function<void(const TypeInfo* settingsType, const Guid& profile)> OnSettingsProfileEdited;

        /// One undoable mutation of a settings block, whatever it touches (its source, or a copy of
        /// a profile's values into it). The live block is mutated, captured and restored, then the
        /// change is applied through the block command, which keeps the block as it was before.
        /// False when there is no such block.
        bool MutateSceneSettings(const TypeInfo* settingsType,
                                 const Function<void(scene::SceneSystem&)>& mutate);

        /// Add a default-constructed component (undoable; fails if already present).
        void AddComponent(const Guid& entity, const TypeInfo* componentType);

        /// Remove a component. Undo restores it with full fidelity through the manager's
        /// serialization when available, otherwise from the reflected properties.
        void RemoveComponent(const Guid& entity, const TypeInfo* componentType);

        /// The scene manager whose component type is `type` (null if none).
        [[nodiscard]] scene::ComponentManagerBase* FindManager(const TypeInfo* type);

        /// One undoable mutation of `entity`'s component of `componentType`, whatever it holds (a
        /// reflected list, or a hidden one like a Script component's behaviours). The live value is
        /// mutated, captured and restored from its serialized form, then pasted, with the paste
        /// command keeping the value from before, so the whole change is one undo step. False when
        /// the entity has no such component.
        bool MutateComponent(const Guid& entity, const TypeInfo* componentType,
                             const Function<void(const Instance&)>& mutate);

        /// The typed form, for a copyable component: the value before is restored by copy, so
        /// runtime state its serialized form leaves out survives.
        template <typename C>
        bool MutateComponent(const Guid& entity, const Function<void(C&)>& mutate)
        {
            scene::ComponentManagerBase* manager = FindManager(&TypeOf<C>());
            const scene::EntityHandle e = Resolve(entity);
            if (manager == nullptr || !e.IsAssigned() || !manager->HasComponent(e))
            {
                return false;
            }
            C* live = static_cast<C*>(manager->GetComponentInstance(e).Pointer());
            if (live == nullptr)
            {
                return false;
            }
            const C before = *live;
            mutate(*live);
            Array<byte> blob = CopyComponent(entity, &TypeOf<C>());
            *live = before;
            return !blob.IsEmpty() &&
                   PasteComponent(entity, Span<const byte>{blob.Data(), blob.Size()});
        }

        /// The instance a property path addresses: the entity's component of `componentType` (an
        /// empty path) or the element of its reflected container property at `path.index`.
        /// `outOwnerType` (optional) receives the type whose properties apply: the component type
        /// or the container's element type. Empty when anything along the path is missing (no
        /// component, no such container, index out of range). It is re-derived on every call, so it
        /// never holds a pointer across a pool move.
        [[nodiscard]] Instance ResolvePropertyOwner(const Guid& entity, const TypeInfo* componentType,
                                                    const ComponentPropertyPath& path,
                                                    const TypeInfo** outOwnerType = nullptr);

        /// True if `possibleAncestor` is `entity` itself or one of its ancestors.
        [[nodiscard]] bool IsSelfOrAncestor(const Guid& entity, const Guid& possibleAncestor);

    private:
        // Depth-first visit of a live subtree (root included).
        template <typename Fn>
        void CollectSubtree(scene::EntityHandle root, Fn&& fn)
        {
            if (!root.IsAssigned())
            {
                return;
            }
            fn(root);
            for (scene::EntityHandle c = m_scene->GetFirstChild(root); c.IsAssigned();
                 c = m_scene->GetNextSibling(c))
            {
                CollectSubtree(c, fn);
            }
        }

        // === Commands ===

        class CreateEntityCommand final : public IEditorCommand
        {
        public:
            CreateEntityCommand(SceneEditContext& ctx, StringView name, const Guid& parent)
                : m_ctx(&ctx), m_name(name), m_parent(parent)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                scene::Scene& scene = m_ctx->Scene();
                const scene::EntityHandle parent = m_ctx->Resolve(m_parent);
                if (m_parent != Guid{} && !parent.IsAssigned())
                {
                    return false;
                } // parent gone

                // First execute records the generated Guid; redo recreates the same identity.
                const scene::EntityHandle entity = (m_id != Guid{})
                                                       ? scene.CreateEntity(m_id, m_name.AsView())
                                                       : scene.CreateEntity(m_name.AsView());
                if (!entity.IsAssigned())
                {
                    return false;
                }
                if (m_id == Guid{})
                {
                    m_id = scene.GetEntityId(entity);
                }
                if (parent.IsAssigned())
                {
                    scene.SetParent(entity, parent);
                }
                return true;
            }

            void Undo() override { m_ctx->Scene().DestroyEntity(m_ctx->Resolve(m_id)); }

            [[nodiscard]] StringView TypeId() const override { return u8"create_entity"; }
            [[nodiscard]] const Guid& CreatedId() const noexcept { return m_id; }

        private:
            SceneEditContext* m_ctx;
            String m_name;
            Guid m_parent;
            Guid m_id;
        };

        // Shared subtree snapshot for the clipboard and duplicate paths. DestroyEntityCommand keeps
        // its own undo records, of the same shape but with a different lifetime.
        struct SubtreeComponentRecord
        {
            String typeId;
            Array<byte> blob;
        };
        struct SubtreeRecord
        {
            Guid id;     // original guid, only for relinking parents (pastes mint fresh ids)
            Guid parent; // nil = subtree root
            String name;
            Transform local;
            bool active = true;
            Array<SubtreeComponentRecord> components;
        };

        // Pre-order capture (parents before children), components via the serializable managers.
        void CaptureSubtreeRecords(scene::EntityHandle root, Array<SubtreeRecord>& out);

        static void WriteSubtreeRecords(BinarySerializer& ar, Array<SubtreeRecord>& records);

        [[nodiscard]] static Array<SubtreeRecord> ParseSubtreeBlob(Span<const byte> blob);

    public:
        // === Prefab instances ===

        /// Spawns an instance of a prefab payload as an undoable command (redo recreates the
        /// same member guids). `payload` is the prefab's "scene" stream, captured by value so
        /// undo/redo stay stable against later asset edits. Returns the instance root's guid
        /// (nil on failure); it becomes the selection.
        Guid SpawnPrefabInstance(const Guid& prefabId, Array<byte> payload,
                                 const Guid& parent = Guid{},
                                 const Transform* rootTransform = nullptr,
                                 const Guid& placeBefore = Guid{});

        /// Replaces `entity`'s subtree with an instance of `prefabId` (the second half of
        /// create-from-selection): one undo group that spawns at the same parent and transform,
        /// then destroys the original.
        Guid ReplaceWithPrefabInstance(const Guid& entity, const Guid& prefabId,
                                       Array<byte> payload);

        /// Reverts a prefab member's component to its spawn-time baseline, undoably. Template
        /// components restore their baseline payload (added again if the user removed them);
        /// components the user added (with no baseline) are removed. No-op for non-members.
        bool RevertComponentToBaseline(const Guid& entity, const TypeInfo* componentType);

        class SpawnPrefabCommand final : public IEditorCommand
        {
        public:
            SpawnPrefabCommand(SceneEditContext& ctx, const Guid& prefabId, Array<byte> payload,
                               const Guid& parent, const Transform* rootTransform,
                               const Guid& placeBefore = Guid{})
                : m_ctx(&ctx), m_prefabId(prefabId), m_payload(Move(payload)), m_parent(parent),
                  m_placeBefore(placeBefore)
            {
                if (rootTransform != nullptr)
                {
                    m_rootTransform = *rootTransform;
                    m_hasTransform = true;
                }
            }

            [[nodiscard]] bool Execute() override
            {
                scene::Scene& scene = m_ctx->Scene();
                MemoryStream stream;
                (void)stream.Write(m_payload.Data(), m_payload.Size());
                (void)stream.Seek(0, SeekOrigin::Begin);
                const scene::EntityHandle parent = m_ctx->Resolve(m_parent);
                Array<const scene::Scene::PendingPrefabInstance*> subPtrs;
                for (const auto& sub : m_nestedPins)
                {
                    subPtrs.PushBack(sub.Get());
                }
                const scene::EntityHandle root = scene::SpawnPrefab(
                    scene, stream, m_prefabId, parent,
                    m_preassigned.Size() > 0 ? &m_preassigned : nullptr, m_ctx->PrefabResolver(),
                    subPtrs.IsEmpty() ? nullptr : &subPtrs);
                if (!root.IsAssigned())
                {
                    return false;
                }
                if (m_hasTransform)
                {
                    scene.SetLocalTransform(root, m_rootTransform);
                }
                if (m_placeBefore != Guid{})
                {
                    const scene::EntityHandle before = m_ctx->Resolve(m_placeBefore);
                    if (before.IsAssigned())
                    {
                        scene.MoveBefore(root, before);
                    }
                }
                m_rootId = scene.GetEntityId(root);
                if (m_preassigned.Size() == 0)
                {
                    // First run: pin the minted member guids so redo recreates them exactly,
                    // including every nested instance's members (as sub-records with the template
                    // placement).
                    if (scene::Scene::PrefabInstanceState* state =
                            scene.FindPrefabInstanceByRoot(m_rootId))
                    {
                        for (usize i = 0; i < state->sourceIds.Size(); ++i)
                        {
                            m_preassigned.InsertOrAssign(state->sourceIds[i], state->liveIds[i]);
                        }
                    }
                    scene.ForEachPrefabInstance(
                        [&](scene::Scene::PrefabInstanceState& nested)
                        {
                            if (nested.ownerRootEntityId != m_rootId)
                            {
                                return;
                            }
                            auto pin =
                                MakeUnique<scene::Scene::PendingPrefabInstance>(editor::EditorRootAllocator());
                            pin->prefabId = nested.prefabId;
                            pin->sourceIds = nested.sourceIds;
                            pin->liveIds = nested.liveIds;
                            pin->nestedRootSourceId = nested.nestedRootSourceId;
                            pin->applyPlacement = false;
                            m_nestedPins.PushBack(
                                static_cast<UniquePtr<scene::Scene::PendingPrefabInstance>&&>(pin));
                        });
                }
                m_ctx->ResolveRestoredResources(); // spawned refs render this frame
                return true;
            }

            void Undo() override
            {
                scene::Scene& scene = m_ctx->Scene();
                // Destroy every live member (the root takes its subtree; FindEntity guards
                // members already gone), then the bookkeeping.
                for (const auto& kv : m_preassigned)
                {
                    const scene::EntityHandle e = scene.FindEntity(kv.value);
                    if (e.IsAssigned())
                    {
                        scene.DestroyEntity(e);
                    }
                }
                scene.RemovePrefabInstance(m_rootId);
            }

            [[nodiscard]] StringView TypeId() const override { return u8"spawn_prefab"; }
            [[nodiscard]] Guid RootGuid() const { return m_rootId; }

        private:
            SceneEditContext* m_ctx;
            Guid m_prefabId;
            Array<byte> m_payload;
            Guid m_parent;
            Guid m_placeBefore; // sibling slot (replace flow: the entity being replaced)
            Transform m_rootTransform{};
            bool m_hasTransform = false;
            Guid m_rootId;
            HashMap<Guid, Guid> m_preassigned;
            Array<UniquePtr<scene::Scene::PendingPrefabInstance>>
                m_nestedPins; // source -> live; pinned on first Execute
        };

    private:
        Guid RunPasteCommand(Array<SubtreeRecord> records, const Guid& parent);

        // Instantiates captured records with fresh guids; a map from old to new guids relinks
        // parents within the subtree. Redo recreates the same fresh guids (minted once, on the
        // first Execute).
        class PasteEntitiesCommand final : public IEditorCommand
        {
        public:
            PasteEntitiesCommand(SceneEditContext& ctx, Array<SubtreeRecord> records,
                                 const Guid& parent)
                : m_ctx(&ctx), m_records(Move(records)), m_parent(parent)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                if (m_records.IsEmpty())
                {
                    return false;
                }
                scene::Scene& scene = m_ctx->Scene();

                if (m_newIds.IsEmpty())
                {
                    // First run: mint the fresh identities through the scene (re-rolls until free).
                    for (const SubtreeRecord& record : m_records)
                    {
                        const scene::EntityHandle e = scene.CreateEntity(record.name.AsView());
                        m_newIds.PushBack(scene.GetEntityId(e));
                    }
                }
                else
                {
                    // Redo: the ids are free again (undo destroyed them), so recreate them exactly.
                    for (usize i = 0; i < m_records.Size(); ++i)
                    {
                        (void)scene.CreateEntity(m_newIds[i], m_records[i].name.AsView());
                    }
                }

                for (usize i = 0; i < m_records.Size(); ++i)
                {
                    const SubtreeRecord& record = m_records[i];
                    const scene::EntityHandle e = m_ctx->Resolve(m_newIds[i]);
                    scene.SetLocalTransform(e, record.local);
                    scene.SetActive(e, record.active);
                    // Parent: nil = the paste target; else the remapped intra-subtree parent.
                    scene::EntityHandle parent{};
                    if (record.parent == Guid{})
                    {
                        parent = m_ctx->Resolve(m_parent);
                    }
                    else
                    {
                        for (usize j = 0; j < m_records.Size(); ++j)
                        {
                            if (m_records[j].id == record.parent)
                            {
                                parent = m_ctx->Resolve(m_newIds[j]);
                                break;
                            }
                        }
                    }
                    if (parent.IsAssigned())
                    {
                        scene.SetParent(e, parent);
                    }
                    for (const SubtreeComponentRecord& component : record.components)
                    {
                        scene::ComponentManagerBase* mgr =
                            scene.FindManagerBySerializationId(component.typeId.AsView());
                        if (mgr == nullptr)
                        {
                            continue;
                        }
                        MemoryStream buffer;
                        (void)buffer.Write(component.blob.Data(), component.blob.Size());
                        (void)buffer.Seek(0, SeekOrigin::Begin);
                        BinarySerializer ar(buffer, SerializeMode::Read);
                        mgr->ReadComponent(ar, e);
                    }
                }
                m_ctx->ResolveRestoredResources(); // pasted refs render this frame, not next load
                return true;
            }

            void Undo() override
            {
                // Destroying the pasted root takes the whole subtree with it (records are
                // pre-order: index 0 is the root).
                const scene::EntityHandle root = m_ctx->Resolve(m_newIds[0]);
                if (root.IsAssigned())
                {
                    m_ctx->Scene().DestroyEntity(root);
                }
            }

            [[nodiscard]] StringView TypeId() const override { return u8"paste_entities"; }
            [[nodiscard]] Guid RootGuid() const
            {
                return m_newIds.IsEmpty() ? Guid{} : m_newIds[0];
            }

        private:
            SceneEditContext* m_ctx;
            Array<SubtreeRecord> m_records;
            Guid m_parent;
            Array<Guid> m_newIds; // parallel to m_records; minted on first Execute
        };

        // Apply a copied component blob to an entity; undo restores the exact prior state
        // (previous payload, or removal when the entity didn't have the component).
        class PasteComponentCommand final : public IEditorCommand
        {
        public:
            PasteComponentCommand(SceneEditContext& ctx, const Guid& entity, Span<const byte> blob)
                : m_ctx(&ctx), m_entity(entity)
            {
                m_blob.Reserve(blob.Size());
                for (byte b : blob)
                {
                    m_blob.PushBack(b);
                }
            }

            [[nodiscard]] bool Execute() override
            {
                scene::Scene& scene = m_ctx->Scene();
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return false;
                }

                MemoryStream buffer;
                (void)buffer.Write(m_blob.Data(), m_blob.Size());
                (void)buffer.Seek(0, SeekOrigin::Begin);
                BinarySerializer ar(buffer, SerializeMode::Read);
                foundation::core::Serialize(ar, "type", m_typeId);
                scene::ComponentManagerBase* mgr =
                    scene.FindManagerBySerializationId(m_typeId.AsView());
                if (mgr == nullptr || !ar.IsOk())
                {
                    return false;
                }

                // Snapshot the prior state once (Execute reruns on redo with the same result).
                if (!m_captured)
                {
                    m_hadComponent = mgr->HasComponent(e);
                    if (m_hadComponent)
                    {
                        MemoryStream prior;
                        BinarySerializer prev(prior, SerializeMode::Write);
                        mgr->WriteComponent(prev, e);
                        const Span<const byte> bytes = prior.Bytes();
                        m_previous.Reserve(bytes.Size());
                        for (byte b : bytes)
                        {
                            m_previous.PushBack(b);
                        }
                    }
                    m_captured = true;
                }

                mgr->ReadComponent(ar, e); // adds or overwrites
                m_ctx->ResolveRestoredResources();
                return ar.IsOk();
            }

            void Undo() override
            {
                scene::Scene& scene = m_ctx->Scene();
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                scene::ComponentManagerBase* mgr =
                    scene.FindManagerBySerializationId(m_typeId.AsView());
                if (!e.IsAssigned() || mgr == nullptr)
                {
                    return;
                }
                if (!m_hadComponent)
                {
                    mgr->RemoveComponent(e);
                    return;
                }
                MemoryStream buffer;
                (void)buffer.Write(m_previous.Data(), m_previous.Size());
                (void)buffer.Seek(0, SeekOrigin::Begin);
                BinarySerializer ar(buffer, SerializeMode::Read);
                mgr->ReadComponent(ar, e);
                m_ctx->ResolveRestoredResources();
            }

            [[nodiscard]] StringView TypeId() const override { return u8"paste_component"; }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            Array<byte> m_blob;
            String m_typeId;
            Array<byte> m_previous;
            bool m_hadComponent = false;
            bool m_captured = false;
        };

        class DestroyEntityCommand final : public IEditorCommand
        {
        public:
            DestroyEntityCommand(SceneEditContext& ctx, const Guid& entity)
                : m_ctx(&ctx), m_entity(entity)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                scene::Scene& scene = m_ctx->Scene();
                const scene::EntityHandle root = m_ctx->Resolve(m_entity);
                if (!root.IsAssigned())
                {
                    return false;
                }

                // Snapshot the subtree pre-order (parents before children) so Undo can recreate
                // top-down and parent immediately.
                m_records.Clear();
                m_ctx->CollectSubtree(root,
                                      [this, &scene](scene::EntityHandle e)
                                      {
                                          EntityRecord record;
                                          record.id = scene.GetEntityId(e);
                                          record.parent = scene.GetEntityId(scene.GetParent(e));
                                          record.name = String(scene.GetEntityName(e));
                                          record.local = scene.GetLocalTransform(e);
                                          record.active = scene.IsActive(e);
                                          scene.ForEachManager(
                                              [&](scene::ComponentManagerBase& mgr)
                                              {
                                                  if (!mgr.IsSerializable() || !mgr.HasComponent(e))
                                                  {
                                                      return;
                                                  }
                                                  ComponentRecord component;
                                                  component.typeId =
                                                      String(mgr.SerializationTypeId());
                                                  MemoryStream buffer;
                                                  BinarySerializer ar(buffer, SerializeMode::Write);
                                                  mgr.WriteComponent(ar, e);
                                                  const Span<const byte> bytes = buffer.Bytes();
                                                  component.blob.Reserve(bytes.Size());
                                                  for (byte b : bytes)
                                                  {
                                                      component.blob.PushBack(b);
                                                  }
                                                  record.components.PushBack(Move(component));
                                              });
                                          m_records.PushBack(Move(record));
                                      });

                scene.DestroyEntity(root);
                return true;
            }

            void Undo() override
            {
                scene::Scene& scene = m_ctx->Scene();
                // Pre-order records: parent entities are recreated before their children.
                for (const EntityRecord& record : m_records)
                {
                    const scene::EntityHandle e =
                        scene.CreateEntity(record.id, record.name.AsView());
                    scene.SetLocalTransform(e, record.local);
                    scene.SetActive(e, record.active);
                    const scene::EntityHandle parent = m_ctx->Resolve(record.parent);
                    if (parent.IsAssigned())
                    {
                        scene.SetParent(e, parent);
                    }
                    for (const ComponentRecord& component : record.components)
                    {
                        scene::ComponentManagerBase* mgr =
                            scene.FindManagerBySerializationId(component.typeId.AsView());
                        if (mgr == nullptr)
                        {
                            continue;
                        }
                        MemoryStream buffer;
                        (void)buffer.Write(component.blob.Data(), component.blob.Size());
                        (void)buffer.Seek(0, SeekOrigin::Begin);
                        BinarySerializer ar(buffer, SerializeMode::Read);
                        mgr->ReadComponent(ar, e);
                    }
                }
                m_ctx->ResolveRestoredResources();
            }

            [[nodiscard]] StringView TypeId() const override { return u8"destroy_entity"; }

        private:
            struct ComponentRecord
            {
                String typeId;
                Array<byte> blob;
            };
            struct EntityRecord
            {
                Guid id;
                Guid parent; // empty = root (or a parent outside the subtree resolved at undo)
                String name;
                Transform local;
                bool active = true;
                Array<ComponentRecord> components;
            };

            SceneEditContext* m_ctx;
            Guid m_entity;
            Array<EntityRecord> m_records;
        };

        class RenameEntityCommand final : public IEditorCommand
        {
        public:
            RenameEntityCommand(SceneEditContext& ctx, const Guid& entity, StringView newName)
                : m_ctx(&ctx), m_entity(entity), m_newName(newName)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return false;
                }
                if (!m_hasOld)
                {
                    m_oldName = String(m_ctx->Scene().GetEntityName(e));
                    m_hasOld = true;
                }
                m_ctx->Scene().SetEntityName(e, m_newName.AsView());
                return true;
            }

            void Undo() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (e.IsAssigned())
                {
                    m_ctx->Scene().SetEntityName(e, m_oldName.AsView());
                }
            }

            [[nodiscard]] StringView TypeId() const override { return u8"rename_entity"; }

            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<RenameEntityCommand&>(previous);
                if (prev.m_entity != m_entity)
                {
                    return false;
                }
                prev.m_newName = Move(m_newName); // previous keeps its original old name
                return true;
            }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            String m_newName;
            String m_oldName;
            bool m_hasOld = false;
        };

        class ReparentEntityCommand final : public IEditorCommand
        {
        public:
            ReparentEntityCommand(SceneEditContext& ctx, const Guid& entity, const Guid& newParent)
                : m_ctx(&ctx), m_entity(entity), m_newParent(newParent)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                scene::Scene& scene = m_ctx->Scene();
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return false;
                }
                const scene::EntityHandle parent = m_ctx->Resolve(m_newParent);
                if (m_newParent != Guid{} && !parent.IsAssigned())
                {
                    return false;
                }
                // Refuse cycles (reparenting onto self or a descendant).
                if (m_newParent != Guid{} && m_ctx->IsSelfOrAncestor(m_newParent, m_entity))
                {
                    return false;
                }

                if (!m_hasOld)
                {
                    m_oldParent = scene.GetEntityId(scene.GetParent(e));
                    // The exact old slot: the sibling the entity sat before (nil = it was last).
                    m_oldNextSibling = scene.GetEntityId(scene.GetNextSibling(e));
                    m_oldLocal = scene.GetLocalTransform(e);
                    m_hasOld = true;
                }
                if (m_oldParent == m_newParent)
                {
                    return false;
                } // a no-op: keep it off the undo stack
                // Editor semantics: the entity stays put in the world across a reparent (its local
                // transform is recomputed through a TRS decompose).
                scene.SetParent(e, parent, /*keepWorldTransform*/ true);
                return true;
            }

            void Undo() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return;
                }
                scene::Scene& scene = m_ctx->Scene();
                scene.SetParent(e, m_ctx->Resolve(m_oldParent));
                // SetParent appends to the end of the sibling list; restore the exact slot, or the
                // undone entity would visibly jump to the bottom of its list.
                const scene::EntityHandle before = m_ctx->Resolve(m_oldNextSibling);
                if (before.IsAssigned())
                {
                    scene.MoveBefore(e, before);
                }
                scene.SetLocalTransform(e, m_oldLocal); // exact, no decompose drift
            }

            [[nodiscard]] StringView TypeId() const override { return u8"reparent_entity"; }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            Guid m_newParent;
            Guid m_oldParent;
            Guid m_oldNextSibling;
            Transform m_oldLocal;
            bool m_hasOld = false;
        };

        // Sibling reorder: place `entity` before `sibling` (an empty sibling means the end of the
        // root list). Undo restores the exact previous position through the captured old next
        // sibling (empty when it was last under its old parent).
        class MoveEntityCommand final : public IEditorCommand
        {
        public:
            MoveEntityCommand(SceneEditContext& ctx, const Guid& entity, const Guid& sibling)
                : m_ctx(&ctx), m_entity(entity), m_sibling(sibling)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                scene::Scene& scene = m_ctx->Scene();
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return false;
                }
                const scene::EntityHandle sibling = m_ctx->Resolve(m_sibling);
                if (m_sibling != Guid{} && !sibling.IsAssigned())
                {
                    return false;
                }
                if (m_sibling == m_entity)
                {
                    return false;
                }
                // Cycle: the target slot's parent lies inside the moved entity's own subtree.
                if (sibling.IsAssigned())
                {
                    const scene::EntityHandle parent = scene.GetParent(sibling);
                    if (parent.IsAssigned() &&
                        m_ctx->IsSelfOrAncestor(scene.GetEntityId(parent), m_entity))
                    {
                        return false;
                    }
                }

                if (!m_hasOld)
                {
                    m_oldParent = scene.GetEntityId(scene.GetParent(e));
                    m_oldNext = scene.GetEntityId(scene.GetNextSibling(e));
                    m_oldLocal = scene.GetLocalTransform(e);
                    m_hasOld = true;
                }

                // Keep the world transform only when the parent changes; a same-parent reorder
                // keeps the exact local (no decompose round-trip noise).
                const scene::EntityHandle newParent = sibling.IsAssigned()
                                                          ? scene.GetParent(sibling)
                                                          : scene::EntityHandle::Invalid();
                const bool parentChanges = scene.GetEntityId(newParent) != m_oldParent;

                const u64 before = scene.Revision();
                if (sibling.IsAssigned())
                {
                    scene.MoveBefore(e, sibling, parentChanges);
                }
                else
                {
                    scene.SetParent(e, scene::EntityHandle::Invalid(), parentChanges);
                }
                return scene.Revision() != before; // an unchanged position is a no-op, dropped
            }

            void Undo() override
            {
                scene::Scene& scene = m_ctx->Scene();
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return;
                }
                const scene::EntityHandle oldNext = m_ctx->Resolve(m_oldNext);
                if (oldNext.IsAssigned())
                {
                    scene.MoveBefore(e, oldNext);
                }
                else
                {
                    scene.SetParent(e, m_ctx->Resolve(m_oldParent));
                } // was last: append
                scene.SetLocalTransform(e, m_oldLocal); // exact, no decompose drift
            }

            [[nodiscard]] StringView TypeId() const override { return u8"move_entity"; }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            Guid m_sibling;
            Guid m_oldParent;
            Guid m_oldNext;
            Transform m_oldLocal;
            bool m_hasOld = false;
        };

        class SetActiveCommand final : public IEditorCommand
        {
        public:
            SetActiveCommand(SceneEditContext& ctx, const Guid& entity, bool active)
                : m_ctx(&ctx), m_entity(entity), m_active(active)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return false;
                }
                m_old = m_ctx->Scene().IsActive(e);
                if (m_old == m_active)
                {
                    return false;
                } // no-op
                m_ctx->Scene().SetActive(e, m_active);
                return true;
            }
            void Undo() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (e.IsAssigned())
                {
                    m_ctx->Scene().SetActive(e, m_old);
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"set_active"; }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            bool m_active;
            bool m_old = false;
        };

        // The settings counterpart of SetResourceRefCommand: the reference lives on a scene
        // system's settings block (no entity), and the address is re-derived from the scene each
        // apply. It handles any reference-shaped field through its reference traits (the id) and
        // the system's own resolve (the rebind), so nothing names T.
        class SetSceneSettingReferenceCommand final : public IEditorCommand
        {
        public:
            SetSceneSettingReferenceCommand(SceneEditContext& ctx, const TypeInfo* settingsType,
                                            const char* property, const Guid& value,
                                            foundation::resource::ResourceManager* resources)
                : m_ctx(&ctx), m_settingsType(settingsType), m_property(property), m_new(value),
                  m_resources(resources)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const ReferenceOps* ops = nullptr;
                void* value = Resolve(ops);
                if (value == nullptr)
                {
                    return false;
                }
                if (!m_hasOld)
                {
                    const Guid* current = ops->Id(value);
                    m_old = current != nullptr ? *current : Guid{};
                    m_hasOld = true;
                }
                Apply(m_new);
                return true;
            }
            void Undo() override { Apply(m_old); }
            [[nodiscard]] StringView TypeId() const override { return u8"set_scene_setting_ref"; }

        private:
            void* Resolve(const ReferenceOps*& ops)
            {
                if (!m_routed)
                {
                    m_profile = m_ctx->SettingsEditProfile(m_settingsType, m_property);
                    m_routed = true;
                }
                void* target = m_ctx->SettingsEditTarget(m_settingsType, m_profile);
                const PropertyInfo* prop = FindProperty(*m_settingsType, m_property);
                if (target == nullptr || prop == nullptr || prop->address == nullptr ||
                    prop->type == nullptr || prop->type->reference == nullptr)
                {
                    return nullptr;
                }
                ops = prop->type->reference;
                return prop->address(Instance{target, m_settingsType});
            }
            void Apply(const Guid& id)
            {
                const ReferenceOps* ops = nullptr;
                void* value = Resolve(ops);
                if (value == nullptr)
                {
                    return;
                }
                ops->SetId(value, id);
                if (m_resources != nullptr)
                {
                    // The system binds its references (this one among them) as a load does.
                    m_ctx->FindSystemBySettingsType(m_settingsType)->ResolveResources(*m_resources);
                }
                m_ctx->NoteSettingsProfileEdited(m_settingsType, m_profile);
            }

            SceneEditContext* m_ctx;
            const TypeInfo* m_settingsType;
            const char* m_property;
            Guid m_new;
            Guid m_old;
            Guid m_profile; // where the edit lands (SettingsEditProfile), fixed at first apply
            bool m_routed = false;
            bool m_hasOld = false;
            foundation::resource::ResourceManager* m_resources;
        };

        template <typename T>
        class SetResourceRefCommand final : public IEditorCommand
        {
        public:
            SetResourceRefCommand(SceneEditContext& ctx, const Guid& entity, const TypeInfo* type,
                                  const ComponentPropertyPath& path, const char* property,
                                  const Guid& value, foundation::resource::ResourceManager* resources)
                : m_ctx(&ctx), m_entity(entity), m_type(type), m_path(path), m_property(property),
                  m_new(value), m_resources(resources)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                foundation::resource::Ref<T>* ref = ResolveRef();
                if (ref == nullptr)
                {
                    return false;
                }
                if (!m_hasOld)
                {
                    m_old = ref->id;
                    m_hasOld = true;
                }
                ref->SetId(m_new);
                ref->Rebind(m_resources);
                return true;
            }
            void Undo() override
            {
                if (foundation::resource::Ref<T>* ref = ResolveRef())
                {
                    ref->SetId(m_old);
                    ref->Rebind(m_resources);
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"set_resource_ref"; }

        private:
            // Component pools move on add/remove, so the address re-derives every apply.
            [[nodiscard]] foundation::resource::Ref<T>* ResolveRef()
            {
                const TypeInfo* ownerType = nullptr;
                const Instance owner =
                    m_ctx->ResolvePropertyOwner(m_entity, m_type, m_path, &ownerType);
                const PropertyInfo* prop = (owner.IsEmpty() || ownerType == nullptr)
                                               ? nullptr
                                               : FindProperty(*ownerType, m_property);
                void* address = (prop != nullptr && prop->address != nullptr)
                                    ? prop->address(owner)
                                    : nullptr;
                return static_cast<foundation::resource::Ref<T>*>(address);
            }

            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_type;
            ComponentPropertyPath m_path;
            const char* m_property;
            Guid m_new;
            Guid m_old;
            bool m_hasOld = false;
            foundation::resource::ResourceManager* m_resources;
        };

        /// The generic counterpart of SetResourceRefCommand: the property's type carries its
        /// ReferenceOps, so no T is named. Re-resolves the entity's subtree after each apply.
        class SetReferenceCommand final : public IEditorCommand
        {
        public:
            SetReferenceCommand(SceneEditContext& ctx, const Guid& entity, const TypeInfo* type,
                                const ComponentPropertyPath& path, const char* property,
                                const Guid& value, foundation::resource::ResourceManager* resources)
                : m_ctx(&ctx), m_entity(entity), m_type(type), m_path(path), m_property(property),
                  m_new(value), m_resources(resources)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const PropertyInfo* prop = nullptr;
                void* address = ResolveAddress(&prop);
                if (address == nullptr)
                {
                    return false;
                }
                const ReferenceOps& ops = *prop->type->reference;
                if (!m_hasOld)
                {
                    m_old = *ops.Id(address);
                    m_hasOld = true;
                }
                Apply(ops, address, m_new);
                return true;
            }
            void Undo() override
            {
                const PropertyInfo* prop = nullptr;
                if (void* address = ResolveAddress(&prop))
                {
                    Apply(*prop->type->reference, address, m_old);
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"set_reference"; }

        private:
            void Apply(const ReferenceOps& ops, void* address, const Guid& id)
            {
                ops.SetId(address, id);
                ops.ClearBinding(address);
                if (m_resources != nullptr)
                {
                    scene::ResolveEntityResources(m_ctx->Scene(), m_ctx->Resolve(m_entity),
                                                  *m_resources);
                }
            }
            // Component pools move on add/remove, so the address re-derives every apply; the
            // property must be a reference type.
            [[nodiscard]] void* ResolveAddress(const PropertyInfo** outProp)
            {
                const TypeInfo* ownerType = nullptr;
                const Instance owner =
                    m_ctx->ResolvePropertyOwner(m_entity, m_type, m_path, &ownerType);
                const PropertyInfo* prop = (owner.IsEmpty() || ownerType == nullptr)
                                               ? nullptr
                                               : FindProperty(*ownerType, m_property);
                if (prop == nullptr || prop->address == nullptr || prop->type == nullptr ||
                    !IsReferenceType(*prop->type))
                {
                    return nullptr;
                }
                *outProp = prop;
                return prop->address(owner);
            }

            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_type;
            ComponentPropertyPath m_path;
            const char* m_property;
            Guid m_new;
            Guid m_old;
            bool m_hasOld = false;
            foundation::resource::ResourceManager* m_resources;
        };

        // The entity-reference counterpart of SetResourceRefCommand: writes a component's
        // EntityRef.id through the property address, re-derived each apply because pools move, with
        // undo. No resource rebind: an EntityRef is a plain guid holder resolved at use.
        class SetEntityRefCommand final : public IEditorCommand
        {
        public:
            SetEntityRefCommand(SceneEditContext& ctx, const Guid& entity, const TypeInfo* type,
                                const ComponentPropertyPath& path, const char* property,
                                const Guid& value)
                : m_ctx(&ctx), m_entity(entity), m_type(type), m_path(path), m_property(property),
                  m_new(value)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                foundation::scene::EntityRef* ref = ResolveRef();
                if (ref == nullptr)
                {
                    return false;
                }
                if (!m_hasOld)
                {
                    m_old = ref->id;
                    m_hasOld = true;
                }
                ref->id = m_new;
                return true;
            }
            void Undo() override
            {
                if (foundation::scene::EntityRef* ref = ResolveRef())
                {
                    ref->id = m_old;
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"set_entity_ref"; }

        private:
            [[nodiscard]] foundation::scene::EntityRef* ResolveRef()
            {
                const TypeInfo* ownerType = nullptr;
                const Instance owner =
                    m_ctx->ResolvePropertyOwner(m_entity, m_type, m_path, &ownerType);
                const PropertyInfo* prop = (owner.IsEmpty() || ownerType == nullptr)
                                               ? nullptr
                                               : FindProperty(*ownerType, m_property);
                void* address = (prop != nullptr && prop->address != nullptr)
                                    ? prop->address(owner)
                                    : nullptr;
                return static_cast<foundation::scene::EntityRef*>(address);
            }

            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_type;
            ComponentPropertyPath m_path;
            const char* m_property;
            Guid m_new;
            Guid m_old;
            bool m_hasOld = false;
        };

        class SetTransformCommand final : public IEditorCommand
        {
        public:
            SetTransformCommand(SceneEditContext& ctx, const Guid& entity,
                                const Transform& transform)
                : m_ctx(&ctx), m_entity(entity), m_new(transform)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return false;
                }
                if (!m_hasOld)
                {
                    m_old = m_ctx->Scene().GetLocalTransform(e);
                    m_hasOld = true;
                }
                m_ctx->Scene().SetLocalTransform(e, m_new);
                return true;
            }
            void Undo() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (e.IsAssigned())
                {
                    m_ctx->Scene().SetLocalTransform(e, m_old);
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"set_transform"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<SetTransformCommand&>(previous);
                if (prev.m_entity != m_entity)
                {
                    return false;
                }
                prev.m_new = m_new; // previous keeps its original old transform
                return true;
            }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            Transform m_new;
            Transform m_old;
            bool m_hasOld = false;
        };

        // Replaces a scene system's whole settings block with a serialized blob; undo writes back
        // the block's prior serialized state.
        class SetSceneSettingsBlockCommand final : public IEditorCommand
        {
        public:
            SetSceneSettingsBlockCommand(SceneEditContext& ctx, const TypeInfo* settingsType,
                                         Array<byte>&& newBlob)
                : m_ctx(&ctx), m_settingsType(settingsType),
                  m_new(static_cast<Array<byte>&&>(newBlob))
            {
            }

            [[nodiscard]] bool Execute() override
            {
                scene::SceneSystem* system = m_ctx->FindSystemBySettingsType(m_settingsType);
                if (system == nullptr)
                {
                    return false;
                }
                if (m_old.IsEmpty())
                {
                    MemoryStream buffer;
                    BinarySerializer writer(buffer, SerializeMode::Write);
                    WriteSettings(*system, writer);
                    const Span<const byte> bytes = buffer.Bytes();
                    m_old.Reserve(bytes.Size());
                    for (byte b : bytes)
                    {
                        m_old.PushBack(b);
                    }
                }
                const bool applied = Apply(*system, m_new);
                m_ctx->ResolveRestoredResources(); // the block's references (a profile) bind
                return applied;
            }
            void Undo() override
            {
                scene::SceneSystem* system = m_ctx->FindSystemBySettingsType(m_settingsType);
                if (system != nullptr)
                {
                    (void)Apply(*system, m_old);
                    m_ctx->ResolveRestoredResources();
                }
            }
            [[nodiscard]] StringView TypeId() const override
            {
                return u8"set_scene_settings_block";
            }

        private:
            // Settings blobs carry the type's data-version chain (like the scene-file settings
            // path), so the same reader accepts them. A settings-less system (no SettingsType)
            // falls back to raw.
            static void WriteSettings(scene::SceneSystem& system, ISerializer& ar)
            {
                const TypeInfo* type = system.SettingsType();
                if (type == nullptr)
                {
                    system.SerializeSettings(ar);
                    return;
                }
                foundation::core::BeginVersionedPayload(ar, *type);
                system.SerializeSettings(ar);
                foundation::core::EndVersionedPayload(ar);
            }
            static bool Apply(scene::SceneSystem& system, const Array<byte>& blob)
            {
                MemoryStream buffer;
                if (buffer.Write(blob.Data(), blob.Size()) != blob.Size())
                {
                    return false;
                }
                (void)buffer.Seek(0, SeekOrigin::Begin);
                BinarySerializer reader(buffer, SerializeMode::Read);
                WriteSettings(system, reader); // Begin/EndVersionedPayload works both directions
                return true;
            }

            SceneEditContext* m_ctx;
            const TypeInfo* m_settingsType;
            Array<byte> m_new;
            Array<byte> m_old;
        };

        // Sets a property on a scene system's settings block, either by Variant (typed get and set)
        // or as a raw integer: a Variant of an enum type known only by TypeInfo cannot be
        // constructed, so those fields are written in place through PropertyInfo::address. The
        // instance is re-derived from the scene each apply; systems are stable, but re-deriving
        // keeps the command valid across snapshot restores.
        class SetSceneSettingCommand final : public IEditorCommand
        {
        public:
            SetSceneSettingCommand(SceneEditContext& ctx, const TypeInfo* settingsType,
                                   const char* property, const Variant& value)
                : m_ctx(&ctx), m_settingsType(settingsType), m_property(property), m_new(value)
            {
            }

            SetSceneSettingCommand(SceneEditContext& ctx, const TypeInfo* settingsType,
                                   const char* property, i64 rawValue)
                : m_ctx(&ctx), m_settingsType(settingsType), m_property(property),
                  m_newRaw(rawValue), m_raw(true)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const PropertyInfo* prop = nullptr;
                const Instance settings = ResolveSettings(&prop);
                if (settings.IsEmpty() || prop == nullptr)
                {
                    return false;
                }

                if (m_raw)
                {
                    void* address = (prop->address != nullptr) ? prop->address(settings) : nullptr;
                    if (address == nullptr)
                    {
                        return false;
                    }
                    if (!m_hasOld)
                    {
                        m_oldRaw = ReadEnumValue(address, *prop->type);
                        m_hasOld = true;
                    }
                    WriteEnumValue(address, *prop->type, m_newRaw);
                    m_ctx->NoteSettingsProfileEdited(m_settingsType, m_profile);
                    return true;
                }

                if (!m_hasOld)
                {
                    m_old = GetProperty(*prop, settings);
                    m_hasOld = true;
                }
                const bool set = SetProperty(*prop, settings, m_new).IsOk();
                m_ctx->NoteSettingsProfileEdited(m_settingsType, m_profile);
                return set;
            }

            void Undo() override
            {
                const PropertyInfo* prop = nullptr;
                const Instance settings = ResolveSettings(&prop);
                if (settings.IsEmpty() || prop == nullptr)
                {
                    return;
                }
                if (m_raw)
                {
                    if (void* address =
                            (prop->address != nullptr) ? prop->address(settings) : nullptr)
                    {
                        WriteEnumValue(address, *prop->type, m_oldRaw);
                    }
                }
                else
                {
                    (void)SetProperty(*prop, settings, m_old);
                }
                m_ctx->NoteSettingsProfileEdited(m_settingsType, m_profile);
            }

            [[nodiscard]] StringView TypeId() const override { return u8"set_scene_setting"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<SetSceneSettingCommand&>(previous);
                if (prev.m_settingsType != m_settingsType || prev.m_raw != m_raw ||
                    prev.m_profile != m_profile || !CStrEq(prev.m_property, m_property))
                {
                    return false;
                }
                prev.m_new = m_new; // previous keeps its original old value
                prev.m_newRaw = m_newRaw;
                return true;
            }

        private:
            [[nodiscard]] static bool CStrEq(const char* a, const char* b) noexcept
            {
                usize i = 0;
                while (a[i] != 0 && a[i] == b[i])
                {
                    ++i;
                }
                return a[i] == b[i];
            }

            [[nodiscard]] Instance ResolveSettings(const PropertyInfo** outProp)
            {
                if (!m_routed)
                {
                    m_profile = m_ctx->SettingsEditProfile(m_settingsType, m_property);
                    m_routed = true;
                }
                void* target = m_ctx->SettingsEditTarget(m_settingsType, m_profile);
                if (target == nullptr)
                {
                    return {};
                }
                *outProp = FindProperty(*m_settingsType, m_property);
                return Instance{target, m_settingsType};
            }

            SceneEditContext* m_ctx;
            const TypeInfo* m_settingsType;
            const char* m_property;
            Variant m_new;
            Variant m_old;
            i64 m_newRaw = 0;
            i64 m_oldRaw = 0;
            Guid m_profile; // where the edit lands (SettingsEditProfile), fixed at first apply
            bool m_routed = false;
            bool m_raw = false;
            bool m_hasOld = false;
        };

        class SetComponentPropertyCommand final : public IEditorCommand
        {
        public:
            SetComponentPropertyCommand(SceneEditContext& ctx, const Guid& entity,
                                        const TypeInfo* componentType,
                                        const ComponentPropertyPath& path, const char* property,
                                        const Variant& value)
                : m_ctx(&ctx), m_entity(entity), m_componentType(componentType), m_path(path),
                  m_property(property), m_new(value)
            {
            }

            SetComponentPropertyCommand(SceneEditContext& ctx, const Guid& entity,
                                        const TypeInfo* componentType,
                                        const ComponentPropertyPath& path, const char* property,
                                        i64 rawValue)
                : m_ctx(&ctx), m_entity(entity), m_componentType(componentType), m_path(path),
                  m_property(property), m_newRaw(rawValue), m_raw(true)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const PropertyInfo* prop = nullptr;
                const Instance component = ResolveComponent(&prop);
                if (component.IsEmpty() || prop == nullptr)
                {
                    return false;
                }

                if (m_raw)
                {
                    void* address = (prop->address != nullptr) ? prop->address(component) : nullptr;
                    if (address == nullptr)
                    {
                        return false;
                    }
                    if (!m_hasOld)
                    {
                        m_oldRaw = ReadEnumValue(address, *prop->type);
                        m_hasOld = true;
                    }
                    WriteEnumValue(address, *prop->type, m_newRaw);
                    return true;
                }

                if (!m_hasOld)
                {
                    m_old = GetProperty(*prop, component);
                    m_hasOld = true;
                }
                return SetProperty(*prop, component, m_new).IsOk();
            }

            void Undo() override
            {
                const PropertyInfo* prop = nullptr;
                const Instance component = ResolveComponent(&prop);
                if (component.IsEmpty() || prop == nullptr)
                {
                    return;
                }
                if (m_raw)
                {
                    if (void* address =
                            (prop->address != nullptr) ? prop->address(component) : nullptr)
                    {
                        WriteEnumValue(address, *prop->type, m_oldRaw);
                    }
                }
                else
                {
                    (void)SetProperty(*prop, component, m_old);
                }
            }

            [[nodiscard]] StringView TypeId() const override { return u8"set_component_property"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<SetComponentPropertyCommand&>(previous);
                if (prev.m_entity != m_entity || prev.m_componentType != m_componentType ||
                    prev.m_raw != m_raw || !(prev.m_path == m_path) ||
                    !detail_CStrEq(prev.m_property, m_property))
                {
                    return false;
                }
                prev.m_new = m_new; // previous keeps its original old value
                prev.m_newRaw = m_newRaw;
                return true;
            }

        private:
            [[nodiscard]] static bool detail_CStrEq(const char* a, const char* b) noexcept
            {
                usize i = 0;
                while (a[i] != 0 && a[i] == b[i])
                {
                    ++i;
                }
                return a[i] == b[i];
            }

            [[nodiscard]] Instance ResolveComponent(const PropertyInfo** outProperty)
            {
                // The owner the path addresses: the component, or a struct element of one of its
                // container properties (re-derived every time, as pools move).
                const TypeInfo* ownerType = nullptr;
                const Instance owner =
                    m_ctx->ResolvePropertyOwner(m_entity, m_componentType, m_path, &ownerType);
                if (owner.IsEmpty() || ownerType == nullptr)
                {
                    return {};
                }
                *outProperty = FindProperty(*ownerType, m_property);
                return owner;
            }

            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_componentType;
            ComponentPropertyPath m_path;
            const char* m_property; // static string from PropertyInfo::name
            Variant m_new;
            Variant m_old;
            i64 m_newRaw = 0;
            i64 m_oldRaw = 0;
            bool m_raw = false;
            bool m_hasOld = false;
        };

        class AddComponentCommand final : public IEditorCommand
        {
        public:
            AddComponentCommand(SceneEditContext& ctx, const Guid& entity, const TypeInfo* type)
                : m_ctx(&ctx), m_entity(entity), m_type(type)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                scene::ComponentManagerBase* mgr = m_ctx->FindManager(m_type);
                if (!e.IsAssigned() || mgr == nullptr)
                {
                    return false;
                }
                return mgr->AddDefaultComponent(e);
            }
            void Undo() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                scene::ComponentManagerBase* mgr = m_ctx->FindManager(m_type);
                if (e.IsAssigned() && mgr != nullptr)
                {
                    mgr->RemoveComponent(e);
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"add_component"; }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_type;
        };

        class RemoveComponentCommand final : public IEditorCommand
        {
        public:
            RemoveComponentCommand(SceneEditContext& ctx, const Guid& entity, const TypeInfo* type)
                : m_ctx(&ctx), m_entity(entity), m_type(type)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                scene::ComponentManagerBase* mgr = m_ctx->FindManager(m_type);
                if (!e.IsAssigned() || mgr == nullptr || !mgr->HasComponent(e))
                {
                    return false;
                }

                // Snapshot for undo: the serialization blob when the manager persists (full
                // fidelity), else every reflected property (covers tool-only components).
                m_blob.Clear();
                m_properties.Clear();
                if (mgr->IsSerializable())
                {
                    MemoryStream buffer;
                    BinarySerializer ar(buffer, SerializeMode::Write);
                    mgr->WriteComponent(ar, e);
                    const Span<const byte> bytes = buffer.Bytes();
                    m_blob.Reserve(bytes.Size());
                    for (byte b : bytes)
                    {
                        m_blob.PushBack(b);
                    }
                }
                else
                {
                    const Instance component = mgr->GetComponentInstance(e);
                    for (const PropertyInfo& prop : Properties(*m_type))
                    {
                        m_properties.PushBack(
                            PropertySnapshot{prop.name, GetProperty(prop, component)});
                    }
                }

                mgr->RemoveComponent(e);
                return true;
            }

            void Undo() override
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                scene::ComponentManagerBase* mgr = m_ctx->FindManager(m_type);
                if (!e.IsAssigned() || mgr == nullptr)
                {
                    return;
                }
                if (mgr->IsSerializable() && !m_blob.IsEmpty())
                {
                    MemoryStream buffer;
                    (void)buffer.Write(m_blob.Data(), m_blob.Size());
                    (void)buffer.Seek(0, SeekOrigin::Begin);
                    BinarySerializer ar(buffer, SerializeMode::Read);
                    mgr->ReadComponent(ar, e); // adds and fills
                    m_ctx->ResolveRestoredResources(); // rebind the restored refs' proxies so the
                                                       // component renders this frame (mesh and
                                                       // material caches), as paste and spawn do
                    return;
                }
                if (!mgr->AddDefaultComponent(e))
                {
                    return;
                }
                const Instance component = mgr->GetComponentInstance(e);
                for (const PropertySnapshot& snap : m_properties)
                {
                    if (const PropertyInfo* prop = FindProperty(*m_type, snap.name))
                    {
                        (void)SetProperty(*prop, component, snap.value);
                    }
                }
                m_ctx->ResolveRestoredResources();
            }

            [[nodiscard]] StringView TypeId() const override { return u8"remove_component"; }

        private:
            struct PropertySnapshot
            {
                const char* name;
                Variant value;
            };
            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_type;
            Array<byte> m_blob;
            Array<PropertySnapshot> m_properties;
        };

        scene::Scene* m_scene;
        foundation::resource::ResourceManager* m_resources = nullptr;
        scene::PrefabPayloadResolver
            m_prefabResolver; // optional; borrowed (SceneSubsystem owns it through the page)
        EditorCommandStack* m_commands; // borrowed (the page owns its stack)
        Selection<Guid> m_selection;
    };
}
