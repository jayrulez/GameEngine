// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net.manager - StateReplication driven end to end through NetworkManager over the
// simulated transport: a server-assigned networked entity's replicated state reaches a connected client's scene.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.net;
import foundation.net.replication;
import foundation.net.manager;
import foundation.scene;

using namespace foundation::core;
namespace net = foundation::net;
namespace scene = foundation::scene;

namespace
{
    struct RepMover
    {
        Float3 position{0, 0, 0};
        i32 health = 0;
    };

    inline void Serialize(ISerializer& ar, RepMover& m)
    {
        foundation::core::Serialize(ar, "position", m.position);
        foundation::core::Serialize(ar, "health", m.health);
    }

    class RepMoverManager final : public scene::SerializableComponentManager<RepMover>
    {
    public:
        RepMoverManager() : SerializableComponentManager(u8"test.RepMover") {}
    };
}

REFLECT_VALUE(RepMover, "rtti::net::test")
{
    builder.Property<&RepMover::position>("position")
        .PropAttribute(net::kReplicatedAttribute, true);
    builder.Property<&RepMover::health>("health").PropAttribute(net::kReplicatedAttribute, true);
}

TEST_CASE("net-manager: state replicates server -> client through the manager + transport")
{
    RttiRegisterValue_RepMover();
    foundation::net::RegisterReplicationComponents();

    net::SimConditions sim;
    sim.latencyMs = 15.0f;
    sim.lossPct = 0.1f;
    sim.seed = 7;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());

    scene::Scene serverScene{DefaultAllocator()};
    serverScene.AddSystem<net::NetworkComponentManager>();
    RepMoverManager* serverMovers = serverScene.AddSystem<RepMoverManager>();
    server.SetReplicatedScene(&serverScene);

    scene::Scene clientScene{DefaultAllocator()};
    clientScene.AddSystem<net::NetworkComponentManager>();
    clientScene.AddSystem<RepMoverManager>();
    client.SetReplicatedScene(&clientScene);

    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());

    // Handshake.
    for (int i = 0; i < 60 && server.Session().PeerCount() == 0u; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
    }
    REQUIRE(server.Session().PeerCount() == 1u);

    // Server spawns a networked entity with replicated state.
    const scene::EntityHandle e = serverScene.CreateEntity(u8"Unit");
    RepMover& m = serverMovers->Add(e);
    m.position = Float3{3, 0, -2};
    m.health = 42;
    const net::NetworkId id = server.Replication().AssignNetworkId(serverScene, e);
    REQUIRE(id.IsValid());

    // Drive until the client's scene mirrors it (reliable and ordered, so it converges).
    scene::EntityHandle ce = scene::EntityHandle::Invalid();
    for (int i = 0; i < 400; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
        ce = client.Replication().FindEntity(id);
        if (clientScene.IsValid(ce) && clientScene.GetSystem<RepMoverManager>()->Get(ce) != nullptr)
        {
            break;
        }
    }
    REQUIRE(clientScene.IsValid(ce));
    const RepMover* rc = clientScene.GetSystem<RepMoverManager>()->Get(ce);
    REQUIRE(rc != nullptr);
    CHECK(rc->health == 42);
    CHECK(rc->position == Float3{3, 0, -2});

    // A server-side change propagates on the next ticks.
    m.health = 99;
    for (int i = 0; i < 200 && rc->health != 99; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
        rc = clientScene.GetSystem<RepMoverManager>()->Get(ce);
    }
    CHECK(rc->health == 99);
}

TEST_CASE("net-manager: a NetworkedTransform replicates an entity's movement server -> client")
{
    foundation::net::RegisterReplicationComponents();

    net::SimConditions sim;
    sim.latencyMs = 15.0f;
    sim.seed = 11;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    client.SetInterpolationDelayMs(
        0.0); // sample the latest state (deterministic once movement stops)

    // Server scene: an authored networked entity (NetworkComponent and NetworkedTransform), not yet
    // assigned an id. SetReplicatedScene on the server assigns it, with no manual AssignNetworkId.
    scene::Scene serverScene{DefaultAllocator()};
    serverScene.AddSystem<net::NetworkComponentManager>();
    serverScene.AddSystem<net::NetworkedTransformComponentManager>();
    const scene::EntityHandle e = serverScene.CreateEntity(u8"Mover");
    serverScene.GetSystem<net::NetworkComponentManager>()->Add(e);
    serverScene.GetSystem<net::NetworkedTransformComponentManager>()->Add(e);
    Transform t0;
    t0.position = Float3{1, 0, 0};
    serverScene.SetLocalTransform(e, t0);

    scene::Scene clientScene{DefaultAllocator()};
    clientScene.AddSystem<net::NetworkComponentManager>();
    clientScene.AddSystem<net::NetworkedTransformComponentManager>();

    server.StartServer(/*dedicated=*/true);
    server.SetReplicatedScene(&serverScene); // server: auto-assigns the authored entity a NetworkId
    client.SetReplicatedScene(&clientScene);
    (void)client.ConnectTo(sv->LocalEndpoint());

    const net::NetworkId id = serverScene.GetSystem<net::NetworkComponentManager>()->Get(e)->id;
    REQUIRE(id.IsValid()); // auto-assigned on SetReplicatedScene (server)

    // Drive until the client mirrors the entity.
    scene::EntityHandle ce = scene::EntityHandle::Invalid();
    for (int i = 0; i < 400 && !clientScene.IsValid(ce); ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
        ce = client.Replication().FindEntity(id);
    }
    REQUIRE(clientScene.IsValid(ce));

    // Move the server entity's local transform; the client's transform follows (CaptureEntityTransforms,
    // then the wire, then ApplyEntityTransforms, with no manual sync).
    Transform t1;
    t1.position = Float3{9, 3, -5};
    serverScene.SetLocalTransform(e, t1);
    for (int i = 0; i < 300; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
    }
    const Transform ct = clientScene.GetLocalTransform(ce);
    CHECK(ct.position.x == doctest::Approx(9.0f));
    CHECK(ct.position.y == doctest::Approx(3.0f));
    CHECK(ct.position.z == doctest::Approx(-5.0f));
}

TEST_CASE("net-manager: a shared authored scene matches by stable id (no duplicate on the client)")
{
    foundation::net::RegisterReplicationComponents();

    net::SimConditions sim;
    sim.seed = 21;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    client.SetInterpolationDelayMs(0.0);

    // Both peers author the same entity with the same Guid (as two games loading one scene from
    // disk do). There is no hand-authored id: it is derived from the Guid on both sides.
    const Guid sharedGuid{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
    const auto authorEntity = [&](scene::Scene& s) -> scene::EntityHandle
    {
        s.AddSystem<net::NetworkComponentManager>();
        s.AddSystem<net::NetworkedTransformComponentManager>();
        const scene::EntityHandle e = s.CreateEntity(sharedGuid, u8"Shared");
        s.GetSystem<net::NetworkComponentManager>()->Add(e); // id 0: derived from sharedGuid
        s.GetSystem<net::NetworkedTransformComponentManager>()->Add(e);
        return e;
    };
    scene::Scene serverScene{DefaultAllocator()};
    const scene::EntityHandle se = authorEntity(serverScene);
    scene::Scene clientScene{DefaultAllocator()};
    const scene::EntityHandle ce = authorEntity(clientScene);

    server.StartServer(/*dedicated=*/true);
    server.SetReplicatedScene(&serverScene); // derives and registers the id for se
    client.SetReplicatedScene(
        &clientScene); // derives the same id for ce (the client's own authored entity)
    (void)client.ConnectTo(sv->LocalEndpoint());

    const net::NetworkId id = serverScene.GetSystem<net::NetworkComponentManager>()->Get(se)->id;
    REQUIRE(id.IsValid());
    CHECK(clientScene.GetSystem<net::NetworkComponentManager>()->Get(ce)->id ==
          id); // both agree, no authoring

    // Move the server's shared entity; the client's same entity follows, and no second one is minted.
    Transform t;
    t.position = Float3{4, 5, 6};
    serverScene.SetLocalTransform(se, t);
    for (int i = 0; i < 400; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
    }

    CHECK(client.Replication().FindEntity(id) == ce); // matched the client's authored entity
    CHECK(clientScene.GetSystem<net::NetworkComponentManager>()->OwnerHandles().Size() ==
          1u); // no duplicate
    const Transform ct = clientScene.GetLocalTransform(ce);
    CHECK(ct.position.x == doctest::Approx(4.0f));
    CHECK(ct.position.z == doctest::Approx(6.0f));
}

// A joiner that loads the host's map after connecting (the map named in the host's welcome): with the host waiting for ready, the joiner hears RPCs but no state until it says it is
// ready, and then gets everything from a full snapshot, an entity made before its ready included.
TEST_CASE("net-manager: a held joiner hears no state until its ready, then all of it")
{
    RttiRegisterValue_RepMover();
    foundation::net::RegisterReplicationComponents();

    net::SimConditions sim;
    sim.latencyMs = 15.0f;
    sim.seed = 11;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    server.SetWaitForReady(true);

    scene::Scene serverScene{DefaultAllocator()};
    serverScene.AddSystem<net::NetworkComponentManager>();
    RepMoverManager* serverMovers = serverScene.AddSystem<RepMoverManager>();
    server.SetReplicatedScene(&serverScene);
    scene::Scene clientScene{DefaultAllocator()};
    clientScene.AddSystem<net::NetworkComponentManager>();
    clientScene.AddSystem<RepMoverManager>();
    client.SetReplicatedScene(&clientScene);

    bool welcomed = false;
    client.Rpc().On(u8"map", [&welcomed](net::PeerId, net::BitReader&) { welcomed = true; });
    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());
    const auto tick = [&](int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            network.Advance(10.0f);
            server.Update(10.0f);
            client.Update(10.0f);
        }
    };
    for (int i = 0; i < 60 && server.Session().PeerCount() == 0u; ++i)
        tick(1);
    REQUIRE(server.Session().PeerCount() == 1u);
    const net::PeerId joiner = server.Session().Peers()[0].id;
    CHECK(server.IsHeld(joiner));

    // State on the host while the joiner is held: none of it reaches the joiner, an RPC does.
    const scene::EntityHandle e = serverScene.CreateEntity(u8"Unit");
    RepMover& m = serverMovers->Add(e);
    m.health = 42;
    const net::NetworkId id = server.Replication().AssignNetworkId(serverScene, e);
    REQUIRE(id.IsValid());
    server.Rpc().Call(server.Session(), joiner, u8"map");
    tick(60);
    CHECK(welcomed);
    CHECK_FALSE(clientScene.IsValid(client.Replication().FindEntity(id)));

    // Ready: the host lets it go, and the next state is the whole of it.
    client.SendReady();
    scene::EntityHandle ce = scene::EntityHandle::Invalid();
    for (int i = 0; i < 200 && !clientScene.IsValid(ce); ++i)
    {
        tick(1);
        ce = client.Replication().FindEntity(id);
    }
    CHECK_FALSE(server.IsHeld(joiner));
    REQUIRE(clientScene.IsValid(ce));
    const RepMover* rc = clientScene.GetSystem<RepMoverManager>()->Get(ce);
    REQUIRE(rc != nullptr);
    CHECK(rc->health == 42);
}

// The default: a host that does not wait sends state at once.
TEST_CASE("net-manager: a host that does not wait for ready holds no joiner")
{
    net::SimConditions sim;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    CHECK_FALSE(server.WaitsForReady());
    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());
    for (int i = 0; i < 60 && server.Session().PeerCount() == 0u; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
    }
    REQUIRE(server.Session().PeerCount() == 1u);
    CHECK_FALSE(server.IsHeld(server.Session().Peers()[0].id));
}

