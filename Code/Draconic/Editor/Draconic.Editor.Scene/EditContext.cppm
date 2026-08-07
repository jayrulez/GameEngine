// Draconic::EditorScene - :edit partition.
//
// SceneEditContext: the per-page scene mutation mediator (design doc §3.4/§3.6 - the Lumix
// WorldEditor role, but PER PAGE, never global: multi-scene). Every mutation is an
// IEditorCommand on the page's stack; nothing edits the scene directly. Commands reference
// entities by persistent Guid (stable across destroy/undo - handles are generation-guarded
// pool slots and die with the entity), resolving through the scene at execute time.
//
// DestroyEntity's undo restores the FULL serialized subtree - names, transforms, active flags,
// hierarchy, and every serializable component (via the managers' WriteComponent/ReadComponent,
// the same per-entity routing SerializeScene uses). This is the fix for Sedulous's lossy
// "TODO: Restore components and children".
//
// Reparents preserve the WORLD transform (Scene keep-world overloads + TRS decompose); undo
// restores the captured local exactly (no decompose round-trip drift). Same-parent reorders
// keep the local untouched.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.scene:edit;

import draconic.foundation;
import draconic.resource;
import draconic.scene;
import draconic.scene.resource; // ResolveSceneResources (pasted/restored refs bind immediately)
import draconic.editor.core;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace scene = draconic::scene;

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
        /// materialize components from blobs (paste, duplicate, destroy-undo) resolve the
        /// scene's resource refs immediately - without it, a pasted mesh ref stays unbound and
        /// nothing renders until the next scene load runs the resolve pass.
        void SetResources(draconic::resource::ResourceManager* resources) noexcept;
        void ResolveRestoredResources();

        /// Payload resolver for NESTED prefab records (wired by the page from the source DB).
        /// Without it, spawning a prefab that contains other prefabs skips the nested parts.
        void SetPrefabResolver(scene::PrefabPayloadResolver resolver);
        [[nodiscard]] const scene::PrefabPayloadResolver* PrefabResolver() const noexcept;

        /// Per-page entity selection, by persistent Guid (survives destroy/undo round-trips).
        [[nodiscard]] Selection<Guid>& EntitySelection() noexcept { return m_selection; }

        // === Entity clipboard / duplication ===
        //
        // A captured subtree is a self-contained binary blob: pre-order entity records
        // (original guid for intra-subtree parent relinking + name/transform/active) each with
        // its serializable components (typeId + payload). Pasting instantiates the records with
        // FRESH guids (per-scene identity; commands stay guid-routed), so the same blob pastes
        // repeatedly and across scenes/pages. Blob kind for the editor clipboard: "entities".

        /// Capture `entity`'s subtree into a clipboard blob (empty on failure).
        [[nodiscard]] Array<byte> CopyEntity(const Guid& entity);

        /// Paste a captured subtree as a child of `parent` (root when nil/unresolvable).
        /// Returns the new root entity's Guid (nil on failure); it becomes the selection.
        Guid PasteEntities(Span<const byte> blob, const Guid& parent = Guid{});

        /// Duplicate an entity subtree as a sibling (same parent). Returns the copy's root
        /// Guid (nil on failure); it becomes the selection. One undo step.
        Guid DuplicateEntity(const Guid& entity);

        [[nodiscard]] String UniqueSiblingName(StringView base, scene::EntityHandle parent);

        // === Component clipboard ===
        // Blob: typeId string + the manager's WriteComponent payload. Kind: "component".

        /// Capture one serializable component into a clipboard blob (empty on failure).
        [[nodiscard]] Array<byte> CopyComponent(const Guid& entity, const TypeInfo* componentType);

        /// The component type id a clipboard blob carries ("" when unreadable) - for menu labels.
        [[nodiscard]] static String PeekComponentTypeId(Span<const byte> blob);

        /// Paste a copied component onto `entity` (adds it, or overwrites the existing one).
        /// Undo restores the prior state exactly. False when the blob/manager doesn't apply.
        bool PasteComponent(const Guid& entity, Span<const byte> blob);

        /// Resolve a selected/stored Guid to a live handle (Invalid if the entity is gone).
        [[nodiscard]] scene::EntityHandle Resolve(const Guid& id);

        // === Mutations (each an undoable command; failed executes are dropped by the stack) ===

        /// Create an empty entity (child of `parent` when valid). Returns its Guid (empty on
        /// failure). Redo recreates the SAME Guid. The new entity becomes the selection.
        Guid CreateEntity(StringView name, const Guid& parent = Guid{});

        /// Destroy an entity and its subtree (undo restores everything, components included).
        void DestroyEntity(const Guid& entity);

        /// Rename an entity (consecutive renames of the same entity merge into one undo entry).
        void RenameEntity(const Guid& entity, StringView newName);

        /// Reparent an entity (`newParent` empty = make root; appended at the END of the new
        /// parent's children). Cycles are refused (the command's Execute fails and drops).
        void ReparentEntity(const Guid& entity, const Guid& newParent);

        /// Sibling reorder: move an entity immediately BEFORE `sibling` (under sibling's
        /// parent), or - when `sibling` is empty - to the END of the root list. Undo restores
        /// the previous parent AND position. Cycles/no-ops are refused.
        void MoveEntityBefore(const Guid& entity, const Guid& sibling);

        /// Set an entity's active flag (undoable).
        void SetEntityActive(const Guid& entity, bool active);

        /// Set an entity's local transform. Consecutive edits of the same entity MERGE into one
        /// undo entry (inspector field scrubs, gizmo drags).
        void SetLocalTransform(const Guid& entity, const Transform& transform);

        /// Set a reflected component property by Variant. Consecutive edits of the same
        /// entity+component+property MERGE.
        void SetComponentProperty(const Guid& entity, const TypeInfo* componentType,
                                  const char* property, const Variant& value);

        /// Point a component's resource::Ref<T> property at a new asset (the inspector's
        /// picker): writes the Guid through PropertyInfo::address and rebinds through the
        /// ResourceManager (nullable - the ref then resolves on the next scene open). Undo
        /// restores the previous target. NOT merged (each pick is its own undo entry).
        template <typename T>
        void SetComponentResourceRef(const Guid& entity, const TypeInfo* componentType,
                                     const char* property, const Guid& value,
                                     draconic::resource::ResourceManager* resources)
        {
            (void)m_commands->Execute(UniquePtr<IEditorCommand>(
                DefaultAllocator().New<SetResourceRefCommand<T>>(*this, entity, componentType,
                                                                 property, value, resources),
                DefaultAllocator()));
        }

        /// Set a reflected component property whose type a Variant cannot construct at runtime
        /// (enums known only by TypeInfo): writes the underlying integer through
        /// PropertyInfo::address. Merges like SetComponentProperty.
        void SetComponentPropertyRaw(const Guid& entity, const TypeInfo* componentType,
                                     const char* property, i64 value);

        /// Set a reflected property on a SCENE SYSTEM's settings block (the scene inspector,
        /// shown when no entity is selected). Same merge semantics as SetComponentProperty
        /// (scrubs collapse into one undo entry); the settings instance is re-derived from
        /// the scene each apply.
        void SetSceneSettingProperty(const TypeInfo* settingsType, const char* property,
                                     const Variant& value);

        /// Point a scene-system setting's resource::Ref<T> at a new asset (the scene
        /// inspector's picker) - the settings twin of SetComponentResourceRef. NOT merged.
        template <typename T>
        void SetSceneSettingResourceRef(const TypeInfo* settingsType, const char* property,
                                        const Guid& value,
                                        draconic::resource::ResourceManager* resources)
        {
            (void)m_commands->Execute(
                UniquePtr<IEditorCommand>(DefaultAllocator().New<SetSceneSettingRefCommand<T>>(
                                              *this, settingsType, property, value, resources),
                                          DefaultAllocator()));
        }

        /// Whole-block scene-settings edit (shapes reflection rows can't express - e.g.
        /// the collision-group matrix): `newBlob` = the settings serialized via the
        /// system's own SerializeSettings; undo restores the prior serialized state.
        /// The caller builds the blob from an EDITED COPY - the live settings are only
        /// touched through the command (so undo/redo replay cleanly).
        bool ApplySceneSettingsBlock(const TypeInfo* settingsType, Array<byte> newBlob);

        /// Enum flavor of SetSceneSettingProperty (underlying integer through
        /// PropertyInfo::address - same reason as SetComponentPropertyRaw).
        void SetSceneSettingPropertyRaw(const TypeInfo* settingsType, const char* property,
                                        i64 value);

        /// The scene system whose SettingsType() is `settingsType` (null if none).
        [[nodiscard]] scene::SceneSystem* FindSystemBySettingsType(const TypeInfo* settingsType);

        /// Add a default-constructed component (undoable; fails if already present).
        void AddComponent(const Guid& entity, const TypeInfo* componentType);

        /// Remove a component (undo restores it - full fidelity via the manager's serialization
        /// when available, else the reflected properties).
        void RemoveComponent(const Guid& entity, const TypeInfo* componentType);

        /// The scene manager whose component type is `type` (null if none).
        [[nodiscard]] scene::ComponentManagerBase* FindManager(const TypeInfo* type);

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

                // First execute records the generated Guid; redo recreates the SAME identity.
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

        // Shared subtree snapshot for the clipboard/duplicate path (DestroyEntityCommand keeps
        // its own undo records - same shape, different lifetime).
        struct SubtreeComponentRecord
        {
            String typeId;
            Array<byte> blob;
        };
        struct SubtreeRecord
        {
            Guid id;     // ORIGINAL guid (parent relinking only - pastes mint fresh ids)
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
        /// SAME member guids). `payload` is the prefab's "scene" stream, captured by value so
        /// undo/redo stay stable against later asset edits. Returns the instance root's guid
        /// (nil on failure); it becomes the selection.
        Guid SpawnPrefabInstance(const Guid& prefabId, Array<byte> payload,
                                 const Guid& parent = Guid{},
                                 const Transform* rootTransform = nullptr,
                                 const Guid& placeBefore = Guid{});

        /// Replaces `entity`'s subtree with an instance of `prefabId` (create-from-selection's
        /// second half): one undo group [spawn at the same parent/transform, destroy original].
        Guid ReplaceWithPrefabInstance(const Guid& entity, const Guid& prefabId,
                                       Array<byte> payload);

        /// Reverts a prefab member's component to its spawn-time baseline, UNDOABLY:
        /// template components restore their baseline payload (re-added if the user removed
        /// them), user-ADDED components (no baseline) are removed. No-op for non-members.
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
                    // First run: pin the minted member guids so redo recreates them exactly -
                    // including every NESTED instance's members (as sub-records with the
                    // template placement).
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
                                MakeUnique<scene::Scene::PendingPrefabInstance>(DefaultAllocator());
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

        // Instantiate captured records with FRESH guids; the old->new map relinks intra-subtree
        // parents. Redo recreates the SAME fresh guids (minted once, on the first Execute).
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
                    // Redo: the ids are free again (undo destroyed them) - recreate them exactly.
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
                // Destroying the pasted ROOT takes the whole subtree with it (records are
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
                draconic::foundation::Serialize(ar, "type", m_typeId);
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

                // Snapshot the subtree PRE-ORDER (parents before children) so Undo can recreate
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
                Guid parent; // empty = root (or a parent OUTSIDE the subtree resolved at undo)
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
                prev.m_newName = Move(m_newName); // previous keeps its ORIGINAL old name
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
                    // The exact old slot: the sibling the entity sat BEFORE (nil = it was last).
                    m_oldNextSibling = scene.GetEntityId(scene.GetNextSibling(e));
                    m_oldLocal = scene.GetLocalTransform(e);
                    m_hasOld = true;
                }
                if (m_oldParent == m_newParent)
                {
                    return false;
                } // no-op - don't pollute undo
                // Editor semantics: the entity STAYS PUT in the world across a reparent (its
                // local transform is recomputed via TRS decompose).
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
                // SetParent appends to the END of the sibling list; restore the exact slot
                // (otherwise the undone entity visibly jumps to the bottom of its list).
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

        // Sibling reorder: place `entity` before `sibling` (empty sibling = end of the ROOT
        // list). Undo restores the exact previous position via the captured old next-sibling
        // (empty = was last under its old parent).
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
                // Cycle: the target slot's PARENT lies inside the moved entity's own subtree.
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

                // Keep the world transform only when the PARENT changes; a same-parent reorder
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
                return scene.Revision() != before; // unchanged position = no-op, drop
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

        // The settings twin of SetResourceRefCommand: the Ref lives on a scene SYSTEM's
        // settings block (no entity); the address re-derives from the scene each apply.
        template <typename T>
        class SetSceneSettingRefCommand final : public IEditorCommand
        {
        public:
            SetSceneSettingRefCommand(SceneEditContext& ctx, const TypeInfo* settingsType,
                                      const char* property, const Guid& value,
                                      draconic::resource::ResourceManager* resources)
                : m_ctx(&ctx), m_settingsType(settingsType), m_property(property), m_new(value),
                  m_resources(resources)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                draconic::resource::Ref<T>* ref = ResolveRef();
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
                if (draconic::resource::Ref<T>* ref = ResolveRef())
                {
                    ref->SetId(m_old);
                    ref->Rebind(m_resources);
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"set_scene_setting_ref"; }

        private:
            [[nodiscard]] draconic::resource::Ref<T>* ResolveRef()
            {
                scene::SceneSystem* system = m_ctx->FindSystemBySettingsType(m_settingsType);
                if (system == nullptr)
                {
                    return nullptr;
                }
                const Instance settings{system->SettingsInstance(), m_settingsType};
                const PropertyInfo* prop = FindProperty(*m_settingsType, m_property);
                void* address = (prop != nullptr && prop->address != nullptr)
                                    ? prop->address(settings)
                                    : nullptr;
                return static_cast<draconic::resource::Ref<T>*>(address);
            }

            SceneEditContext* m_ctx;
            const TypeInfo* m_settingsType;
            const char* m_property;
            Guid m_new;
            Guid m_old;
            bool m_hasOld = false;
            draconic::resource::ResourceManager* m_resources;
        };

        template <typename T>
        class SetResourceRefCommand final : public IEditorCommand
        {
        public:
            SetResourceRefCommand(SceneEditContext& ctx, const Guid& entity, const TypeInfo* type,
                                  const char* property, const Guid& value,
                                  draconic::resource::ResourceManager* resources)
                : m_ctx(&ctx), m_entity(entity), m_type(type), m_property(property), m_new(value),
                  m_resources(resources)
            {
            }

            [[nodiscard]] bool Execute() override
            {
                draconic::resource::Ref<T>* ref = ResolveRef();
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
                if (draconic::resource::Ref<T>* ref = ResolveRef())
                {
                    ref->SetId(m_old);
                    ref->Rebind(m_resources);
                }
            }
            [[nodiscard]] StringView TypeId() const override { return u8"set_resource_ref"; }

        private:
            // Component pools move on add/remove, so the address re-derives every apply.
            [[nodiscard]] draconic::resource::Ref<T>* ResolveRef()
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                scene::ComponentManagerBase* mgr = m_ctx->FindManager(m_type);
                if (!e.IsAssigned() || mgr == nullptr)
                {
                    return nullptr;
                }
                const Instance component = mgr->GetComponentInstance(e);
                const PropertyInfo* prop =
                    component.IsEmpty() ? nullptr : FindProperty(*m_type, m_property);
                void* address = (prop != nullptr && prop->address != nullptr)
                                    ? prop->address(component)
                                    : nullptr;
                return static_cast<draconic::resource::Ref<T>*>(address);
            }

            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_type;
            const char* m_property;
            Guid m_new;
            Guid m_old;
            bool m_hasOld = false;
            draconic::resource::ResourceManager* m_resources;
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
                prev.m_new = m_new; // previous keeps its ORIGINAL old transform
                return true;
            }

        private:
            SceneEditContext* m_ctx;
            Guid m_entity;
            Transform m_new;
            Transform m_old;
            bool m_hasOld = false;
        };

        // One command for both property paths: Variant (typed get/set) and raw integer (enums -
        // a Variant of a type known only by TypeInfo cannot be constructed, so those fields are
        // written in place through PropertyInfo::address).
        // Mirrors SetComponentPropertyCommand for a SCENE SYSTEM's settings block (no entity;
        // the instance is re-derived from the scene each apply - systems are stable, but the
        // re-derive keeps the command valid across snapshot restores).
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
                    system->SerializeSettings(writer);
                    const Span<const byte> bytes = buffer.Bytes();
                    m_old.Reserve(bytes.Size());
                    for (byte b : bytes)
                    {
                        m_old.PushBack(b);
                    }
                }
                return Apply(*system, m_new);
            }
            void Undo() override
            {
                scene::SceneSystem* system = m_ctx->FindSystemBySettingsType(m_settingsType);
                if (system != nullptr)
                {
                    (void)Apply(*system, m_old);
                }
            }
            [[nodiscard]] StringView TypeId() const override
            {
                return u8"set_scene_settings_block";
            }

        private:
            static bool Apply(scene::SceneSystem& system, const Array<byte>& blob)
            {
                MemoryStream buffer;
                if (buffer.Write(blob.Data(), blob.Size()) != blob.Size())
                {
                    return false;
                }
                (void)buffer.Seek(0, SeekOrigin::Begin);
                BinarySerializer reader(buffer, SerializeMode::Read);
                system.SerializeSettings(reader);
                return true;
            }

            SceneEditContext* m_ctx;
            const TypeInfo* m_settingsType;
            Array<byte> m_new;
            Array<byte> m_old;
        };

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
                        m_oldRaw = ReadRawInt(address, prop->type->size);
                        m_hasOld = true;
                    }
                    WriteRawInt(address, prop->type->size, m_newRaw);
                    return true;
                }

                if (!m_hasOld)
                {
                    m_old = GetProperty(*prop, settings);
                    m_hasOld = true;
                }
                return SetProperty(*prop, settings, m_new).IsOk();
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
                        WriteRawInt(address, prop->type->size, m_oldRaw);
                    }
                }
                else
                {
                    (void)SetProperty(*prop, settings, m_old);
                }
            }

            [[nodiscard]] StringView TypeId() const override { return u8"set_scene_setting"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<SetSceneSettingCommand&>(previous);
                if (prev.m_settingsType != m_settingsType || prev.m_raw != m_raw ||
                    !CStrEq(prev.m_property, m_property))
                {
                    return false;
                }
                prev.m_new = m_new; // previous keeps its ORIGINAL old value
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
            [[nodiscard]] static i64 ReadRawInt(const void* address, u32 size) noexcept
            {
                switch (size)
                {
                case 1:
                    return *static_cast<const i8*>(address);
                case 2:
                    return *static_cast<const i16*>(address);
                case 8:
                    return *static_cast<const i64*>(address);
                default:
                    return *static_cast<const i32*>(address);
                }
            }
            static void WriteRawInt(void* address, u32 size, i64 value) noexcept
            {
                switch (size)
                {
                case 1:
                    *static_cast<i8*>(address) = static_cast<i8>(value);
                    break;
                case 2:
                    *static_cast<i16*>(address) = static_cast<i16>(value);
                    break;
                case 8:
                    *static_cast<i64*>(address) = value;
                    break;
                default:
                    *static_cast<i32*>(address) = static_cast<i32>(value);
                    break;
                }
            }

            [[nodiscard]] Instance ResolveSettings(const PropertyInfo** outProp)
            {
                scene::SceneSystem* system = m_ctx->FindSystemBySettingsType(m_settingsType);
                if (system == nullptr)
                {
                    return {};
                }
                *outProp = FindProperty(*m_settingsType, m_property);
                return Instance{system->SettingsInstance(), m_settingsType};
            }

            SceneEditContext* m_ctx;
            const TypeInfo* m_settingsType;
            const char* m_property;
            Variant m_new;
            Variant m_old;
            i64 m_newRaw = 0;
            i64 m_oldRaw = 0;
            bool m_raw = false;
            bool m_hasOld = false;
        };

        class SetComponentPropertyCommand final : public IEditorCommand
        {
        public:
            SetComponentPropertyCommand(SceneEditContext& ctx, const Guid& entity,
                                        const TypeInfo* componentType, const char* property,
                                        const Variant& value)
                : m_ctx(&ctx), m_entity(entity), m_componentType(componentType),
                  m_property(property), m_new(value)
            {
            }

            SetComponentPropertyCommand(SceneEditContext& ctx, const Guid& entity,
                                        const TypeInfo* componentType, const char* property,
                                        i64 rawValue)
                : m_ctx(&ctx), m_entity(entity), m_componentType(componentType),
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
                        m_oldRaw = ReadRaw(address, prop->type->size);
                        m_hasOld = true;
                    }
                    WriteRaw(address, prop->type->size, m_newRaw);
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
                        WriteRaw(address, prop->type->size, m_oldRaw);
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
                    prev.m_raw != m_raw || !detail_CStrEq(prev.m_property, m_property))
                {
                    return false;
                }
                prev.m_new = m_new; // previous keeps its ORIGINAL old value
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
            [[nodiscard]] static i64 ReadRaw(const void* address, u32 size) noexcept
            {
                switch (size)
                {
                case 1:
                    return *static_cast<const i8*>(address);
                case 2:
                    return *static_cast<const i16*>(address);
                case 8:
                    return *static_cast<const i64*>(address);
                default:
                    return *static_cast<const i32*>(address);
                }
            }
            static void WriteRaw(void* address, u32 size, i64 value) noexcept
            {
                switch (size)
                {
                case 1:
                    *static_cast<i8*>(address) = static_cast<i8>(value);
                    break;
                case 2:
                    *static_cast<i16*>(address) = static_cast<i16>(value);
                    break;
                case 8:
                    *static_cast<i64*>(address) = value;
                    break;
                default:
                    *static_cast<i32*>(address) = static_cast<i32>(value);
                    break;
                }
            }

            [[nodiscard]] Instance ResolveComponent(const PropertyInfo** outProperty)
            {
                const scene::EntityHandle e = m_ctx->Resolve(m_entity);
                if (!e.IsAssigned())
                {
                    return {};
                }
                scene::ComponentManagerBase* mgr = m_ctx->FindManager(m_componentType);
                if (mgr == nullptr)
                {
                    return {};
                }
                const Instance component = mgr->GetComponentInstance(e);
                if (component.IsEmpty())
                {
                    return {};
                }
                *outProperty = FindProperty(*m_componentType, m_property);
                return component;
            }

            SceneEditContext* m_ctx;
            Guid m_entity;
            const TypeInfo* m_componentType;
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
                    mgr->ReadComponent(ar, e); // adds + fills
                    m_ctx->ResolveRestoredResources(); // re-bind the restored refs' proxies so the
                                                       // component renders this frame (mesh/material
                                                       // caches) - matches the paste / spawn paths
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
        draconic::resource::ResourceManager* m_resources = nullptr;
        scene::PrefabPayloadResolver
            m_prefabResolver; // borrowed (optional)              // borrowed (SceneSubsystem owns it via the page)
        EditorCommandStack* m_commands; // borrowed (the page owns its stack)
        Selection<Guid> m_selection;
    };
}
