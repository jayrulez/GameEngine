// draconic.net.replication - NetworkId, the replicated-field layout harvest, and the reflection-
// driven Variant<->wire codec. The central bet: a component marks fields Replicated and the wire
// format is GENERATED from reflection - no hand-written per-component net code.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.net; // BitWriter / BitReader
import draconic.net.replication;
import draconic.scene; // Scene / EntityHandle / SerializableComponentManager

using namespace draconic::foundation;
namespace net = draconic::net;
namespace scene = draconic::scene;

namespace
{
    // A stand-in networked component: a transform-like mix of replicated + local-only fields, plus a
    // marked-but-unsupported field (String) to exercise layout exclusion.
    struct Mover
    {
        Float3 position{0.0f, 0.0f, 0.0f};           // replicated
        Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f}; // replicated
        f32 speed = 0.0f;                            // replicated
        bool grounded = false;                       // replicated
        i32 health = 0;                              // replicated
        f32 localOnly = 0.0f;                        // NOT replicated (no marker)
        String label; // marked replicated but unsupported type -> excluded
    };

    // ADL serialization (required to instantiate SerializableComponentManager<Mover>).
    inline void Serialize(ISerializer& ar, Mover& m)
    {
        draconic::foundation::Serialize(ar, "position", m.position);
        draconic::foundation::Serialize(ar, "rotation", m.rotation);
        draconic::foundation::Serialize(ar, "speed", m.speed);
        draconic::foundation::Serialize(ar, "grounded", m.grounded);
        draconic::foundation::Serialize(ar, "health", m.health);
        draconic::foundation::Serialize(ar, "localOnly", m.localOnly);
        draconic::foundation::Serialize(ar, "label", m.label);
    }

    // A serializable pool so Mover can live in a scene + carry the wire type tag "test.Mover".
    class MoverManager final : public scene::SerializableComponentManager<Mover>
    {
    public:
        MoverManager() : SerializableComponentManager(u8"test.Mover") {}
    };
}

DRACONIC_REFLECT_VALUE(Mover, "draconic::net::test")
{
    builder.Property<&Mover::position>("position").PropAttribute(net::kReplicatedAttribute, true);
    builder.Property<&Mover::rotation>("rotation").PropAttribute(net::kReplicatedAttribute, true);
    builder.Property<&Mover::speed>("speed").PropAttribute(net::kReplicatedAttribute, true);
    builder.Property<&Mover::grounded>("grounded").PropAttribute(net::kReplicatedAttribute, true);
    builder.Property<&Mover::health>("health").PropAttribute(net::kReplicatedAttribute, true);
    builder.Property<&Mover::localOnly>("localOnly"); // no marker -> local
    builder.Property<&Mover::label>("label").PropAttribute(net::kReplicatedAttribute,
                                                           true); // unsupported
}

TEST_CASE("replication: NetworkId validity + equality")
{
    CHECK_FALSE(net::NetworkId::Invalid().IsValid());
    CHECK(net::NetworkId{7}.IsValid());
    CHECK(net::NetworkId{7} == net::NetworkId{7});
    CHECK(net::NetworkId{7} != net::NetworkId{8});
}

TEST_CASE("replication: layout harvest picks marked + supported fields only")
{
    DraconicRegisterValue_Mover();
    const Span<const PropertyInfo* const> layout = net::ReplicatedProperties(TypeOf<Mover>());
    // position, rotation, speed, grounded, health = 5. localOnly (unmarked) + label (unsupported) out.
    REQUIRE(layout.Size() == 5u);
    CHECK(StringView(reinterpret_cast<const char8_t*>(layout[0]->name)) == u8"position");
    CHECK(StringView(reinterpret_cast<const char8_t*>(layout[4]->name)) == u8"health");
}

TEST_CASE("replication: a component round-trips its replicated fields through the wire")
{
    DraconicRegisterValue_Mover();

    Mover source;
    source.position = Float3{1.5f, -2.0f, 3.25f};
    source.rotation = Quaternion{0.1f, 0.2f, 0.3f, 0.9f};
    source.speed = 12.5f;
    source.grounded = true;
    source.health = 77;
    source.localOnly = 999.0f; // must NOT cross the wire
    source.label = String(u8"ignored");

    net::BitWriter writer;
    const usize wrote = net::WriteReplicatedState(writer, Instance::From(&source));
    CHECK(wrote == 5u);

    Mover dest; // all defaults
    net::BitReader reader(writer.Data());
    const usize read = net::ReadReplicatedState(reader, Instance::From(&dest));
    CHECK(read == 5u);
    CHECK(reader.Ok());

    CHECK(dest.position == source.position);
    CHECK(dest.rotation.x == doctest::Approx(0.1f));
    CHECK(dest.rotation.w == doctest::Approx(0.9f));
    CHECK(dest.speed == doctest::Approx(12.5f));
    CHECK(dest.grounded == true);
    CHECK(dest.health == 77);
    CHECK(dest.localOnly == doctest::Approx(0.0f)); // local field untouched by replication
    CHECK(dest.label.IsEmpty());                    // unsupported field never replicated
}

TEST_CASE("replication: field codec round-trips supported scalars + rejects unsupported")
{
    // f64 (full precision), Float3, i32.
    net::BitWriter writer;
    CHECK(net::WriteFieldValue(writer, Variant::From<f64>(3.141592653589793)));
    CHECK(net::WriteFieldValue(writer, Variant::From<Float3>(Float3{4.0f, 5.0f, 6.0f})));
    CHECK(net::WriteFieldValue(writer, Variant::From<i32>(-42)));
    // A String value has no codec support -> false, nothing written.
    CHECK_FALSE(net::WriteFieldValue(writer, Variant::From<String>(String(u8"nope"))));

    net::BitReader reader(writer.Data());
    Variant a, b, c;
    REQUIRE(net::ReadFieldValue(reader, &TypeOf<f64>(), a));
    REQUIRE(net::ReadFieldValue(reader, &TypeOf<Float3>(), b));
    REQUIRE(net::ReadFieldValue(reader, &TypeOf<i32>(), c));
    CHECK(*a.TryGet<f64>() == doctest::Approx(3.141592653589793));
    CHECK(*b.TryGet<Float3>() == Float3{4.0f, 5.0f, 6.0f});
    CHECK(*c.TryGet<i32>() == -42);
    // Reading an unsupported type consumes nothing and reports false.
    Variant d;
    CHECK_FALSE(net::ReadFieldValue(reader, &TypeOf<String>(), d));
}

TEST_CASE("replication: a full snapshot round-trips networked entities server -> client")
{
    DraconicRegisterValue_Mover();
    net::RegisterReplicationComponents();

    // --- server scene: two networked entities, each with a Mover ---
    scene::Scene server;
    server.AddSystem<net::NetworkComponentManager>();
    MoverManager* serverMovers = server.AddSystem<MoverManager>();
    net::StateReplication serverRep;

    const scene::EntityHandle a = server.CreateEntity(u8"A");
    Mover& ma = serverMovers->Add(a);
    ma.position = Float3{1.0f, 2.0f, 3.0f};
    ma.speed = 10.0f;
    ma.health = 100;
    ma.grounded = true;
    const net::NetworkId idA = serverRep.AssignNetworkId(server, a);

    const scene::EntityHandle b = server.CreateEntity(u8"B");
    Mover& mb = serverMovers->Add(b);
    mb.position = Float3{-4.0f, 0.0f, 9.0f};
    mb.speed = 2.5f;
    mb.health = 42;
    const net::NetworkId idB = serverRep.AssignNetworkId(server, b);

    REQUIRE(idA.IsValid());
    REQUIRE(idB.IsValid());
    REQUIRE(idA != idB);
    CHECK(serverRep.NetworkedCount() == 2u);

    // --- capture on the server ---
    net::BitWriter writer;
    serverRep.CaptureSnapshot(server, writer);

    // --- apply into a fresh client scene with the SAME managers ---
    scene::Scene client;
    client.AddSystem<net::NetworkComponentManager>();
    MoverManager* clientMovers = client.AddSystem<MoverManager>();
    net::StateReplication clientRep;

    net::BitReader reader(writer.Data());
    clientRep.ApplySnapshot(client, reader);
    CHECK(reader.Ok());
    CHECK(clientRep.NetworkedCount() == 2u);

    // --- the client now mirrors the server's replicated state, keyed by NetworkId ---
    const scene::EntityHandle ca = clientRep.FindEntity(idA);
    const scene::EntityHandle cb = clientRep.FindEntity(idB);
    REQUIRE(client.IsValid(ca));
    REQUIRE(client.IsValid(cb));
    const Mover* rca = clientMovers->Get(ca);
    const Mover* rcb = clientMovers->Get(cb);
    REQUIRE(rca != nullptr);
    REQUIRE(rcb != nullptr);
    CHECK(rca->position == Float3{1.0f, 2.0f, 3.0f});
    CHECK(rca->speed == doctest::Approx(10.0f));
    CHECK(rca->health == 100);
    CHECK(rca->grounded == true);
    CHECK(rcb->position == Float3{-4.0f, 0.0f, 9.0f});
    CHECK(rcb->speed == doctest::Approx(2.5f));
    CHECK(rcb->health == 42);

    // --- re-applying an updated snapshot mutates in place (same entities, no duplicates) ---
    ma.health = 55;
    net::BitWriter writer2;
    serverRep.CaptureSnapshot(server, writer2);
    net::BitReader reader2(writer2.Data());
    clientRep.ApplySnapshot(client, reader2);
    CHECK(clientRep.NetworkedCount() == 2u); // no new entities minted
    CHECK(clientMovers->Get(clientRep.FindEntity(idA))->health == 55);
}

TEST_CASE(
    "replication: NetworkedTransform bridges the entity transform through capture -> wire -> apply")
{
    net::RegisterReplicationComponents();

    scene::Scene server;
    server.AddSystem<net::NetworkComponentManager>();
    server.AddSystem<net::NetworkedTransformComponentManager>();
    net::StateReplication serverRep;

    const scene::EntityHandle e = server.CreateEntity(u8"E");
    server.GetSystem<net::NetworkedTransformComponentManager>()->Add(e);
    Transform t;
    t.position = Float3{5.0f, 6.0f, 7.0f};
    t.scale = Float3{2.0f, 2.0f, 2.0f};
    server.SetLocalTransform(e, t);
    const net::NetworkId id = serverRep.AssignNetworkId(server, e);
    REQUIRE(id.IsValid());

    // Server: pull the entity's transform into the component, then snapshot.
    net::CaptureEntityTransforms(server);
    CHECK(server.GetSystem<net::NetworkedTransformComponentManager>()->Get(e)->position ==
          Float3{5.0f, 6.0f, 7.0f});
    net::BitWriter writer;
    serverRep.CaptureSnapshot(server, writer);

    // Client: apply the snapshot (fills the component), then push it onto the entity's transform.
    scene::Scene client;
    client.AddSystem<net::NetworkComponentManager>();
    client.AddSystem<net::NetworkedTransformComponentManager>();
    net::StateReplication clientRep;
    net::BitReader reader(writer.Data());
    clientRep.ApplySnapshot(client, reader);
    REQUIRE(reader.Ok());

    const scene::EntityHandle ce = clientRep.FindEntity(id);
    REQUIRE(client.IsValid(ce));
    net::ApplyEntityTransforms(client);
    const Transform ct = client.GetLocalTransform(ce);
    CHECK(ct.position == Float3{5.0f, 6.0f, 7.0f});
    CHECK(ct.scale == Float3{2.0f, 2.0f, 2.0f});
}

TEST_CASE(
    "replication: AssignSceneNetworkIds assigns ids to authored-networked entities (idempotent)")
{
    net::RegisterReplicationComponents();

    scene::Scene scene;
    auto* netMgr = scene.AddSystem<net::NetworkComponentManager>();
    net::StateReplication rep;

    const scene::EntityHandle a = scene.CreateEntity(u8"A");
    netMgr->Add(a);
    const scene::EntityHandle b = scene.CreateEntity(u8"B");
    netMgr->Add(b);
    scene.CreateEntity(u8"plain"); // no NetworkComponent -> never assigned

    CHECK_FALSE(netMgr->Get(a)->id.IsValid());
    rep.AssignSceneNetworkIds(scene);
    CHECK(netMgr->Get(a)->id.IsValid());
    CHECK(netMgr->Get(b)->id.IsValid());
    CHECK(netMgr->Get(a)->id != netMgr->Get(b)->id);

    // Idempotent: a second pass keeps the same ids (already-assigned entities are untouched).
    const net::NetworkId idA = netMgr->Get(a)->id;
    rep.AssignSceneNetworkIds(scene);
    CHECK(netMgr->Get(a)->id == idA);
}

TEST_CASE("replication: per-peer delta sends only what changed since the peer's last delta")
{
    DraconicRegisterValue_Mover();
    net::RegisterReplicationComponents();

    scene::Scene server;
    server.AddSystem<net::NetworkComponentManager>();
    MoverManager* movers = server.AddSystem<MoverManager>();
    net::StateReplication rep;

    const scene::EntityHandle a = server.CreateEntity(u8"A");
    Mover& ma = movers->Add(a);
    ma.position = Float3{1.0f, 0.0f, 0.0f};
    ma.health = 10;
    const net::NetworkId idA = rep.AssignNetworkId(server, a);
    const scene::EntityHandle b = server.CreateEntity(u8"B");
    Mover& mb = movers->Add(b);
    mb.position = Float3{0.0f, 1.0f, 0.0f};
    mb.health = 20;
    const net::NetworkId idB = rep.AssignNetworkId(server, b);

    const u32 peer = 1;

    scene::Scene client;
    client.AddSystem<net::NetworkComponentManager>();
    MoverManager* cmovers = client.AddSystem<MoverManager>();
    net::StateReplication crep;

    // First delta: everything new for this peer -> 2 entries, and the client mirrors it.
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, peer, w);
        CHECK(n == 2u);
        net::BitReader r(w.Data());
        crep.ApplyDelta(client, r);
        CHECK(r.Ok());
    }
    CHECK(crep.NetworkedCount() == 2u);
    CHECK(cmovers->Get(crep.FindEntity(idA))->health == 10);
    CHECK(cmovers->Get(crep.FindEntity(idB))->health == 20);

    // Nothing changed -> empty delta.
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, peer, w);
        CHECK(n == 0u);
    }

    // Change only A -> the delta carries just A.
    ma.health = 99;
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, peer, w);
        CHECK(n == 1u);
        net::BitReader r(w.Data());
        crep.ApplyDelta(client, r);
    }
    CHECK(cmovers->Get(crep.FindEntity(idA))->health == 99);
    CHECK(cmovers->Get(crep.FindEntity(idB))->health == 20); // B untouched by A's delta

    // Despawn B on the server -> the delta marks it removed -> the client destroys it.
    const scene::EntityHandle cb = crep.FindEntity(idB);
    server.DestroyEntity(b);
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, peer, w);
        CHECK(n == 1u);
        net::BitReader r(w.Data());
        crep.ApplyDelta(client, r);
    }
    CHECK_FALSE(client.IsValid(cb));

    // ForgetPeer (disconnect) -> the next delta re-sends everything (only A remains -> 1 new entry).
    rep.ForgetPeer(peer);
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, peer, w);
        CHECK(n == 1u);
    }
}

TEST_CASE("replication: late-join full snapshot spawns prefabs via the spawn handler")
{
    DraconicRegisterValue_Mover();
    net::RegisterReplicationComponents();

    // Server: two entities network-spawned from prefabs, each with replicated Mover state.
    scene::Scene server;
    server.AddSystem<net::NetworkComponentManager>();
    MoverManager* movers = server.AddSystem<MoverManager>();
    net::StateReplication rep;

    const Guid pfxA{0xAAAA, 0x1111};
    const Guid pfxB{0xBBBB, 0x2222};
    const scene::EntityHandle a = server.CreateEntity(u8"A");
    movers->Add(a).health = 7;
    const net::NetworkId idA = rep.AssignNetworkId(server, a, pfxA);
    const scene::EntityHandle b = server.CreateEntity(u8"B");
    movers->Add(b).health = 8;
    const net::NetworkId idB = rep.AssignNetworkId(server, b, pfxB);

    // A late-joining client whose spawn handler stands in for SpawnPrefab: it records the prefab id
    // and produces a Mover-bearing entity (as the real prefab would).
    scene::Scene client;
    client.AddSystem<net::NetworkComponentManager>();
    MoverManager* cmovers = client.AddSystem<MoverManager>();
    net::StateReplication crep;
    Array<Guid> spawned;
    crep.SetSpawnHandler(
        [&](scene::Scene& s, const Guid& p, net::NetworkId) -> scene::EntityHandle
        {
            spawned.PushBack(p);
            const scene::EntityHandle e = s.CreateEntity();
            s.GetSystem<MoverManager>()->Add(e);
            return e;
        });

    net::BitWriter w;
    rep.CaptureSnapshot(server, w);
    net::BitReader r(w.Data());
    crep.ApplySnapshot(client, r);
    CHECK(r.Ok());

    // The handler ran once per entity with the right prefab id, and the client mirrors the state.
    REQUIRE(spawned.Size() == 2u);
    bool haveA = false, haveB = false;
    for (const Guid& g : spawned)
    {
        if (g == pfxA)
        {
            haveA = true;
        }
        if (g == pfxB)
        {
            haveB = true;
        }
    }
    CHECK(haveA);
    CHECK(haveB);
    CHECK(cmovers->Get(crep.FindEntity(idA))->health == 7);
    CHECK(cmovers->Get(crep.FindEntity(idB))->health == 8);
    // The client recorded the source prefab on the tag (host-migration / re-spawn use it later).
    net::NetworkComponentManager* cnet = client.GetSystem<net::NetworkComponentManager>();
    CHECK(cnet->Get(crep.FindEntity(idA))->prefab == pfxA);
}

TEST_CASE("replication: a delta spawns a newly-added networked entity via its prefab")
{
    DraconicRegisterValue_Mover();
    net::RegisterReplicationComponents();

    scene::Scene server;
    server.AddSystem<net::NetworkComponentManager>();
    MoverManager* movers = server.AddSystem<MoverManager>();
    net::StateReplication rep;
    const u32 peer = 1;

    scene::Scene client;
    client.AddSystem<net::NetworkComponentManager>();
    client.AddSystem<MoverManager>();
    net::StateReplication crep;
    Array<Guid> spawned;
    crep.SetSpawnHandler(
        [&](scene::Scene& s, const Guid& p, net::NetworkId) -> scene::EntityHandle
        {
            spawned.PushBack(p);
            const scene::EntityHandle e = s.CreateEntity();
            s.GetSystem<MoverManager>()->Add(e);
            return e;
        });

    // First delta: one prefab-spawned entity.
    const Guid pfx1{1, 2};
    const scene::EntityHandle a = server.CreateEntity();
    movers->Add(a).health = 1;
    rep.AssignNetworkId(server, a, pfx1);
    {
        net::BitWriter w;
        rep.CaptureDelta(server, peer, w);
        net::BitReader r(w.Data());
        crep.ApplyDelta(client, r);
    }
    REQUIRE(spawned.Size() == 1u);
    CHECK(spawned[0] == pfx1);

    // A second entity added later -> the next delta carries just its spawn.
    const Guid pfx2{3, 4};
    const scene::EntityHandle b = server.CreateEntity();
    movers->Add(b).health = 2;
    rep.AssignNetworkId(server, b, pfx2);
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, peer, w);
        CHECK(n == 1u);
        net::BitReader r(w.Data());
        crep.ApplyDelta(client, r);
    }
    REQUIRE(spawned.Size() == 2u);
    CHECK(spawned[1] == pfx2);
    CHECK(crep.NetworkedCount() == 2u);
}

TEST_CASE("replication: LerpFieldValue interpolates floats/vectors and snaps discrete types")
{
    CHECK(*net::LerpFieldValue(Variant::From<f32>(0.0f), Variant::From<f32>(10.0f), 0.5f)
               .TryGet<f32>() == doctest::Approx(5.0f));
    const Variant v = net::LerpFieldValue(Variant::From<Float3>(Float3{0, 0, 0}),
                                          Variant::From<Float3>(Float3{2, 4, 6}), 0.25f);
    CHECK(*v.TryGet<Float3>() == Float3{0.5f, 1.0f, 1.5f});
    // bool is discrete -> snaps to `a`.
    CHECK(*net::LerpFieldValue(Variant::From<bool>(false), Variant::From<bool>(true), 0.9f)
               .TryGet<bool>() == false);
    CHECK(net::IsInterpolatableType(&TypeOf<Float3>()));
    CHECK(net::IsInterpolatableType(&TypeOf<Quaternion>()));
    CHECK_FALSE(net::IsInterpolatableType(&TypeOf<i32>()));
}

TEST_CASE(
    "replication: interpolation buffer lerps transforms and snaps discrete fields at render time")
{
    DraconicRegisterValue_Mover();
    net::InterpolationBuffer buf;
    const net::NetworkId id{1};
    const u32 typeHash = 0xABCDu;

    // Two states 100 ms apart.
    Mover s0;
    s0.position = Float3{0, 0, 0};
    s0.speed = 0.0f;
    s0.health = 10;
    buf.Record(id, typeHash, 0.0, Instance::From(&s0));
    Mover s1;
    s1.position = Float3{10, 0, 0};
    s1.speed = 5.0f;
    s1.health = 20;
    buf.Record(id, typeHash, 100.0, Instance::From(&s1));
    CHECK(buf.TrackedEntities() == 1u);

    Mover out;
    // Midway: position + speed lerp; health (discrete) holds the earlier bracket's value.
    REQUIRE(buf.Sample(id, typeHash, 50.0, Instance::From(&out)));
    CHECK(out.position.x == doctest::Approx(5.0f));
    CHECK(out.speed == doctest::Approx(2.5f));
    CHECK(out.health == 10);

    // At the newer sample's time -> its value.
    buf.Sample(id, typeHash, 100.0, Instance::From(&out));
    CHECK(out.position.x == doctest::Approx(10.0f));

    // Before the window -> clamp to earliest (no extrapolation).
    buf.Sample(id, typeHash, -30.0, Instance::From(&out));
    CHECK(out.position.x == doctest::Approx(0.0f));

    // Forget drops the timeline.
    buf.Forget(id);
    CHECK_FALSE(buf.Sample(id, typeHash, 50.0, Instance::From(&out)));
    CHECK(buf.TrackedEntities() == 0u);
}

TEST_CASE("replication: per-peer relevancy hides non-relevant entities and removes them on exit "
          "(fog of war)")
{
    DraconicRegisterValue_Mover();
    net::RegisterReplicationComponents();

    scene::Scene server;
    server.AddSystem<net::NetworkComponentManager>();
    MoverManager* movers = server.AddSystem<MoverManager>();
    net::StateReplication rep;

    const scene::EntityHandle a = server.CreateEntity(u8"A");
    movers->Add(a).health = 1;
    const net::NetworkId idA = rep.AssignNetworkId(server, a);
    const scene::EntityHandle b = server.CreateEntity(u8"B");
    movers->Add(b).health = 2;
    const net::NetworkId idB = rep.AssignNetworkId(server, b);

    // Peer 1 can currently see A; peer 2 sees everything. A flag lets A leave peer-1 relevance later.
    bool peer1SeesA = true;
    rep.SetRelevance(
        [&](u32 peerId, net::NetworkId id, scene::EntityHandle) -> bool
        {
            if (peerId == 1u)
            {
                return id == idA && peer1SeesA;
            }
            return true; // peer 2: full visibility
        });

    // Two client scenes.
    auto makeClient = [](scene::Scene& s)
    {
        s.AddSystem<net::NetworkComponentManager>();
        s.AddSystem<MoverManager>();
    };
    scene::Scene c1;
    makeClient(c1);
    net::StateReplication crep1;
    scene::Scene c2;
    makeClient(c2);
    net::StateReplication crep2;
    MoverManager* c1movers = c1.GetSystem<MoverManager>();

    // Peer 1's delta: only A crosses (B is hidden).
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, 1u, w);
        CHECK(n == 1u);
        net::BitReader r(w.Data());
        crep1.ApplyDelta(c1, r);
    }
    CHECK(crep1.NetworkedCount() == 1u);
    CHECK(c1.IsValid(crep1.FindEntity(idA)));
    CHECK_FALSE(c1.IsValid(crep1.FindEntity(idB))); // B never sent to peer 1

    // Peer 2's delta: both A and B.
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, 2u, w);
        CHECK(n == 2u);
        net::BitReader r(w.Data());
        crep2.ApplyDelta(c2, r);
    }
    CHECK(crep2.NetworkedCount() == 2u);

    // A leaves peer 1's relevance -> peer 1's next delta REMOVES A (client destroys it).
    const scene::EntityHandle localA = crep1.FindEntity(idA);
    REQUIRE(c1.IsValid(localA));
    peer1SeesA = false;
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, 1u, w);
        CHECK(n == 1u);
        net::BitReader r(w.Data());
        crep1.ApplyDelta(c1, r);
    }
    CHECK_FALSE(c1.IsValid(localA)); // hidden state actively destroyed on the client
    CHECK(crep1.NetworkedCount() == 0u);

    // Peer 2 is unaffected by peer 1's relevance and sees no change -> empty delta.
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, 2u, w);
        CHECK(n == 0u);
    }

    // A re-enters peer 1's relevance -> it re-spawns as a fresh entry.
    peer1SeesA = true;
    {
        net::BitWriter w;
        const usize n = rep.CaptureDelta(server, 1u, w);
        CHECK(n == 1u);
        net::BitReader r(w.Data());
        crep1.ApplyDelta(c1, r);
    }
    CHECK(crep1.NetworkedCount() == 1u);
    CHECK(c1movers->Get(crep1.FindEntity(idA))->health == 1);
}

TEST_CASE("replication: ApplyDelta records interpolatable state, SampleInterpolation smooths it")
{
    DraconicRegisterValue_Mover();
    net::RegisterReplicationComponents();

    scene::Scene server;
    server.AddSystem<net::NetworkComponentManager>();
    MoverManager* smovers = server.AddSystem<MoverManager>();
    net::StateReplication srep;
    const scene::EntityHandle a = server.CreateEntity();
    Mover& sm = smovers->Add(a);
    sm.position = Float3{0, 0, 0};
    sm.health = 5;
    const net::NetworkId id = srep.AssignNetworkId(server, a);

    scene::Scene client;
    client.AddSystem<net::NetworkComponentManager>();
    MoverManager* cmovers = client.AddSystem<MoverManager>();
    net::StateReplication crep;
    net::InterpolationBuffer buf;
    const u32 peer = 1;

    // Delta 1 recorded at server time 0 (position 0).
    {
        net::BitWriter w;
        srep.CaptureDelta(server, peer, w);
        net::BitReader r(w.Data());
        crep.ApplyDelta(client, r, buf, 0.0);
    }
    // Move, delta 2 recorded at server time 100 (position 10).
    sm.position = Float3{10, 0, 0};
    {
        net::BitWriter w;
        srep.CaptureDelta(server, peer, w);
        net::BitReader r(w.Data());
        crep.ApplyDelta(client, r, buf, 100.0);
    }

    const scene::EntityHandle ce = crep.FindEntity(id);
    REQUIRE(client.IsValid(ce));
    CHECK(cmovers->Get(ce)->position.x == doctest::Approx(10.0f)); // direct apply = latest

    // Render halfway between the two samples -> interpolated to the midpoint.
    crep.SampleInterpolation(client, buf, 50.0);
    CHECK(cmovers->Get(ce)->position.x == doctest::Approx(5.0f));

    // Render at the earliest sample time -> the earliest value.
    crep.SampleInterpolation(client, buf, 0.0);
    CHECK(cmovers->Get(ce)->position.x == doctest::Approx(0.0f));
}
