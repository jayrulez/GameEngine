// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.net - replication rides the per-scene FIXED lane:
// NetworkSceneSystem::OnFixedUpdate drives the endpoint's UpdateReplication, while the transport half
// (UpdateTransport) pumps the socket per-frame. These tests prove the round-trip works when driven by
// Scene::FixedUpdate, and that a scene whose fixed lane does not run produces no deltas even though the
// connection stays live.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;         // Context (drives the subsystem PostUpdate pump)
import foundation.scene;
import foundation.net;             // SimDatagramNetwork / IDatagramSocket
import foundation.net.replication; // RegisterReplicationComponents + component managers
import foundation.net.manager;     // NetworkManager (endpoint) + UpdateTransport/UpdateReplication
import engine.net;                 // NetworkSceneSystem + engine::net::AddNetworkSceneManagers

using namespace foundation::core;
namespace scene = foundation::scene;
namespace net = foundation::net;

namespace
{
    // Wire an endpoint to a scene: install the net managers + the fixed-lane driver, make the endpoint
    // replicate the scene, and point the scene's NetworkSceneSystem at the endpoint (what the
    // NetworkController does at the edges in production).
    void Bind(net::NetworkManager& endpoint, scene::Scene& s)
    {
        engine::net::AddNetworkSceneManagers(s); // component managers + NetworkSceneSystem
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

    // An authored networked entity on the server (NetworkComponent + NetworkedTransform); SetReplicatedScene
    // auto-assigned it a NetworkId inside Bind.
    const scene::EntityHandle e = serverScene.CreateEntity(u8"Mover");
    serverScene.GetSystem<net::NetworkComponentManager>()->Add(e);
    serverScene.GetSystem<net::NetworkedTransformComponentManager>()->Add(e);
    Transform t0;
    t0.position = Float3{1, 0, 0};
    serverScene.SetLocalTransform(e, t0);
    // Re-run the server's id assignment now the entity exists (author-then-assign, as the demo does).
    server.SetReplicatedScene(&serverScene);
    const net::NetworkId id = serverScene.GetSystem<net::NetworkComponentManager>()->Get(e)->id;
    REQUIRE(id.IsValid());

    // Handshake over TRANSPORT only (no scene fixed lane needed - the connection is scene-independent).
    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());
    for (int i = 0; i < 60 && server.Session().PeerCount() == 0u; ++i)
    {
        network.Advance(10.0f);
        server.UpdateTransport(10.0f);
        client.UpdateTransport(10.0f);
    }
    REQUIRE(server.Session().PeerCount() == 1u);

    // Move the server entity, then drive replication ON THE SCENE FIXED LANE (server captures in
    // serverScene.FixedUpdate, client samples in clientScene.FixedUpdate).
    Transform t1;
    t1.position = Float3{9, 3, -5};
    serverScene.SetLocalTransform(e, t1);

    scene::EntityHandle ce = scene::EntityHandle::Invalid();
    for (int i = 0; i < 400; ++i)
    {
        serverScene.FixedUpdate(kStep);  // -> NetworkSceneSystem -> server.UpdateReplication (capture+send)
        server.UpdateTransport(10.0f);   // flush queued send + recv
        network.Advance(10.0f);
        client.UpdateTransport(10.0f);   // recv + buffer the delta
        clientScene.FixedUpdate(kStep);  // -> NetworkSceneSystem -> client.UpdateReplication (sample+apply)
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

    // Pump TRANSPORT for many frames but NEVER tick the server scene's fixed lane (a paused scene).
    for (int i = 0; i < 200; ++i)
    {
        network.Advance(10.0f);
        server.UpdateTransport(10.0f);
        client.UpdateTransport(10.0f);
        clientScene.FixedUpdate(kStep); // client lane runs; there is simply nothing to apply
    }
    // The connection is live (transport pumped) but no state was ever captured -> the client saw nothing.
    CHECK(server.Session().PeerCount() == 1u);
    CHECK(!clientScene.IsValid(client.Replication().FindEntity(id)));

    // Now run the SERVER's fixed lane: capture begins, and the client mirrors the entity.
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
    s.FixedUpdate(kStep); // no endpoint -> OnFixedUpdate is a no-op (must not crash)
    CHECK(sys->Endpoint() == nullptr);
}

TEST_CASE("net-subsystem: the transport pump drives every enumerated endpoint per frame")
{
    // The NetworkSubsystem owns the per-frame transport pump: its PostUpdate visits every live endpoint
    // (via the app-provided source) and drives UpdateTransport. Driven here through a real Context (the
    // production lane), a server + client connect purely because the subsystem pumps them - no
    // DriveNetwork, no app fan-out.
    foundation::net::RegisterReplicationComponents();
    net::SimDatagramNetwork network(foundation::core::DefaultAllocator());
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());
    server.StartServer(/*dedicated=*/true);
    (void)client.ConnectTo(sv->LocalEndpoint());

    foundation::runtime::SubsystemManager ctx(foundation::core::DefaultAllocator());
    engine::net::NetworkSubsystem* netSub = ctx.AddSubsystem<engine::net::NetworkSubsystem>();
    ctx.Startup();
    netSub->SetEndpointSource(
        [&](const Function<void(net::NetworkManager&)>& visit)
        {
            visit(server);
            visit(client);
        });

    for (int i = 0; i < 200 && server.Session().PeerCount() == 0u; ++i)
    {
        network.Advance(10.0f);
        ctx.PostUpdate(0.010f); // 10 ms as seconds -> the subsystem pumps UpdateTransport(10ms)
    }
    CHECK(server.Session().PeerCount() == 1u); // the subsystem's pump alone connected the peer
}
