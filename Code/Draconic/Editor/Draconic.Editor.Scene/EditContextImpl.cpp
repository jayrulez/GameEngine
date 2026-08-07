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

module draconic.editor.scene;

import draconic.foundation;
import draconic.resource;
import draconic.scene;
import draconic.scene.resource; // ResolveSceneResources (pasted/restored refs bind immediately)
import draconic.editor.core;

using namespace draconic::foundation;

namespace draconic::editor
{
    void SceneEditContext::SetResources(draconic::resource::ResourceManager* resources) noexcept
    {
        m_resources = resources;
    }

    void SceneEditContext::ResolveRestoredResources()
    {
        if (m_resources != nullptr)
        {
            scene::ResolveSceneResources(*m_scene, *m_resources);
        }
    }

    void SceneEditContext::SetPrefabResolver(scene::PrefabPayloadResolver resolver)
    {
        m_prefabResolver = static_cast<scene::PrefabPayloadResolver&&>(resolver);
    }

    const scene::PrefabPayloadResolver* SceneEditContext::PrefabResolver() const noexcept
    {
        return m_prefabResolver ? &m_prefabResolver : nullptr;
    }

    Array<byte> SceneEditContext::CopyEntity(const Guid& entity)
    {
        Array<byte> blob;
        const scene::EntityHandle root = Resolve(entity);
        if (!root.IsAssigned())
        {
            return blob;
        }
        Array<SubtreeRecord> records;
        CaptureSubtreeRecords(root, records);
        MemoryStream buffer;
        BinarySerializer ar(buffer, SerializeMode::Write);
        WriteSubtreeRecords(ar, records);
        if (!ar.IsOk())
        {
            return blob;
        }
        const Span<const byte> bytes = buffer.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    Guid SceneEditContext::PasteEntities(Span<const byte> blob, const Guid& parent)
    {
        Array<SubtreeRecord> records = ParseSubtreeBlob(blob);
        if (records.IsEmpty())
        {
            return Guid{};
        }
        return RunPasteCommand(Move(records), parent);
    }

    Guid SceneEditContext::DuplicateEntity(const Guid& entity)
    {
        const scene::EntityHandle root = Resolve(entity);
        if (!root.IsAssigned())
        {
            return Guid{};
        }
        Array<SubtreeRecord> records;
        CaptureSubtreeRecords(root, records);
        if (records.IsEmpty())
        {
            return Guid{};
        }
        const Guid parent = m_scene->GetEntityId(m_scene->GetParent(root));
        // "Box" -> "Box (2)" (first free counter among the copy's future siblings), so
        // original and copy are distinguishable at a glance.
        records[0].name = UniqueSiblingName(records[0].name.AsView(), m_scene->GetParent(root));
        return RunPasteCommand(Move(records), parent);
    }

    String SceneEditContext::UniqueSiblingName(StringView base, scene::EntityHandle parent)
    {
        // Strip an existing " (n)" suffix so "Box (2)" duplicates to "Box (3)", not
        // "Box (2) (2)".
        StringView stem = base;
        if (!stem.IsEmpty() && stem[stem.Size() - 1] == utf8char(')'))
        {
            usize open = stem.Size();
            for (usize i = stem.Size(); i > 0; --i)
            {
                if (stem[i - 1] == utf8char('('))
                {
                    open = i - 1;
                    break;
                }
            }
            if (open >= 2 && stem[open - 1] == utf8char(' '))
            {
                stem = stem.SubStr(0, open - 1);
            }
        }
        for (u32 counter = 2;; ++counter)
        {
            String candidate(stem);
            candidate += u8" (";
            utf8char digits[12];
            u32 value = counter, n = 0;
            do
            {
                digits[n++] = static_cast<utf8char>('0' + (value % 10));
                value /= 10;
            } while (value != 0);
            while (n > 0)
            {
                candidate.PushBack(digits[--n]);
            }
            candidate += u8")";
            bool taken = false;
            m_scene->ForEachEntity(
                [&](scene::EntityHandle e)
                {
                    if (m_scene->GetParent(e) == parent &&
                        m_scene->GetEntityName(e) == candidate.AsView())
                    {
                        taken = true;
                    }
                });
            if (!taken)
            {
                return candidate;
            }
        }
    }

    Array<byte> SceneEditContext::CopyComponent(const Guid& entity, const TypeInfo* componentType)
    {
        Array<byte> blob;
        const scene::EntityHandle e = Resolve(entity);
        scene::ComponentManagerBase* mgr = FindManager(componentType);
        if (!e.IsAssigned() || mgr == nullptr || !mgr->IsSerializable() || !mgr->HasComponent(e))
        {
            return blob;
        }
        MemoryStream buffer;
        BinarySerializer ar(buffer, SerializeMode::Write);
        String typeId = String(mgr->SerializationTypeId());
        draconic::foundation::Serialize(ar, "type", typeId);
        mgr->WriteComponent(ar, e);
        if (!ar.IsOk())
        {
            return blob;
        }
        const Span<const byte> bytes = buffer.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    String SceneEditContext::PeekComponentTypeId(Span<const byte> blob)
    {
        MemoryStream buffer;
        (void)buffer.Write(blob.Data(), blob.Size());
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(buffer, SerializeMode::Read);
        String typeId;
        draconic::foundation::Serialize(ar, "type", typeId);
        return ar.IsOk() ? typeId : String{};
    }

    bool SceneEditContext::PasteComponent(const Guid& entity, Span<const byte> blob)
    {
        PasteComponentCommand* raw =
            DefaultAllocator().New<PasteComponentCommand>(*this, entity, blob);
        return m_commands->Execute(UniquePtr<IEditorCommand>(raw, DefaultAllocator()));
    }

    scene::EntityHandle SceneEditContext::Resolve(const Guid& id)
    {
        return m_scene->FindEntity(id);
    }

    Guid SceneEditContext::CreateEntity(StringView name, const Guid& parent)
    {
        CreateEntityCommand* raw = DefaultAllocator().New<CreateEntityCommand>(*this, name, parent);
        if (!m_commands->Execute(UniquePtr<IEditorCommand>(raw, DefaultAllocator())))
        {
            return Guid{}; // raw was destroyed by the failed Execute
        }
        const Guid id = raw->CreatedId();
        m_selection.Set(id);
        return id;
    }

    void SceneEditContext::DestroyEntity(const Guid& entity)
    {
        if (!Resolve(entity).IsAssigned())
        {
            return;
        }
        // Deselect the whole doomed subtree up front (selection is not undoable).
        m_selection.Remove(entity);
        CollectSubtree(Resolve(entity), [this](scene::EntityHandle e)
                       { m_selection.Remove(m_scene->GetEntityId(e)); });
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<DestroyEntityCommand>(*this, entity), DefaultAllocator()));
    }

    void SceneEditContext::RenameEntity(const Guid& entity, StringView newName)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<RenameEntityCommand>(*this, entity, newName),
            DefaultAllocator()));
    }

    void SceneEditContext::ReparentEntity(const Guid& entity, const Guid& newParent)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<ReparentEntityCommand>(*this, entity, newParent),
            DefaultAllocator()));
    }

    void SceneEditContext::MoveEntityBefore(const Guid& entity, const Guid& sibling)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<MoveEntityCommand>(*this, entity, sibling), DefaultAllocator()));
    }

    void SceneEditContext::SetEntityActive(const Guid& entity, bool active)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<SetActiveCommand>(*this, entity, active), DefaultAllocator()));
    }

    void SceneEditContext::SetLocalTransform(const Guid& entity, const Transform& transform)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<SetTransformCommand>(*this, entity, transform),
            DefaultAllocator()));
    }

    void SceneEditContext::SetComponentProperty(const Guid& entity, const TypeInfo* componentType,
                                                const char* property, const Variant& value)
    {
        (void)m_commands->Execute(
            UniquePtr<IEditorCommand>(DefaultAllocator().New<SetComponentPropertyCommand>(
                                          *this, entity, componentType, property, value),
                                      DefaultAllocator()));
    }

    void SceneEditContext::SetComponentPropertyRaw(const Guid& entity,
                                                   const TypeInfo* componentType,
                                                   const char* property, i64 value)
    {
        (void)m_commands->Execute(
            UniquePtr<IEditorCommand>(DefaultAllocator().New<SetComponentPropertyCommand>(
                                          *this, entity, componentType, property, value),
                                      DefaultAllocator()));
    }

    void SceneEditContext::SetSceneSettingProperty(const TypeInfo* settingsType,
                                                   const char* property, const Variant& value)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<SetSceneSettingCommand>(*this, settingsType, property, value),
            DefaultAllocator()));
    }

    bool SceneEditContext::ApplySceneSettingsBlock(const TypeInfo* settingsType,
                                                   Array<byte> newBlob)
    {
        SetSceneSettingsBlockCommand* raw = DefaultAllocator().New<SetSceneSettingsBlockCommand>(
            *this, settingsType, static_cast<Array<byte>&&>(newBlob));
        return m_commands->Execute(UniquePtr<IEditorCommand>(raw, DefaultAllocator()));
    }

    void SceneEditContext::SetSceneSettingPropertyRaw(const TypeInfo* settingsType,
                                                      const char* property, i64 value)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<SetSceneSettingCommand>(*this, settingsType, property, value),
            DefaultAllocator()));
    }

    scene::SceneSystem* SceneEditContext::FindSystemBySettingsType(const TypeInfo* settingsType)
    {
        scene::SceneSystem* found = nullptr;
        m_scene->ForEachSystem(
            [&](scene::SceneSystem& s)
            {
                if (found == nullptr && s.SettingsType() == settingsType)
                {
                    found = &s;
                }
            });
        return found;
    }

    void SceneEditContext::AddComponent(const Guid& entity, const TypeInfo* componentType)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<AddComponentCommand>(*this, entity, componentType),
            DefaultAllocator()));
    }

    void SceneEditContext::RemoveComponent(const Guid& entity, const TypeInfo* componentType)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<RemoveComponentCommand>(*this, entity, componentType),
            DefaultAllocator()));
    }

    scene::ComponentManagerBase* SceneEditContext::FindManager(const TypeInfo* type)
    {
        scene::ComponentManagerBase* found = nullptr;
        m_scene->ForEachManager(
            [&](scene::ComponentManagerBase& mgr)
            {
                if (mgr.ComponentType() == type)
                {
                    found = &mgr;
                }
            });
        return found;
    }

    bool SceneEditContext::IsSelfOrAncestor(const Guid& entity, const Guid& possibleAncestor)
    {
        scene::EntityHandle e = Resolve(entity);
        const scene::EntityHandle anc = Resolve(possibleAncestor);
        if (!anc.IsAssigned())
        {
            return false;
        }
        for (; e.IsAssigned(); e = m_scene->GetParent(e))
        {
            if (e == anc)
            {
                return true;
            }
        }
        return false;
    }

    void SceneEditContext::CaptureSubtreeRecords(scene::EntityHandle root,
                                                 Array<SubtreeRecord>& out)
    {
        scene::Scene& scene = *m_scene;
        const Guid rootParent = scene.GetEntityId(scene.GetParent(root));
        CollectSubtree(root,
                       [&scene, &out, &rootParent](scene::EntityHandle e)
                       {
                           SubtreeRecord record;
                           record.id = scene.GetEntityId(e);
                           const Guid parent = scene.GetEntityId(scene.GetParent(e));
                           record.parent =
                               (parent == rootParent && out.IsEmpty()) ? Guid{} : parent;
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
                                   SubtreeComponentRecord component;
                                   component.typeId = String(mgr.SerializationTypeId());
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
                           out.PushBack(Move(record));
                       });
    }

    void SceneEditContext::WriteSubtreeRecords(BinarySerializer& ar, Array<SubtreeRecord>& records)
    {
        u32 count = static_cast<u32>(records.Size());
        draconic::foundation::Serialize(ar, "count", count);
        for (SubtreeRecord& r : records)
        {
            ar.Key("id");
            ar.GuidValue(r.id);
            ar.Key("parent");
            ar.GuidValue(r.parent);
            draconic::foundation::Serialize(ar, "name", r.name);
            draconic::foundation::Serialize(ar, "position", r.local.position);
            draconic::foundation::Serialize(ar, "rotation", r.local.rotation);
            draconic::foundation::Serialize(ar, "scale", r.local.scale);
            draconic::foundation::Serialize(ar, "active", r.active);
            u32 componentCount = static_cast<u32>(r.components.Size());
            draconic::foundation::Serialize(ar, "components", componentCount);
            for (SubtreeComponentRecord& c : r.components)
            {
                draconic::foundation::Serialize(ar, "type", c.typeId);
                draconic::foundation::Serialize(ar, "blob", c.blob);
            }
        }
    }

    Array<SceneEditContext::SubtreeRecord> SceneEditContext::ParseSubtreeBlob(Span<const byte> blob)
    {
        Array<SubtreeRecord> records;
        MemoryStream buffer;
        (void)buffer.Write(blob.Data(), blob.Size());
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(buffer, SerializeMode::Read);
        u32 count = 0;
        draconic::foundation::Serialize(ar, "count", count);
        for (u32 i = 0; i < count && ar.IsOk(); ++i)
        {
            SubtreeRecord r;
            ar.Key("id");
            ar.GuidValue(r.id);
            ar.Key("parent");
            ar.GuidValue(r.parent);
            draconic::foundation::Serialize(ar, "name", r.name);
            draconic::foundation::Serialize(ar, "position", r.local.position);
            draconic::foundation::Serialize(ar, "rotation", r.local.rotation);
            draconic::foundation::Serialize(ar, "scale", r.local.scale);
            draconic::foundation::Serialize(ar, "active", r.active);
            u32 componentCount = 0;
            draconic::foundation::Serialize(ar, "components", componentCount);
            for (u32 c = 0; c < componentCount && ar.IsOk(); ++c)
            {
                SubtreeComponentRecord component;
                draconic::foundation::Serialize(ar, "type", component.typeId);
                draconic::foundation::Serialize(ar, "blob", component.blob);
                r.components.PushBack(Move(component));
            }
            records.PushBack(Move(r));
        }
        if (!ar.IsOk())
        {
            records.Clear();
        }
        return records;
    }

    Guid SceneEditContext::SpawnPrefabInstance(const Guid& prefabId, Array<byte> payload,
                                               const Guid& parent, const Transform* rootTransform,
                                               const Guid& placeBefore)
    {
        SpawnPrefabCommand* raw = DefaultAllocator().New<SpawnPrefabCommand>(
            *this, prefabId, Move(payload), parent, rootTransform, placeBefore);
        if (!m_commands->Execute(UniquePtr<IEditorCommand>(raw, DefaultAllocator())))
        {
            return Guid{};
        }
        const Guid created = raw->RootGuid();
        m_selection.Set(created);
        return created;
    }

    Guid SceneEditContext::ReplaceWithPrefabInstance(const Guid& entity, const Guid& prefabId,
                                                     Array<byte> payload)
    {
        const scene::EntityHandle live = Resolve(entity);
        if (!live.IsAssigned())
        {
            return Guid{};
        }
        const scene::EntityHandle parentHandle = m_scene->GetParent(live);
        const Guid parent = parentHandle.IsAssigned() ? m_scene->GetEntityId(parentHandle) : Guid{};
        const Transform placement = m_scene->GetLocalTransform(live);

        m_commands->BeginGroup(u8"prefab_replace");
        // placeBefore = the original itself: the instance takes its exact sibling slot
        // (spawn otherwise appends, dropping the replaced entity to the hierarchy's end).
        const Guid root = SpawnPrefabInstance(prefabId, Move(payload), parent, &placement, entity);
        if (!root.IsNil())
        {
            DestroyEntity(entity);
        }
        m_commands->EndGroup();
        if (!root.IsNil())
        {
            m_selection.Set(root);
        }
        return root;
    }

    bool SceneEditContext::RevertComponentToBaseline(const Guid& entity,
                                                     const TypeInfo* componentType)
    {
        scene::PrefabMemberInfo member;
        if (!scene::FindPrefabMember(*m_scene, entity, member))
        {
            return false;
        }
        scene::ComponentManagerBase* manager = FindManager(componentType);
        if (manager == nullptr)
        {
            return false;
        }
        const Guid sourceId = member.state->sourceIds[member.memberIndex];
        const scene::Scene::PrefabComponentBaseline* baseline =
            scene::FindPrefabBaseline(*member.state, sourceId, manager->SerializationTypeId());
        if (baseline == nullptr)
        {
            // Added by the user: revert = remove (undoable through the existing command).
            const scene::EntityHandle live = Resolve(entity);
            if (live.IsAssigned() && manager->HasComponent(live))
            {
                RemoveComponent(entity, componentType);
            }
            return true;
        }
        // The clipboard blob format (typeId + payload) rides the undoable paste path.
        MemoryStream buffer;
        BinarySerializer ar(buffer, SerializeMode::Write);
        String typeId = String(manager->SerializationTypeId());
        draconic::foundation::Serialize(ar, "type", typeId);
        if (!ar.IsOk())
        {
            return false;
        }
        const usize headerSize = buffer.Bytes().Size();
        (void)headerSize;
        (void)buffer.Write(reinterpret_cast<const byte*>(baseline->blob.Data()),
                           baseline->blob.Size());
        return PasteComponent(entity, buffer.Bytes());
    }

    Guid SceneEditContext::RunPasteCommand(Array<SubtreeRecord> records, const Guid& parent)
    {
        PasteEntitiesCommand* raw =
            DefaultAllocator().New<PasteEntitiesCommand>(*this, Move(records), parent);
        if (!m_commands->Execute(UniquePtr<IEditorCommand>(raw, DefaultAllocator())))
        {
            return Guid{};
        }
        const Guid created = raw->RootGuid();
        m_selection.Set(created);
        return created;
    }
}
