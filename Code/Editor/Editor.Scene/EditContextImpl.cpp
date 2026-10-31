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

module editor.scene;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource; // ResolveSceneResources (pasted/restored refs bind immediately)
import editor.core;

using namespace foundation::core;
namespace scene = foundation::scene;

namespace editor
{
    void SceneEditContext::SetResources(foundation::resource::ResourceManager* resources) noexcept
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

    void SceneEditContext::ResolveComponentResources(const Guid& entity, const TypeInfo* componentType)
    {
        scene::ComponentManagerBase* manager = FindManager(componentType);
        const scene::EntityHandle e = Resolve(entity);
        if (m_resources != nullptr && manager != nullptr && e.IsAssigned())
            manager->ResolveEntityResources(e, *m_resources);
    }

    void SceneEditContext::ResolveSettingsResources(const TypeInfo* settingsType)
    {
        if (scene::SceneSystem* system = FindSystemBySettingsType(settingsType); system != nullptr && m_resources != nullptr)
            system->ResolveResources(*m_resources);
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
        // "Box" becomes "Box (2)" (the first free counter among the copy's future siblings), so the
        // original and the copy are distinguishable at a glance.
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
        foundation::core::Serialize(ar, "type", typeId);
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
        foundation::core::Serialize(ar, "type", typeId);
        return ar.IsOk() ? typeId : String{};
    }

    bool SceneEditContext::PasteComponent(const Guid& entity, Span<const byte> blob)
    {
        PasteComponentCommand* raw =
            m_scene->Allocator().New<PasteComponentCommand>(*this, entity, blob);
        return m_commands->Execute(UniquePtr<IEditorCommand>(raw, m_scene->Allocator()));
    }

    scene::EntityHandle SceneEditContext::Resolve(const Guid& id)
    {
        return m_scene->FindEntity(id);
    }

    Guid SceneEditContext::CreateEntity(StringView name, const Guid& parent)
    {
        CreateEntityCommand* raw = m_scene->Allocator().New<CreateEntityCommand>(*this, name, parent);
        if (!m_commands->Execute(UniquePtr<IEditorCommand>(raw, m_scene->Allocator())))
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
            m_scene->Allocator().New<DestroyEntityCommand>(*this, entity), m_scene->Allocator()));
    }

    void SceneEditContext::RenameEntity(const Guid& entity, StringView newName)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<RenameEntityCommand>(*this, entity, newName),
            m_scene->Allocator()));
    }

    void SceneEditContext::ReparentEntity(const Guid& entity, const Guid& newParent)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<ReparentEntityCommand>(*this, entity, newParent),
            m_scene->Allocator()));
    }

    void SceneEditContext::MoveEntityBefore(const Guid& entity, const Guid& sibling)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<MoveEntityCommand>(*this, entity, sibling), m_scene->Allocator()));
    }

    void SceneEditContext::SetEntityActive(const Guid& entity, bool active)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<SetActiveCommand>(*this, entity, active), m_scene->Allocator()));
    }

    void SceneEditContext::SetLocalTransform(const Guid& entity, const Transform& transform)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<SetTransformCommand>(*this, entity, transform),
            m_scene->Allocator()));
    }

    void SceneEditContext::SetComponentProperty(const Guid& entity, const TypeInfo* componentType,
                                                const char* property, const Variant& value)
    {
        SetComponentProperty(entity, componentType, ComponentPropertyPath{}, property, value);
    }

    void SceneEditContext::SetComponentProperty(const Guid& entity, const TypeInfo* componentType,
                                                const ComponentPropertyPath& path,
                                                const char* property, const Variant& value)
    {
        (void)m_commands->Execute(
            UniquePtr<IEditorCommand>(m_scene->Allocator().New<SetComponentPropertyCommand>(
                                          *this, entity, componentType, path, property, value),
                                      m_scene->Allocator()));
    }

    void SceneEditContext::SetComponentPropertyRaw(const Guid& entity,
                                                   const TypeInfo* componentType,
                                                   const char* property, i64 value)
    {
        SetComponentPropertyRaw(entity, componentType, ComponentPropertyPath{}, property, value);
    }

    void SceneEditContext::SetComponentPropertyRaw(const Guid& entity,
                                                   const TypeInfo* componentType,
                                                   const ComponentPropertyPath& path,
                                                   const char* property, i64 value)
    {
        (void)m_commands->Execute(
            UniquePtr<IEditorCommand>(m_scene->Allocator().New<SetComponentPropertyCommand>(
                                          *this, entity, componentType, path, property, value),
                                      m_scene->Allocator()));
    }

    Instance SceneEditContext::ResolvePropertyOwner(const Guid& entity,
                                                    const TypeInfo* componentType,
                                                    const ComponentPropertyPath& path,
                                                    const TypeInfo** outOwnerType)
    {
        if (outOwnerType != nullptr)
        {
            *outOwnerType = nullptr;
        }
        const scene::EntityHandle e = Resolve(entity);
        scene::ComponentManagerBase* mgr = FindManager(componentType);
        if (!e.IsAssigned() || mgr == nullptr || componentType == nullptr)
        {
            return {};
        }
        const Instance component = mgr->GetComponentInstance(e);
        if (component.IsEmpty())
        {
            return {};
        }
        if (path.IsEmpty())
        {
            if (outOwnerType != nullptr)
            {
                *outOwnerType = componentType;
            }
            return component;
        }
        const PropertyInfo* containerProp = FindProperty(*componentType, path.container);
        if (containerProp == nullptr || containerProp->type == nullptr ||
            containerProp->type->container == nullptr || containerProp->address == nullptr)
        {
            return {};
        }
        const ContainerInfo& ci = *containerProp->type->container;
        const Instance container(containerProp->address(component), containerProp->type);
        if (container.Pointer() == nullptr || path.index >= ContainerSize(ci, container))
        {
            return {};
        }
        const Instance element = ContainerAddressAt(ci, container, path.index);
        if (element.IsEmpty() || element.Type() == nullptr)
        {
            return {};
        }
        if (outOwnerType != nullptr)
        {
            *outOwnerType = element.Type();
        }
        return element;
    }

    void SceneEditContext::SetSceneSettingProperty(const TypeInfo* settingsType,
                                                   const char* property, const Variant& value)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<SetSceneSettingCommand>(*this, settingsType, property, value),
            m_scene->Allocator()));
    }

    bool SceneEditContext::ApplySceneSettingsBlock(const TypeInfo* settingsType,
                                                   Array<byte> newBlob)
    {
        SetSceneSettingsBlockCommand* raw = m_scene->Allocator().New<SetSceneSettingsBlockCommand>(
            *this, settingsType, static_cast<Array<byte>&&>(newBlob));
        return m_commands->Execute(UniquePtr<IEditorCommand>(raw, m_scene->Allocator()));
    }

    void SceneEditContext::SetSceneSettingPropertyRaw(const TypeInfo* settingsType,
                                                      const char* property, i64 value)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<SetSceneSettingCommand>(*this, settingsType, property, value),
            m_scene->Allocator()));
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

    Guid SceneEditContext::SettingsEditProfile(const TypeInfo* settingsType, const char* property)
    {
        scene::SceneSystem* system = FindSystemBySettingsType(settingsType);
        const PropertyInfo* prop =
            (system != nullptr) ? FindProperty(*settingsType, property) : nullptr;
        if (prop == nullptr || system->SettingsProfileType() == nullptr ||
            FindAttribute(*prop, u8"sceneOnly") != nullptr)
        {
            return Guid{};
        }
        return system->SettingsProfile();
    }

    void* SceneEditContext::SettingsEditTarget(const TypeInfo* settingsType, const Guid& profile)
    {
        scene::SceneSystem* system = FindSystemBySettingsType(settingsType);
        if (system == nullptr)
        {
            return nullptr;
        }
        if (profile.IsNil())
        {
            return system->SettingsInstance();
        }
        // The profile the edit was made in, only while the block still uses it (an undo after
        // the source moved on reaches it again once that move is undone first).
        return system->SettingsProfile() == profile ? system->EffectiveSettingsInstance() : nullptr;
    }

    void SceneEditContext::NoteSettingsProfileEdited(const TypeInfo* settingsType,
                                                     const Guid& profile)
    {
        if (!profile.IsNil() && OnSettingsProfileEdited)
        {
            OnSettingsProfileEdited(settingsType, profile);
        }
    }

    namespace
    {
        // A settings block in the shape SetSceneSettingsBlockCommand reads: version-wrapped.
        Array<byte> CaptureSettings(scene::SceneSystem& system, const TypeInfo& type)
        {
            MemoryStream buffer;
            BinarySerializer writer(buffer, SerializeMode::Write);
            foundation::core::BeginVersionedPayload(writer, type);
            system.SerializeSettings(writer);
            foundation::core::EndVersionedPayload(writer);
            Array<byte> blob;
            const Span<const byte> bytes = buffer.Bytes();
            blob.Reserve(bytes.Size());
            for (byte b : bytes)
            {
                blob.PushBack(b);
            }
            return blob;
        }
    }

    bool SceneEditContext::MutateSceneSettings(const TypeInfo* settingsType,
                                               const Function<void(scene::SceneSystem&)>& mutate)
    {
        scene::SceneSystem* system = FindSystemBySettingsType(settingsType);
        if (system == nullptr || settingsType == nullptr)
        {
            return false;
        }
        const Array<byte> before = CaptureSettings(*system, *settingsType);
        mutate(*system);
        Array<byte> after = CaptureSettings(*system, *settingsType);
        // Restore the block (a non-undoable read), then apply the mutated one as a command, which
        // captures the block before: the whole mutation is one undo step.
        {
            MemoryStream buffer;
            (void)buffer.Write(before.Data(), before.Size());
            (void)buffer.Seek(0, SeekOrigin::Begin);
            BinarySerializer reader(buffer, SerializeMode::Read);
            foundation::core::BeginVersionedPayload(reader, *settingsType);
            system->SerializeSettings(reader);
            foundation::core::EndVersionedPayload(reader);
        }
        return ApplySceneSettingsBlock(settingsType, Move(after));
    }

    void SceneEditContext::AddComponent(const Guid& entity, const TypeInfo* componentType)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<AddComponentCommand>(*this, entity, componentType),
            m_scene->Allocator()));
    }

    void SceneEditContext::RemoveComponent(const Guid& entity, const TypeInfo* componentType)
    {
        (void)m_commands->Execute(UniquePtr<IEditorCommand>(
            m_scene->Allocator().New<RemoveComponentCommand>(*this, entity, componentType),
            m_scene->Allocator()));
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

    bool SceneEditContext::MutateComponent(const Guid& entity, const TypeInfo* componentType,
                                           const Function<void(const Instance&)>& mutate)
    {
        const scene::EntityHandle e = Resolve(entity);
        scene::ComponentManagerBase* manager = FindManager(componentType);
        if (manager == nullptr || !e.IsAssigned() || !manager->HasComponent(e))
        {
            return false;
        }
        Array<byte> before = CopyComponent(entity, componentType); // the value before
        if (before.IsEmpty())
        {
            return false;
        }
        mutate(manager->GetComponentInstance(e));
        Array<byte> after = CopyComponent(entity, componentType);
        // Restore the live value (a non-undoable read), then paste the mutated one: the paste
        // command captures the value before, so the whole mutation is one undo step.
        {
            MemoryStream buffer;
            (void)buffer.Write(before.Data(), before.Size());
            (void)buffer.Seek(0, SeekOrigin::Begin);
            BinarySerializer ar(buffer, SerializeMode::Read);
            String typeId;
            foundation::core::Serialize(ar, "type", typeId);
            manager->ReadComponent(ar, e);
        }
        return !after.IsEmpty() && PasteComponent(entity, Span<const byte>{after.Data(), after.Size()});
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
        foundation::core::Serialize(ar, "count", count);
        for (SubtreeRecord& r : records)
        {
            ar.Key("id");
            ar.GuidValue(r.id);
            ar.Key("parent");
            ar.GuidValue(r.parent);
            foundation::core::Serialize(ar, "name", r.name);
            foundation::core::Serialize(ar, "position", r.local.position);
            foundation::core::Serialize(ar, "rotation", r.local.rotation);
            foundation::core::Serialize(ar, "scale", r.local.scale);
            foundation::core::Serialize(ar, "active", r.active);
            u32 componentCount = static_cast<u32>(r.components.Size());
            foundation::core::Serialize(ar, "components", componentCount);
            for (SubtreeComponentRecord& c : r.components)
            {
                foundation::core::Serialize(ar, "type", c.typeId);
                foundation::core::Serialize(ar, "blob", c.blob);
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
        foundation::core::Serialize(ar, "count", count);
        for (u32 i = 0; i < count && ar.IsOk(); ++i)
        {
            SubtreeRecord r;
            ar.Key("id");
            ar.GuidValue(r.id);
            ar.Key("parent");
            ar.GuidValue(r.parent);
            foundation::core::Serialize(ar, "name", r.name);
            foundation::core::Serialize(ar, "position", r.local.position);
            foundation::core::Serialize(ar, "rotation", r.local.rotation);
            foundation::core::Serialize(ar, "scale", r.local.scale);
            foundation::core::Serialize(ar, "active", r.active);
            u32 componentCount = 0;
            foundation::core::Serialize(ar, "components", componentCount);
            for (u32 c = 0; c < componentCount && ar.IsOk(); ++c)
            {
                SubtreeComponentRecord component;
                foundation::core::Serialize(ar, "type", component.typeId);
                foundation::core::Serialize(ar, "blob", component.blob);
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
        SpawnPrefabCommand* raw = m_scene->Allocator().New<SpawnPrefabCommand>(
            *this, prefabId, Move(payload), parent, rootTransform, placeBefore);
        if (!m_commands->Execute(UniquePtr<IEditorCommand>(raw, m_scene->Allocator())))
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
        // placeBefore is the original itself, so the instance takes its exact sibling slot
        // (spawning otherwise appends, dropping the replaced entity to the end of the hierarchy).
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
            // Added by the user: reverting removes it (undoable through the existing command).
            const scene::EntityHandle live = Resolve(entity);
            if (live.IsAssigned() && manager->HasComponent(live))
            {
                RemoveComponent(entity, componentType);
            }
            return true;
        }
        // The clipboard blob format (typeId and payload) goes through the undoable paste path.
        MemoryStream buffer;
        BinarySerializer ar(buffer, SerializeMode::Write);
        String typeId = String(manager->SerializationTypeId());
        foundation::core::Serialize(ar, "type", typeId);
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
            m_scene->Allocator().New<PasteEntitiesCommand>(*this, Move(records), parent);
        if (!m_commands->Execute(UniquePtr<IEditorCommand>(raw, m_scene->Allocator())))
        {
            return Guid{};
        }
        const Guid created = raw->RootGuid();
        m_selection.Set(created);
        return created;
    }
}
