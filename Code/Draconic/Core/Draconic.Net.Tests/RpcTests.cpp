// draconic.net:rpc - name-hashed RPCs with wire-serialized args over a NetSession.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.net;

using namespace draconic::foundation;
namespace net = draconic::net;

namespace
{
    // A connected client+server session pair over the sim, with a step pump.
    struct Pair
    {
        net::SimDatagramNetwork network;
        net::IDatagramSocket* sv;
        net::NetSession server;
        net::NetSession client;
        net::PeerId serverPeer = net::kInvalidPeer;

        explicit Pair(const net::SimConditions& sim = {})
            : network(sim), sv(network.CreateSocket()), server(*sv), client(*network.CreateSocket())
        {
            server.StartServer();
            serverPeer = client.Connect(sv->LocalEndpoint());
        }
        void Step()
        {
            network.Advance(10.0f);
            server.Update(10.0f);
            client.Update(10.0f);
        }
        void Pump(int n)
        {
            for (int i = 0; i < n; ++i)
                Step();
        }
        [[nodiscard]] net::PeerId ClientPeerOnServer() const
        {
            return server.PeerCount() > 0 ? server.Peers()[0].id : net::kInvalidPeer;
        }
    };
}

TEST_CASE("rpc: name hashing is stable + differs by name")
{
    CHECK(net::RpcTable::Hash(u8"spawn") == net::RpcTable::Hash(u8"spawn"));
    CHECK(net::RpcTable::Hash(u8"spawn") != net::RpcTable::Hash(u8"despawn"));
}

TEST_CASE("rpc: client -> server call delivers args to the registered handler")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.lossPct = 0.2f;
    sim.seed = 5;
    Pair fx(sim);
    fx.Pump(20); // connect

    net::PeerId gotSender = net::kInvalidPeer;
    u32 gotA = 0;
    f32 gotB = 0.0f;
    net::RpcTable serverRpc;
    serverRpc.On(u8"order",
                 [&](net::PeerId sender, net::BitReader& args)
                 {
                     gotSender = sender;
                     gotA = args.ReadU32();
                     gotB = args.ReadFloat();
                 });

    net::RpcTable clientRpc;
    clientRpc.Call(fx.client, fx.serverPeer, u8"order",
                   [](net::BitWriter& w)
                   {
                       w.WriteU32(1337u);
                       w.WriteFloat(2.5f);
                   });

    for (int i = 0; i < 200 && gotSender == net::kInvalidPeer; ++i)
    {
        fx.Step();
        serverRpc.Pump(fx.server);
    }
    CHECK(gotSender == fx.ClientPeerOnServer());
    CHECK(gotA == 1337u);
    CHECK(gotB == doctest::Approx(2.5f));
}

TEST_CASE("rpc: server CallAll reaches every client; unregistered ids are ignored")
{
    net::SimConditions sim;
    sim.latencyMs = 15.0f;
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetSession server(*sv);
    net::NetSession a(*network.CreateSocket());
    net::NetSession b(*network.CreateSocket());
    server.StartServer();
    a.Connect(sv->LocalEndpoint());
    b.Connect(sv->LocalEndpoint());
    for (int i = 0; i < 40; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        a.Update(10.0f);
        b.Update(10.0f);
    }
    REQUIRE(server.PeerCount() == 2u);

    int aHits = 0, bHits = 0;
    net::RpcTable ra, rb;
    ra.On(u8"tick",
          [&](net::PeerId, net::BitReader& args)
          {
              CHECK(args.ReadU32() == 7u);
              ++aHits;
          });
    rb.On(u8"tick",
          [&](net::PeerId, net::BitReader& args)
          {
              CHECK(args.ReadU32() == 7u);
              ++bHits;
          });

    net::RpcTable serverRpc;
    serverRpc.CallAll(server, u8"tick", [](net::BitWriter& w) { w.WriteU32(7u); });
    // Also fire an RPC no client registered - must be silently ignored.
    serverRpc.CallAll(server, u8"unknown_event");

    for (int i = 0; i < 120 && (aHits == 0 || bHits == 0); ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        a.Update(10.0f);
        b.Update(10.0f);
        ra.Pump(a);
        rb.Pump(b);
    }
    CHECK(aHits == 1);
    CHECK(bHits == 1);
}

TEST_CASE("rpc: Pump forwards non-RPC events to onOther")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    Pair fx(sim);
    fx.Pump(20);

    // client sends a raw (non-RPC) message on channel 0
    const u8 raw[] = {0xAB};
    fx.client.Send(fx.serverPeer, 0, Span<const byte>(reinterpret_cast<const byte*>(raw), 1),
                   net::Reliability::ReliableOrdered);

    net::RpcTable serverRpc;
    bool sawRaw = false, sawConnected = false;
    for (int i = 0; i < 60 && !sawRaw; ++i)
    {
        fx.Step();
        serverRpc.Pump(fx.server,
                       [&](const net::NetEvent& ev)
                       {
                           if (ev.kind == net::NetEventKind::Connected)
                           {
                               sawConnected = true;
                           }
                           else if (ev.kind == net::NetEventKind::Received && ev.channel == 0 &&
                                    ev.payload.Size() == 1 &&
                                    static_cast<u8>(ev.payload[0]) == 0xABu)
                           {
                               sawRaw = true;
                           }
                       });
    }
    CHECK(sawConnected);
    CHECK(sawRaw);
}
