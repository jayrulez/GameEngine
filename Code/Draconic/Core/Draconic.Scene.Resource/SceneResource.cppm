/// Draconic::SceneResource - the `draconic.scene.resource` module.
///
/// Whole-scene serialization (the runtime LOAD side; the editor save side is in
/// draconic.scene.editor). A scene is stored as one content-DB Instance: a small
/// SceneDocument primary (the name, so the instance materializes + is discoverable)
/// plus a "scene" data stream holding the serialized world.
///
/// SerializeScene is the bidirectional core: it runs the entity / transform / component
/// outer-join over an ISerializer. Entities persist by Guid (parent links + component
/// owners are stored as Guids and relinked on load). Components are serialized by their
/// owning manager (the typed manager is the serializer - value components carry no
/// vtable) and routed back on load by a stable string type id. The thin "component
/// type -> manager" routing is the only scene-specific registry; the managers
/// themselves already exist on the target scene (injected via ISceneAware), so load
/// deserializes into them.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.scene.resource;

import draconic.foundation;
import draconic.resource;
import draconic.content;
import draconic.scene;
import draconic.xml;
import draconic.xml.serialization;

using namespace draconic::foundation;

export namespace draconic::scene
{

    // Minimal primary object so a scene is a first-class content-DB instance (carries the
    // name for discovery; the heavy world data lives in the "scene" data stream).
    class SceneDocument final : public ISerializable
    {
        DRACONIC_OBJECT(SceneDocument, ISerializable)
    public:
        String name;
        void Serialize(ISerializer& ar) override { draconic::foundation::Serialize(ar, "name", name); }
    };

    namespace detail
    {
        // Scene-stream header (v2+): a magic sentinel no legacy stream can start with (a legacy
        // stream begins with the scene NAME's u32 length - always small), then the format
        // version. v2 adds length-prefixed component + system-settings records, so readers SKIP
        // unknown types instead of aborting (and template payloads can be sliced without a
        // live scene). v3 switches guids to the proper serializer form (canonical string in
        // text; the binary raw-16-bytes are BIT-IDENTICAL to the old hi/lo u64 pair) and
        // transform keys to their full names (position/rotation/scale, was pos/rot/scl).
        constexpr u32 kSceneStreamMagic = 0xD5C35CEEu;
        constexpr u32 kSceneStreamVersion = 3;

        // Writers use the current version (default). READERS pass the stream's sniffed header
        // version so v2 text saves (hi/lo guid fields, pos/rot/scl keys) still load; the next
        // save upgrades them. Binary is unaffected either way (keys are no-ops and the guid
        // bytes are identical), so the gate only ever matters on the XML path.
        void SerializeGuid(ISerializer& ar, const char* key, Guid& g,
                           u32 version = kSceneStreamVersion)
        {
            if (version >= 3)
            {
                draconic::foundation::Serialize(ar, key, g); // ISerializer::GuidValue
                return;
            }
            ar.Key(key);
            draconic::foundation::Serialize(ar, "hi", g.high);
            draconic::foundation::Serialize(ar, "lo", g.low);
        }

        // Peek the stream version. Leaves the stream positioned AFTER the header (v2+) or back
        // at the start (legacy v1 - no header).
        u32 ReadSceneStreamVersion(IStream& stream)
        {
            const i64 start = stream.Tell();
            u32 first = 0;
            if (stream.Read(&first, sizeof(first)) != sizeof(first) || first != kSceneStreamMagic)
            {
                (void)stream.Seek(start, SeekOrigin::Begin);
                return 1;
            }
            u32 version = 1;
            if (stream.Read(&version, sizeof(version)) != sizeof(version))
            {
                return 1;
            }
            return version;
        }

        // The scene stream's on-disk encoding. Sources are TEXT (XML - diffable, mergeable);
        // staged/cooked products and in-memory snapshots are BINARY. One SerializeScene path
        // feeds both encoders (docs/design/text-scenes.md).
        enum class SceneStreamEncoding : u8
        {
            Binary,
            Text
        };

        // First non-whitespace byte '<' = XML. Binary streams start with the magic or a name
        // length, never '<'. Leaves the stream where it found it.
        [[nodiscard]] inline SceneStreamEncoding DetectSceneStreamEncoding(IStream& stream)
        {
            const i64 start = stream.Tell();
            SceneStreamEncoding encoding = SceneStreamEncoding::Binary;
            u8 b = 0;
            while (stream.Read(&b, 1) == 1)
            {
                if (b == u8' ' || b == u8'\t' || b == u8'\r' || b == u8'\n')
                {
                    continue;
                }
                encoding = (b == u8'<') ? SceneStreamEncoding::Text : SceneStreamEncoding::Binary;
                break;
            }
            (void)stream.Seek(start, SeekOrigin::Begin);
            return encoding;
        }

        // Read-side seam: sniffs the encoding and owns whichever serializer (and, for text,
        // the parsed document) the stream needs. Open() returns null on an unparseable text
        // stream. For BINARY the stream is left at the start - SerializeScene's own
        // serializer-level sniff handles the header; for TEXT the header elements are read
        // the same way through the XML serializer.
        class SceneStreamReader
        {
        public:
            [[nodiscard]] Serializer* Open(IStream& stream)
            {
                m_encoding = DetectSceneStreamEncoding(stream);
                if (m_encoding == SceneStreamEncoding::Binary)
                {
                    m_binary = MakeUnique<BinarySerializer>(DefaultAllocator(), stream,
                                                            SerializeMode::Read);
                    return m_binary.Get();
                }
                Array<byte> bytes;
                bytes.Resize(static_cast<usize>(stream.Size() - stream.Tell()));
                if (!bytes.IsEmpty() && stream.Read(bytes.Data(), bytes.Size()) != bytes.Size())
                {
                    return nullptr;
                }
                const StringView text(reinterpret_cast<const utf8char*>(bytes.Data()),
                                      bytes.Size());
                if (m_doc.Parse(text) != draconic::xml::XmlResult::Ok)
                {
                    return nullptr;
                }
                m_xml = MakeUnique<draconic::xml::XmlSerializer>(DefaultAllocator(), m_doc);
                return m_xml.Get();
            }
            [[nodiscard]] SceneStreamEncoding Encoding() const noexcept { return m_encoding; }

        private:
            SceneStreamEncoding m_encoding = SceneStreamEncoding::Binary;
            draconic::xml::XmlDocument m_doc;
            UniquePtr<BinarySerializer> m_binary;
            UniquePtr<draconic::xml::XmlSerializer> m_xml;
        };

        // One component record, in the section's encoding: TEXT = own object scope with the
        // component fields inline (diffable, skippable by scope); BINARY = v2 blob record.
        void WriteComponentRecord(ISerializer& ar, Scene& scene, ComponentManagerBase& m,
                                  EntityHandle owner, bool text);

        void WriteSceneStreamHeader(ISerializer& ar)
        {
            u32 magic = kSceneStreamMagic;
            u32 version = kSceneStreamVersion;
            draconic::foundation::Serialize(ar, "magic", magic);
            draconic::foundation::Serialize(ar, "version", version);
        }

        void SerializeTransform(ISerializer& ar, Transform& t, u32 version = kSceneStreamVersion)
        {
            if (version >= 3)
            {
                draconic::foundation::Serialize(ar, "position", t.position);
                draconic::foundation::Serialize(ar, "rotation", t.rotation);
                draconic::foundation::Serialize(ar, "scale", t.scale);
                return;
            }
            draconic::foundation::Serialize(ar, "pos", t.position);
            draconic::foundation::Serialize(ar, "rot", t.rotation);
            draconic::foundation::Serialize(ar, "scl", t.scale);
        }
        // Pre-order subtree walk (parents before children, siblings in list order).
        void CollectSubtree(Scene& scene, EntityHandle root, Array<EntityHandle>& out)
        {
            Array<EntityHandle> stack;
            stack.PushBack(root);
            while (!stack.IsEmpty())
            {
                EntityHandle e = stack.Back();
                stack.PopBack();
                out.PushBack(e);
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

        // One component serialized to bytes (binary): baseline capture + save-time diffing.
        // Scene streams are always binary, so blob payloads replay through the same backend.
        void ComponentToBlob(ComponentManagerBase& manager, EntityHandle owner, Array<u8>& out)
        {
            MemoryStream buffer;
            BinarySerializer ar(buffer, SerializeMode::Write);
            manager.WriteComponent(ar, owner);
            out.Clear();
            const Span<const byte> bytes = buffer.Bytes();
            out.Reserve(bytes.Size());
            for (byte b : bytes)
            {
                out.PushBack(static_cast<u8>(b));
            }
        }

        void ComponentFromBlob(ComponentManagerBase& manager, EntityHandle owner,
                               Span<const u8> blob)
        {
            MemoryStream buffer;
            (void)buffer.Write(reinterpret_cast<const byte*>(blob.Data()), blob.Size());
            (void)buffer.Seek(0, SeekOrigin::Begin);
            BinarySerializer ar(buffer, SerializeMode::Read);
            manager.ReadComponent(ar, owner);
        }

        void WriteComponentRecord(ISerializer& ar, Scene& scene, ComponentManagerBase& m,
                                  EntityHandle owner, bool text)
        {
            Guid ownerId = scene.GetEntityId(owner);
            String typeId = String(m.SerializationTypeId());
            if (text)
            {
                ar.BeginObject();
                SerializeGuid(ar, "owner", ownerId);
                draconic::foundation::Serialize(ar, "type", typeId);
                ar.Key("data");
                ar.BeginObject();
                m.WriteComponent(ar, owner);
                ar.EndObject();
                ar.EndObject();
                return;
            }
            SerializeGuid(ar, "owner", ownerId);
            draconic::foundation::Serialize(ar, "type", typeId);
            Array<u8> blob;
            ComponentToBlob(m, owner, blob);
            draconic::foundation::Serialize(ar, "data", blob);
        }

        [[nodiscard]] inline bool BlobsEqual(Span<const u8> a, Span<const u8> b)
        {
            if (a.Size() != b.Size())
            {
                return false;
            }
            for (usize i = 0; i < a.Size(); ++i)
            {
                if (a[i] != b[i])
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] inline bool TransformsEqual(const Transform& a, const Transform& b)
        {
            return a.position.x == b.position.x && a.position.y == b.position.y &&
                   a.position.z == b.position.z && a.rotation.x == b.rotation.x &&
                   a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
                   a.rotation.w == b.rotation.w && a.scale.x == b.scale.x &&
                   a.scale.y == b.scale.y && a.scale.z == b.scale.z;
        }
    }

    namespace detail
    {
        // Live-vs-baseline deltas for one instance, in the pending-descriptor shape a scene
        // load parks: SerializeScene's Referenced write serializes it, and prefab rebuild
        // (template changed) re-applies it onto a fresh respawn.
        [[nodiscard]] inline UniquePtr<Scene::PendingPrefabInstance>
        ComputeInstanceDeltas(Scene& scene, Scene::PrefabInstanceState& state)
        {
            auto pending = MakeUnique<Scene::PendingPrefabInstance>(DefaultAllocator());
            pending->prefabId = state.prefabId;
            EntityHandle root = scene.FindEntity(state.rootEntityId);
            EntityHandle parent =
                root.IsAssigned() ? scene.GetParent(root) : EntityHandle::Invalid();
            pending->parentEntityId = parent.IsAssigned() ? scene.GetEntityId(parent) : Guid{};
            pending->rootTransform =
                root.IsAssigned() ? scene.GetLocalTransform(root) : Transform{};
            pending->sourceIds = state.sourceIds;
            pending->liveIds = state.liveIds;
            pending->rootLiveId = state.rootEntityId;
            pending->ownerRootEntityId = state.ownerRootEntityId;
            pending->nestedRootSourceId = state.nestedRootSourceId;
            EntityHandle nextSibling =
                root.IsAssigned() ? scene.GetNextSibling(root) : EntityHandle::Invalid();
            pending->nextSiblingId =
                nextSibling.IsAssigned() ? scene.GetEntityId(nextSibling) : Guid{};
            // Un-moved root = no scene placement override: nested respawns let the owner
            // template's placement through (and pick up template edits to it).
            for (usize i = 0; i < state.liveIds.Size(); ++i)
            {
                if (state.liveIds[i] == state.rootEntityId)
                {
                    if (root.IsAssigned() && i < state.baselineTransforms.Size())
                    {
                        pending->applyPlacement =
                            !TransformsEqual(pending->rootTransform, state.baselineTransforms[i]);
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
                if (live != root)
                {
                    Transform t = scene.GetLocalTransform(live);
                    if (!TransformsEqual(t, state.baselineTransforms[i]))
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
                        const Scene::PrefabComponentBaseline* baseline = nullptr;
                        for (const Scene::PrefabComponentBaseline& b : state.componentBaselines)
                        {
                            if (b.sourceEntity == state.sourceIds[i] &&
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
                            ComponentToBlob(m, live, blob);
                            if (baseline == nullptr)
                            {
                                op.op = 1u; // add
                                op.blob = static_cast<Array<u8>&&>(blob);
                            }
                            else if (!BlobsEqual(Span<const u8>{blob.Data(), blob.Size()},
                                                 Span<const u8>{baseline->blob.Data(),
                                                                baseline->blob.Size()}))
                            {
                                op.op = 0u; // modify
                                op.blob = static_cast<Array<u8>&&>(blob);
                            }
                            else
                            {
                                return; // unchanged
                            }
                        }
                        else if (baseline != nullptr)
                        {
                            op.op = 2u; // remove
                        }
                        else
                        {
                            return; // never had it
                        }
                        pending->componentOps.PushBack(
                            static_cast<Scene::PendingPrefabComponentOp&&>(op));
                    });
            }
            return pending;
        }

        // Re-applies a pending descriptor's DELTAS onto a freshly spawned instance (`state` is
        // the spawn's registered state - its member map routes source ids to live entities).
        void ApplyPendingDeltas(Scene& scene, Scene::PrefabInstanceState* state,
                                const Scene::PendingPrefabInstance& pending)
        {
            auto liveOf = [&](const Guid& sourceId) -> EntityHandle
            {
                if (state != nullptr)
                {
                    for (usize i = 0; i < state->sourceIds.Size(); ++i)
                    {
                        if (state->sourceIds[i] == sourceId)
                        {
                            return scene.FindEntity(state->liveIds[i]);
                        }
                    }
                }
                return EntityHandle::Invalid();
            };
            for (const Guid& dead : pending.destroyedMembers)
            {
                EntityHandle e = liveOf(dead);
                if (e.IsAssigned())
                {
                    scene.DestroyEntity(e);
                }
            }
            for (usize i = 0;
                 i < pending.overrideTransformIds.Size() && i < pending.overrideTransforms.Size();
                 ++i)
            {
                EntityHandle e = liveOf(pending.overrideTransformIds[i]);
                if (e.IsAssigned())
                {
                    scene.SetLocalTransform(e, pending.overrideTransforms[i]);
                }
            }
            for (const Scene::PendingPrefabComponentOp& op : pending.componentOps)
            {
                EntityHandle e = liveOf(op.sourceEntity);
                ComponentManagerBase* manager =
                    scene.FindManagerBySerializationId(op.typeId.AsView());
                if (!e.IsAssigned() || manager == nullptr)
                {
                    continue;
                }
                if (op.op == 2u)
                {
                    if (manager->HasComponent(e))
                    {
                        manager->RemoveComponent(e);
                    }
                }
                else
                {
                    ComponentFromBlob(*manager, e, Span<const u8>{op.blob.Data(), op.blob.Size()});
                }
            }
        }

        // Restores captured sibling order after spawns APPENDED entities at the end of their
        // parents: each fix moves `entity` immediately before `nextSibling`. A target that is
        // itself a moved entity settles over multiple passes (chains anchor on entities that
        // never move); a parent mismatch skips the fix - order restoration never REPARENTS.
        struct SiblingOrderFix
        {
            Guid entity{};
            Guid nextSibling{};
        };
        void RestoreSiblingOrder(Scene& scene, const Array<SiblingOrderFix>& fixes)
        {
            for (usize pass = 0; pass <= fixes.Size(); ++pass)
            {
                bool changed = false;
                for (const SiblingOrderFix& fix : fixes)
                {
                    if (fix.nextSibling == Guid{})
                    {
                        continue;
                    }
                    EntityHandle entity = scene.FindEntity(fix.entity);
                    EntityHandle sibling = scene.FindEntity(fix.nextSibling);
                    if (!entity.IsAssigned() || !sibling.IsAssigned())
                    {
                        continue;
                    }
                    if (scene.GetParent(entity) != scene.GetParent(sibling))
                    {
                        continue;
                    }
                    if (scene.GetNextSibling(entity) == sibling)
                    {
                        continue;
                    }
                    scene.MoveBefore(entity, sibling);
                    changed = true;
                }
                if (!changed)
                {
                    break;
                }
            }
        }

        // One nested-instance record, serialized (the ref+delta shape shared by scene files'
        // prefab sections and prefab payloads' trailing records). `wireNested` gates the P4
        // link fields for pre-nesting saves.
        // `scene` supplies the component managers the TEXT encoding needs: an op's blob
        // deserializes into a transient scratch entity so its FIELDS serialize inline
        // (diffable), then the scratch is removed. Binary keeps the blob wire. An op whose
        // manager is unknown falls back to the hex blob (form=0) so it survives transcodes.
        void WritePrefabRecord(ISerializer& ar, Scene& scene, Scene::PendingPrefabInstance& d,
                               bool text)
        {
            SerializeGuid(ar, "prefab", d.prefabId);
            SerializeGuid(ar, "parent", d.parentEntityId);
            SerializeTransform(ar, d.rootTransform);
            SerializeGuid(ar, "rootLive", d.rootLiveId);
            SerializeGuid(ar, "owner", d.ownerRootEntityId);
            SerializeGuid(ar, "nestedSrcRoot", d.nestedRootSourceId);
            SerializeGuid(ar, "nextSibling", d.nextSiblingId);
            u8 placement = d.applyPlacement ? 1u : 0u;
            draconic::foundation::Serialize(ar, "placement", placement);

            u32 memberCount = static_cast<u32>(d.sourceIds.Size());
            ar.Key("members");
            ar.BeginArray(memberCount);
            for (u32 i = 0; i < memberCount; ++i)
            {
                SerializeGuid(ar, "src", d.sourceIds[i]);
                SerializeGuid(ar, "live", d.liveIds[i]);
            }
            ar.EndArray();

            u32 destroyedCount = static_cast<u32>(d.destroyedMembers.Size());
            ar.Key("destroyed");
            ar.BeginArray(destroyedCount);
            for (Guid& dead : d.destroyedMembers)
            {
                SerializeGuid(ar, "src", dead);
            }
            ar.EndArray();

            u32 transformCount = static_cast<u32>(d.overrideTransformIds.Size());
            ar.Key("transformOverrides");
            ar.BeginArray(transformCount);
            for (u32 i = 0; i < transformCount; ++i)
            {
                SerializeGuid(ar, "src", d.overrideTransformIds[i]);
                SerializeTransform(ar, d.overrideTransforms[i]);
            }
            ar.EndArray();

            u32 opCount = static_cast<u32>(d.componentOps.Size());
            ar.Key("componentOps");
            ar.BeginArray(opCount);
            EntityHandle scratch = EntityHandle::Invalid();
            for (Scene::PendingPrefabComponentOp& op : d.componentOps)
            {
                if (text)
                {
                    ar.BeginObject();
                    SerializeGuid(ar, "src", op.sourceEntity);
                    draconic::foundation::Serialize(ar, "type", op.typeId);
                    draconic::foundation::Serialize(ar, "op", op.op);
                    if (op.op != 2u)
                    { // remove ops carry no payload
                        ComponentManagerBase* m =
                            scene.FindManagerBySerializationId(op.typeId.AsView());
                        u8 form = (m != nullptr) ? 1u : 0u;
                        draconic::foundation::Serialize(ar, "form", form);
                        if (m != nullptr)
                        {
                            if (!scratch.IsAssigned())
                            {
                                scratch = scene.CreateEntity(u8"__op_write");
                            }
                            ComponentFromBlob(*m, scratch,
                                              Span<const u8>{op.blob.Data(), op.blob.Size()});
                            ar.Key("data");
                            ar.BeginObject();
                            m->WriteComponent(ar, scratch);
                            ar.EndObject();
                            m->RemoveComponent(scratch);
                        }
                        else
                        {
                            draconic::foundation::Serialize(ar, "blob", op.blob);
                        }
                    }
                    ar.EndObject();
                    continue;
                }
                SerializeGuid(ar, "src", op.sourceEntity);
                draconic::foundation::Serialize(ar, "type", op.typeId);
                draconic::foundation::Serialize(ar, "op", op.op);
                draconic::foundation::Serialize(ar, "blob", op.blob);
            }
            if (scratch.IsAssigned())
            {
                scene.DestroyEntity(scratch);
            }
            ar.EndArray();
        }

        // Corruption guard for the count-driven loops below: a record with more entries than
        // this is a misparse (ISerializer exposes no error state to poll mid-read, and a
        // garbage count must not allocate unbounded - it hung project open before the retired
        // mode-2 layout was refused outright).
        constexpr u32 kMaxPrefabRecordEntries = 1u << 20;

        void ReadPrefabRecord(ISerializer& ar, Scene& scene, Scene::PendingPrefabInstance& pending,
                              bool wireNested, bool text, u32 version = kSceneStreamVersion)
        {
            SerializeGuid(ar, "prefab", pending.prefabId, version);
            SerializeGuid(ar, "parent", pending.parentEntityId, version);
            SerializeTransform(ar, pending.rootTransform, version);
            if (wireNested)
            {
                SerializeGuid(ar, "rootLive", pending.rootLiveId, version);
                SerializeGuid(ar, "owner", pending.ownerRootEntityId, version);
                SerializeGuid(ar, "nestedSrcRoot", pending.nestedRootSourceId, version);
                SerializeGuid(ar, "nextSibling", pending.nextSiblingId, version);
                u8 placement = 1;
                draconic::foundation::Serialize(ar, "placement", placement);
                pending.applyPlacement = placement != 0;
            }

            u32 memberCount = 0;
            ar.Key("members");
            ar.BeginArray(memberCount);
            if (memberCount > kMaxPrefabRecordEntries)
            {
                return;
            }
            for (u32 i = 0; i < memberCount; ++i)
            {
                Guid src, live;
                SerializeGuid(ar, "src", src, version);
                SerializeGuid(ar, "live", live, version);
                pending.sourceIds.PushBack(src);
                pending.liveIds.PushBack(live);
            }
            ar.EndArray();

            u32 destroyedCount = 0;
            ar.Key("destroyed");
            ar.BeginArray(destroyedCount);
            if (destroyedCount > kMaxPrefabRecordEntries)
            {
                return;
            }
            for (u32 i = 0; i < destroyedCount; ++i)
            {
                Guid d;
                SerializeGuid(ar, "src", d, version);
                pending.destroyedMembers.PushBack(d);
            }
            ar.EndArray();

            u32 transformCount = 0;
            ar.Key("transformOverrides");
            ar.BeginArray(transformCount);
            if (transformCount > kMaxPrefabRecordEntries)
            {
                return;
            }
            for (u32 i = 0; i < transformCount; ++i)
            {
                Guid src;
                Transform t;
                SerializeGuid(ar, "src", src, version);
                SerializeTransform(ar, t, version);
                pending.overrideTransformIds.PushBack(src);
                pending.overrideTransforms.PushBack(t);
            }
            ar.EndArray();

            u32 opCount = 0;
            ar.Key("componentOps");
            ar.BeginArray(opCount);
            if (opCount > kMaxPrefabRecordEntries)
            {
                return;
            }
            EntityHandle scratch = EntityHandle::Invalid();
            for (u32 i = 0; i < opCount; ++i)
            {
                Scene::PendingPrefabComponentOp op;
                if (text)
                {
                    ar.BeginObject();
                    SerializeGuid(ar, "src", op.sourceEntity, version);
                    draconic::foundation::Serialize(ar, "type", op.typeId);
                    draconic::foundation::Serialize(ar, "op", op.op);
                    bool keep = true;
                    if (op.op != 2u)
                    {
                        u8 form = 0;
                        draconic::foundation::Serialize(ar, "form", form);
                        if (form == 1u)
                        {
                            ComponentManagerBase* m =
                                scene.FindManagerBySerializationId(op.typeId.AsView());
                            if (m != nullptr)
                            {
                                if (!scratch.IsAssigned())
                                {
                                    scratch = scene.CreateEntity(u8"__op_read");
                                }
                                ar.Key("data");
                                ar.BeginObject();
                                m->ReadComponent(ar, scratch);
                                ar.EndObject();
                                ComponentToBlob(*m, scratch, op.blob);
                                m->RemoveComponent(scratch);
                            }
                            else
                            {
                                // Inline fields need the manager to decode - the op drops,
                                // like any unknown-type record.
                                DRACONIC_LOG_WARNING(
                                    u8"Scene", u8"dropping override of unknown component type '{}'",
                                    op.typeId);
                                keep = false;
                            }
                        }
                        else
                        {
                            draconic::foundation::Serialize(ar, "blob", op.blob);
                        }
                    }
                    ar.EndObject();
                    if (keep)
                    {
                        pending.componentOps.PushBack(
                            static_cast<Scene::PendingPrefabComponentOp&&>(op));
                    }
                    continue;
                }
                SerializeGuid(ar, "src", op.sourceEntity, version);
                draconic::foundation::Serialize(ar, "type", op.typeId);
                draconic::foundation::Serialize(ar, "op", op.op);
                draconic::foundation::Serialize(ar, "blob", op.blob);
                pending.componentOps.PushBack(static_cast<Scene::PendingPrefabComponentOp&&>(op));
            }
            if (scratch.IsAssigned())
            {
                scene.DestroyEntity(scratch);
            }
            ar.EndArray();
        }

        // Instances whose ROOT lies inside `root`'s subtree (excluding `root`'s own instance):
        // captures turn these into nested records; their members leave the flat arrays.
        void CollectContainedInstances(Scene& scene, EntityHandle root,
                                       Array<Scene::PrefabInstanceState*>& outStates,
                                       HashMap<Guid, u8>& outMembers)
        {
            scene.ForEachPrefabInstance(
                [&](Scene::PrefabInstanceState& s)
                {
                    EntityHandle e = scene.FindEntity(s.rootEntityId);
                    if (!e.IsAssigned() || e == root)
                    {
                        return;
                    }
                    bool inside = false;
                    for (EntityHandle p = scene.GetParent(e); p.IsAssigned();
                         p = scene.GetParent(p))
                    {
                        if (p == root)
                        {
                            inside = true;
                            break;
                        }
                    }
                    if (!inside)
                    {
                        return;
                    }
                    outStates.PushBack(&s);
                    for (const Guid& live : s.liveIds)
                    {
                        outMembers.InsertOrAssign(live, 1u);
                    }
                });
        }

        // Re-captures an instance's baselines from its CURRENT state. Nesting uses this after
        // applying an owner payload's record deltas: the owner's customization of a nested
        // instance becomes part of the BASELINE (so scene saves record only scene-level edits,
        // and owner-template changes propagate on rebuild instead of being pinned as overrides).
        void RecaptureBaselines(Scene& scene, Scene::PrefabInstanceState& state)
        {
            state.componentBaselines.Clear();
            for (usize i = 0; i < state.sourceIds.Size(); ++i)
            {
                EntityHandle live = scene.FindEntity(state.liveIds[i]);
                if (!live.IsAssigned())
                {
                    continue;
                }
                if (i < state.baselineTransforms.Size())
                {
                    state.baselineTransforms[i] = scene.GetLocalTransform(live);
                }
                scene.ForEachManager(
                    [&](ComponentManagerBase& m)
                    {
                        if (!m.IsSerializable() || !m.HasComponent(live))
                        {
                            return;
                        }
                        Scene::PrefabComponentBaseline baseline;
                        baseline.sourceEntity = state.sourceIds[i];
                        baseline.typeId = String(m.SerializationTypeId());
                        ComponentToBlob(m, live, baseline.blob);
                        state.componentBaselines.PushBack(
                            static_cast<Scene::PrefabComponentBaseline&&>(baseline));
                    });
            }
        }
    }

    // Bidirectional whole-scene serialization: scene name, the entity table + transform
    // hierarchy, components, then SCENE-SYSTEM SETTINGS (environment/sky etc. - systems
    // exposing SettingsType(), serialized under their own versioned payloads). On read,
    // `scene` should be freshly created with its component managers + systems already present
    // (so records route into their pools / settings blocks).
    //
    // `legacyProbe`: the settings section was appended AFTER the format shipped; streams saved
    // before it simply END at the component array. Readers that have the underlying stream
    // pass it here - at the settings boundary, exhausted stream = legacy save, settings keep
    // their defaults (the next save upgrades). Null = the section is expected (fresh writes,
    // snapshots). Write mode always writes it.
    // How prefab instances persist in a scene stream:
    //  - Referenced (scene files): instance members are EXCLUDED from the entity/component
    //    arrays; a trailing section stores ref + deltas per instance. Loading parks pending
    //    descriptors on the scene - run ResolveScenePrefabs afterwards to respawn them.
    //  - Expanded (snapshots, e.g. play-in-editor): members serialize flat like every other
    //    entity, plus the instance state (member maps + baselines) verbatim, so a restore
    //    rebuilds exact prefab bookkeeping WITHOUT needing a payload resolver.
    enum class ScenePrefabMode : u8
    {
        Referenced = 0,
        Expanded = 1
    };

    // Wire values of the prefab-section mode tag. 0/1 = the P1..P3 formats (no nesting links);
    // 2/3 = the same sections plus per-record nesting links (rootLive/owner/nestedSrcRoot).
    // Writers emit 2/3; readers accept all four (older saves upgrade on the next write).
    namespace detail
    {
        constexpr u8 kPrefabWireReferenced = 0; // pre-P4 (no nesting links) - still read
        constexpr u8 kPrefabWireExpanded = 1;
        constexpr u8 kPrefabWireReferenced2 = 2; // RETIRED P4 layout: readers REFUSE it (a
                                                 // misparse turns array counts into garbage
                                                 // -> OOM); the section skips with a warning
        constexpr u8 kPrefabWireExpanded2 = 3;
        constexpr u8 kPrefabWireReferenced3 = 4; // nesting links + sibling order + placement
    }

    // `includeSettings`: prefab payloads write an EMPTY system-settings section (a prefab is a
    // subtree template, not a world - and SpawnPrefab must be able to walk PAST the section to
    // reach the nested-instance records without applying settings to the target scene).
    void SerializeScene(ISerializer& ar, Scene& scene, IStream* legacyProbe = nullptr,
                        ScenePrefabMode prefabMode = ScenePrefabMode::Referenced,
                        bool includeSettings = true,
                        detail::SceneStreamEncoding encoding = detail::SceneStreamEncoding::Binary);

    // Loads a cooked scene's "scene" data stream into `scene` (which must already have its
    // component managers). Returns NotFound if the stream is missing.
    // Post-load resolve pass (asset-pipeline design §8): bind every component's resource::Ref
    // through the manager. Run after LoadScene once a ResourceManager over the cooked DB exists;
    // idempotent (re-binding an already-bound ref is a cache hit).
    void ResolveSceneResources(Scene& scene, draconic::resource::ResourceManager& resources);

    // ============================== Prefabs (P1: ref + deltas) ==================================
    //
    // A prefab is a content-DB instance whose "scene" data stream is a LoadScene-compatible
    // capture of an entity subtree (PrefabDocument primary carries the name - a DISTINCT type so
    // pages/creators/pickers tell prefabs from scenes, but the stream format is identical, so the
    // scene editor page opens prefabs unchanged). Spawning instantiates the payload into a target
    // scene with fresh (or preassigned) guids and records a PrefabInstanceState (member guid map +
    // spawn-time BASELINES). Scenes persist instances as ref + deltas: overrides are DERIVED at
    // save time by comparing live state against the baselines, so nothing tracks edits and
    // undo/redo can never desynchronize the override set (docs/design/prefabs.md).

    class PrefabDocument final : public ISerializable
    {
        DRACONIC_OBJECT(PrefabDocument, ISerializable)
    public:
        String name;
        void Serialize(ISerializer& ar) override { draconic::foundation::Serialize(ar, "name", name); }
    };

    /// Captures `root`'s subtree into a LoadScene-compatible stream (entities + components + an
    /// empty settings section + nested-instance records): the prefab PAYLOAD. Written with the
    /// subtree's OWN guids - they become the stable sourceEntityIds that every instance's deltas
    /// key on. Prefab instances INSIDE the subtree capture as nested RECORDS (ref + the
    /// instance's current deltas), not flattened entities - selecting a group that contains
    /// instances and making it a prefab preserves the links.
    namespace detail
    {
        Status CapturePrefabBody(Serializer& ar, bool text, Scene& scene, EntityHandle root);
    }

    /// Sources capture as TEXT (XML) by default; Binary remains for in-memory/test payloads
    /// (spawn sniffs, so either reads back).
    Status CapturePrefab(Scene& scene, EntityHandle root, IStream& out,
                         detail::SceneStreamEncoding encoding = detail::SceneStreamEncoding::Text);

    namespace detail
    {
        Status CapturePrefabBody(Serializer& ar, bool text, Scene& scene, EntityHandle root)
        {
            detail::WriteSceneStreamHeader(ar);

            String name = String(scene.GetEntityName(root));
            draconic::foundation::Serialize(ar, "name", name);

            Array<Scene::PrefabInstanceState*> contained;
            HashMap<Guid, u8> nestedMembers;
            detail::CollectContainedInstances(scene, root, contained, nestedMembers);

            Array<EntityHandle> allHandles;
            detail::CollectSubtree(scene, root, allHandles);
            Array<EntityHandle> handles;
            for (EntityHandle e : allHandles)
            {
                if (nestedMembers.Find(scene.GetEntityId(e)) == nullptr)
                {
                    handles.PushBack(e);
                }
            }

            ar.Key("entities");
            u32 entityCount = static_cast<u32>(handles.Size());
            ar.BeginArray(entityCount);
            for (EntityHandle e : handles)
            {
                Guid id = scene.GetEntityId(e);
                String ename = String(scene.GetEntityName(e));
                u8 active = scene.IsActive(e) ? 1u : 0u;
                // The subtree ROOT records a nil parent (spawn re-parents it at the target).
                Guid parentId = (e == root) ? Guid{} : scene.GetEntityId(scene.GetParent(e));
                Transform t = scene.GetLocalTransform(e);
                detail::SerializeGuid(ar, "id", id);
                draconic::foundation::Serialize(ar, "name", ename);
                draconic::foundation::Serialize(ar, "active", active);
                detail::SerializeGuid(ar, "parent", parentId);
                detail::SerializeTransform(ar, t);
            }
            ar.EndArray();

            struct Record
            {
                ComponentManagerBase* manager;
                EntityHandle owner;
            };
            Array<Record> records;
            scene.ForEachManager(
                [&](ComponentManagerBase& m)
                {
                    if (!m.IsSerializable())
                    {
                        return;
                    }
                    for (EntityHandle owner : m.OwnerHandles())
                    {
                        for (EntityHandle e : handles)
                        {
                            if (e == owner)
                            {
                                records.PushBack(Record{&m, owner});
                                break;
                            }
                        }
                    }
                });
            ar.Key("components");
            u32 componentCount = static_cast<u32>(records.Size());
            ar.BeginArray(componentCount);
            for (Record& r : records)
            {
                detail::WriteComponentRecord(ar, scene, *r.manager, r.owner, text);
            }
            ar.EndArray();

            // Empty settings section: keeps the stream LoadScene-compatible (the prefab EDIT page
            // loads it like any scene; spawn walks PAST it to the nested records).
            u32 settingsCount = 0;
            ar.Key("systemSettings");
            ar.BeginArray(settingsCount);
            ar.EndArray();

            // Nested-instance records (P4): contained instances as ref + current deltas, in the
            // payload's namespace (live guids ARE the namespace ids here; a previously-nested
            // instance keeps its stable identity so existing spawns keep matching).
            u8 sectionMode = detail::kPrefabWireReferenced3;
            draconic::foundation::Serialize(ar, "prefabMode", sectionMode);
            u32 recordCount = static_cast<u32>(contained.Size());
            ar.Key("prefabInstances");
            ar.BeginArray(recordCount);
            for (Scene::PrefabInstanceState* state : contained)
            {
                UniquePtr<Scene::PendingPrefabInstance> d =
                    detail::ComputeInstanceDeltas(scene, *state);
                if (!state->nestedRootSourceId.IsNil())
                {
                    d->rootLiveId = state->nestedRootSourceId;
                }
                d->ownerRootEntityId = Guid{};
                d->nestedRootSourceId = Guid{};
                detail::WritePrefabRecord(ar, scene, *d, text);
            }
            ar.EndArray();
            return ar.IsOk() ? Status{} : ar.GetStatus();
        }
    } // namespace detail

    /// Maps a prefab id to its payload stream (editor: source DB; player: cooked DB).
    using PrefabPayloadResolver = Function<UniquePtr<IStream>(const Guid&)>;

    /// Spawns a prefab payload into `scene`: creates every payload entity with a FRESH guid
    /// (or the caller's preassigned one - scene loading preserves saved identities this way),
    /// relinks parents inside the instance, parents payload roots under `parent` (invalid =
    /// scene root), and registers a PrefabInstanceState with spawn-time baselines. Returns the
    /// instance root (the FIRST payload root), or invalid on a malformed payload.
    ///
    /// NESTING (P4): payloads written since nesting carry a trailing record section - the FLAT
    /// FOREST of every instance the template contains at any depth (each relative to its OWN
    /// template, so spawning never recurses and reference cycles cannot loop). With a `resolver`
    /// each record's template spawns as a linked nested instance: the record's deltas (the
    /// owner's customization) apply and then the baselines RE-capture, so scene saves record
    /// only scene-level edits and owner-template changes propagate on rebuild. Without a
    /// resolver the records are skipped with a warning (entities absent).
    /// `nestedSceneDeltas` = the scene's saved sub-records for this instance's nested children
    /// (matched by nestedRootSourceId == record rootLive): preserved guids + scene-level deltas.
    EntityHandle
    SpawnPrefab(Scene& scene, IStream& payload, const Guid& prefabId,
                EntityHandle parent = EntityHandle::Invalid(),
                const HashMap<Guid, Guid>* preassigned = nullptr,
                const PrefabPayloadResolver* resolver = nullptr,
                const Array<const Scene::PendingPrefabInstance*>* nestedSceneDeltas = nullptr,
                bool spawnNested = true);

    /// The prefab member owning `entityId`, if any: the instance state + the member's index
    /// (source id = state->sourceIds[index]). Inspector indicators and revert key on this.
    struct PrefabMemberInfo
    {
        Scene::PrefabInstanceState* state = nullptr;
        usize memberIndex = 0;
    };
    [[nodiscard]] bool FindPrefabMember(Scene& scene, const Guid& entityId, PrefabMemberInfo& out);

    [[nodiscard]] const Scene::PrefabComponentBaseline*
    FindPrefabBaseline(const Scene::PrefabInstanceState& state, const Guid& sourceId,
                       StringView typeId);

    /// True when the member's component differs from its spawn-time baseline (an OVERRIDE):
    /// modified bytes, added (no baseline), or removed (baseline without a live component).
    [[nodiscard]] bool IsPrefabComponentOverridden(Scene& scene, const PrefabMemberInfo& member,
                                                   ComponentManagerBase& manager);

    /// Captures an INSTANCE's current state as a template payload (apply-to-prefab): records are
    /// written with the members' SOURCE ids - other instances' deltas stay keyed correctly -
    /// while entities the user ADDED under the instance keep their live guids as brand-new
    /// source ids. The instance root records a nil parent, and ITS transform is written as the
    /// template's root transform (an applied placement is not part of the template).
    // Live-vs-PURE-TEMPLATE deltas for one instance: what apply-to-prefab writes into the OWNER
    // payload's nested record (the instance's baselines have the owner customization folded in,
    // so a plain baseline diff would lose it). v2 payloads carry length-prefixed component
    // blobs, so the template baselines read STRAIGHT off the stream - no scene mutation. Legacy
    // payloads fall back to the (degraded) baseline diff.
    UniquePtr<Scene::PendingPrefabInstance>
    ComputeInstanceDeltasVsTemplate(Scene& scene, Scene::PrefabInstanceState& state,
                                    IStream& templatePayload);

    namespace detail
    {
        Status CaptureInstanceAsTemplateBody(Serializer& ar, bool text, Scene& scene,
                                             Scene::PrefabInstanceState& state,
                                             const PrefabPayloadResolver* resolver);
    }

    /// Sources capture as TEXT (XML) by default - the payload lands in the prefab ASSET.
    Status CaptureInstanceAsTemplate(
        Scene& scene, Scene::PrefabInstanceState& state, IStream& out,
        const PrefabPayloadResolver* resolver = nullptr,
        detail::SceneStreamEncoding encoding = detail::SceneStreamEncoding::Text);

    namespace detail
    {
        Status CaptureInstanceAsTemplateBody(Serializer& ar, bool text, Scene& scene,
                                             Scene::PrefabInstanceState& state,
                                             const PrefabPayloadResolver* resolver)
        {
            EntityHandle root = scene.FindEntity(state.rootEntityId);
            if (!root.IsAssigned())
            {
                return Status{ErrorCode::NotFound};
            }

            // live guid -> source id substitution for every surviving member.
            HashMap<Guid, Guid> sourceOf;
            for (usize i = 0; i < state.sourceIds.Size(); ++i)
            {
                sourceOf.InsertOrAssign(state.liveIds[i], state.sourceIds[i]);
            }
            auto substituted = [&](const Guid& live) -> Guid
            {
                const Guid* source = sourceOf.Find(live);
                return (source != nullptr) ? *source
                                           : live; // user-added entities: live guid = new source id
            };

            detail::WriteSceneStreamHeader(ar);
            String name = String(scene.GetEntityName(root));
            draconic::foundation::Serialize(ar, "name", name);

            // Instances inside the subtree become nested RECORDS (owner-linked ones keep their
            // stable identity; a user-spawned instance inside gets ABSORBED as a new record).
            Array<Scene::PrefabInstanceState*> contained;
            HashMap<Guid, u8> nestedMembers;
            detail::CollectContainedInstances(scene, root, contained, nestedMembers);

            Array<EntityHandle> allHandles;
            detail::CollectSubtree(scene, root, allHandles);
            Array<EntityHandle> handles;
            for (EntityHandle e : allHandles)
            {
                if (nestedMembers.Find(scene.GetEntityId(e)) == nullptr)
                {
                    handles.PushBack(e);
                }
            }

            // The root's live transform is this instance's PLACEMENT, not template content (root
            // transforms never propagate between instances - the Unity semantic); write the
            // spawn-time baseline (the template-authored root transform) instead, so an applied
            // placement never leaks into the asset.
            Transform rootTemplateTransform = scene.GetLocalTransform(root);
            for (usize i = 0; i < state.liveIds.Size(); ++i)
            {
                if (state.liveIds[i] == state.rootEntityId && i < state.baselineTransforms.Size())
                {
                    rootTemplateTransform = state.baselineTransforms[i];
                    break;
                }
            }

            ar.Key("entities");
            u32 entityCount = static_cast<u32>(handles.Size());
            ar.BeginArray(entityCount);
            for (EntityHandle e : handles)
            {
                Guid id = substituted(scene.GetEntityId(e));
                String ename = String(scene.GetEntityName(e));
                u8 active = scene.IsActive(e) ? 1u : 0u;
                Guid parentId =
                    (e == root) ? Guid{} : substituted(scene.GetEntityId(scene.GetParent(e)));
                Transform t = (e == root) ? rootTemplateTransform : scene.GetLocalTransform(e);
                detail::SerializeGuid(ar, "id", id);
                draconic::foundation::Serialize(ar, "name", ename);
                draconic::foundation::Serialize(ar, "active", active);
                detail::SerializeGuid(ar, "parent", parentId);
                detail::SerializeTransform(ar, t);
            }
            ar.EndArray();

            struct Record
            {
                ComponentManagerBase* manager;
                EntityHandle owner;
            };
            Array<Record> records;
            scene.ForEachManager(
                [&](ComponentManagerBase& m)
                {
                    if (!m.IsSerializable())
                    {
                        return;
                    }
                    for (EntityHandle owner : m.OwnerHandles())
                    {
                        for (EntityHandle e : handles)
                        {
                            if (e == owner)
                            {
                                records.PushBack(Record{&m, owner});
                                break;
                            }
                        }
                    }
                });
            ar.Key("components");
            u32 componentCount = static_cast<u32>(records.Size());
            ar.BeginArray(componentCount);
            for (Record& r : records)
            {
                Guid ownerId = substituted(scene.GetEntityId(r.owner));
                String typeId = String(r.manager->SerializationTypeId());
                if (text)
                {
                    ar.BeginObject();
                    detail::SerializeGuid(ar, "owner", ownerId);
                    draconic::foundation::Serialize(ar, "type", typeId);
                    ar.Key("data");
                    ar.BeginObject();
                    r.manager->WriteComponent(ar, r.owner);
                    ar.EndObject();
                    ar.EndObject();
                    continue;
                }
                detail::SerializeGuid(ar, "owner", ownerId);
                draconic::foundation::Serialize(ar, "type", typeId);
                Array<u8> blob;
                detail::ComponentToBlob(*r.manager, r.owner, blob);
                draconic::foundation::Serialize(ar, "data", blob);
            }
            ar.EndArray();

            u32 settingsCount = 0;
            ar.Key("systemSettings");
            ar.BeginArray(settingsCount);
            ar.EndArray();

            // Nested records: deltas vs the PURE child template when the resolver provides it (the
            // instance's baselines already absorbed this owner's customization - a baseline diff
            // would silently drop it); baseline diff is the degraded fallback.
            u8 sectionMode = detail::kPrefabWireReferenced3;
            draconic::foundation::Serialize(ar, "prefabMode", sectionMode);
            u32 recordCount = static_cast<u32>(contained.Size());
            ar.Key("prefabInstances");
            ar.BeginArray(recordCount);
            for (Scene::PrefabInstanceState* nested : contained)
            {
                UniquePtr<IStream> childPayload = (resolver != nullptr && *resolver)
                                                      ? (*resolver)(nested->prefabId)
                                                      : UniquePtr<IStream>{};
                UniquePtr<Scene::PendingPrefabInstance> d;
                if (childPayload.Get() != nullptr)
                {
                    d = ComputeInstanceDeltasVsTemplate(scene, *nested, *childPayload);
                }
                else
                {
                    DRACONIC_LOG_WARNING(u8"Scene", u8"apply-to-prefab: nested template unresolved "
                                                    u8"- owner customization may be lost");
                    d = detail::ComputeInstanceDeltas(scene, *nested);
                }
                if (!nested->nestedRootSourceId.IsNil())
                {
                    d->rootLiveId = nested->nestedRootSourceId;
                }
                d->parentEntityId = substituted(d->parentEntityId);
                d->nextSiblingId = substituted(d->nextSiblingId);
                d->ownerRootEntityId = Guid{};
                d->nestedRootSourceId = Guid{};
                detail::WritePrefabRecord(ar, scene, *d, text);
            }
            ar.EndArray();
            return ar.IsOk() ? Status{} : ar.GetStatus();
        }
    } // namespace detail

    /// Discards an instance's deltas: respawn from `payload` with the PRESERVED member guids and
    /// placement (parent + root transform), applying nothing else. Returns false if the root is
    /// gone or the payload fails to spawn.
    bool RevertPrefabInstance(Scene& scene, const Guid& rootEntityId, Span<const byte> payload,
                              const PrefabPayloadResolver* resolver = nullptr);

    /// Resolves the PENDING prefab instances a scene load parked (SerializeScene reads the
    /// ref+delta section but has no DB access): `resolver` maps a prefab id to its payload
    /// stream (editor: source DB; runtime: cooked DB). Respawns each instance with its SAVED
    /// member guids, re-applies the root placement and the deltas. Unresolvable prefabs are
    /// skipped with a warning (their entities are simply absent).
    void ResolveScenePrefabs(Scene& scene, const PrefabPayloadResolver& resolver);

    /// The template changed (its asset was saved / its product reloaded): rebuild every affected
    /// TOP-LEVEL instance in `scene` from its template, preserving the user's deltas. An instance
    /// is affected when it IS `prefabId` or its payload REFERENCES it (nested at any depth -
    /// referencedPrefabIds, recorded at spawn); referencing instances rebuild from their OWN
    /// template via `resolver`. Owned nested instances rebuild with their owner (scene-level
    /// deltas + member guids preserved); a user-spawned instance INSIDE a rebuilt one respawns
    /// standalone afterwards, and plain user entities parented under members are detached before
    /// the teardown and re-attached after (they used to be silently destroyed). Returns the
    /// number of instances rebuilt.
    u32 RebuildPrefabInstances(Scene& scene, const Guid& prefabId, Span<const byte> payload,
                               const PrefabPayloadResolver* resolver = nullptr);

    /// Export staging: re-encode a scene/prefab SOURCE stream (text or binary) to the BINARY
    /// wire the player loads. `scratch` must be an EMPTY scene carrying the app's FULL manager
    /// set (create it through the SceneSubsystem so ISceneAware injection covers every
    /// component type - a hand-listed set would silently drop components). Parked prefab
    /// pendings re-emit verbatim (SerializeScene write), so no resolver/spawn is needed.
    /// The caller clears `scratch` afterwards.
    [[nodiscard]] Result<Array<byte>> TranscodeSceneStreamToBinary(IStream& in, Scene& scratch,
                                                                   bool includeSettings);

    Status LoadScene(draconic::content::Instance& instance, Scene& scene);

    // A full-scene snapshot for the editor's Simulate loop (play-in-editor design: snapshot ->
    // run -> restore). Capture serializes through the SAME path as .scene saves; Restore drains
    // every entity in the target and deserializes back INTO THE SAME Scene instance, so borrowed
    // references to the scene (pages, edit contexts, guid selections) stay valid - entity guids
    // are part of the snapshot, so guid-keyed state re-resolves after restore.
    class SceneSnapshot
    {
    public:
        /// Serialize `scene` into a memory snapshot. Null on serializer failure.
        [[nodiscard]] static UniquePtr<SceneSnapshot> Capture(Scene& scene)
        {
            MemoryStream buffer;
            BinarySerializer ar(buffer, SerializeMode::Write);
            // Expanded: members serialize flat + instance state verbatim, so Restore needs no
            // prefab payload resolver (snapshots must be self-contained).
            SerializeScene(ar, scene, nullptr, ScenePrefabMode::Expanded);
            if (!ar.IsOk())
            {
                return UniquePtr<SceneSnapshot>{};
            }
            UniquePtr<SceneSnapshot> snapshot = MakeUnique<SceneSnapshot>(DefaultAllocator());
            const Span<const byte> bytes = buffer.Bytes();
            snapshot->m_blob.Reserve(bytes.Size());
            for (byte b : bytes)
            {
                snapshot->m_blob.PushBack(b);
            }
            return snapshot;
        }

        /// Drain `scene` and rebuild it from the snapshot. Pass the resource manager to re-bind
        /// component refs immediately (the same post-load resolve pass scene loading runs).
        [[nodiscard]] Status Restore(Scene& scene,
                                     draconic::resource::ResourceManager* resources = nullptr)
        {
            // Drain: destroy every root (children go with them). Collect first - destroying
            // while iterating the entity storage is undefined.
            Array<EntityHandle> roots;
            scene.ForEachEntity(
                [&](EntityHandle e)
                {
                    if (!scene.GetParent(e).IsAssigned())
                    {
                        roots.PushBack(e);
                    }
                });
            for (EntityHandle root : roots)
            {
                scene.DestroyEntity(root);
            }
            scene.ClearPrefabInstances(); // the snapshot's Expanded section repopulates state

            MemoryStream buffer;
            (void)buffer.Write(m_blob.Data(), m_blob.Size());
            (void)buffer.Seek(0, SeekOrigin::Begin);
            BinarySerializer ar(buffer, SerializeMode::Read);
            SerializeScene(ar, scene, nullptr, ScenePrefabMode::Expanded);
            if (!ar.IsOk())
            {
                return ar.GetStatus();
            }
            if (resources != nullptr)
            {
                ResolveSceneResources(scene, *resources);
            }
            return Status{};
        }

    private:
        Array<byte> m_blob;
    };

} // namespace draconic::scene
