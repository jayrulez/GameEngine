// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net.Replication - the `foundation.net.replication` module.
///
/// StateReplication: a stable per-entity NetworkId and authority, and a reflection-driven field
/// codec. A component marks properties as replicated through the per-property attribute system, so
/// no component needs hand-written network code; this layer harvests that layout once per type and
/// streams the marked fields through the `:wire` BitWriter and BitReader. Snapshots, per-peer deltas,
/// spawning, relevancy and client interpolation are built on that codec.

module;
#include "Core/Prelude.h"

export module foundation.net.replication;

import foundation.core;
import foundation.net;      // BitWriter, BitReader (:wire)
import foundation.scene;    // Scene, EntityHandle, ComponentManagerBase (snapshot assembly)
import foundation.resource; // ResourceManager (SerializableComponentManager's ResolveResources hook)

using namespace foundation::core;
namespace core = foundation::core;
namespace scene = foundation::scene;

export namespace foundation::net
{

    // ---- identity and authority -----------------------------------------------------------------

    // A stable, server-assigned handle for a replicated entity. Wire-carried; 0 = unassigned. Distinct
    // from EntityHandle (index+generation, process-local) and from the persisted Guid (disk identity):
    // NetworkId is the compact id peers agree on for the lifetime of a networked entity.
    struct NetworkId
    {
        u32 value = 0;
        [[nodiscard]] static constexpr NetworkId Invalid() noexcept { return NetworkId{0}; }
        [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr bool operator==(const NetworkId& o) const noexcept
        {
            return value == o.value;
        }
        [[nodiscard]] constexpr bool operator!=(const NetworkId& o) const noexcept
        {
            return value != o.value;
        }
    };

    // Who owns an entity's replicated state. Server authority is the default (it resists cheating);
    // per-entity Client authority is the escape hatch for host migration and client-owned avatars.
    enum class NetworkAuthority : u8
    {
        Server,
        Client
    };

    // ---- replicated-field marking (through the reflection attribute system) ---------------------

    // The property-attribute key a component sets to opt a field into replication:
    //   builder.Property<&C::position>("position").PropAttribute(kReplicatedAttribute, true);
    // An ASCII const char* to match TypeBuilder::PropAttribute's key type (attribute keys are stored as
    // const char*). The value is reserved for priority or precision descriptors; presence alone
    // means "replicate".
    inline constexpr const char* kReplicatedAttribute = "net.replicated";

    // True if this property is marked for replication and its field type is supported by the codec.
    // A marked field of an unsupported type is left out of the layout (with a one-time warning), so a
    // server and client harvesting the same TypeInfo always agree on the field set.
    [[nodiscard]] bool IsReplicated(const PropertyInfo& property);

    // True if the field codec can encode a value of this type (bool, the integer widths, f32, f64,
    // Float2 to Float4, Quaternion). Enums, strings, Guids and resource Refs are not supported.
    [[nodiscard]] bool IsFieldTypeSupported(const TypeInfo* type) noexcept;

    // The ordered replicated-property layout for a component type, harvested from reflection and cached.
    // Both peers derive it from the same TypeInfo, so the order and set match by construction. Empty for a
    // type with no replicated fields.
    [[nodiscard]] Span<const PropertyInfo* const> ReplicatedProperties(const TypeInfo& type);

    // ---- the field codec (Variant <-> wire) -----------------------------------------------------

    // Encode one reflected field value. Dispatches on the Variant's held type; returns false (writes
    // nothing) for an unsupported type. Public so deltas and RPCs can reuse it.
    [[nodiscard]] bool WriteFieldValue(BitWriter& writer, const Variant& value);

    // Decode one reflected field of the given type from the stream into `out`. Returns false (reads
    // nothing) for an unsupported type. The type is the property's declared type: the stream is
    // self-describing only by position, so the reader takes the layout from the shared harvest.
    [[nodiscard]] bool ReadFieldValue(BitReader& reader, const TypeInfo* type, Variant& out);

    // ---- component-level state sync --------------------------------------------------------------

    // Write every replicated field of `instance` (a live component reached via reflection, e.g. from
    // ComponentManagerBase::GetComponentInstance) in layout order. Returns the field count written.
    usize WriteReplicatedState(BitWriter& writer, const Instance& instance);

    // Read and apply the replicated fields written by WriteReplicatedState back onto `instance`, in the
    // same layout order. Returns the field count applied. A read past the end of the stream stops early
    // (the wire is overflow-safe); returns what was applied.
    usize ReadReplicatedState(BitReader& reader, const Instance& instance);

    // ---- the networked-entity tag (a scene component) -------------------------------------------

    // Tags an entity as replicated. The server assigns the NetworkId (StateReplication::AssignNetworkId);
    // the client mirrors it. It is saved with the scene (a designer can mark an entity networked in
    // the editor), so it uses SerializableComponentManager; its own fields are identity, not replicated
    // state, and carry no kReplicatedAttribute, so the field codec never touches them.
    struct NetworkComponent
    {
        NetworkId id{};
        NetworkAuthority authority = NetworkAuthority::Server;
        Guid
            prefab{}; // the source prefab for network spawn (nil = a networked entity with no prefab)
        // The player it belongs to: 0 = the host, else the client's PeerId on the server (the id
        // the session's welcome tells that client). Set at spawn, sent in the spawn record, so every
        // peer knows whose an entity is. Runtime state: not saved with the scene.
        u32 owner = 0;
    };

    // ADL serialization for scene persistence (bidirectional; the enum goes through a temporary u8).
    inline void Serialize(ISerializer& ar, NetworkComponent& c)
    {
        foundation::core::Serialize(ar, "id", c.id.value);
        u8 authority = static_cast<u8>(c.authority);
        foundation::core::Serialize(ar, "authority", authority);
        c.authority = static_cast<NetworkAuthority>(authority);
        foundation::core::Serialize(
            ar, "prefab", c.prefab); // the Guid overload routes through ISerializer::GuidValue
    }

    class NetworkComponentManager final
        : public scene::SerializableComponentManager<NetworkComponent>
    {
    public:
        NetworkComponentManager() : SerializableComponentManager(u8"net.Network") {}
    };

    // ---- networked transform (the common case: replicate an entity's movement) -------------------

    // A replicated transform: mirrors an entity's local transform over the network. Add it alongside a
    // NetworkComponent to replicate movement. Unlike NetworkComponent's identity fields, its fields are
    // replicated, so the field codec and interpolation handle them. The server copies the entity
    // transform into this component before sending (CaptureEntityTransforms), and the client applies
    // the interpolated component back onto the entity transform after receiving (ApplyEntityTransforms).
    // This bridges reflected-component replication and the unreflected TransformData the entity system
    // stores.
    struct NetworkedTransform
    {
        Float3 position = Float3::Zero;
        Quaternion rotation = Quaternion::Identity;
        Float3 scale = Float3::One;
    };

    inline void Serialize(ISerializer& ar, NetworkedTransform& t)
    {
        foundation::core::Serialize(ar, "position", t.position);
        foundation::core::Serialize(ar, "rotation", t.rotation);
        foundation::core::Serialize(ar, "scale", t.scale);
    }

    class NetworkedTransformComponentManager final
        : public scene::SerializableComponentManager<NetworkedTransform>
    {
    public:
        NetworkedTransformComponentManager() : SerializableComponentManager(u8"net.Transform") {}
    };

    // The network component managers for a scene, installed by the NetworkSubsystem at runtime and by
    // headless scene consumers through Engine.Composition. A new manager added here is added to the
    // composition too.
    inline void AddNetworkSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<NetworkComponentManager>();          // identity (NetworkId, authority, prefab)
        scene.AddSystem<NetworkedTransformComponentManager>(); // replicated transform (the common case)
    }

    // Server: copy each entity's live local transform into its NetworkedTransform component, so the
    // replication capture that follows sends the authoritative pose. Call before CaptureDelta or
    // a snapshot.
    void CaptureEntityTransforms(scene::Scene& scene);
    // Client: write each NetworkedTransform component (already interpolated by SampleInterpolation) back
    // onto its entity's local transform, so the visual follows the replicated pose. Call after sampling.
    void ApplyEntityTransforms(scene::Scene& scene);

    // Registers NetworkComponent and NetworkedTransform reflection (call once before a networked scene is
    // built; the snapshot path needs the patched TypeInfo for its versioned records). Idempotent.
    void RegisterReplicationComponents();

    // Surfaces NetworkComponent to scripts: NetworkComponent.of(entity).authority for authority-gated
    // gameplay, and the NetworkAuthority enum. Called by the composition root alongside
    // RegisterNetScriptFacade (the Net session facade).
    void RegisterNetworkComponentScriptFacade();

    // ---- client-side interpolation (smooth playback of low-rate updates) ------------------------

    // True if this field type is smoothly interpolated (rather than snapped) by LerpFieldValue.
    [[nodiscard]] bool IsInterpolatableType(const TypeInfo* type) noexcept;

    // Interpolate one reflected field between two samples, t in [0,1]. f32, f64 and Float2 to Float4
    // lerp; Quaternion nlerps along the shortest path; other types (bool, integers) snap to `a`, the
    // value in effect at the render time (the earlier bracketing sample). Both sides hold the same
    // field type.
    [[nodiscard]] Variant LerpFieldValue(const Variant& a, const Variant& b, f32 t);

    // A per-entity, per-component timeline of received replicated states, sampled at a delayed render
    // time so low-rate updates play back smoothly (Valve and Fiedler style snapshot interpolation). The
    // caller chooses the delay by passing renderTimeMs (now minus the interpolation delay). Record on
    // network receive; Sample each frame to write the interpolated fields onto the live component.
    class InterpolationBuffer
    {
    public:
        void SetHistoryMs(f64 ms) noexcept { m_historyMs = ms; }

        // Snapshot a component's replicated fields at `timestampMs` (layout order, via reflection).
        void Record(NetworkId id, u32 componentTypeHash, f64 timestampMs,
                    const Instance& component);
        // Write the fields interpolated at `renderTimeMs` onto `component`; false if no samples exist.
        // Clamps to the earliest or latest sample outside the buffered window (no extrapolation).
        bool Sample(NetworkId id, u32 componentTypeHash, f64 renderTimeMs,
                    const Instance& component) const;
        // Drop an entity's timelines (despawn or disconnect).
        void Forget(NetworkId id);
        [[nodiscard]] usize TrackedEntities() const noexcept { return m_entities.Size(); }

    private:
        struct StateSample
        {
            f64 time = 0.0;
            Array<Variant> fields;
        }; // fields in replicated-layout order
        struct Timeline
        {
            Array<StateSample> samples;
        }; // ascending by time
        f64 m_historyMs = 1000.0;
        HashMap<u32, HashMap<u32, Timeline>> m_entities; // networkId -> (typeHash -> timeline)
    };

    // ---- the replication model interface and StateReplication -----------------------------------

    // The interface a replication architecture implements: StateReplication (server-authoritative
    // property snapshots, here) or a command-based model such as lockstep. A snapshot is written on
    // the server and applied on the client, both over the :wire stream.
    class IReplicationModel
    {
    public:
        virtual ~IReplicationModel() = default;
        virtual void CaptureSnapshot(scene::Scene& scene, BitWriter& out) = 0;
        virtual void ApplySnapshot(scene::Scene& scene, BitReader& in) = 0;
    };

    // Server-authoritative state replication. The full snapshot (every replicated component of every
    // networked entity) is the primitive that per-peer deltas, late joins, baselines, interpolation
    // and relevancy build on.
    //
    // A snapshot is: VarU32 entityCount, then per entity { U32 networkId, VarU32 componentCount, then
    // per component { string SerializationTypeId, VarU32 blobBytes, blob } }. The per-component
    // length prefix lets a peer that lacks a component type skip it (forward compatibility) instead
    // of desynchronizing the reader.
    class StateReplication final : public IReplicationModel
    {
    public:
        // The client's prefab-spawn hook: resolve a network-spawned prefab into a local entity (the host
        // wires this to its content database through SpawnPrefab). With no handler, or a nil prefab id,
        // a bare entity is created instead: enough to round-trip state, but no prefab structure or
        // visuals.
        using SpawnHandler =
            core::Function<scene::EntityHandle(scene::Scene&, const Guid&, NetworkId)>;
        void SetSpawnHandler(SpawnHandler handler);
        // Whether a spawn handler is installed (the NetworkController applies its resolver to each new
        // endpoint; this lets tests assert that wiring).
        [[nodiscard]] bool HasSpawnHandler() const noexcept { return static_cast<bool>(m_spawnHandler); }

        // Per-peer relevancy (interest): return true if `id` should be replicated to `peerId`. Fog of
        // war is a security matter, not only bandwidth. Unset, everything is relevant to everyone. When
        // an entity leaves a peer's relevance, its next delta removes it on that client (destroyed, so
        // hidden state cannot be read from memory to cheat); the server never sends what a peer may not
        // see.
        using RelevanceFn =
            core::Function<bool(u32 peerId, NetworkId id, scene::EntityHandle entity)>;
        void SetRelevance(RelevanceFn fn);

        // Server: give an entity a NetworkId (adds the NetworkComponent if absent), returning it. A
        // re-registered entity keeps its id. `prefab` records the source prefab so a client can network-
        // spawn it (nil for a bare networked entity). Records the id -> entity mapping for capture.
        NetworkId AssignNetworkId(scene::Scene& scene, scene::EntityHandle entity,
                                  const Guid& prefab = {}, u32 owner = 0);

        // Server: give every entity authored with a NetworkComponent a NetworkId derived from its
        // authored Guid, and register id -> entity. Adding a NetworkComponent in the editor is enough
        // for the server to replicate the entity on start, with no hand-authored ids. Idempotent.
        void AssignSceneNetworkIds(scene::Scene& scene);

        // Client: compute the same Guid-derived id the server did (both peers load the same authored
        // scene, so the same entity Guid gives the same id) and register id -> local entity, so incoming
        // replication updates the authored entity instead of creating a duplicate.
        void RegisterAuthoredEntities(scene::Scene& scene);

        void CaptureSnapshot(scene::Scene& scene, BitWriter& out) override;
        void ApplySnapshot(scene::Scene& scene, BitReader& in) override;

        // Server: write the delta for one peer: only the entities and components that changed since
        // this peer's last delta, plus removed entities. Returns the number of entries written (0 =
        // nothing changed). The baseline is what was last sent to this peer, so the output needs
        // reliable, ordered delivery or the baseline diverges. Updates the peer baseline.
        usize CaptureDelta(scene::Scene& scene, u32 peerId, BitWriter& out);
        // Client: apply a delta: changed components applied in place, removed entities destroyed.
        void ApplyDelta(scene::Scene& scene, BitReader& in);
        // Same, but also record each applied interpolatable component into `interp` at `timestampMs`
        // (the server's capture time), for smooth playback. Sample it back each frame via
        // SampleInterpolation. Components with no interpolatable field are applied directly (not buffered).
        void ApplyDelta(scene::Scene& scene, BitReader& in, InterpolationBuffer& interp,
                        f64 timestampMs);
        // Client, per render frame: write each networked interpolatable component's value at `renderTimeMs`
        // (synced network time minus the interpolation delay) from the buffer onto the live scene.
        void SampleInterpolation(scene::Scene& scene, InterpolationBuffer& interp,
                                 f64 renderTimeMs) const;
        // Drop a peer's baseline on disconnect; its next CaptureDelta re-sends everything as new.
        void ForgetPeer(u32 peerId);

        // The local entity for a NetworkId (invalid if unknown), from the id -> entity map populated by
        // AssignNetworkId (server) or ApplySnapshot's find-or-create (client).
        [[nodiscard]] scene::EntityHandle FindEntity(NetworkId id) const;
        [[nodiscard]] usize NetworkedCount() const noexcept { return m_netIdToEntity.Size(); }

    private:
        // Client: the entity for this id. First sight of a spawn record with a prefab id routes through
        // the spawn handler (prefab instance); otherwise a bare tagged entity is created.
        scene::EntityHandle FindOrCreateEntity(scene::Scene& scene, u32 networkId,
                                               const Guid& prefab, u32 owner, bool spawn);
        // Shared apply loop: read `count` component records (tag and length-prefixed blob) onto an entity.
        // If `interp` is set, records each applied interpolatable component at `timestampMs`.
        void ApplyComponentRecords(scene::Scene& scene, scene::EntityHandle entity, NetworkId id,
                                   u32 count, BitReader& in, InterpolationBuffer* interp,
                                   f64 timestampMs);
        // Read a count-prefixed run of entity records (the payload snapshots and deltas share) and apply them.
        void ApplyEntries(scene::Scene& scene, BitReader& in, InterpolationBuffer* interp,
                          f64 timestampMs);

        // A peer's last-sent state (the delta baseline), per networked entity, per component.
        struct ComponentBaseline
        {
            u32 typeHash = 0;
            Array<byte> blob;
        };
        struct EntityBaseline
        {
            Array<ComponentBaseline> components;
        };
        struct PeerBaseline
        {
            HashMap<u32, EntityBaseline> entities;
        }; // networkId -> its components

        u32 m_nextNetworkId = 0; // server-side monotonic id allocator (0 stays "unassigned")
        HashMap<u32, scene::EntityHandle> m_netIdToEntity;
        HashMap<u32, PeerBaseline> m_peerBaselines; // peerId -> baseline
        SpawnHandler m_spawnHandler; // client-side prefab resolver (null = bare create)
        RelevanceFn m_relevance;     // server-side per-peer interest filter (null = all)
    };

}
