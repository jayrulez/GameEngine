// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Net.Replication - implementation unit: the field codec and the cached
// replicated-property layout harvest. They are kept out of the interface unit so the static layout
// cache and the type-dispatch table live in one translation unit.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module foundation.net.replication;

import foundation.core;
import foundation.net;
import foundation.scene;
import foundation.script.facades; // ComponentOf<T>, RegisterExtra* (the script `.of` surface)

using namespace foundation::core;
namespace scene = foundation::scene;

namespace foundation::net
{

    namespace
    {
        // Reflected property and type names and attribute keys are ASCII const char*; borrow them as
        // UTF-8 views (StringView is char8_t-based; the reflection layer stores and compares keys the
        // same way).
        [[nodiscard]] StringView Ascii(const char* s) noexcept
        {
            return s != nullptr ? StringView(reinterpret_cast<const char8_t*>(s)) : StringView{};
        }
        [[nodiscard]] const Attribute* FindReplicatedMark(const PropertyInfo& property) noexcept
        {
            return FindAttribute(property, Ascii(kReplicatedAttribute));
        }

        // Length-prefixed UTF-8 on the wire (the component type tag, SerializationTypeId).
        void WriteWireString(BitWriter& writer, StringView s)
        {
            writer.WriteVarU32(static_cast<u32>(s.Size()));
            writer.WriteBytes(Span<const byte>(reinterpret_cast<const byte*>(s.Data()), s.Size()));
        }
        [[nodiscard]] String ReadWireString(BitReader& reader)
        {
            const u32 n = reader.ReadVarU32();
            if (n == 0 || !reader.Ok())
            {
                return String{};
            }
            Array<byte> buf;
            buf.Resize(n);
            reader.ReadBytes(Span<byte>(buf.Data(), buf.Size()));
            return String(StringView(reinterpret_cast<const char8_t*>(buf.Data()), n));
        }

        // FNV-1a over the component's on-disk type id: the baseline key (the same hash as RpcTable::Hash).
        [[nodiscard]] u32 HashTypeId(StringView s) noexcept
        {
            u32 h = 2166136261u;
            for (usize i = 0; i < s.Size(); ++i)
            {
                h ^= static_cast<u8>(s.Data()[i]);
                h *= 16777619u;
            }
            return h;
        }
        [[nodiscard]] bool BlobsEqual(const Array<byte>& a, Span<const byte> b) noexcept
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
        [[nodiscard]] Array<byte> CopyBlob(Span<const byte> b)
        {
            Array<byte> out;
            out.Resize(b.Size());
            for (usize i = 0; i < b.Size(); ++i)
            {
                out[i] = b[i];
            }
            return out;
        }

        // Entity-record flags (shared by the snapshot and delta payloads).
        constexpr u8 kFlagRemoved = 1u << 0; // entity despawned: destroy locally, no fields follow
        constexpr u8 kFlagSpawn = 1u << 1;   // first delivery to this peer: a prefab Guid follows

        void WriteGuid(BitWriter& w, const Guid& g)
        {
            w.WriteU64(g.high);
            w.WriteU64(g.low);
        }
        [[nodiscard]] Guid ReadGuid(BitReader& r)
        {
            Guid g;
            g.high = r.ReadU64();
            g.low = r.ReadU64();
            return g;
        }

        // True if the component type has at least one replicated field worth interpolating (transform
        // like). Purely discrete components are applied directly, never buffered or smoothed.
        [[nodiscard]] bool HasInterpolatableField(const TypeInfo& type)
        {
            for (const PropertyInfo* p : ReplicatedProperties(type))
            {
                if (IsInterpolatableType(p->type))
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool IsFieldTypeSupported(const TypeInfo* type) noexcept
    {
        if (type == nullptr)
        {
            return false;
        }
        return type == &TypeOf<bool>() || type == &TypeOf<f32>() || type == &TypeOf<f64>() ||
               type == &TypeOf<i8>() || type == &TypeOf<u8>() || type == &TypeOf<i16>() ||
               type == &TypeOf<u16>() || type == &TypeOf<i32>() || type == &TypeOf<u32>() ||
               type == &TypeOf<i64>() || type == &TypeOf<u64>() || type == &TypeOf<Float2>() ||
               type == &TypeOf<Float3>() || type == &TypeOf<Float4>() ||
               type == &TypeOf<Quaternion>();
    }

    bool IsReplicated(const PropertyInfo& property)
    {
        return FindReplicatedMark(property) != nullptr && IsFieldTypeSupported(property.type);
    }

    Span<const PropertyInfo* const> ReplicatedProperties(const TypeInfo& type)
    {
        // Single-threaded (fixed lane). Cache the harvested layout per type; the const PropertyInfo*
        // are stable (they live in the type's static TypeData).
        static HashMap<const TypeInfo*, Array<const PropertyInfo*>> cache;
        if (const Array<const PropertyInfo*>* found = cache.Find(&type))
        {
            return Span<const PropertyInfo* const>{found->Data(), found->Size()};
        }

        Array<const PropertyInfo*> layout;
        for (const PropertyInfo& property : Properties(type))
        {
            if (FindReplicatedMark(property) == nullptr)
            {
                continue;
            }
            if (!IsFieldTypeSupported(property.type))
            {
                LOG_WARNING(u8"Net",
                                     u8"replicated field '{}' on '{}' has an unsupported type - "
                                     u8"excluded from replication",
                                     Ascii(property.name), Ascii(type.name));
                continue;
            }
            layout.PushBack(&property);
        }
        cache.InsertOrAssign(&type, Move(layout));
        const Array<const PropertyInfo*>& stored = *cache.Find(&type);
        return Span<const PropertyInfo* const>{stored.Data(), stored.Size()};
    }

    bool WriteFieldValue(BitWriter& writer, const Variant& value)
    {
        if (const bool* v = value.TryGet<bool>())
        {
            writer.WriteBool(*v);
            return true;
        }
        if (const f32* v = value.TryGet<f32>())
        {
            writer.WriteFloat(*v);
            return true;
        }
        if (const f64* v = value.TryGet<f64>())
        {
            u64 bits = 0;
            MemCopy(&bits, v, sizeof(bits));
            writer.WriteU64(bits);
            return true;
        }
        if (const i8* v = value.TryGet<i8>())
        {
            writer.WriteU8(static_cast<u8>(*v));
            return true;
        }
        if (const u8* v = value.TryGet<u8>())
        {
            writer.WriteU8(*v);
            return true;
        }
        if (const i16* v = value.TryGet<i16>())
        {
            writer.WriteU16(static_cast<u16>(*v));
            return true;
        }
        if (const u16* v = value.TryGet<u16>())
        {
            writer.WriteU16(*v);
            return true;
        }
        if (const i32* v = value.TryGet<i32>())
        {
            writer.WriteI32(*v);
            return true;
        }
        if (const u32* v = value.TryGet<u32>())
        {
            writer.WriteU32(*v);
            return true;
        }
        if (const i64* v = value.TryGet<i64>())
        {
            writer.WriteU64(static_cast<u64>(*v));
            return true;
        }
        if (const u64* v = value.TryGet<u64>())
        {
            writer.WriteU64(*v);
            return true;
        }
        if (const Float2* v = value.TryGet<Float2>())
        {
            writer.WriteFloat(v->x);
            writer.WriteFloat(v->y);
            return true;
        }
        if (const Float3* v = value.TryGet<Float3>())
        {
            writer.WriteFloat(v->x);
            writer.WriteFloat(v->y);
            writer.WriteFloat(v->z);
            return true;
        }
        if (const Float4* v = value.TryGet<Float4>())
        {
            writer.WriteFloat(v->x);
            writer.WriteFloat(v->y);
            writer.WriteFloat(v->z);
            writer.WriteFloat(v->w);
            return true;
        }
        if (const Quaternion* v = value.TryGet<Quaternion>())
        {
            writer.WriteFloat(v->x);
            writer.WriteFloat(v->y);
            writer.WriteFloat(v->z);
            writer.WriteFloat(v->w);
            return true;
        }
        return false;
    }

    bool ReadFieldValue(BitReader& reader, const TypeInfo* type, Variant& out)
    {
        if (type == &TypeOf<bool>())
        {
            out = Variant::From<bool>(reader.ReadBool());
            return true;
        }
        if (type == &TypeOf<f32>())
        {
            out = Variant::From<f32>(reader.ReadFloat());
            return true;
        }
        if (type == &TypeOf<f64>())
        {
            const u64 bits = reader.ReadU64();
            f64 v = 0.0;
            MemCopy(&v, &bits, sizeof(v));
            out = Variant::From<f64>(v);
            return true;
        }
        if (type == &TypeOf<i8>())
        {
            out = Variant::From<i8>(static_cast<i8>(reader.ReadU8()));
            return true;
        }
        if (type == &TypeOf<u8>())
        {
            out = Variant::From<u8>(reader.ReadU8());
            return true;
        }
        if (type == &TypeOf<i16>())
        {
            out = Variant::From<i16>(static_cast<i16>(reader.ReadU16()));
            return true;
        }
        if (type == &TypeOf<u16>())
        {
            out = Variant::From<u16>(reader.ReadU16());
            return true;
        }
        if (type == &TypeOf<i32>())
        {
            out = Variant::From<i32>(reader.ReadI32());
            return true;
        }
        if (type == &TypeOf<u32>())
        {
            out = Variant::From<u32>(reader.ReadU32());
            return true;
        }
        if (type == &TypeOf<i64>())
        {
            out = Variant::From<i64>(static_cast<i64>(reader.ReadU64()));
            return true;
        }
        if (type == &TypeOf<u64>())
        {
            out = Variant::From<u64>(reader.ReadU64());
            return true;
        }
        if (type == &TypeOf<Float2>())
        {
            Float2 v;
            v.x = reader.ReadFloat();
            v.y = reader.ReadFloat();
            out = Variant::From<Float2>(v);
            return true;
        }
        if (type == &TypeOf<Float3>())
        {
            Float3 v;
            v.x = reader.ReadFloat();
            v.y = reader.ReadFloat();
            v.z = reader.ReadFloat();
            out = Variant::From<Float3>(v);
            return true;
        }
        if (type == &TypeOf<Float4>())
        {
            Float4 v;
            v.x = reader.ReadFloat();
            v.y = reader.ReadFloat();
            v.z = reader.ReadFloat();
            v.w = reader.ReadFloat();
            out = Variant::From<Float4>(v);
            return true;
        }
        if (type == &TypeOf<Quaternion>())
        {
            Quaternion v;
            v.x = reader.ReadFloat();
            v.y = reader.ReadFloat();
            v.z = reader.ReadFloat();
            v.w = reader.ReadFloat();
            out = Variant::From<Quaternion>(v);
            return true;
        }
        return false;
    }

    usize WriteReplicatedState(BitWriter& writer, const Instance& instance)
    {
        if (instance.Type() == nullptr)
        {
            return 0;
        }
        usize written = 0;
        for (const PropertyInfo* property : ReplicatedProperties(*instance.Type()))
        {
            const Variant value = GetProperty(*property, instance);
            if (WriteFieldValue(writer, value))
            {
                ++written;
            }
        }
        return written;
    }

    usize ReadReplicatedState(BitReader& reader, const Instance& instance)
    {
        if (instance.Type() == nullptr)
        {
            return 0;
        }
        usize applied = 0;
        for (const PropertyInfo* property : ReplicatedProperties(*instance.Type()))
        {
            Variant value;
            if (!ReadFieldValue(reader, property->type, value))
            {
                break;
            }
            if (!reader.Ok())
            {
                break;
            } // ran past the end: do not apply a garbage read; the caller sees the short count
            (void)SetProperty(*property, instance, value);
            ++applied;
        }
        return applied;
    }

    // ---- NetworkComponent reflection (versioned records need the patched TypeInfo) ---------------

    REFLECT_ENUM(NetworkAuthority, "rtti::net")
    {
        builder.Value("Server", NetworkAuthority::Server);
        builder.Value("Client", NetworkAuthority::Client);
    }

    REFLECT_VALUE(NetworkComponent, "rtti::net")
    {
        builder.Attribute("displayName", String(u8"Network Identity"))
            .Attribute("category", String(u8"Networking"))
            .Attribute("description",
                       String(u8"Marks the entity as networked, giving it a network id and an "
                              u8"owner, server or client."))
            .DataVersion(1);
        // Script: NetworkComponent.of(entity) reads `authority` (whether the server or a client owns
        // this entity) for authority-gated gameplay, and `id` for logging. They are read-only:
        // replication owns them, and none of them is itself replicated, so a script assigning
        // `authority` would silently desynchronise the two sides. `prefab` is replication's
        // bookkeeping and is not exposed.
        builder.Method<&foundation::script::ComponentOf<NetworkComponent>, NetworkComponent>("of");
        builder.Property<&NetworkComponent::id>("id", PropertyFlags::ReadOnly);
        builder.Property<&NetworkComponent::authority>("authority", PropertyFlags::ReadOnly);
        builder.Property<&NetworkComponent::owner>("owner", PropertyFlags::ReadOnly); // whose: 0 = the host
    }

    // NetworkedTransform: unlike NetworkComponent, its fields are replicated (kReplicatedAttribute), so
    // the field codec captures and applies them and interpolation smooths them.
    REFLECT_VALUE(NetworkedTransform, "rtti::net")
    {
        builder.Attribute("displayName", String(u8"Networked Transform"))
            .Attribute("category", String(u8"Networking"))
            .Attribute("description",
                       String(u8"Sends the entity's position, rotation and scale over the "
                              u8"network, smoothed on clients."))
            .DataVersion(1);
        builder.Property<&NetworkedTransform::position>("position")
            .PropAttribute(kReplicatedAttribute, true);
        builder.Property<&NetworkedTransform::rotation>("rotation")
            .PropAttribute(kReplicatedAttribute, true);
        builder.Property<&NetworkedTransform::scale>("scale").PropAttribute(kReplicatedAttribute,
                                                                            true);
    }

    void CaptureEntityTransforms(scene::Scene& scene)
    {
        auto* mgr = scene.GetSystem<NetworkedTransformComponentManager>();
        if (mgr == nullptr)
        {
            return;
        }
        mgr->ForEach(
            [&](NetworkedTransform& nt, scene::EntityHandle e)
            {
                // An effectively inactive entity's replicated state freezes (no capture). It stays in
                // snapshots, since existence and identity are not simulation; the flag itself is not
                // replicated.
                if (!scene.IsEffectivelyActive(e))
                {
                    return;
                }
                const Transform t = scene.GetLocalTransform(e);
                nt.position = t.position;
                nt.rotation = t.rotation;
                nt.scale = t.scale;
            });
    }

    void ApplyEntityTransforms(scene::Scene& scene)
    {
        auto* mgr = scene.GetSystem<NetworkedTransformComponentManager>();
        if (mgr == nullptr)
        {
            return;
        }
        mgr->ForEach(
            [&](NetworkedTransform& nt, scene::EntityHandle e)
            {
                if (!scene.IsEffectivelyActive(e))
                {
                    return; // frozen locally too (the flag is scene data on both sides)
                }
                Transform t;
                t.position = nt.position;
                t.rotation = nt.rotation;
                t.scale = nt.scale;
                scene.SetLocalTransform(e, t);
            });
    }

    void RegisterReplicationComponents()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_NetworkAuthority();
            RttiRegisterValue_NetworkComponent();
            GlobalTypeRegistry().Register(TypeOf<NetworkComponent>());
            RttiRegisterValue_NetworkedTransform();
            GlobalTypeRegistry().Register(TypeOf<NetworkedTransform>());
            return true;
        }();
        (void)once;
    }

    void RegisterNetworkComponentScriptFacade()
    {
        RegisterReplicationComponents(); // build the TypeData (including NetworkComponent's `of`) first
        // Surface NetworkComponent to script (NetworkComponent.of(entity).authority): register it, seed
        // it as an emission root and name it for the behavior prelude, and register the NetworkAuthority
        // enum so `authority` can be read and compared. NetworkedTransform is replication plumbing and
        // is not exposed to scripts.
        GlobalTypeRegistry().Register(TypeOf<NetworkAuthority>());
        GlobalTypeRegistry().Register(TypeOf<NetworkComponent>());
        foundation::script::RegisterExtraScriptRootType(&TypeOf<NetworkComponent>());
        foundation::script::RegisterExtraFacadeName(u8"NetworkComponent");
        // NetworkAuthority (an enum) is not facade-named. Some backends do not emit enum classes, so
        // `import ... for NetworkAuthority` in the behavior prelude would fail to resolve and break the
        // compile of every behavior. The enum crosses as its underlying int in script; AngelScript binds
        // it by registry, so no prelude name is needed.
    }

    // ---- StateReplication -----------------------------------------------------------------------

    void StateReplication::SetSpawnHandler(SpawnHandler handler) { m_spawnHandler = Move(handler); }
    void StateReplication::SetRelevance(RelevanceFn fn) { m_relevance = Move(fn); }

    NetworkId StateReplication::AssignNetworkId(scene::Scene& scene, scene::EntityHandle entity,
                                                const Guid& prefab, u32 owner)
    {
        auto* netMgr = scene.GetSystem<NetworkComponentManager>();
        if (netMgr == nullptr)
        {
            return NetworkId::Invalid();
        }
        NetworkComponent& nc = netMgr->Has(entity) ? *netMgr->Get(entity) : netMgr->Add(entity);
        if (!nc.id.IsValid())
        {
            nc.id = NetworkId{++m_nextNetworkId};
        } // 0 stays "unassigned"
        if (!prefab.IsNil())
        {
            nc.prefab = prefab;
        }
        nc.owner = owner;
        m_netIdToEntity.InsertOrAssign(nc.id.value, entity);
        return nc.id;
    }

    namespace
    {
        // A stable NetworkId derived from an entity's authored Guid: both peers load the same scene, so
        // they compute the same id for the same entity without hand-authored ids. Never 0 (0 =
        // unassigned). Collisions are astronomically unlikely at a scene's entity count.
        inline NetworkId DeterministicNetworkId(const Guid& g) noexcept
        {
            u64 h = g.high ^ (g.low * 0x9E3779B97F4A7C15ull);
            h ^= h >> 32;
            const u32 v = static_cast<u32>(h);
            return NetworkId{v == 0u ? 1u : v};
        }
    }

    void StateReplication::AssignSceneNetworkIds(scene::Scene& scene)
    {
        auto* netMgr = scene.GetSystem<NetworkComponentManager>();
        if (netMgr == nullptr)
        {
            return;
        }
        // Safe during ForEach: only the existing NetworkComponents are mutated (no structural change).
        netMgr->ForEach(
            [&](NetworkComponent& nc, scene::EntityHandle e)
            {
                if (!nc.id.IsValid())
                {
                    nc.id = DeterministicNetworkId(scene.GetEntityId(e));
                }
                m_netIdToEntity.InsertOrAssign(nc.id.value, e);
                if (nc.id.value > m_nextNetworkId)
                {
                    m_nextNetworkId = nc.id.value;
                } // keep mints clear
            });
    }

    void StateReplication::RegisterAuthoredEntities(scene::Scene& scene)
    {
        auto* netMgr = scene.GetSystem<NetworkComponentManager>();
        if (netMgr == nullptr)
        {
            return;
        }
        // Compute the same Guid-derived id the server did, so incoming replication resolves to the
        // client's own authored entity (not a duplicate).
        netMgr->ForEach(
            [&](NetworkComponent& nc, scene::EntityHandle e)
            {
                if (!nc.id.IsValid())
                {
                    nc.id = DeterministicNetworkId(scene.GetEntityId(e));
                }
                m_netIdToEntity.InsertOrAssign(nc.id.value, e);
            });
    }

    scene::EntityHandle StateReplication::FindEntity(NetworkId id) const
    {
        if (const scene::EntityHandle* found = m_netIdToEntity.Find(id.value))
        {
            return *found;
        }
        return scene::EntityHandle::Invalid();
    }

    scene::EntityHandle StateReplication::FindOrCreateEntity(scene::Scene& scene, u32 networkId,
                                                             const Guid& prefab, u32 owner, bool spawn)
    {
        if (const scene::EntityHandle* found = m_netIdToEntity.Find(networkId))
        {
            if (scene.IsValid(*found))
            {
                return *found;
            }
        }
        // A spawn record with a prefab id and an installed handler gives a full prefab instance; otherwise
        // a bare entity.
        scene::EntityHandle e = scene::EntityHandle::Invalid();
        if (spawn && !prefab.IsNil() && m_spawnHandler)
        {
            e = m_spawnHandler(scene, prefab, NetworkId{networkId});
        }
        if (!e.IsAssigned())
        {
            e = scene.CreateEntity();
        }
        if (auto* netMgr = scene.GetSystem<NetworkComponentManager>())
        {
            NetworkComponent& nc = netMgr->Has(e) ? *netMgr->Get(e) : netMgr->Add(e);
            nc.id = NetworkId{networkId};
            nc.authority =
                NetworkAuthority::Server; // the client's view: the server owns this entity
            nc.prefab = prefab;
            if (spawn)
            {
                nc.owner = owner; // whose it is (a later update carries no spawn record)
            }
        }
        m_netIdToEntity.InsertOrAssign(networkId, e);
        return e;
    }

    namespace
    {
        // One component's networked state for `entity`, as the bytes the wire carries: what a
        // manager with state of its own writes (IReplicatedStateSource), else its reflected fields
        // marked replicated. False when it contributes nothing (no state, no wire tag).
        bool CaptureComponentState(scene::ComponentManagerBase& m, scene::EntityHandle entity, Array<byte>& out)
        {
            if (!m.IsSerializable() || m.SerializationTypeId().IsEmpty())
            {
                return false; // need a wire tag
            }
            if (scene::IReplicatedStateSource* source = m.AsReplicatedStateSource())
            {
                return m.HasComponent(entity) && source->WriteReplicatedState(entity, out);
            }
            const Instance inst = m.GetComponentInstance(entity);
            if (inst.Type() == nullptr || ReplicatedProperties(*inst.Type()).IsEmpty())
            {
                return false;
            }
            BitWriter fields;
            (void)WriteReplicatedState(fields, inst);
            const Span<const byte> blob = fields.Data();
            out.Clear();
            out.Reserve(blob.Size());
            for (const byte b : blob)
            {
                out.PushBack(b);
            }
            return true;
        }
    }

    void StateReplication::CaptureSnapshot(scene::Scene& scene, BitWriter& out)
    {
        auto* netMgr = scene.GetSystem<NetworkComponentManager>();
        if (netMgr == nullptr)
        {
            out.WriteVarU32(0);
            return;
        }

        // Snapshot the assigned networked entities from the tag pool.
        struct Ent
        {
            u32 id;
            Guid prefab;
            u32 owner;
            scene::EntityHandle handle;
        };
        Array<Ent> entities;
        netMgr->ForEach(
            [&](NetworkComponent& nc, scene::EntityHandle e)
            {
                if (nc.id.IsValid())
                {
                    entities.PushBack(Ent{nc.id.value, nc.prefab, nc.owner, e});
                }
            });

        out.WriteVarU32(static_cast<u32>(entities.Size()));
        for (const Ent& ent : entities)
        {
            // A full snapshot sends everything as a spawn: a fresh (late-joining) client reconstructs
            // each entity via its prefab id, then applies the state on top.
            out.WriteU32(ent.id);
            out.WriteU8(kFlagSpawn);
            WriteGuid(out, ent.prefab);
            out.WriteVarU32(ent.owner);
            // Gather this entity's serializable components that carry replicated fields.
            struct Captured
            {
                StringView typeId;
                Array<byte> blob;
            };
            Array<Captured> comps;
            scene.ForEachManager(
                [&](scene::ComponentManagerBase& m)
                {
                    Array<byte> blob;
                    if (CaptureComponentState(m, ent.handle, blob))
                    {
                        comps.PushBack(Captured{m.SerializationTypeId(), Move(blob)});
                    }
                });
            out.WriteVarU32(static_cast<u32>(comps.Size()));
            for (const Captured& comp : comps)
            {
                // Length-prefix each component blob so a peer lacking the type can skip it (forward compatibility).
                WriteWireString(out, comp.typeId);
                out.WriteVarU32(static_cast<u32>(comp.blob.Size()));
                out.WriteBytes(comp.blob.AsSpan());
            }
        }
    }

    void StateReplication::ApplyComponentRecords(scene::Scene& scene, scene::EntityHandle entity,
                                                 NetworkId id, u32 count, BitReader& in,
                                                 InterpolationBuffer* interp, f64 timestampMs)
    {
        for (u32 j = 0; j < count && in.Ok(); ++j)
        {
            const String typeId = ReadWireString(in);
            const u32 blobBytes = in.ReadVarU32();
            Array<byte> blob;
            blob.Resize(blobBytes);
            if (blobBytes > 0)
            {
                in.ReadBytes(Span<byte>(blob.Data(), blob.Size()));
            }
            if (!in.Ok())
            {
                break;
            }

            scene::ComponentManagerBase* m = scene.FindManagerBySerializationId(typeId.AsView());
            if (m == nullptr)
            {
                continue;
            } // unknown type on this peer: the blob is already consumed (skip)
            if (scene::IReplicatedStateSource* source = m->AsReplicatedStateSource())
            {
                // Its own state, its own format: only for an entity that has the component (the
                // prefab spawn brought it; a bare entity has nothing for it to apply to).
                if (m->HasComponent(entity))
                {
                    source->ReadReplicatedState(entity, Span<const byte>(blob.Data(), blob.Size()));
                }
                continue;
            }
            if (m->GetComponentInstance(entity).Type() == nullptr)
            {
                (void)m->AddDefaultComponent(entity);
            }
            const Instance inst = m->GetComponentInstance(entity);
            if (inst.Type() == nullptr)
            {
                continue;
            }
            BitReader fields(Span<const byte>(blob.Data(), blob.Size()));
            (void)ReadReplicatedState(fields, inst);

            // Buffer components with interpolatable fields for smooth playback; apply others directly.
            if (interp != nullptr && HasInterpolatableField(*inst.Type()))
            {
                interp->Record(id, HashTypeId(typeId.AsView()), timestampMs, inst);
            }
        }
    }

    void StateReplication::ApplyEntries(scene::Scene& scene, BitReader& in,
                                        InterpolationBuffer* interp, f64 timestampMs)
    {
        // The payload snapshots and deltas share: count, then per entity { id, flags, [prefab if spawn],
        // [componentCount and records unless removed] }.
        const u32 entryCount = in.ReadVarU32();
        for (u32 i = 0; i < entryCount && in.Ok(); ++i)
        {
            const u32 networkId = in.ReadU32();
            const u8 flags = in.ReadU8();
            if (!in.Ok())
            {
                break;
            }

            if ((flags & kFlagRemoved) != 0u)
            { // despawn
                if (const scene::EntityHandle* h = m_netIdToEntity.Find(networkId))
                {
                    if (scene.IsValid(*h))
                    {
                        scene.DestroyEntity(*h);
                    }
                }
                (void)m_netIdToEntity.Remove(networkId);
                if (interp != nullptr)
                {
                    interp->Forget(NetworkId{networkId});
                }
                continue;
            }

            const bool spawn = (flags & kFlagSpawn) != 0u;
            const Guid prefab = spawn ? ReadGuid(in) : Guid{};
            const u32 owner = spawn ? in.ReadVarU32() : 0u; // the spawn record: prefab, then owner
            if (!in.Ok())
            {
                break;
            }
            const scene::EntityHandle entity = FindOrCreateEntity(scene, networkId, prefab, owner, spawn);
            const u32 componentCount = in.ReadVarU32();
            ApplyComponentRecords(scene, entity, NetworkId{networkId}, componentCount, in, interp,
                                  timestampMs);
        }
    }

    // The full snapshot (late join) and the per-peer delta share the entry payload; they differ only in
    // what the capture side emits (everything as spawns, or only changes), so applying is one path.
    void StateReplication::ApplySnapshot(scene::Scene& scene, BitReader& in)
    {
        ApplyEntries(scene, in, nullptr, 0.0);
    }
    void StateReplication::ApplyDelta(scene::Scene& scene, BitReader& in)
    {
        ApplyEntries(scene, in, nullptr, 0.0);
    }
    void StateReplication::ApplyDelta(scene::Scene& scene, BitReader& in,
                                      InterpolationBuffer& interp, f64 timestampMs)
    {
        ApplyEntries(scene, in, &interp, timestampMs);
    }

    void StateReplication::SampleInterpolation(scene::Scene& scene, InterpolationBuffer& interp,
                                               f64 renderTimeMs) const
    {
        auto* netMgr = scene.GetSystem<NetworkComponentManager>();
        if (netMgr == nullptr)
        {
            return;
        }
        netMgr->ForEach(
            [&](NetworkComponent& nc, scene::EntityHandle e)
            {
                if (!nc.id.IsValid() || !scene.IsEffectivelyActive(e))
                {
                    return; // inactive: no interpolation sampling (state is frozen)
                }
                scene.ForEachManager(
                    [&](scene::ComponentManagerBase& m)
                    {
                        const Instance inst = m.GetComponentInstance(e);
                        if (inst.Type() == nullptr)
                        {
                            return;
                        }
                        if (!m.IsSerializable() || m.SerializationTypeId().IsEmpty())
                        {
                            return;
                        }
                        if (!HasInterpolatableField(*inst.Type()))
                        {
                            return;
                        }
                        (void)interp.Sample(nc.id, HashTypeId(m.SerializationTypeId()),
                                            renderTimeMs, inst);
                    });
            });
    }

    void StateReplication::ForgetPeer(u32 peerId) { (void)m_peerBaselines.Remove(peerId); }

    usize StateReplication::CaptureDelta(scene::Scene& scene, u32 peerId, BitWriter& out)
    {
        auto* netMgr = scene.GetSystem<NetworkComponentManager>();
        if (netMgr == nullptr)
        {
            out.WriteVarU32(0);
            return 0;
        }

        // Get-or-insert this peer's baseline.
        if (m_peerBaselines.Find(peerId) == nullptr)
        {
            m_peerBaselines.InsertOrAssign(peerId, PeerBaseline{});
        }
        PeerBaseline& base = *m_peerBaselines.Find(peerId);

        // One delta entry: a changed or new entity (its changed component records) or a removal. A new
        // entity (absent from the baseline) is a spawn and carries its prefab id so the client can
        // network-spawn it.
        struct CompRecord
        {
            StringView typeId;
            Array<byte> blob;
        };
        struct Entry
        {
            u32 id = 0;
            bool removed = false;
            bool spawn = false;
            Guid prefab{};
            u32 owner = 0;
            Array<CompRecord> comps;
        };
        Array<Entry> entries;
        HashMap<u32, EntityBaseline> nextBaseline; // becomes the baseline after this capture
        HashMap<u32, u8>
            present; // all present networked ids (despawn detection, relevance-independent)

        netMgr->ForEach(
            [&](NetworkComponent& nc, scene::EntityHandle e)
            {
                if (!nc.id.IsValid())
                {
                    return;
                }
                const u32 nid = nc.id.value;
                present.InsertOrAssign(nid, u8{1});
                const EntityBaseline* oldEb = base.entities.Find(nid);

                // Relevancy (fog of war): an irrelevant entity is never sent, and if the peer has it (in its
                // baseline) it is removed (the client destroys it, leaving no hidden state in memory).
                if (m_relevance && !m_relevance(peerId, NetworkId{nid}, e))
                {
                    if (oldEb != nullptr)
                    {
                        entries.PushBack(Entry{nid, true, false, Guid{}, 0, {}});
                    }
                    return; // do not add to nextBaseline: the peer must not know this entity
                }

                EntityBaseline newEb;
                Array<CompRecord> changed;
                scene.ForEachManager(
                    [&](scene::ComponentManagerBase& m)
                    {
                        Array<byte> state;
                        if (!CaptureComponentState(m, e, state))
                        {
                            return;
                        }
                        const Span<const byte> blob = state.AsSpan();
                        const u32 typeHash = HashTypeId(m.SerializationTypeId());

                        // Changed if the component is new to this peer or its bytes differ from the last sent.
                        const Array<byte>* prev = nullptr;
                        if (oldEb != nullptr)
                        {
                            for (const ComponentBaseline& cb : oldEb->components)
                            {
                                if (cb.typeHash == typeHash)
                                {
                                    prev = &cb.blob;
                                    break;
                                }
                            }
                        }
                        if (prev == nullptr || !BlobsEqual(*prev, blob))
                        {
                            changed.PushBack(CompRecord{m.SerializationTypeId(), CopyBlob(blob)});
                        }
                        newEb.components.PushBack(ComponentBaseline{typeHash, CopyBlob(blob)});
                    });

                if (oldEb == nullptr || !changed.IsEmpty())
                { // new entity (spawn), or something changed
                    entries.PushBack(
                        Entry{nid, false, /*spawn=*/oldEb == nullptr, nc.prefab, nc.owner, Move(changed)});
                }
                nextBaseline.InsertOrAssign(nid, Move(newEb));
            });

        // Removals: baseline entities no longer present in the scene (true despawns). Relevance-based
        // removals were emitted above; those entities still exist (they are in `present`), so this loop
        // skips them and nothing is removed twice.
        for (const auto& entry : base.entities)
        {
            if (!present.Contains(entry.key))
            {
                entries.PushBack(Entry{entry.key, true, false, Guid{}, 0, {}});
            }
        }

        out.WriteVarU32(static_cast<u32>(entries.Size()));
        for (const Entry& entry : entries)
        {
            out.WriteU32(entry.id);
            u8 flags = 0;
            if (entry.removed)
            {
                flags |= kFlagRemoved;
            }
            if (entry.spawn)
            {
                flags |= kFlagSpawn;
            }
            out.WriteU8(flags);
            if (entry.removed)
            {
                continue;
            }
            if (entry.spawn)
            {
                WriteGuid(out, entry.prefab);
                out.WriteVarU32(entry.owner);
            }
            out.WriteVarU32(static_cast<u32>(entry.comps.Size()));
            for (const CompRecord& comp : entry.comps)
            {
                WriteWireString(out, comp.typeId);
                out.WriteVarU32(static_cast<u32>(comp.blob.Size()));
                out.WriteBytes(Span<const byte>(comp.blob.Data(), comp.blob.Size()));
            }
        }

        base.entities = Move(nextBaseline); // commit the last sent (reliable and ordered, so delivered)
        return entries.Size();
    }

}
