// Draconic::NetReplication - implementation unit: the field codec + the cached replicated-property
// layout harvest. Free functions (no reflect bodies), but kept out of the interface unit so the
// static layout cache + the type-dispatch table live in one TU.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.net.replication;

import draconic.foundation;
import draconic.net;
import draconic.scene;
import draconic.script.facades; // ComponentOf<T> + RegisterExtra* (the script `.of` surface, Track A)

using namespace draconic::foundation;

namespace draconic::net
{

    namespace
    {
        // Reflected property/type names + attribute keys are ASCII const char*; borrow them as UTF-8
        // views (StringView is char8_t-based; the reflection layer stores/compares keys the same way).
        [[nodiscard]] StringView Ascii(const char* s) noexcept
        {
            return s != nullptr ? StringView(reinterpret_cast<const char8_t*>(s)) : StringView{};
        }
        [[nodiscard]] const Attribute* FindReplicatedMark(const PropertyInfo& property) noexcept
        {
            return FindAttribute(property, Ascii(kReplicatedAttribute));
        }

        // Length-prefixed UTF-8 on the wire (the component type tag - SerializationTypeId).
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

        // FNV-1a over the component's on-disk type id - the baseline key (same family as RpcTable::Hash).
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

        // Entity-record flags (shared by the snapshot + delta payloads).
        constexpr u8 kFlagRemoved = 1u << 0; // entity despawned - destroy locally, no fields follow
        constexpr u8 kFlagSpawn = 1u << 1;   // first delivery to this peer - a prefab Guid follows

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

        // True if the component type has at least one replicated field worth interpolating (transform-
        // like). Pure-discrete components are applied directly, never buffered/smoothed.
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
                DRACONIC_LOG_WARNING(u8"Net",
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
            } // ran past the end - don't apply a garbage read; caller sees the short count
            (void)SetProperty(*property, instance, value);
            ++applied;
        }
        return applied;
    }

    // ---- NetworkComponent reflection (versioned records need the patched TypeInfo) ---------------

    DRACONIC_REFLECT_ENUM(NetworkAuthority, "draconic::net")
    {
        builder.Value("Server", NetworkAuthority::Server);
        builder.Value("Client", NetworkAuthority::Client);
    }

    DRACONIC_REFLECT_VALUE(NetworkComponent, "draconic::net")
    {
        builder.DataVersion(1);
        // Script (Track A): NetworkComponent.of(entity) -> read `authority` (is this entity server- or
        // client-owned) for authority-gated gameplay. The Net session facade stays app-global (Net.*).
        builder.Method<&draconic::script::ComponentOf<NetworkComponent>, NetworkComponent>("of");
        builder.Property<&NetworkComponent::id>("id");
        builder.Property<&NetworkComponent::authority>("authority");
        builder.Property<&NetworkComponent::prefab>("prefab");
    }

    // NetworkedTransform: unlike NetworkComponent, its fields ARE replicated (kReplicatedAttribute), so
    // the field codec captures/applies them and interpolation smooths them.
    DRACONIC_REFLECT_VALUE(NetworkedTransform, "draconic::net")
    {
        builder.DataVersion(1);
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
            DraconicRegisterEnum_NetworkAuthority();
            DraconicRegisterValue_NetworkComponent();
            GlobalTypeRegistry().Register(TypeOf<NetworkComponent>());
            DraconicRegisterValue_NetworkedTransform();
            GlobalTypeRegistry().Register(TypeOf<NetworkedTransform>());
            return true;
        }();
        (void)once;
    }

    void RegisterNetworkComponentScriptFacade()
    {
        RegisterReplicationComponents(); // build the TypeData (incl NetworkComponent's `of`) first
        // Surface NetworkComponent to script (NetworkComponent.of(entity).authority): register + seed
        // the Wren emission root + name it for the behavior prelude, plus the NetworkAuthority enum so
        // `authority` reads/compares. (NetworkedTransform is engine-managed replication plumbing, not a
        // script surface.)
        GlobalTypeRegistry().Register(TypeOf<NetworkAuthority>());
        GlobalTypeRegistry().Register(TypeOf<NetworkComponent>());
        draconic::script::RegisterExtraScriptRootType(&TypeOf<NetworkComponent>());
        draconic::script::RegisterExtraFacadeName(u8"NetworkComponent");
        // NOTE: NetworkAuthority (an enum) is deliberately NOT facade-named. Wren does not emit enum
        // classes, so `import ... for NetworkAuthority` in the behavior prelude would fail to resolve
        // and break EVERY behavior's compile. The enum crosses as its underlying int in Wren; in
        // AngelScript it binds by registry (named) - handled by each backend, no prelude name needed.
    }

    // ---- StateReplication -----------------------------------------------------------------------

    void StateReplication::SetSpawnHandler(SpawnHandler handler) { m_spawnHandler = Move(handler); }
    void StateReplication::SetRelevance(RelevanceFn fn) { m_relevance = Move(fn); }

    NetworkId StateReplication::AssignNetworkId(scene::Scene& scene, scene::EntityHandle entity,
                                                const Guid& prefab)
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
        m_netIdToEntity.InsertOrAssign(nc.id.value, entity);
        return nc.id;
    }

    namespace
    {
        // A stable NetworkId derived from an entity's authored Guid: both peers load the SAME scene, so
        // they compute the SAME id for the same entity WITHOUT any hand-authored ids. Never 0 (0 =
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
        // Compute the SAME Guid-derived id the server did, so incoming replication resolves to the
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
                                                             const Guid& prefab, bool spawn)
    {
        if (const scene::EntityHandle* found = m_netIdToEntity.Find(networkId))
        {
            if (scene.IsValid(*found))
            {
                return *found;
            }
        }
        // A spawn record with a prefab id + a wired handler => a full prefab instance; else a bare entity.
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
        }
        m_netIdToEntity.InsertOrAssign(networkId, e);
        return e;
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
            scene::EntityHandle handle;
        };
        Array<Ent> entities;
        netMgr->ForEach(
            [&](NetworkComponent& nc, scene::EntityHandle e)
            {
                if (nc.id.IsValid())
                {
                    entities.PushBack(Ent{nc.id.value, nc.prefab, e});
                }
            });

        out.WriteVarU32(static_cast<u32>(entities.Size()));
        for (const Ent& ent : entities)
        {
            // A full snapshot is "everything, as a spawn" - a fresh (late-joining) client reconstructs
            // each entity via its prefab id, then applies the state on top.
            out.WriteU32(ent.id);
            out.WriteU8(kFlagSpawn);
            WriteGuid(out, ent.prefab);
            // Gather this entity's serializable components that carry replicated fields.
            Array<scene::ComponentManagerBase*> comps;
            scene.ForEachManager(
                [&](scene::ComponentManagerBase& m)
                {
                    const Instance inst = m.GetComponentInstance(ent.handle);
                    if (inst.Type() == nullptr)
                    {
                        return;
                    }
                    if (ReplicatedProperties(*inst.Type()).IsEmpty())
                    {
                        return;
                    }
                    if (!m.IsSerializable() || m.SerializationTypeId().IsEmpty())
                    {
                        return;
                    } // need a wire tag
                    comps.PushBack(&m);
                });
            out.WriteVarU32(static_cast<u32>(comps.Size()));
            for (scene::ComponentManagerBase* m : comps)
            {
                // Length-prefix each component blob so a peer lacking the type can skip it (forward-compat).
                BitWriter fields;
                (void)WriteReplicatedState(fields, m->GetComponentInstance(ent.handle));
                const Span<const byte> blob = fields.Data();
                WriteWireString(out, m->SerializationTypeId());
                out.WriteVarU32(static_cast<u32>(blob.Size()));
                out.WriteBytes(blob);
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
            } // unknown type on this peer - blob already consumed (skip)
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

            // Buffer components with interpolatable fields for smooth playback; others stay direct-applied.
            if (interp != nullptr && HasInterpolatableField(*inst.Type()))
            {
                interp->Record(id, HashTypeId(typeId.AsView()), timestampMs, inst);
            }
        }
    }

    void StateReplication::ApplyEntries(scene::Scene& scene, BitReader& in,
                                        InterpolationBuffer* interp, f64 timestampMs)
    {
        // The unified snapshot/delta payload: count, then per entity { id, flags, [prefab if spawn],
        // [componentCount + records unless removed] }.
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
            if (!in.Ok())
            {
                break;
            }
            const scene::EntityHandle entity = FindOrCreateEntity(scene, networkId, prefab, spawn);
            const u32 componentCount = in.ReadVarU32();
            ApplyComponentRecords(scene, entity, NetworkId{networkId}, componentCount, in, interp,
                                  timestampMs);
        }
    }

    // Both the full snapshot (late-join) and the per-peer delta share the entry payload; the only
    // difference is what the CAPTURE side emits (all-spawn+full vs changed-only), so apply is one path.
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
                if (!nc.id.IsValid())
                {
                    return;
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

        // One delta entry: a changed/new entity (its changed component records) or a removal. A new
        // entity (absent from the baseline) is a SPAWN and carries its prefab id so the client can
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

                // Relevancy / fog-of-war: an irrelevant entity is NEVER sent, and if the peer currently has
                // it (in its baseline) it is REMOVED (client destroys it - no hidden state to memory-read).
                if (m_relevance && !m_relevance(peerId, NetworkId{nid}, e))
                {
                    if (oldEb != nullptr)
                    {
                        entries.PushBack(Entry{nid, true, false, Guid{}, {}});
                    }
                    return; // do not add to nextBaseline: the peer must not know this entity
                }

                EntityBaseline newEb;
                Array<CompRecord> changed;
                scene.ForEachManager(
                    [&](scene::ComponentManagerBase& m)
                    {
                        const Instance inst = m.GetComponentInstance(e);
                        if (inst.Type() == nullptr)
                        {
                            return;
                        }
                        if (ReplicatedProperties(*inst.Type()).IsEmpty())
                        {
                            return;
                        }
                        if (!m.IsSerializable() || m.SerializationTypeId().IsEmpty())
                        {
                            return;
                        }

                        BitWriter fields;
                        (void)WriteReplicatedState(fields, inst);
                        const Span<const byte> blob = fields.Data();
                        const u32 typeHash = HashTypeId(m.SerializationTypeId());

                        // Changed if the component is new to this peer or its bytes differ from last-sent.
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
                        Entry{nid, false, /*spawn=*/oldEb == nullptr, nc.prefab, Move(changed)});
                }
                nextBaseline.InsertOrAssign(nid, Move(newEb));
            });

        // Removals: baseline entities no longer PRESENT in the scene (true despawns). Relevance-based
        // removals were already emitted above; those entities still exist (are in `present`), so this
        // loop skips them - no double removal.
        for (const auto& entry : base.entities)
        {
            if (!present.Contains(entry.key))
            {
                entries.PushBack(Entry{entry.key, true, false, Guid{}, {}});
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
            }
            out.WriteVarU32(static_cast<u32>(entry.comps.Size()));
            for (const CompRecord& comp : entry.comps)
            {
                WriteWireString(out, comp.typeId);
                out.WriteVarU32(static_cast<u32>(comp.blob.Size()));
                out.WriteBytes(Span<const byte>(comp.blob.Data(), comp.blob.Size()));
            }
        }

        base.entities = Move(nextBaseline); // commit last-sent (reliable-ordered => delivered)
        return entries.Size();
    }

}
