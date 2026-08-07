// draconic.net:session - roles, peer registry, lifecycle, broadcast (over the sim).
#include <doctest/doctest.h>
#include <initializer_list>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.net;

using namespace draconic::foundation;
namespace net = draconic::net;

namespace
{
    Array<byte> Bytes(std::initializer_list<u8> vals)
    {
        Array<byte> out;
        out.Resize(vals.size());
        usize i = 0;
        for (u8 v : vals)
        {
            out[i++] = static_cast<byte>(v);
        }
        return out;
    }
    // Collect Received first-bytes from a session.
    Array<u8> DrainReceived(net::NetSession& s)
    {
        Array<u8> out;
        net::NetEvent ev;
        while (s.PollEvent(ev))
        {
            if (ev.kind == net::NetEventKind::Received && ev.payload.Size() > 0)
            {
                out.PushBack(static_cast<u8>(ev.payload[0]));
            }
        }
        return out;
    }
}

TEST_CASE("session: roles are set by StartServer / Connect")
{
    net::SimDatagramNetwork network;
    net::NetSession server(*network.CreateSocket());
    net::NetSession client(*network.CreateSocket());
    CHECK(server.Role() == net::NetRole::None);
    server.StartServer(/*dedicated=*/true);
    CHECK(server.Role() == net::NetRole::DedicatedServer);
    CHECK(server.IsServer());
    client.Connect(net::DatagramEndpoint{1});
    CHECK(client.Role() == net::NetRole::Client);
    CHECK(client.IsClient());
}

TEST_CASE("session: server tracks connected clients; disconnect updates the registry")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetSession server(*sv);
    net::NetSession clientA(*network.CreateSocket());
    net::NetSession clientB(*network.CreateSocket());
    server.StartServer();
    const net::PeerId sa = clientA.Connect(sv->LocalEndpoint());
    clientB.Connect(sv->LocalEndpoint());

    for (int i = 0; i < 40; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        clientA.Update(10.0f);
        clientB.Update(10.0f);
    }
    CHECK(server.PeerCount() == 2u);
    CHECK(clientA.PeerCount() == 1u); // the server
    CHECK(clientA.ServerPeer() == sa);

    // clientA leaves.
    clientA.Disconnect(sa);
    for (int i = 0; i < 20; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        clientB.Update(10.0f);
    }
    CHECK(server.PeerCount() == 1u);
}

TEST_CASE("session: client clock syncs to the server's authoritative time (RTT-adjusted)")
{
    net::SimConditions sim;
    sim.latencyMs = 40.0f; // ~80ms RTT
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetSession server(*sv);
    net::NetSession client(*network.CreateSocket());
    server.SetTimeSyncIntervalMs(100.0f);
    client.SetFixedStepMs(50.0f);
    server.SetFixedStepMs(50.0f);
    server.StartServer();
    client.Connect(sv->LocalEndpoint());

    // Run a while so the server clock is well ahead of the freshly-started client, and syncs flow.
    for (int i = 0; i < 200; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
    }

    CHECK(client.HasClockSync());
    // The client's synced network time should track the server's (within a few RTT of tolerance).
    const f64 diff = server.NetworkTimeMs() - client.NetworkTimeMs();
    CHECK(diff < 60.0);
    CHECK(diff > -60.0);
    // And the synced network tick should match closely (50ms step).
    const long long tickDiff =
        static_cast<long long>(server.NetworkTick()) - static_cast<long long>(client.NetworkTick());
    CHECK(tickDiff <= 1);
    CHECK(tickDiff >= -1);
}

TEST_CASE("session: Broadcast reaches every client; BroadcastExcept skips one")
{
    net::SimConditions sim;
    sim.latencyMs = 15.0f;
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetSession server(*sv);
    net::NetSession clientA(*network.CreateSocket());
    net::NetSession clientB(*network.CreateSocket());
    server.StartServer();
    clientA.Connect(sv->LocalEndpoint());
    clientB.Connect(sv->LocalEndpoint());
    for (int i = 0; i < 40; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        clientA.Update(10.0f);
        clientB.Update(10.0f);
    }
    REQUIRE(server.PeerCount() == 2u);
    (void)DrainReceived(clientA);
    (void)DrainReceived(clientB);

    // Broadcast a message to all.
    Array<byte> msg = Bytes({0x99});
    server.Broadcast(0, msg.AsSpan(), net::Reliability::ReliableOrdered);
    // Except the first peer.
    Array<byte> msg2 = Bytes({0x77});
    const net::PeerId firstPeer = server.Peers()[0].id;
    server.BroadcastExcept(firstPeer, 0, msg2.AsSpan(), net::Reliability::ReliableOrdered);

    Array<u8> gotA, gotB;
    for (int i = 0; i < 80; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        clientA.Update(10.0f);
        clientB.Update(10.0f);
        for (u8 b : DrainReceived(clientA))
            gotA.PushBack(b);
        for (u8 b : DrainReceived(clientB))
            gotB.PushBack(b);
    }
    // Both got the broadcast (0x99); exactly one of them (the non-excepted) also got 0x77.
    const auto has = [](const Array<u8>& a, u8 v)
    {
        for (u8 x : a)
            if (x == v)
                return true;
        return false;
    };
    CHECK(has(gotA, 0x99u));
    CHECK(has(gotB, 0x99u));
    const int got77 = (has(gotA, 0x77u) ? 1 : 0) + (has(gotB, 0x77u) ? 1 : 0);
    CHECK(got77 == 1); // exactly the non-excepted peer
}
