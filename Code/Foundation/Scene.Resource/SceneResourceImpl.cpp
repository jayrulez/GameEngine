// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Scene - foundation.scene.resource implementation unit.
//
// Out-of-line definitions for SceneResource's public serialization API (sec 3.2 / sec 10.6):
// the free-function bodies (SerializeScene, CapturePrefab, SpawnPrefab, LoadScene, ...) and the
// RTTI_DEFINE_OBJECT reflection bodies. SceneResource.cppm keeps the declarations, the
// detail-namespace helpers, and the document classes.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module foundation.scene.resource;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.scene;
import foundation.xml;
import foundation.xml.serialization;

using namespace foundation::core;

namespace foundation::scene
{
    void SerializeScene(ISerializer& ar, Scene& scene, ScenePrefabMode prefabMode,
                        bool includeSettings, detail::SceneStreamEncoding encoding)
    {
        const bool writing = ar.Mode() == SerializeMode::Write;
        const bool text = encoding == detail::SceneStreamEncoding::Text;
        (void)text;

        // Stream header: writers emit the current one; readers REQUIRE it (magic + the current
        // version) - any other stream fails here, before a single record is read.
        if (writing)
        {
            detail::WriteSceneStreamHeader(ar);
        }
        else
        {
            u32 magic = 0;
            u32 version = 0;
            foundation::core::Serialize(ar, "magic", magic);
            foundation::core::Serialize(ar, "version", version);
            if (magic != detail::kSceneStreamMagic || version != detail::kSceneStreamVersion)
            {
                LOG_ERROR(u8"Scene", u8"scene stream is not in the current format (version {} - "
                                     u8"expected {}); re-save it with the build that wrote it",
                          magic == detail::kSceneStreamMagic ? version : 0u,
                          detail::kSceneStreamVersion);
                ar.FailPayload(ErrorCode::NotSupported);
                return;
            }
        }

        // Referenced writes exclude prefab-instance members from the plain entity/component
        // arrays (they respawn from their prefab at load; only deltas persist).
        HashMap<Guid, u8> prefabMembers;
        if (writing && prefabMode == ScenePrefabMode::Referenced)
        {
            scene.ForEachPrefabInstance(
                [&](Scene::PrefabInstanceState& state)
                {
                    for (const Guid& live : state.liveIds)
                    {
                        prefabMembers.InsertOrAssign(live, 1u);
                    }
                });
        }

        // --- name ---
        String name = writing ? String(scene.Name()) : String{};
        foundation::core::Serialize(ar, "name", name);
        if (!writing)
        {
            scene.SetName(name.AsView());
        }

        // --- entities (id, name, active, parent-id, local transform) ---
        u32 entityCount = 0;
        Array<EntityHandle> handles;
        if (writing)
        {
            // TREE order (roots in list order, depth-first children): load recreates entities and
            // relinks parents in FILE order, so sibling ORDER round-trips (the editor hierarchy is
            // reorderable; pool order would shuffle siblings back to creation order).
            Array<EntityHandle> stack;
            for (EntityHandle r = scene.GetFirstRoot(); r.IsAssigned(); r = scene.GetNextSibling(r))
            {
                stack.PushBack(r);
                while (!stack.IsEmpty())
                {
                    EntityHandle e = stack.Back();
                    stack.PopBack();
                    // Prefab-instance members persist as ref+deltas, never as plain records.
                    // Their non-member children (user entities parented INTO an instance) still
                    // serialize - their saved parent guid stays valid because instances respawn
                    // with their SAVED member guids.
                    if (prefabMembers.Find(scene.GetEntityId(e)) == nullptr)
                    {
                        handles.PushBack(e);
                    }
                    // Push children reversed so they POP in list order.
                    Array<EntityHandle> kids;
                    for (EntityHandle c = scene.GetFirstChild(e); c.IsAssigned();
                         c = scene.GetNextSibling(c))
                    {
                        kids.PushBack(c);
                    }
                    for (usize i = kids.Size(); i-- > 0;)
                    {
                        stack.PushBack(kids[i]);
                    }
                }
            }
            entityCount = static_cast<u32>(handles.Size());
        }
        ar.Key("entities");
        ar.BeginArray(entityCount);
        if (writing)
        {
            for (EntityHandle e : handles)
            {
                Guid id = scene.GetEntityId(e);
                String ename = String(scene.GetEntityName(e));
                u8 active = scene.IsActive(e) ? 1u : 0u;
                EntityHandle parent = scene.GetParent(e);
                Guid parentId = parent.IsAssigned() ? scene.GetEntityId(parent) : Guid{};
                Transform t = scene.GetLocalTransform(e);
                detail::SerializeGuid(ar, "id", id);
                foundation::core::Serialize(ar, "name", ename);
                foundation::core::Serialize(ar, "active", active);
                detail::SerializeGuid(ar, "parent", parentId);
                detail::SerializeTransform(ar, t);
            }
        }
        else
        {
            Array<Guid> ids;
            Array<Guid> parents;
            for (u32 i = 0; i < entityCount; ++i)
            {
                Guid id;
                String ename;
                u8 active = 0;
                Guid parentId;
                Transform t;
                detail::SerializeGuid(ar, "id", id);
                foundation::core::Serialize(ar, "name", ename);
                foundation::core::Serialize(ar, "active", active);
                detail::SerializeGuid(ar, "parent", parentId);
                detail::SerializeTransform(ar, t);
                // Corrupt-save recovery: a duplicate entity guid (the pre-fix RNG-collision bug)
                // gets a FRESH id so every entity stays uniquely addressable. Records addressed
                // to the shared guid (components, parent links) route to its FIRST holder.
                EntityHandle h;
                if (scene.FindEntity(id).IsAssigned())
                {
                    LOG_WARNING(
                        u8"Scene",
                        u8"duplicate entity guid in save for '{}' - assigning a fresh id", ename);
                    h = scene.CreateEntity(ename.AsView());
                    ids.PushBack(scene.GetEntityId(h)); // parent RELINK by the fresh id
                }
                else
                {
                    h = scene.CreateEntity(id, ename.AsView());
                    ids.PushBack(id);
                }
                scene.SetActive(h, active != 0);
                scene.SetLocalTransform(h, t);
                parents.PushBack(parentId);
            }
            // relink parents now that every entity exists
            for (usize i = 0; i < ids.Size(); ++i)
            {
                if (parents[i] != Guid{})
                {
                    EntityHandle child = scene.FindEntity(ids[i]);
                    EntityHandle parent = scene.FindEntity(parents[i]);
                    if (child.IsAssigned() && parent.IsAssigned())
                    {
                        scene.SetParent(child, parent);
                    }
                }
            }
        }
        ar.EndArray();

        // --- components (owner-id, type-id, data) ---
        // NOTE: records are written inline; load requires the owning manager to be present
        // on `scene` (the normal case - managers are injected before load). Skipping an
        // unknown component type (length-prefixed records) is a future robustness item.
        u32 componentCount = 0;
        struct Record
        {
            ComponentManagerBase* manager;
            EntityHandle owner;
        };
        Array<Record> records;
        if (writing)
        {
            scene.ForEachManager(
                [&](ComponentManagerBase& m)
                {
                    if (!m.IsSerializable())
                    {
                        return;
                    }
                    for (EntityHandle owner : m.OwnerHandles())
                    {
                        if (prefabMembers.Find(scene.GetEntityId(owner)) != nullptr)
                        {
                            continue;
                        }
                        records.PushBack(Record{&m, owner});
                    }
                });
            componentCount = static_cast<u32>(records.Size());
            // Unresolved records (their manager was absent at load) write back VERBATIM in the
            // encoding they were captured in - a save never drops a plugin's components
            // (game-native-code.md S3). A cross-encoding write cannot re-encode an unknown
            // type: those are dropped LOUDLY.
            u32 dropped = 0;
            for (const Scene::UnresolvedComponent& u : scene.UnresolvedComponents())
            {
                if (u.text == text)
                {
                    ++componentCount;
                }
                else
                {
                    ++dropped;
                }
            }
            if (dropped > 0)
            {
                LOG_WARNING(u8"Scene",
                            u8"dropping {} unresolved component record(s) captured in the other "
                            u8"encoding (their plugin was not loaded; load it and re-save to "
                            u8"convert)",
                            dropped);
            }
        }
        ar.Key("components");
        ar.BeginArray(componentCount);
        if (writing)
        {
            // TEXT: records inline in object scopes (diffable, skip-by-scope); BINARY v2:
            // length-prefixed blobs so readers can skip types this build doesn't know.
            for (Record& r : records)
            {
                detail::WriteComponentRecord(ar, scene, *r.manager, r.owner, text);
            }
            for (const Scene::UnresolvedComponent& u : scene.UnresolvedComponents())
            {
                if (u.text != text)
                {
                    continue;
                }
                Guid ownerId = u.owner;
                String typeId = u.typeId;
                Array<u8> payload = u.payload;
                if (text)
                {
                    ar.BeginObject();
                    detail::SerializeGuid(ar, "owner", ownerId);
                    foundation::core::Serialize(ar, "type", typeId);
                    (void)ar.RawRemainder(payload); // re-injects the captured <data> element
                    ar.EndObject();
                }
                else
                {
                    detail::SerializeGuid(ar, "owner", ownerId);
                    foundation::core::Serialize(ar, "type", typeId);
                    foundation::core::Serialize(ar, "data", payload);
                }
            }
        }
        else
        {
            HashMap<String, u8> warned;
            for (u32 i = 0; i < componentCount; ++i)
            {
                if (text)
                {
                    ar.BeginObject();
                    Guid ownerId;
                    String typeId;
                    detail::SerializeGuid(ar, "owner", ownerId);
                    foundation::core::Serialize(ar, "type", typeId);
                    EntityHandle owner = scene.FindEntity(ownerId);
                    ComponentManagerBase* manager =
                        scene.FindManagerBySerializationId(typeId.AsView());
                    if (owner.IsAssigned() && manager != nullptr)
                    {
                        ar.Key("data");
                        ar.BeginObject();
                        manager->ReadComponent(ar, owner);
                        ar.EndObject();
                    }
                    else if (manager == nullptr)
                    {
                        // Preserve verbatim (S3): the record's remaining element (<data>) is
                        // captured raw and written back untouched on save; it resolves into a
                        // real component the moment its manager arrives.
                        Scene::UnresolvedComponent record;
                        record.owner = ownerId;
                        record.typeId = typeId;
                        record.text = true;
                        if (ar.RawRemainder(record.payload))
                        {
                            scene.AddUnresolvedComponent(Move(record));
                        }
                        if (warned.Find(typeId) == nullptr)
                        {
                            warned.InsertOrAssign(typeId, 1u);
                            LOG_WARNING(u8"Scene",
                                        u8"component type '{}' has no manager in this build - "
                                        u8"records kept unresolved (preserved on save)",
                                        typeId);
                        }
                    }
                    ar.EndObject();
                    continue;
                }
                Guid ownerId;
                String typeId;
                detail::SerializeGuid(ar, "owner", ownerId);
                foundation::core::Serialize(ar, "type", typeId);
                EntityHandle owner = scene.FindEntity(ownerId);
                ComponentManagerBase* manager = scene.FindManagerBySerializationId(typeId.AsView());
                Array<u8> blob;
                foundation::core::Serialize(ar, "data", blob);
                if (owner.IsAssigned() && manager != nullptr)
                {
                    detail::ComponentFromBlob(*manager, owner,
                                              Span<const u8>{blob.Data(), blob.Size()});
                }
                else if (manager == nullptr)
                {
                    Scene::UnresolvedComponent record; // preserved verbatim (S3)
                    record.owner = ownerId;
                    record.typeId = typeId;
                    record.payload = Move(blob);
                    record.text = false;
                    scene.AddUnresolvedComponent(Move(record));
                    if (warned.Find(typeId) == nullptr)
                    {
                        warned.InsertOrAssign(typeId, 1u);
                        LOG_WARNING(u8"Scene",
                                    u8"component type '{}' has no manager in this build - "
                                    u8"records kept unresolved (preserved on save)",
                                    typeId);
                    }
                }
            }
        }
        ar.EndArray();

        // --- scene-system settings (id, versioned payload) ---
        u32 settingsCount = 0;
        if (writing && includeSettings)
        {
            scene.ForEachSystem(
                [&](SceneSystem& s)
                {
                    if (s.SettingsType() != nullptr)
                    {
                        ++settingsCount;
                    }
                });
        }
        ar.Key("systemSettings");
        if (writing)
        {
            // Preserved settings of absent systems write back verbatim in their encoding (S3).
            u32 droppedSettings = 0;
            for (const Scene::UnresolvedSettings& u : scene.UnresolvedSettingsRecords())
            {
                if (u.text == text)
                {
                    ++settingsCount;
                }
                else
                {
                    ++droppedSettings;
                }
            }
            if (droppedSettings > 0)
            {
                LOG_WARNING(u8"Scene",
                            u8"dropping {} unresolved settings record(s) captured in the other "
                            u8"encoding (their plugin was not loaded)",
                            droppedSettings);
            }
        }
        ar.BeginArray(settingsCount);
        if (writing)
        {
            // v2 binary: each system's settings serialize into a length-prefixed blob - readers
            // skip systems this build doesn't have instead of aborting the section. TEXT gets
            // the same skippability from per-record object scopes with the payload INLINE.
            scene.ForEachSystem(
                [&](SceneSystem& s)
                {
                    if (s.SettingsType() == nullptr || !includeSettings)
                    {
                        return;
                    }
                    String id(s.SettingsId());
                    if (text)
                    {
                        ar.BeginObject();
                        foundation::core::Serialize(ar, "system", id);
                        foundation::core::BeginVersionedPayload(ar, *s.SettingsType());
                        ar.Key("settings");
                        ar.BeginObject();
                        s.SerializeSettings(ar);
                        ar.EndObject();
                        foundation::core::EndVersionedPayload(ar);
                        ar.EndObject();
                        return;
                    }
                    foundation::core::Serialize(ar, "system", id);
                    MemoryStream buffer;
                    {
                        BinarySerializer sub(buffer, SerializeMode::Write);
                        foundation::core::BeginVersionedPayload(sub, *s.SettingsType());
                        sub.Key("settings");
                        sub.BeginObject();
                        s.SerializeSettings(sub);
                        sub.EndObject();
                        foundation::core::EndVersionedPayload(sub);
                    }
                    Array<u8> blob;
                    const Span<const byte> bytes = buffer.Bytes();
                    blob.Reserve(bytes.Size());
                    for (byte b : bytes)
                    {
                        blob.PushBack(static_cast<u8>(b));
                    }
                    foundation::core::Serialize(ar, "data", blob);
                });
            for (const Scene::UnresolvedSettings& u : scene.UnresolvedSettingsRecords())
            {
                if (u.text != text)
                {
                    continue;
                }
                String id = u.systemId;
                Array<u8> payload = u.payload;
                if (text)
                {
                    ar.BeginObject();
                    foundation::core::Serialize(ar, "system", id);
                    (void)ar.RawRemainder(payload); // dataVersions + <settings>, verbatim
                    ar.EndObject();
                }
                else
                {
                    foundation::core::Serialize(ar, "system", id);
                    foundation::core::Serialize(ar, "data", payload);
                }
            }
        }
        else
        {
            for (u32 i = 0; i < settingsCount; ++i)
            {
                if (text)
                {
                    ar.BeginObject();
                    String id;
                    foundation::core::Serialize(ar, "system", id);
                    SceneSystem* textTarget = nullptr;
                    scene.ForEachSystem(
                        [&](SceneSystem& s)
                        {
                            if (textTarget == nullptr && s.SettingsType() != nullptr &&
                                s.SettingsId() == id.AsView())
                            {
                                textTarget = &s;
                            }
                        });
                    if (textTarget != nullptr)
                    {
                        foundation::core::BeginVersionedPayload(ar, *textTarget->SettingsType());
                        ar.Key("settings");
                        ar.BeginObject();
                        textTarget->SerializeSettings(ar);
                        ar.EndObject();
                        foundation::core::EndVersionedPayload(ar);
                    }
                    else
                    {
                        Scene::UnresolvedSettings record; // preserved verbatim (S3)
                        record.systemId = id;
                        record.text = true;
                        if (ar.RawRemainder(record.payload))
                        {
                            scene.AddUnresolvedSettings(Move(record));
                        }
                        LOG_WARNING(u8"Scene",
                                    u8"settings of system '{}' kept unresolved (no such system in "
                                    u8"this build; preserved on save)",
                                    id);
                    }
                    ar.EndObject();
                    continue;
                }
                String id;
                foundation::core::Serialize(ar, "system", id);
                SceneSystem* target = nullptr;
                scene.ForEachSystem(
                    [&](SceneSystem& s)
                    {
                        if (target == nullptr && s.SettingsType() != nullptr &&
                            s.SettingsId() == id.AsView())
                        {
                            target = &s;
                        }
                    });
                Array<u8> blob;
                foundation::core::Serialize(ar, "data", blob);
                if (target == nullptr)
                {
                    Scene::UnresolvedSettings record; // preserved verbatim (S3)
                    record.systemId = id;
                    record.payload = Move(blob);
                    record.text = false;
                    scene.AddUnresolvedSettings(Move(record));
                    LOG_WARNING(u8"Scene",
                                u8"settings of system '{}' kept unresolved (no such system in "
                                u8"this build; preserved on save)",
                                id);
                    continue;
                }
                MemoryStream buffer;
                (void)buffer.Write(reinterpret_cast<const byte*>(blob.Data()), blob.Size());
                (void)buffer.Seek(0, SeekOrigin::Begin);
                BinarySerializer sub(buffer, SerializeMode::Read);
                foundation::core::BeginVersionedPayload(sub, *target->SettingsType());
                sub.Key("settings");
                sub.BeginObject();
                target->SerializeSettings(sub);
                sub.EndObject();
                foundation::core::EndVersionedPayload(sub);
            }
        }
        ar.EndArray();

        // --- prefab instances (appended after settings) ---
        u8 sectionMode = (prefabMode == ScenePrefabMode::Referenced)
                             ? detail::kPrefabWireReferenced3
                             : detail::kPrefabWireExpanded2;
        foundation::core::Serialize(ar, "prefabMode", sectionMode);
        if (!writing && sectionMode != detail::kPrefabWireReferenced3 &&
            sectionMode != detail::kPrefabWireExpanded2)
        {
            LOG_ERROR(u8"Scene", u8"prefab section uses a retired layout ({}) - the scene fails "
                                 u8"to load; re-save it with the build that wrote it",
                      static_cast<u32>(sectionMode));
            ar.FailPayload(ErrorCode::NotSupported);
            return;
        }
        const bool wireReferenced = sectionMode == detail::kPrefabWireReferenced3;

        if (wireReferenced)
        {
            // Ref + deltas: prefab id, placement, the SAVED member guid map (respawn preserves
            // entity identity across load), and overrides DERIVED right here by comparing live
            // state against the spawn-time baselines.
            u32 instanceCount = 0;
            if (writing)
            {
                scene.ForEachPrefabInstance([&](Scene::PrefabInstanceState&) { ++instanceCount; });
                // Parked pendings re-emit VERBATIM: a load->save cycle that never ran
                // ResolveScenePrefabs (the export transcode) must not lose the section.
                instanceCount += static_cast<u32>(scene.PendingPrefabInstanceCount());
            }
            ar.Key("prefabInstances");
            ar.BeginArray(instanceCount);
            if (writing)
            {
                scene.ForEachPrefabInstance(
                    [&](Scene::PrefabInstanceState& state)
                    {
                        // Overrides are DERIVED here: live state vs the spawn-time baselines.
                        UniquePtr<Scene::PendingPrefabInstance> d =
                            detail::ComputeInstanceDeltas(scene, state);
                        detail::WritePrefabRecord(ar, scene, *d, text);
                    });
                scene.ForEachPendingPrefabInstance(
                    [&](Scene::PendingPrefabInstance& pending)
                    { detail::WritePrefabRecord(ar, scene, pending, text); });
            }
            else
            {
                for (u32 n = 0; n < instanceCount; ++n)
                {
                    auto pending = MakeUnique<Scene::PendingPrefabInstance>(scene.Allocator());
                    detail::ReadPrefabRecord(ar, scene, *pending, text);
                    scene.AddPendingPrefabInstance(
                        static_cast<UniquePtr<Scene::PendingPrefabInstance>&&>(pending));
                }
            }
            ar.EndArray();
        }
        else
        {
            // Expanded (snapshots): members serialized flat above; persist the instance STATE
            // verbatim (member map + baselines) so restore rebuilds bookkeeping resolver-free.
            u32 instanceCount = 0;
            if (writing)
            {
                scene.ForEachPrefabInstance([&](Scene::PrefabInstanceState&) { ++instanceCount; });
            }
            ar.Key("prefabStates");
            ar.BeginArray(instanceCount);
            if (writing)
            {
                scene.ForEachPrefabInstance(
                    [&](Scene::PrefabInstanceState& state)
                    {
                        detail::SerializeGuid(ar, "prefab", state.prefabId);
                        detail::SerializeGuid(ar, "root", state.rootEntityId);
                        detail::SerializeGuid(ar, "owner", state.ownerRootEntityId);
                        detail::SerializeGuid(ar, "nestedSrcRoot", state.nestedRootSourceId);
                        u32 memberCount = static_cast<u32>(state.sourceIds.Size());
                        ar.Key("members");
                        ar.BeginArray(memberCount);
                        for (u32 i = 0; i < memberCount; ++i)
                        {
                            detail::SerializeGuid(ar, "src", state.sourceIds[i]);
                            detail::SerializeGuid(ar, "live", state.liveIds[i]);
                            detail::SerializeTransform(ar, state.baselineTransforms[i]);
                        }
                        ar.EndArray();
                        u32 baselineCount = static_cast<u32>(state.componentBaselines.Size());
                        ar.Key("baselines");
                        ar.BeginArray(baselineCount);
                        for (Scene::PrefabComponentBaseline& b : state.componentBaselines)
                        {
                            detail::SerializeGuid(ar, "src", b.sourceEntity);
                            foundation::core::Serialize(ar, "type", b.typeId);
                            foundation::core::Serialize(ar, "blob", b.blob);
                        }
                        ar.EndArray();
                    });
            }
            else
            {
                // The reader mirrors the writer above field for field (the Simulate-stop hang
                // was a reader that skipped 32 bytes the writer produced: the next count read
                // was mid-guid garbage and the member loop allocated until the OS killed the
                // editor - wire symmetry is the rule).
                if (instanceCount > detail::kMaxPrefabRecordEntries)
                {
                    return;
                } // misparse guard
                for (u32 n = 0; n < instanceCount; ++n)
                {
                    auto state = MakeUnique<Scene::PrefabInstanceState>(scene.Allocator());
                    detail::SerializeGuid(ar, "prefab", state->prefabId);
                    detail::SerializeGuid(ar, "root", state->rootEntityId);
                    detail::SerializeGuid(ar, "owner", state->ownerRootEntityId);
                    detail::SerializeGuid(ar, "nestedSrcRoot", state->nestedRootSourceId);
                    u32 memberCount = 0;
                    ar.Key("members");
                    ar.BeginArray(memberCount);
                    if (memberCount > detail::kMaxPrefabRecordEntries)
                    {
                        return;
                    } // misparse guard
                    for (u32 i = 0; i < memberCount; ++i)
                    {
                        Guid src, live;
                        Transform t;
                        detail::SerializeGuid(ar, "src", src);
                        detail::SerializeGuid(ar, "live", live);
                        detail::SerializeTransform(ar, t);
                        state->sourceIds.PushBack(src);
                        state->liveIds.PushBack(live);
                        state->baselineTransforms.PushBack(t);
                    }
                    ar.EndArray();
                    u32 baselineCount = 0;
                    ar.Key("baselines");
                    ar.BeginArray(baselineCount);
                    if (baselineCount > detail::kMaxPrefabRecordEntries)
                    {
                        return;
                    } // misparse guard
                    for (u32 i = 0; i < baselineCount; ++i)
                    {
                        Scene::PrefabComponentBaseline b;
                        detail::SerializeGuid(ar, "src", b.sourceEntity);
                        foundation::core::Serialize(ar, "type", b.typeId);
                        foundation::core::Serialize(ar, "blob", b.blob);
                        state->componentBaselines.PushBack(
                            static_cast<Scene::PrefabComponentBaseline&&>(b));
                    }
                    ar.EndArray();
                    scene.AddPrefabInstance(
                        static_cast<UniquePtr<Scene::PrefabInstanceState>&&>(state));
                }
            }
            ar.EndArray();
        }
    }

    void ResolveSceneResources(Scene& scene, foundation::resource::ResourceManager& resources)
    {
        // ALL systems, not just component managers: plain systems' settings blocks can hold
        // resource::Refs too (the environment's sky texture).
        scene.ForEachSystem([&](SceneSystem& system) { system.ResolveResources(resources); });
    }

    void ResolveEntityResources(Scene& scene, EntityHandle root,
                                foundation::resource::ResourceManager& resources)
    {
        if (!scene.IsValid(root))
        {
            return;
        }
        scene.ForEachSystem([&](SceneSystem& system) { system.ResolveEntityResources(root, resources); });
        for (EntityHandle child = scene.GetFirstChild(root); child.IsAssigned();
             child = scene.GetNextSibling(child))
        {
            ResolveEntityResources(scene, child, resources);
        }
    }

    EntityHandle PrefabSpawnSystem::Spawn(const Guid& prefab, const Float3& position,
                                          const Quaternion& rotation, EntityHandle parent)
    {
        if (m_scene == nullptr)
        {
            return EntityHandle::Invalid();
        }
        const EntityHandle root = SpawnInto(*m_scene, m_database, m_resources, prefab, parent);
        if (!root.IsAssigned())
        {
            return EntityHandle::Invalid();
        }
        Transform transform = m_scene->GetLocalTransform(root);
        transform.position = position;
        transform.rotation = rotation;
        m_scene->SetLocalTransform(root, transform);
        return root;
    }

    EntityHandle PrefabSpawnSystem::SpawnInto(Scene& scene,
                                              foundation::content::IContentDatabase* database,
                                              foundation::resource::ResourceManager* resources,
                                              const Guid& prefab, EntityHandle parent)
    {
        if (database == nullptr || prefab.IsNil())
        {
            return EntityHandle::Invalid();
        }
        foundation::content::Instance* instance = database->GetInstance(prefab);
        if (instance == nullptr)
        {
            return EntityHandle::Invalid();
        }
        UniquePtr<IStream> payload = instance->ReadData(u8"scene");
        if (!payload)
        {
            return EntityHandle::Invalid();
        }
        // The prefabs this one nests come from the same database; without a resolver their
        // records would be skipped and the instance would arrive missing its children.
        const PrefabPayloadResolver resolver{[database](const Guid& id) -> UniquePtr<IStream>
                                             {
                                                 foundation::content::Instance* nested =
                                                     database->GetInstance(id);
                                                 return nested != nullptr ? nested->ReadData(u8"scene")
                                                                          : UniquePtr<IStream>{};
                                             }};
        const EntityHandle root = SpawnPrefab(scene, *payload, prefab, parent, nullptr, &resolver);
        // The subtree names its resources by id and nothing has bound them yet.
        if (root.IsAssigned() && resources != nullptr)
        {
            foundation::scene::ResolveEntityResources(scene, root, *resources);
        }
        return root;
    }

    void AddPrefabSpawnSceneManagers(Scene& scene)
    {
        scene.AddSystem<PrefabSpawnSystem>();
    }

    Status CapturePrefab(Scene& scene, EntityHandle root, IStream& out,
                         detail::SceneStreamEncoding encoding)
    {
        if (!root.IsAssigned())
        {
            return Status{ErrorCode::NotFound};
        }
        if (encoding == detail::SceneStreamEncoding::Binary)
        {
            BinarySerializer ar(out, SerializeMode::Write);
            return detail::CapturePrefabBody(ar, false, scene, root);
        }
        foundation::xml::XmlSerializer ar(scene.Allocator());
        const Status body = detail::CapturePrefabBody(ar, true, scene, root);
        if (!body.IsOk())
        {
            return body;
        }
        String textOut;
        ar.GetOutput(textOut);
        return (out.Write(reinterpret_cast<const byte*>(textOut.CStr()), textOut.Size()) ==
                textOut.Size())
                   ? Status{}
                   : Status{ErrorCode::Unknown};
    }

    // Prefab instancing remaps entity OWNERS through `liveBySource` (source guid -> this instance's
    // fresh guid). A component may ALSO reference entities INSIDE its fields via scene::EntityRef -
    // walk the freshly-read component's reflected EntityRef properties and remap any that point INTO
    // the prefab to the instance's copy. References resolving OUTSIDE the prefab (not in the map) are
    // left untouched: they point at the wider scene, not the template. Reflection-driven, so every
    // component with an EntityRef field is covered without per-type code. Called after each component
    // read and BEFORE the baseline blob is captured, so the baseline reflects the remapped value.
    static void RemapPrefabEntityRefs(ComponentManagerBase& manager, EntityHandle owner,
                                      const HashMap<Guid, Guid>& liveBySource)
    {
        const TypeInfo* type = manager.ComponentType();
        if (type == nullptr)
        {
            return;
        }
        const Instance inst = manager.GetComponentInstance(owner);
        if (inst.IsEmpty())
        {
            return;
        }
        const auto remapOne = [&liveBySource](EntityRef* ref)
        {
            if (ref == nullptr || ref->id.IsNil())
            {
                return;
            }
            if (const Guid* live = liveBySource.Find(ref->id))
            {
                ref->id = *live;
            }
        };
        for (const PropertyInfo& p : Properties(*type))
        {
            if (p.address == nullptr)
            {
                continue;
            }
            if (p.type == &TypeOf<EntityRef>())
            {
                remapOne(static_cast<EntityRef*>(p.address(inst)));
                continue;
            }
            // Containers of EntityRef (e.g. an animator's meshEntities list): walk the
            // elements by address so every entry remaps in place.
            if (p.type != nullptr && p.type->container != nullptr &&
                p.type->container->elementType == &TypeOf<EntityRef>() &&
                p.type->container->addressAt != nullptr)
            {
                const Instance field{p.address(inst), p.type};
                const usize count = p.type->container->size(field);
                for (usize i = 0; i < count; ++i)
                {
                    const Instance element = p.type->container->addressAt(field, i);
                    remapOne(static_cast<EntityRef*>(element.Pointer()));
                }
            }
        }
    }

    void detail::RemapOverrideEntityRefs(ComponentManagerBase& manager, EntityHandle owner,
                                         const Scene::PrefabInstanceState& state)
    {
        HashMap<Guid, Guid> liveBySource;
        for (usize i = 0; i < state.sourceIds.Size() && i < state.liveIds.Size(); ++i)
        {
            liveBySource.InsertOrAssign(state.sourceIds[i], state.liveIds[i]);
        }
        RemapPrefabEntityRefs(manager, owner, liveBySource);
    }

    EntityHandle SpawnPrefab(Scene& scene, IStream& payload, const Guid& prefabId,
                             EntityHandle parent, const HashMap<Guid, Guid>* preassigned,
                             const PrefabPayloadResolver* resolver,
                             const Array<const Scene::PendingPrefabInstance*>* nestedSceneDeltas,
                             bool spawnNested)
    {
        detail::SceneStreamReader reader;
        Serializer* opened = reader.Open(payload);
        if (opened == nullptr)
        {
            return EntityHandle::Invalid();
        }
        Serializer& ar = *opened;
        const bool text = reader.Encoding() == detail::SceneStreamEncoding::Text;
        u32 magic = detail::kSceneStreamMagic;
        u32 streamVersion = 0;
        if (text)
        {
            foundation::core::Serialize(ar, "magic", magic);
            foundation::core::Serialize(ar, "version", streamVersion);
        }
        else
        {
            streamVersion = detail::ReadSceneStreamVersion(payload);
        }
        if (magic != detail::kSceneStreamMagic || streamVersion != detail::kSceneStreamVersion)
        {
            LOG_ERROR(u8"Scene", u8"prefab payload is not in the current format (version {} - "
                                 u8"expected {}); re-save the prefab with the build that wrote it",
                      streamVersion, detail::kSceneStreamVersion);
            return EntityHandle::Invalid();
        }

        String name;
        foundation::core::Serialize(ar, "name", name);

        auto state = MakeUnique<Scene::PrefabInstanceState>(scene.Allocator());
        state->prefabId = prefabId;

        u32 entityCount = 0;
        ar.Key("entities");
        ar.BeginArray(entityCount);
        HashMap<Guid, Guid> liveBySource;
        Array<Guid> sourceParents;
        EntityHandle firstRoot = EntityHandle::Invalid();
        for (u32 i = 0; i < entityCount; ++i)
        {
            Guid sourceId;
            String ename;
            u8 active = 0;
            Guid sourceParent;
            Transform t;
            detail::SerializeGuid(ar, "id", sourceId);
            foundation::core::Serialize(ar, "name", ename);
            foundation::core::Serialize(ar, "active", active);
            detail::SerializeGuid(ar, "parent", sourceParent);
            detail::SerializeTransform(ar, t);

            EntityHandle live;
            const Guid* wanted = (preassigned != nullptr) ? preassigned->Find(sourceId) : nullptr;
            if (wanted != nullptr && !scene.FindEntity(*wanted).IsAssigned())
            {
                live = scene.CreateEntity(*wanted, ename.AsView());
            }
            else
            {
                live = scene.CreateEntity(ename.AsView()); // fresh guid (collision-free mint)
            }
            scene.SetActive(live, active != 0);
            scene.SetLocalTransform(live, t);

            const Guid liveId = scene.GetEntityId(live);
            liveBySource.InsertOrAssign(sourceId, liveId);
            state->sourceIds.PushBack(sourceId);
            state->liveIds.PushBack(liveId);
            state->baselineTransforms.PushBack(t);
            sourceParents.PushBack(sourceParent);
            if (sourceParent == Guid{} && !firstRoot.IsAssigned())
            {
                firstRoot = live;
            }
        }
        ar.EndArray();
        if (!ar.IsOk() || !firstRoot.IsAssigned())
        {
            return EntityHandle::Invalid();
        }

        // A prefab is SINGLE-rooted (capture, tinting, and apply-to-prefab all walk one root's
        // subtree). A payload with more than one root is not the current format: refuse it
        // (the spawned entities are torn down) rather than guess where the extras belong.
        for (usize i = 0; i < sourceParents.Size(); ++i)
        {
            if (sourceParents[i] == Guid{} && scene.FindEntity(state->liveIds[i]) != firstRoot)
            {
                LOG_ERROR(u8"Scene", u8"prefab payload has more than one root - not the current "
                                     u8"single-root format; re-save the prefab");
                for (const Guid& live : state->liveIds)
                {
                    EntityHandle stale = scene.FindEntity(live);
                    if (stale.IsAssigned())
                    {
                        scene.DestroyEntity(stale); // immediate outside an update
                    }
                }
                return EntityHandle::Invalid();
            }
        }

        // Relink: payload-internal parents through the map; the instance root under `parent`.
        for (usize i = 0; i < state->sourceIds.Size(); ++i)
        {
            EntityHandle child = scene.FindEntity(state->liveIds[i]);
            if (!child.IsAssigned())
            {
                continue;
            }
            if (sourceParents[i] == Guid{})
            {
                if (parent.IsAssigned())
                {
                    scene.SetParent(child, parent);
                }
            }
            else if (const Guid* liveParent = liveBySource.Find(sourceParents[i]))
            {
                EntityHandle p = scene.FindEntity(*liveParent);
                if (p.IsAssigned())
                {
                    scene.SetParent(child, p);
                }
            }
        }

        // Components: route to remapped owners, then capture each as a spawn-time baseline.
        // Binary payloads carry BLOB records: the blob applies to the live component and
        // unknown types SKIP instead of failing.
        u32 componentCount = 0;
        ar.Key("components");
        ar.BeginArray(componentCount);
        for (u32 i = 0; i < componentCount; ++i)
        {
            if (text)
            {
                ar.BeginObject();
                Guid sourceOwner;
                String typeId;
                detail::SerializeGuid(ar, "owner", sourceOwner);
                foundation::core::Serialize(ar, "type", typeId);
                const Guid* liveId = liveBySource.Find(sourceOwner);
                EntityHandle owner =
                    (liveId != nullptr) ? scene.FindEntity(*liveId) : EntityHandle::Invalid();
                ComponentManagerBase* manager = scene.FindManagerBySerializationId(typeId.AsView());
                if (owner.IsAssigned() && manager != nullptr)
                {
                    ar.Key("data");
                    ar.BeginObject();
                    manager->ReadComponent(ar, owner);
                    ar.EndObject();
                    RemapPrefabEntityRefs(*manager, owner, liveBySource);
                    Scene::PrefabComponentBaseline baseline;
                    baseline.sourceEntity = sourceOwner;
                    baseline.typeId = typeId;
                    detail::ComponentToBlob(*manager, owner, baseline.blob);
                    state->componentBaselines.PushBack(
                        static_cast<Scene::PrefabComponentBaseline&&>(baseline));
                }
                else
                {
                    LOG_WARNING(
                        u8"Scene", u8"prefab component record '{}' skipped (no owner/manager)",
                        typeId);
                }
                ar.EndObject();
                continue;
            }
            Guid sourceOwner;
            String typeId;
            detail::SerializeGuid(ar, "owner", sourceOwner);
            foundation::core::Serialize(ar, "type", typeId);
            const Guid* liveId = liveBySource.Find(sourceOwner);
            EntityHandle owner =
                (liveId != nullptr) ? scene.FindEntity(*liveId) : EntityHandle::Invalid();
            ComponentManagerBase* manager = scene.FindManagerBySerializationId(typeId.AsView());
            Array<u8> blob;
            foundation::core::Serialize(ar, "data", blob);
            if (!owner.IsAssigned() || manager == nullptr)
            {
                LOG_WARNING(
                    u8"Scene", u8"prefab component record '{}' skipped (no owner/manager)",
                    typeId);
                continue;
            }
            detail::ComponentFromBlob(*manager, owner,
                                      Span<const u8>{blob.Data(), blob.Size()});
            RemapPrefabEntityRefs(*manager, owner, liveBySource);
            // Baseline = RE-serialized from the live component, NOT the payload bytes: a
            // component data-version bump would otherwise read as a phantom override on
            // every instance (old-version blob != current-version blob for equal state).
            Scene::PrefabComponentBaseline baseline;
            baseline.sourceEntity = sourceOwner;
            baseline.typeId = typeId;
            detail::ComponentToBlob(*manager, owner, baseline.blob);
            state->componentBaselines.PushBack(
                static_cast<Scene::PrefabComponentBaseline&&>(baseline));
        }
        ar.EndArray();
        if (!ar.IsOk())
        {
            return EntityHandle::Invalid();
        }

        const Guid rootGuid = scene.GetEntityId(firstRoot);
        state->rootEntityId = rootGuid;
        state->referencedPrefabIds.PushBack(prefabId);
        Scene::PrefabInstanceState* ownState = state.Get();
        scene.AddPrefabInstance(static_cast<UniquePtr<Scene::PrefabInstanceState>&&>(state));

        // ---- nested records ----
        // The trailing section: an EMPTY settings section (a prefab is a subtree template, not
        // a world), the Referenced3 mode tag, then the nested-instance records. Anything else
        // is not the current payload shape: the nested instances are skipped LOUDLY (the root
        // subtree above is already live).
        if (!spawnNested)
        {
            return firstRoot;
        }
        u32 settingsCount = 0;
        ar.Key("systemSettings");
        ar.BeginArray(settingsCount);
        ar.EndArray();
        u8 sectionMode = 0;
        foundation::core::Serialize(ar, "prefabMode", sectionMode);
        if (!ar.IsOk() || settingsCount != 0 || sectionMode != detail::kPrefabWireReferenced3)
        {
            LOG_ERROR(u8"Scene", u8"prefab payload's nested-instance section is not in the "
                                 u8"current format - nested instances skipped; re-save the prefab");
            return firstRoot;
        }

        u32 recordCount = 0;
        ar.Key("prefabInstances");
        ar.BeginArray(recordCount);
        Array<UniquePtr<Scene::PendingPrefabInstance>> records;
        for (u32 n = 0; n < recordCount && ar.IsOk(); ++n)
        {
            auto record = MakeUnique<Scene::PendingPrefabInstance>(scene.Allocator());
            detail::ReadPrefabRecord(ar, scene, *record, text);
            records.PushBack(static_cast<UniquePtr<Scene::PendingPrefabInstance>&&>(record));
        }
        ar.EndArray();
        if (!ar.IsOk())
        {
            return firstRoot;
        }

        // Owner-namespace id -> live guid: the payload's own entities, then each spawned
        // record's members (records were saved in registration order, so parents precede).
        HashMap<Guid, Guid> ownerNsToLive;
        for (const auto& kv : liveBySource)
        {
            ownerNsToLive.InsertOrAssign(kv.key, kv.value);
        }

        Array<detail::SiblingOrderFix> recordOrder; // targets in owner namespace until resolved
        for (auto& recordPtr : records)
        {
            Scene::PendingPrefabInstance& r = *recordPtr;
            UniquePtr<IStream> childPayload =
                (resolver != nullptr && *resolver) ? (*resolver)(r.prefabId) : UniquePtr<IStream>{};
            if (childPayload.Get() == nullptr)
            {
                LOG_WARNING(u8"Scene", u8"nested prefab record skipped - payload did not "
                                                u8"resolve (no resolver or missing asset)");
                continue;
            }

            // The scene's saved sub-record for THIS nested instance, if any.
            const Scene::PendingPrefabInstance* sub = nullptr;
            if (nestedSceneDeltas != nullptr)
            {
                for (const Scene::PendingPrefabInstance* candidate : *nestedSceneDeltas)
                {
                    if (candidate != nullptr && candidate->nestedRootSourceId == r.rootLiveId)
                    {
                        sub = candidate;
                        break;
                    }
                }
            }

            HashMap<Guid, Guid> childPreassigned;
            if (sub != nullptr)
            {
                for (usize i = 0; i < sub->sourceIds.Size() && i < sub->liveIds.Size(); ++i)
                {
                    childPreassigned.InsertOrAssign(sub->sourceIds[i], sub->liveIds[i]);
                }
            }

            EntityHandle recordParent = firstRoot;
            if (const Guid* liveParent = ownerNsToLive.Find(r.parentEntityId))
            {
                EntityHandle p = scene.FindEntity(*liveParent);
                if (p.IsAssigned())
                {
                    recordParent = p;
                }
            }

            EntityHandle child = SpawnPrefab(scene, *childPayload, r.prefabId, recordParent,
                                             &childPreassigned, nullptr, nullptr,
                                             /*spawnNested=*/false);
            if (!child.IsAssigned())
            {
                continue;
            }
            const Guid childRootGuid = scene.GetEntityId(child);
            Scene::PrefabInstanceState* childState = scene.FindPrefabInstanceByRoot(childRootGuid);
            if (childState == nullptr)
            {
                continue;
            }
            childState->ownerRootEntityId = rootGuid;
            childState->nestedRootSourceId = r.rootLiveId;

            // Owner customization -> then it BECOMES the baseline; scene-level deltas stay
            // overrides on top.
            scene.SetLocalTransform(child, r.rootTransform);
            detail::ApplyPendingDeltas(scene, childState, r);
            detail::RecaptureBaselines(scene, *childState);
            if (sub != nullptr)
            {
                if (sub->applyPlacement)
                {
                    if (sub->parentEntityId != Guid{})
                    {
                        EntityHandle sceneParent = scene.FindEntity(sub->parentEntityId);
                        if (sceneParent.IsAssigned())
                        {
                            scene.SetParent(child, sceneParent);
                        }
                    }
                    scene.SetLocalTransform(child, sub->rootTransform);
                }
                detail::ApplyPendingDeltas(scene, childState, *sub);
            }

            recordOrder.PushBack(detail::SiblingOrderFix{childRootGuid, r.nextSiblingId});
            ownState->referencedPrefabIds.PushBack(r.prefabId);
            for (usize i = 0; i < r.sourceIds.Size() && i < r.liveIds.Size(); ++i)
            {
                for (usize k = 0; k < childState->sourceIds.Size(); ++k)
                {
                    if (childState->sourceIds[k] == r.sourceIds[i])
                    {
                        ownerNsToLive.InsertOrAssign(r.liveIds[i], childState->liveIds[k]);
                        break;
                    }
                }
            }
        }

        // Records spawned APPENDED after the plain members - restore the captured sibling
        // order (targets resolve through the owner-namespace map, which is complete now).
        for (detail::SiblingOrderFix& fix : recordOrder)
        {
            const Guid* live = ownerNsToLive.Find(fix.nextSibling);
            fix.nextSibling = (live != nullptr) ? *live : Guid{};
        }
        detail::RestoreSiblingOrder(scene, recordOrder);
        return firstRoot;
    }

    bool FindPrefabMember(Scene& scene, const Guid& entityId, PrefabMemberInfo& out)
    {
        bool found = false;
        scene.ForEachPrefabInstance(
            [&](Scene::PrefabInstanceState& state)
            {
                if (found)
                {
                    return;
                }
                for (usize i = 0; i < state.liveIds.Size(); ++i)
                {
                    if (state.liveIds[i] == entityId)
                    {
                        out.state = &state;
                        out.memberIndex = i;
                        found = true;
                        return;
                    }
                }
            });
        return found;
    }

    const Scene::PrefabComponentBaseline*
    FindPrefabBaseline(const Scene::PrefabInstanceState& state, const Guid& sourceId,
                       StringView typeId)
    {
        for (const Scene::PrefabComponentBaseline& b : state.componentBaselines)
        {
            if (b.sourceEntity == sourceId && b.typeId.AsView() == typeId)
            {
                return &b;
            }
        }
        return nullptr;
    }

    bool IsPrefabComponentOverridden(Scene& scene, const PrefabMemberInfo& member,
                                     ComponentManagerBase& manager)
    {
        const Guid sourceId = member.state->sourceIds[member.memberIndex];
        const Scene::PrefabComponentBaseline* baseline =
            FindPrefabBaseline(*member.state, sourceId, manager.SerializationTypeId());
        EntityHandle live = scene.FindEntity(member.state->liveIds[member.memberIndex]);
        const bool has = live.IsAssigned() && manager.HasComponent(live);
        if (!has)
        {
            return baseline != nullptr;
        }
        if (baseline == nullptr)
        {
            return true;
        }
        Array<u8> blob;
        detail::ComponentToBlob(manager, live, blob);
        return !detail::BlobsEqual(Span<const u8>{blob.Data(), blob.Size()},
                                   Span<const u8>{baseline->blob.Data(), baseline->blob.Size()});
    }

    UniquePtr<Scene::PendingPrefabInstance>
    ComputeInstanceDeltasVsTemplate(Scene& scene, Scene::PrefabInstanceState& state,
                                    IStream& templatePayload)
    {
        detail::SceneStreamReader reader;
        const bool text =
            detail::DetectSceneStreamEncoding(templatePayload) == detail::SceneStreamEncoding::Text;
        u32 streamVersion = detail::kSceneStreamVersion;
        if (!text)
        {
            streamVersion = detail::ReadSceneStreamVersion(templatePayload);
            if (streamVersion != detail::kSceneStreamVersion)
            {
                LOG_ERROR(u8"Scene", u8"apply-to-prefab: the child template is not in the current "
                                     u8"format - falling back to the baseline diff (re-save it)");
                return detail::ComputeInstanceDeltas(scene, state);
            }
            (void)templatePayload.Seek(0, SeekOrigin::Begin); // reader re-consumes the header
        }
        Serializer* opened = reader.Open(templatePayload);
        if (opened == nullptr)
        {
            return detail::ComputeInstanceDeltas(scene, state);
        }
        Serializer& ar = *opened;
        if (!text)
        {
            (void)detail::ReadSceneStreamVersion(templatePayload); // advance past the header
        }
        else
        {
            u32 magic = 0;
            foundation::core::Serialize(ar, "magic", magic);
            foundation::core::Serialize(ar, "version", streamVersion);
        }

        String name;
        foundation::core::Serialize(ar, "name", name);

        HashMap<Guid, Transform> templateTransforms;
        u32 entityCount = 0;
        ar.Key("entities");
        ar.BeginArray(entityCount);
        for (u32 i = 0; i < entityCount; ++i)
        {
            Guid id;
            String ename;
            u8 active = 0;
            Guid parentId;
            Transform t;
            detail::SerializeGuid(ar, "id", id);
            foundation::core::Serialize(ar, "name", ename);
            foundation::core::Serialize(ar, "active", active);
            detail::SerializeGuid(ar, "parent", parentId);
            detail::SerializeTransform(ar, t);
            templateTransforms.InsertOrAssign(id, t);
        }
        ar.EndArray();

        struct TemplateBlob
        {
            Guid source;
            String typeId;
            Array<u8> blob;
        };
        Array<TemplateBlob> templateBlobs;
        u32 componentCount = 0;
        ar.Key("components");
        ar.BeginArray(componentCount);
        EntityHandle scratch = EntityHandle::Invalid(); // text reads need a manager to re-blob
        for (u32 i = 0; i < componentCount && ar.IsOk(); ++i)
        {
            TemplateBlob record;
            if (text)
            {
                ar.BeginObject();
                detail::SerializeGuid(ar, "owner", record.source);
                foundation::core::Serialize(ar, "type", record.typeId);
                ComponentManagerBase* manager =
                    scene.FindManagerBySerializationId(record.typeId.AsView());
                if (manager != nullptr)
                {
                    if (!scratch.IsAssigned())
                    {
                        scratch = scene.CreateEntity(u8"__template_read");
                    }
                    ar.Key("data");
                    ar.BeginObject();
                    manager->ReadComponent(ar, scratch);
                    ar.EndObject();
                    detail::ComponentToBlob(*manager, scratch, record.blob);
                    manager->RemoveComponent(scratch);
                    templateBlobs.PushBack(static_cast<TemplateBlob&&>(record));
                }
                ar.EndObject();
                continue;
            }
            detail::SerializeGuid(ar, "owner", record.source);
            foundation::core::Serialize(ar, "type", record.typeId);
            foundation::core::Serialize(ar, "data", record.blob);
            templateBlobs.PushBack(static_cast<TemplateBlob&&>(record));
        }
        ar.EndArray();
        if (scratch.IsAssigned())
        {
            scene.DestroyEntity(scratch);
        }
        if (!ar.IsOk())
        {
            return detail::ComputeInstanceDeltas(scene, state);
        }

        auto pending = MakeUnique<Scene::PendingPrefabInstance>(scene.Allocator());
        pending->prefabId = state.prefabId;
        EntityHandle liveRoot = scene.FindEntity(state.rootEntityId);
        EntityHandle parent =
            liveRoot.IsAssigned() ? scene.GetParent(liveRoot) : EntityHandle::Invalid();
        pending->parentEntityId = parent.IsAssigned() ? scene.GetEntityId(parent) : Guid{};
        pending->rootTransform =
            liveRoot.IsAssigned() ? scene.GetLocalTransform(liveRoot) : Transform{};
        pending->sourceIds = state.sourceIds;
        pending->liveIds = state.liveIds;
        pending->rootLiveId = state.rootEntityId;
        EntityHandle liveNext =
            liveRoot.IsAssigned() ? scene.GetNextSibling(liveRoot) : EntityHandle::Invalid();
        pending->nextSiblingId = liveNext.IsAssigned() ? scene.GetEntityId(liveNext) : Guid{};
        for (usize i = 0; i < state.liveIds.Size(); ++i)
        {
            if (state.liveIds[i] == state.rootEntityId)
            {
                const Transform* templateRoot = templateTransforms.Find(state.sourceIds[i]);
                if (liveRoot.IsAssigned() && templateRoot != nullptr)
                {
                    pending->applyPlacement =
                        !detail::TransformsEqual(pending->rootTransform, *templateRoot);
                }
                break;
            }
        }

        for (usize i = 0; i < state.sourceIds.Size(); ++i)
        {
            EntityHandle live = scene.FindEntity(state.liveIds[i]);
            if (!live.IsAssigned())
            {
                pending->destroyedMembers.PushBack(state.sourceIds[i]);
                continue;
            }
            if (state.liveIds[i] != state.rootEntityId)
            {
                const Transform* baseline = templateTransforms.Find(state.sourceIds[i]);
                Transform t = scene.GetLocalTransform(live);
                if (baseline == nullptr || !detail::TransformsEqual(t, *baseline))
                {
                    pending->overrideTransformIds.PushBack(state.sourceIds[i]);
                    pending->overrideTransforms.PushBack(t);
                }
            }
            scene.ForEachManager(
                [&](ComponentManagerBase& m)
                {
                    if (!m.IsSerializable())
                    {
                        return;
                    }
                    const TemplateBlob* baseline = nullptr;
                    for (const TemplateBlob& b : templateBlobs)
                    {
                        if (b.source == state.sourceIds[i] &&
                            b.typeId.AsView() == m.SerializationTypeId())
                        {
                            baseline = &b;
                            break;
                        }
                    }
                    Scene::PendingPrefabComponentOp op;
                    op.sourceEntity = state.sourceIds[i];
                    op.typeId = String(m.SerializationTypeId());
                    if (m.HasComponent(live))
                    {
                        Array<u8> blob;
                        detail::ComponentToBlob(m, live, blob);
                        if (baseline == nullptr)
                        {
                            op.op = 1u;
                            op.blob = static_cast<Array<u8>&&>(blob);
                        }
                        else if (!detail::BlobsEqual(
                                     Span<const u8>{blob.Data(), blob.Size()},
                                     Span<const u8>{baseline->blob.Data(), baseline->blob.Size()}))
                        {
                            op.op = 0u;
                            op.blob = static_cast<Array<u8>&&>(blob);
                        }
                        else
                        {
                            return;
                        }
                    }
                    else if (baseline != nullptr)
                    {
                        op.op = 2u;
                    }
                    else
                    {
                        return;
                    }
                    pending->componentOps.PushBack(
                        static_cast<Scene::PendingPrefabComponentOp&&>(op));
                });
        }
        return pending;
    }

    Status CaptureInstanceAsTemplate(Scene& scene, Scene::PrefabInstanceState& state, IStream& out,
                                     const PrefabPayloadResolver* resolver,
                                     detail::SceneStreamEncoding encoding)
    {
        if (encoding == detail::SceneStreamEncoding::Binary)
        {
            BinarySerializer ar(out, SerializeMode::Write);
            return detail::CaptureInstanceAsTemplateBody(ar, false, scene, state, resolver);
        }
        foundation::xml::XmlSerializer ar(scene.Allocator());
        const Status body = detail::CaptureInstanceAsTemplateBody(ar, true, scene, state, resolver);
        if (!body.IsOk())
        {
            return body;
        }
        String textOut;
        ar.GetOutput(textOut);
        return (out.Write(reinterpret_cast<const byte*>(textOut.CStr()), textOut.Size()) ==
                textOut.Size())
                   ? Status{}
                   : Status{ErrorCode::Unknown};
    }

    bool RevertPrefabInstance(Scene& scene, const Guid& rootEntityId, Span<const byte> payload,
                              const PrefabPayloadResolver* resolver)
    {
        Scene::PrefabInstanceState* state = scene.FindPrefabInstanceByRoot(rootEntityId);
        if (state == nullptr)
        {
            return false;
        }
        EntityHandle root = scene.FindEntity(rootEntityId);
        if (!root.IsAssigned())
        {
            return false;
        }
        const Guid prefabId = state->prefabId;
        EntityHandle parentHandle = scene.GetParent(root);
        const Guid parentId = parentHandle.IsAssigned() ? scene.GetEntityId(parentHandle) : Guid{};
        const Transform placement = scene.GetLocalTransform(root);
        EntityHandle nextSibling = scene.GetNextSibling(root);
        const Guid nextSiblingId =
            nextSibling.IsAssigned() ? scene.GetEntityId(nextSibling) : Guid{};
        HashMap<Guid, Guid> preassigned;
        for (usize i = 0; i < state->sourceIds.Size(); ++i)
        {
            preassigned.InsertOrAssign(state->sourceIds[i], state->liveIds[i]);
        }
        Array<Guid> liveIds = state->liveIds;

        // Nested instances revert with the owner: keep their member GUIDS (cross-references
        // stay valid) but none of their deltas, and let the template's placement win.
        Array<Scene::PrefabInstanceState*> contained;
        HashMap<Guid, u8> nestedMembers;
        detail::CollectContainedInstances(scene, root, contained, nestedMembers);
        Array<UniquePtr<Scene::PendingPrefabInstance>> subs;
        Array<Guid> subRoots;
        for (Scene::PrefabInstanceState* nested : contained)
        {
            auto sub = MakeUnique<Scene::PendingPrefabInstance>(scene.Allocator());
            sub->prefabId = nested->prefabId;
            sub->sourceIds = nested->sourceIds;
            sub->liveIds = nested->liveIds;
            sub->nestedRootSourceId = nested->nestedRootSourceId.IsNil()
                                          ? nested->rootEntityId
                                          : nested->nestedRootSourceId;
            sub->applyPlacement = false;
            subs.PushBack(static_cast<UniquePtr<Scene::PendingPrefabInstance>&&>(sub));
            subRoots.PushBack(nested->rootEntityId);
            for (const Guid& live : nested->liveIds)
            {
                EntityHandle e = scene.FindEntity(live);
                if (e.IsAssigned())
                {
                    scene.DestroyEntity(e);
                }
            }
        }
        for (const Guid& subRoot : subRoots)
        {
            scene.RemovePrefabInstance(subRoot);
        }

        for (const Guid& live : liveIds)
        {
            EntityHandle e = scene.FindEntity(live);
            if (e.IsAssigned())
            {
                scene.DestroyEntity(e);
            }
        }
        scene.RemovePrefabInstance(rootEntityId);

        Array<const Scene::PendingPrefabInstance*> subPtrs;
        for (const auto& sub : subs)
        {
            subPtrs.PushBack(sub.Get());
        }

        MemoryStream stream;
        (void)stream.Write(payload.Data(), payload.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        EntityHandle parent =
            (parentId != Guid{}) ? scene.FindEntity(parentId) : EntityHandle::Invalid();
        EntityHandle spawned =
            SpawnPrefab(scene, stream, prefabId, parent, &preassigned, resolver, &subPtrs);
        if (!spawned.IsAssigned())
        {
            return false;
        }
        scene.SetLocalTransform(spawned, placement);
        Array<detail::SiblingOrderFix> order;
        order.PushBack(detail::SiblingOrderFix{scene.GetEntityId(spawned), nextSiblingId});
        detail::RestoreSiblingOrder(scene, order);
        return true;
    }

    void ResolveScenePrefabs(Scene& scene, const PrefabPayloadResolver& resolver)
    {
        Array<UniquePtr<Scene::PendingPrefabInstance>> pendings =
            scene.TakePendingPrefabInstances();
        Array<detail::SiblingOrderFix> sceneOrder; // targets are live scene guids

        // Nested records (owner set) don't spawn on their own - their OWNER's spawn consumes
        // them (preserved guids + scene-level deltas layered over the owner customization).
        for (auto& p : pendings)
        {
            if (!p->ownerRootEntityId.IsNil())
            {
                continue;
            }

            UniquePtr<IStream> payload = resolver ? resolver(p->prefabId) : UniquePtr<IStream>{};
            if (payload.Get() == nullptr)
            {
                LOG_WARNING(
                    u8"Scene",
                    u8"prefab instance skipped - payload for its prefab did not resolve");
                continue;
            }
            HashMap<Guid, Guid> preassigned;
            for (usize i = 0; i < p->sourceIds.Size() && i < p->liveIds.Size(); ++i)
            {
                preassigned.InsertOrAssign(p->sourceIds[i], p->liveIds[i]);
            }
            Array<const Scene::PendingPrefabInstance*> subPtrs;
            for (const auto& candidate : pendings)
            {
                if (candidate->ownerRootEntityId == p->rootLiveId &&
                    !candidate->ownerRootEntityId.IsNil())
                {
                    subPtrs.PushBack(candidate.Get());
                }
            }
            EntityHandle parent = (p->parentEntityId != Guid{})
                                      ? scene.FindEntity(p->parentEntityId)
                                      : EntityHandle::Invalid();
            EntityHandle root = SpawnPrefab(scene, *payload, p->prefabId, parent, &preassigned,
                                            &resolver, &subPtrs);
            if (!root.IsAssigned())
            {
                continue;
            }
            scene.SetLocalTransform(root, p->rootTransform);
            detail::ApplyPendingDeltas(scene,
                                       scene.FindPrefabInstanceByRoot(scene.GetEntityId(root)), *p);
            sceneOrder.PushBack(detail::SiblingOrderFix{scene.GetEntityId(root), p->nextSiblingId});
        }
        detail::RestoreSiblingOrder(scene, sceneOrder);

        // Orphaned nested records (their owner record vanished): spawn standalone so the
        // entities aren't silently lost - they become plain top-level instances.
        for (auto& p : pendings)
        {
            if (p->ownerRootEntityId.IsNil())
            {
                continue;
            }
            if (scene.FindEntity(p->rootLiveId).IsAssigned())
            {
                continue;
            } // owner spawned it
            UniquePtr<IStream> payload = resolver ? resolver(p->prefabId) : UniquePtr<IStream>{};
            if (payload.Get() == nullptr)
            {
                continue;
            }
            LOG_WARNING(u8"Scene",
                                 u8"nested prefab record lost its owner - spawning standalone");
            HashMap<Guid, Guid> preassigned;
            for (usize i = 0; i < p->sourceIds.Size() && i < p->liveIds.Size(); ++i)
            {
                preassigned.InsertOrAssign(p->sourceIds[i], p->liveIds[i]);
            }
            EntityHandle parent = (p->parentEntityId != Guid{})
                                      ? scene.FindEntity(p->parentEntityId)
                                      : EntityHandle::Invalid();
            EntityHandle root =
                SpawnPrefab(scene, *payload, p->prefabId, parent, &preassigned, &resolver, nullptr);
            if (!root.IsAssigned())
            {
                continue;
            }
            scene.SetLocalTransform(root, p->rootTransform);
            detail::ApplyPendingDeltas(scene,
                                       scene.FindPrefabInstanceByRoot(scene.GetEntityId(root)), *p);
        }
    }

    u32 RebuildPrefabInstances(Scene& scene, const Guid& prefabId, Span<const byte> payload,
                               const PrefabPayloadResolver* resolver)
    {
        struct RescuedChild
        {
            Guid child;
            Guid parentLiveId;
        };
        struct Item
        {
            UniquePtr<Scene::PendingPrefabInstance> own;
            Array<UniquePtr<Scene::PendingPrefabInstance>> subs;
            Array<Guid> subRoots;
            Array<RescuedChild> rescued;
            Guid root;
        };

        // Snapshot phase: nothing is destroyed until every affected instance's deltas (and its
        // nested instances') are captured.
        Array<Item> items;
        HashMap<Guid, u8> absorbedRoots; // nested/contained roots handled via an owner item
        scene.ForEachPrefabInstance(
            [&](Scene::PrefabInstanceState& state)
            {
                if (!state.ownerRootEntityId.IsNil())
                {
                    return;
                } // rebuilds ride their owner
                bool affected = state.prefabId == prefabId;
                if (!affected)
                {
                    for (const Guid& referenced : state.referencedPrefabIds)
                    {
                        if (referenced == prefabId)
                        {
                            affected = true;
                            break;
                        }
                    }
                }
                if (!affected || absorbedRoots.Find(state.rootEntityId) != nullptr)
                {
                    return;
                }
                EntityHandle root = scene.FindEntity(state.rootEntityId);
                if (!root.IsAssigned())
                {
                    return;
                }

                Item item;
                item.root = state.rootEntityId;
                item.own = detail::ComputeInstanceDeltas(scene, state);

                Array<Scene::PrefabInstanceState*> contained;
                HashMap<Guid, u8> nestedMembers;
                detail::CollectContainedInstances(scene, root, contained, nestedMembers);
                for (Scene::PrefabInstanceState* nested : contained)
                {
                    auto sub = detail::ComputeInstanceDeltas(scene, *nested);
                    sub->nestedRootSourceId = nested->nestedRootSourceId.IsNil()
                                                  ? nested->rootEntityId
                                                  : nested->nestedRootSourceId;
                    item.subs.PushBack(static_cast<UniquePtr<Scene::PendingPrefabInstance>&&>(sub));
                    item.subRoots.PushBack(nested->rootEntityId);
                    absorbedRoots.InsertOrAssign(nested->rootEntityId, 1u);
                }

                // Plain user entities parented under members: detach now, re-attach post-respawn
                // (destroying a member destroys its whole subtree). Keyed by the member's LIVE
                // guid - preassignment preserves member guids through the respawn for OWN and
                // NESTED members alike, so one path rescues children of both. (The old
                // source-id mapping only covered the owner's members; a child under a nested
                // sub-instance member was skipped and died with the teardown.)
                HashMap<Guid, u8> allMembers;
                for (const Guid& live : state.liveIds)
                {
                    allMembers.InsertOrAssign(live, 1u);
                }
                for (const auto& kv : nestedMembers)
                {
                    allMembers.InsertOrAssign(kv.key, 1u);
                }
                for (const auto& kv : allMembers)
                {
                    EntityHandle member = scene.FindEntity(kv.key);
                    if (!member.IsAssigned())
                    {
                        continue;
                    }
                    Array<EntityHandle> kids;
                    for (EntityHandle c = scene.GetFirstChild(member); c.IsAssigned();
                         c = scene.GetNextSibling(c))
                    {
                        kids.PushBack(c);
                    }
                    for (EntityHandle child : kids)
                    {
                        const Guid childGuid = scene.GetEntityId(child);
                        if (allMembers.Find(childGuid) != nullptr)
                        {
                            continue;
                        }
                        item.rescued.PushBack(RescuedChild{childGuid, kv.key});
                        scene.SetParent(child, EntityHandle::Invalid(), true);
                    }
                }
                items.PushBack(static_cast<Item&&>(item));
            });

        u32 rebuilt = 0;
        Array<detail::SiblingOrderFix> rebuildOrder; // live-guid targets captured pre-teardown
        for (Item& item : items)
        {
            Scene::PendingPrefabInstance& p = *item.own;
            for (usize n = 0; n < item.subs.Size(); ++n)
            {
                for (const Guid& live : item.subs[n]->liveIds)
                {
                    EntityHandle e = scene.FindEntity(live);
                    if (e.IsAssigned())
                    {
                        scene.DestroyEntity(e);
                    }
                }
            }
            for (const Guid& subRoot : item.subRoots)
            {
                scene.RemovePrefabInstance(subRoot);
            }
            for (const Guid& live : p.liveIds)
            {
                EntityHandle e = scene.FindEntity(live);
                if (e.IsAssigned())
                {
                    scene.DestroyEntity(e);
                }
            }
            scene.RemovePrefabInstance(item.root);

            // This instance's template bytes: the changed payload when it IS the changed prefab,
            // else its own template via the resolver.
            MemoryStream stream;
            if (p.prefabId == prefabId)
            {
                (void)stream.Write(payload.Data(), payload.Size());
            }
            else
            {
                UniquePtr<IStream> own = (resolver != nullptr && *resolver)
                                             ? (*resolver)(p.prefabId)
                                             : UniquePtr<IStream>{};
                if (own.Get() == nullptr)
                {
                    LOG_WARNING(u8"Scene",
                                         u8"instance referencing the changed prefab could not "
                                         u8"rebuild - its own template did not resolve");
                    continue;
                }
                Array<byte> bytes;
                bytes.Resize(static_cast<usize>(own->Size()));
                (void)own->Read(bytes.Data(), bytes.Size());
                (void)stream.Write(bytes.Data(), bytes.Size());
            }
            (void)stream.Seek(0, SeekOrigin::Begin);

            HashMap<Guid, Guid> preassigned;
            for (usize i = 0; i < p.sourceIds.Size() && i < p.liveIds.Size(); ++i)
            {
                preassigned.InsertOrAssign(p.sourceIds[i], p.liveIds[i]);
            }
            Array<const Scene::PendingPrefabInstance*> subPtrs;
            for (const auto& sub : item.subs)
            {
                subPtrs.PushBack(sub.Get());
            }

            EntityHandle parent = (p.parentEntityId != Guid{}) ? scene.FindEntity(p.parentEntityId)
                                                               : EntityHandle::Invalid();
            EntityHandle root =
                SpawnPrefab(scene, stream, p.prefabId, parent, &preassigned, resolver, &subPtrs);
            if (!root.IsAssigned())
            {
                continue;
            }
            rebuildOrder.PushBack(
                detail::SiblingOrderFix{scene.GetEntityId(root), p.nextSiblingId});
            scene.SetLocalTransform(root, p.rootTransform);
            Scene::PrefabInstanceState* newState =
                scene.FindPrefabInstanceByRoot(scene.GetEntityId(root));
            detail::ApplyPendingDeltas(scene, newState, p);

            // Contained instances the template does NOT record (user-spawned inside): respawn
            // standalone so a template edit never eats user content.
            for (auto& sub : item.subs)
            {
                if (scene.FindEntity(sub->rootLiveId).IsAssigned())
                {
                    continue;
                } // record consumed it
                UniquePtr<IStream> subPayload = (resolver != nullptr && *resolver)
                                                    ? (*resolver)(sub->prefabId)
                                                    : UniquePtr<IStream>{};
                if (subPayload.Get() == nullptr)
                {
                    continue;
                }
                HashMap<Guid, Guid> subPreassigned;
                for (usize i = 0; i < sub->sourceIds.Size() && i < sub->liveIds.Size(); ++i)
                {
                    subPreassigned.InsertOrAssign(sub->sourceIds[i], sub->liveIds[i]);
                }
                EntityHandle subParent = (sub->parentEntityId != Guid{})
                                             ? scene.FindEntity(sub->parentEntityId)
                                             : EntityHandle::Invalid();
                EntityHandle subRoot = SpawnPrefab(scene, *subPayload, sub->prefabId, subParent,
                                                   &subPreassigned, resolver, nullptr);
                if (!subRoot.IsAssigned())
                {
                    continue;
                }
                rebuildOrder.PushBack(
                    detail::SiblingOrderFix{scene.GetEntityId(subRoot), sub->nextSiblingId});
                scene.SetLocalTransform(subRoot, sub->rootTransform);
                detail::ApplyPendingDeltas(
                    scene, scene.FindPrefabInstanceByRoot(scene.GetEntityId(subRoot)), *sub);
            }

            // Re-attach rescued user children: the member guid survived the respawn
            // (preassigned), so a plain lookup finds the new parent. A member the template
            // no longer has falls back to the instance root - the child stays with the
            // instance instead of being orphaned at scene root (or destroyed, pre-fix).
            for (const RescuedChild& rescue : item.rescued)
            {
                EntityHandle child = scene.FindEntity(rescue.child);
                if (!child.IsAssigned())
                {
                    continue;
                }
                EntityHandle member = scene.FindEntity(rescue.parentLiveId);
                scene.SetParent(child, member.IsAssigned() ? member : root, true);
            }
            ++rebuilt;
        }
        detail::RestoreSiblingOrder(scene, rebuildOrder);
        return rebuilt;
    }

    Result<Array<byte>> TranscodeSceneStreamToBinary(IStream& in, Scene& scratch,
                                                     bool includeSettings)
    {
        if (detail::DetectSceneStreamEncoding(in) == detail::SceneStreamEncoding::Binary)
        {
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(in.Size() - in.Tell()));
            if (!bytes.IsEmpty() && in.Read(bytes.Data(), bytes.Size()) != bytes.Size())
            {
                return Err(ErrorCode::Unknown);
            }
            return bytes;
        }
        detail::SceneStreamReader reader;
        Serializer* ar = reader.Open(in);
        if (ar == nullptr)
        {
            return Err(ErrorCode::Internal);
        }
        SerializeScene(*ar, scratch, ScenePrefabMode::Referenced, true,
                       detail::SceneStreamEncoding::Text);
        if (!ar->IsOk())
        {
            return Err(ar->GetStatus().Code());
        }

        MemoryStream out;
        BinarySerializer writer(out, SerializeMode::Write);
        SerializeScene(writer, scratch, ScenePrefabMode::Referenced, includeSettings,
                       detail::SceneStreamEncoding::Binary);
        if (!writer.IsOk())
        {
            return Err(writer.GetStatus().Code());
        }
        Array<byte> bytes;
        const Span<const byte> view = out.Bytes();
        bytes.Reserve(view.Size());
        for (byte b : view)
        {
            bytes.PushBack(b);
        }
        return bytes;
    }

    Status LoadScene(foundation::content::Instance& instance, Scene& scene)
    {
        UniquePtr<IStream> stream = instance.ReadData(u8"scene");
        if (stream.Get() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        detail::SceneStreamReader reader;
        Serializer* ar = reader.Open(*stream);
        if (ar == nullptr)
        {
            return Status{ErrorCode::Internal};
        } // unparseable text stream
        SerializeScene(*ar, scene, ScenePrefabMode::Referenced, true, reader.Encoding());
        // The reader's verdict IS the load's: a refused payload (a stale data version, a retired
        // section layout, a short stream) leaves the scene partially filled, and every caller
        // treats not-ok as "did not load" rather than showing that partial scene as the asset.
        return ar->IsOk() ? Status{} : ar->GetStatus();
    }

    RTTI_DEFINE_OBJECT(SceneDocument, "rtti::scene")

    RTTI_DEFINE_OBJECT(PrefabDocument, "rtti::scene")

    void ResolveUnresolvedComponents(Scene& scene, ComponentManagerBase& manager)
    {
        Array<Scene::UnresolvedComponent> pending;
        scene.TakeUnresolvedComponents(manager.SerializationTypeId(), pending);
        for (Scene::UnresolvedComponent& record : pending)
        {
            const EntityHandle owner = scene.FindEntity(record.owner);
            if (!owner.IsAssigned())
            {
                continue; // its entity is gone - nothing to attach to
            }
            if (!record.text)
            {
                detail::ComponentFromBlob(manager, owner,
                                          Span<const u8>{record.payload.Data(),
                                                         record.payload.Size()});
                continue;
            }
            // TEXT: the captured <data> element, replayed through the XML serializer.
            String wrapped;
            wrapped += StringView(u8"<root>");
            wrapped += StringView(reinterpret_cast<const utf8char*>(record.payload.Data()),
                                  record.payload.Size());
            wrapped += StringView(u8"</root>");
            foundation::xml::XmlDocument doc(scene.Allocator());
            if (doc.Parse(wrapped.AsView()) != foundation::xml::XmlResult::Ok)
            {
                LOG_WARNING(u8"Scene", u8"unresolved component of type '{}' failed to re-parse",
                            record.typeId);
                continue;
            }
            foundation::xml::XmlSerializer ar(doc);
            ar.Key("data");
            ar.BeginObject();
            manager.ReadComponent(ar, owner);
            ar.EndObject();
        }
    }

    void ResolveUnresolvedSettings(Scene& scene, SceneSystem& system)
    {
        if (system.SettingsType() == nullptr)
        {
            return;
        }
        Scene::UnresolvedSettings record;
        if (!scene.TakeUnresolvedSettings(system.SettingsId(), record))
        {
            return;
        }
        if (!record.text)
        {
            MemoryStream buffer;
            (void)buffer.Write(reinterpret_cast<const byte*>(record.payload.Data()),
                               record.payload.Size());
            (void)buffer.Seek(0, SeekOrigin::Begin);
            BinarySerializer sub(buffer, SerializeMode::Read);
            foundation::core::BeginVersionedPayload(sub, *system.SettingsType());
            sub.Key("settings");
            sub.BeginObject();
            system.SerializeSettings(sub);
            sub.EndObject();
            foundation::core::EndVersionedPayload(sub);
            return;
        }
        String wrapped;
        wrapped += StringView(u8"<root>");
        wrapped += StringView(reinterpret_cast<const utf8char*>(record.payload.Data()),
                              record.payload.Size());
        wrapped += StringView(u8"</root>");
        foundation::xml::XmlDocument doc(scene.Allocator());
        if (doc.Parse(wrapped.AsView()) != foundation::xml::XmlResult::Ok)
        {
            LOG_WARNING(u8"Scene", u8"unresolved settings of system '{}' failed to re-parse",
                        record.systemId);
            return;
        }
        foundation::xml::XmlSerializer ar(doc);
        foundation::core::BeginVersionedPayload(ar, *system.SettingsType());
        ar.Key("settings");
        ar.BeginObject();
        system.SerializeSettings(ar);
        ar.EndObject();
        foundation::core::EndVersionedPayload(ar);
    }

    void ResolveAllUnresolvedRecords(Scene& scene)
    {
        if (!scene.UnresolvedComponents().IsEmpty())
        {
            scene.ForEachManager(
                [&](ComponentManagerBase& m)
                {
                    if (m.IsSerializable())
                    {
                        ResolveUnresolvedComponents(scene, m);
                    }
                });
        }
        if (!scene.UnresolvedSettingsRecords().IsEmpty())
        {
            scene.ForEachSystem([&](SceneSystem& sys) { ResolveUnresolvedSettings(scene, sys); });
        }
        // Prefab-instance ops of types that now have a manager: apply + drop.
        scene.ForEachPrefabInstance(
            [&](Scene::PrefabInstanceState& state)
            {
                for (usize i = state.unresolvedComponentOps.Size(); i-- > 0;)
                {
                    const Scene::PendingPrefabComponentOp& op = state.unresolvedComponentOps[i];
                    ComponentManagerBase* manager =
                        scene.FindManagerBySerializationId(op.typeId.AsView());
                    if (manager == nullptr)
                    {
                        continue;
                    }
                    EntityHandle live = EntityHandle::Invalid();
                    for (usize k = 0; k < state.sourceIds.Size(); ++k)
                    {
                        if (state.sourceIds[k] == op.sourceEntity)
                        {
                            live = scene.FindEntity(state.liveIds[k]);
                            break;
                        }
                    }
                    if (live.IsAssigned())
                    {
                        if (op.op == 2u)
                        {
                            if (manager->HasComponent(live))
                            {
                                manager->RemoveComponent(live);
                            }
                        }
                        else
                        {
                            detail::ComponentFromBlob(*manager, live,
                                                      Span<const u8>{op.blob.Data(),
                                                                     op.blob.Size()});
                            detail::RemapOverrideEntityRefs(*manager, live, state);
                        }
                    }
                    state.unresolvedComponentOps.RemoveAt(i);
                }
            });
    }
}
