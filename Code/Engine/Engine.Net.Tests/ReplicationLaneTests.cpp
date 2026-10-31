// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.net - replication rides the per-scene fixed lane: NetworkSceneSystem::OnFixedUpdate drives
// the endpoint's UpdateReplication, while the transport half (UpdateTransport) pumps the socket per
// frame. These tests check the round trip works when driven by Scene::FixedUpdate, and that a scene
// whose fixed lane does not run produces no deltas even though the connection stays live.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;         // SubsystemManager (drives the subsystem PostUpdate pump)
import foundation.run;             // the runs whose endpoints the subsystem pumps
import foundation.scene;
import foundation.net;             // SimDatagramNetwork / IDatagramSocket
import foundation.net.replication; // RegisterReplicationComponents and component managers
import foundation.net.manager;     // NetworkManager (endpoint), UpdateTransport/UpdateReplication
import engine.net;                 // NetworkSceneSystem, engine::net::AddNetworkSceneManagers

using namespace foundation::core;
namespace scene = foundation::scene;
namespace net = foundation::net;

namespace
{
    // Wire an endpoint to a scene: install the net managers and the fixed-lane driver, make the
    // endpoint replicate the scene, and point the scene's NetworkSceneSystem at the endpoint (what
    // the NetworkController does at the edges in production).
    void Bind(net::NetworkManager& endpoint, scene::Scene& s)
    {
        engine::net::AddNetworkSceneManagers(s); // component managers and NetworkSceneSystem
        endpoint.SetReplicatedScene(&s);
        s.GetSystem<engine::net::NetworkSceneSystem>()->SetEndpoint(&endpoint);
    }

    constexpr f32 kStep = 1.0f / 60.0f;
}

TEST_CASE("net-scene-system: replication rides the scene fixed lane (server -> client)")
{
    foundation::net::RegisterReplicationComponents();

    net::SimConditions sim;
    sim.latencyMs = 15.0f;
    sim.seed = 11;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    client.SetInterpolationDelayMs(0.0); // sample the latest state (deterministic once movement stops)

    scene::Scene serverScene{DefaultAllocator()};
    Bind(server, serverScene);
    scene::Scene clientScene{DefaultAllocator()};
    Bind(client, clientScene);

    // An authored networked entity on the server (NetworkComponent and NetworkedTransform);
    // SetReplicatedScene auto-assigned it a NetworkId inside Bind.
    const scene::EntityHandle e = serverScene.CreateEntity(u8"Mover");
    serverScene.GetSystem<net::NetworkComponentManager>()->Add(e);
    serverScene.GetSystem<net::NetworkedTransformComponentManager>()->Add(e);
    Transform t0;
    t0.position = Float3{1, 0, 0};
    serverScene.SetLocalTransform(e, t0);
    // Run the server's id assignment again now the entity exists (author, then assign).
    server.SetReplicatedScene(&serverScene);
    const net::NetworkId id = serverScene.GetSystem<net::NetworkComponentManager>()->Get(e)->id;
    REQUIRE(id.IsValid());

    // Handshake over transport only (no scene fixed lane needed: the connection is
    // scene-independent).
    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());
    for (int i = 0; i < 60 && server.Session().PeerCount() == 0u; ++i)
    {
        network.Advance(10.0f);
        server.UpdateTransport(10.0f);
        client.UpdateTransport(10.0f);
    }
    REQUIRE(server.Session().PeerCount() == 1u);

    // Move the server entity, then drive replication on the scene fixed lane (the server captures
    // in serverScene.FixedUpdate, the client samples in clientScene.FixedUpdate).
    Transform t1;
    t1.position = Float3{9, 3, -5};
    serverScene.SetLocalTransform(e, t1);

    scene::EntityHandle ce = scene::EntityHandle::Invalid();
    for (int i = 0; i < 400; ++i)
    {
        serverScene.FixedUpdate(kStep);  // -> NetworkSceneSystem -> server.UpdateReplication (capture, send)
        server.UpdateTransport(10.0f);   // flush queued sends, receive
        network.Advance(10.0f);
        client.UpdateTransport(10.0f);   // receive and buffer the delta
        clientScene.FixedUpdate(kStep);  // -> NetworkSceneSystem -> client.UpdateReplication (sample, apply)
        ce = client.Replication().FindEntity(id);
        if (clientScene.IsValid(ce))
        {
            // keep driving a bit so the transform converges after the entity appears
        }
    }
    REQUIRE(clientScene.IsValid(ce));
    const Transform ct = clientScene.GetLocalTransform(ce);
    CHECK(ct.position.x == doctest::Approx(9.0f));
    CHECK(ct.position.y == doctest::Approx(3.0f));
    CHECK(ct.position.z == doctest::Approx(-5.0f));
}

TEST_CASE("net-scene-system: a scene whose fixed lane does not run sends no deltas (transport still pumps)")
{
    foundation::net::RegisterReplicationComponents();

    net::SimConditions sim;
    sim.seed = 5;
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator(), sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    client.SetInterpolationDelayMs(0.0);

    scene::Scene serverScene{DefaultAllocator()};
    Bind(server, serverScene);
    scene::Scene clientScene{DefaultAllocator()};
    Bind(client, clientScene);

    const scene::EntityHandle e = serverScene.CreateEntity(u8"Mover");
    serverScene.GetSystem<net::NetworkComponentManager>()->Add(e);
    serverScene.GetSystem<net::NetworkedTransformComponentManager>()->Add(e);
    server.SetReplicatedScene(&serverScene); // assign id
    const net::NetworkId id = serverScene.GetSystem<net::NetworkComponentManager>()->Get(e)->id;

    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());

    // Pump transport for many frames but never tick the server scene's fixed lane (a paused scene).
    for (int i = 0; i < 200; ++i)
    {
        network.Advance(10.0f);
        server.UpdateTransport(10.0f);
        client.UpdateTransport(10.0f);
        clientScene.FixedUpdate(kStep); // client lane runs; there is simply nothing to apply
    }
    // The connection is live (transport pumped) but no state was ever captured, so the client saw
    // nothing.
    CHECK(server.Session().PeerCount() == 1u);
    CHECK(!clientScene.IsValid(client.Replication().FindEntity(id)));

    // Now run the server's fixed lane: capture begins, and the client mirrors the entity.
    scene::EntityHandle ce = scene::EntityHandle::Invalid();
    for (int i = 0; i < 400 && !clientScene.IsValid(ce); ++i)
    {
        serverScene.FixedUpdate(kStep);
        server.UpdateTransport(10.0f);
        network.Advance(10.0f);
        client.UpdateTransport(10.0f);
        clientScene.FixedUpdate(kStep);
        ce = client.Replication().FindEntity(id);
    }
    CHECK(clientScene.IsValid(ce)); // replication is gated on the scene fixed lane, transport is not
}

TEST_CASE("net-scene-system: a NetworkSceneSystem with no endpoint is inert")
{
    scene::Scene s{DefaultAllocator()};
    engine::net::AddNetworkSceneManagers(s);
    engine::net::NetworkSceneSystem* sys = s.GetSystem<engine::net::NetworkSceneSystem>();
    REQUIRE(sys != nullptr);
    CHECK(sys->Endpoint() == nullptr);
    s.FixedUpdate(kStep); // no endpoint: OnFixedUpdate is a no-op (must not crash)
    CHECK(sys->Endpoint() == nullptr);
}

TEST_CASE("net-subsystem: the transport pump drives every enumerated endpoint per frame")
{
    // The NetworkSubsystem owns the per-frame transport pump: its PostUpdate visits every run's
    // live endpoint (through the runs on its subsystem manager) and drives UpdateTransport. Driven
    // here through a real Context (the production lane), a server and client connect purely because
    // the subsystem pumps them, with no other driver.
    foundation::net::RegisterReplicationComponents();
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator());
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());

    // Two runs, each with a network controller holding its endpoint, as a GameInstance has.
    struct Controller final : net::INetworkController
    {
        explicit Controller(net::NetworkManager& endpoint) : endpoint(&endpoint) {}
        bool StartServer(u16, bool) override { return false; }
        bool Connect(StringView, u16) override { return false; }
        void StopNetworking() override {}
        [[nodiscard]] net::NetworkManager* NetEndpoint() const override { return endpoint; }
        net::NetworkManager* endpoint;
    };
    struct TestRun final : foundation::run::RunBase
    {
        TestRun() : RunBase(DefaultAllocator(), u8"test") {}
    };
    Controller serverController(server);
    Controller clientController(client);
    TestRun serverRun;
    TestRun clientRun;
    serverRun.Register<net::INetworkController>(serverController);
    clientRun.Register<net::INetworkController>(clientController);
    foundation::run::RunRegistry runs(DefaultAllocator());
    runs.Add(serverRun);
    runs.Add(clientRun);

    foundation::runtime::SubsystemManager ctx(foundation::core::DefaultAllocator());
    (void)ctx.AddSubsystem<engine::net::NetworkSubsystem>();
    ctx.SetRunManager(&runs);
    ctx.Startup();

    for (int i = 0; i < 200 && server.Session().PeerCount() == 0u; ++i)
    {
        network.Advance(10.0f);
        ctx.PostUpdate(0.010f); // 10 ms as seconds -> the subsystem pumps UpdateTransport(10ms)
    }
    CHECK(server.Session().PeerCount() == 1u); // the subsystem's pump alone connected the peer
}
