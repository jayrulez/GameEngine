// draconic.net.manager - the runtime networking home (NetworkManager) mechanics + facade registration.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.net;
import draconic.net.manager;

using namespace draconic::foundation;
namespace net = draconic::net;

TEST_CASE("net-manager: server + client connect, and an RPC routes through the manager")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.lossPct = 0.2f;
    sim.seed = 3;
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* sv = network.CreateSocket();
    net::NetworkManager server(*sv);
    net::NetworkManager client(*network.CreateSocket());

    server.StartServer(/*dedicated=*/true);
    const net::PeerId sp = client.ConnectTo(sv->LocalEndpoint());
    CHECK(server.Session().IsServer());
    CHECK(client.Session().IsClient());

    // A server-side order handler, driven by the manager's own RPC pump in Update.
    bool got = false;
    f64 arg = 0.0;
    net::PeerId from = net::kInvalidPeer;
    server.Rpc().On(u8"order",
                    [&](net::PeerId sender, net::BitReader& r)
                    {
                        const u64 bits = r.ReadU64();
                        MemCopy(&arg, &bits, sizeof(arg));
                        from = sender;
                        got = true;
                    });

    for (int i = 0; i < 40; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
    }
    CHECK(server.Session().PeerCount() == 1u);

    // Client fires an order via its manager's RPC table.
    client.Rpc().Call(client.Session(), sp, u8"order",
                      [](net::BitWriter& w)
                      {
                          const f64 v = 42.5;
                          u64 b = 0;
                          MemCopy(&b, &v, sizeof(b));
                          w.WriteU64(b);
                      });

    for (int i = 0; i < 200 && !got; ++i)
    {
        network.Advance(10.0f);
        server.Update(10.0f);
        client.Update(10.0f);
    }
    CHECK(got);
    CHECK(arg == doctest::Approx(42.5));
    CHECK(from == server.Session().Peers()[0].id);
}

TEST_CASE(
    "net-manager: HostServer/JoinServer own their UDP sockets and exchange an RPC over localhost")
{
    // The self-contained runtime path: each endpoint opens its OWN real UDP socket (no shared sim
    // network, no externally-owned socket) - two NetworkManagers in one process talking over real
    // loopback, exactly the in-editor server<->client scenario.
    UniquePtr<net::NetworkManager> server =
        net::NetworkManager::HostServer(/*port=*/0, /*dedicated=*/true);
    REQUIRE(static_cast<bool>(server));
    CHECK(server->Session().IsServer());
    CHECK(server->BoundPort() != 0u); // OS-assigned, surfaced so a client can reach it

    UniquePtr<net::NetworkManager> client =
        net::NetworkManager::JoinServer(u8"127.0.0.1", server->BoundPort());
    REQUIRE(static_cast<bool>(client));
    CHECK(client->Session().IsClient());
    CHECK(client->BoundPort() != server->BoundPort()); // distinct ephemeral port

    bool got = false;
    f64 arg = 0.0;
    server->Rpc().On(u8"order",
                     [&](net::PeerId, net::BitReader& r)
                     {
                         const u64 bits = r.ReadU64();
                         MemCopy(&arg, &bits, sizeof(arg));
                         got = true;
                     });

    for (int i = 0; i < 400 && server->Session().PeerCount() == 0u; ++i)
    {
        server->Update(16.0f);
        client->Update(16.0f);
        SleepMilliseconds(1);
    }
    REQUIRE(server->Session().PeerCount() == 1u);

    client->Rpc().Call(client->Session(), client->Session().ServerPeer(), u8"order",
                       [](net::BitWriter& w)
                       {
                           const f64 v = 7.25;
                           u64 b = 0;
                           MemCopy(&b, &v, sizeof(b));
                           w.WriteU64(b);
                       });

    for (int i = 0; i < 400 && !got; ++i)
    {
        server->Update(16.0f);
        client->Update(16.0f);
        SleepMilliseconds(1);
    }
    CHECK(got);
    CHECK(arg == doctest::Approx(7.25));
}

TEST_CASE("net-manager: the Net facade type registers")
{
    net::RegisterNetScriptFacade(); // idempotent; must not crash + registers the reflected type
    const TypeInfo& ti = net::Net::StaticType();
    CHECK(PropertyCount(ti) == 0u); // a facade has methods, not properties
    // (End-to-end script binding on both backends is exercised in FacadeScriptTests.cpp.)
}

TEST_CASE("net-startup: role=None yields an inactive runtime (single-player)")
{
    net::NetworkStartup cfg; // role defaults to None
    net::NetworkRuntime rt = net::StartNetworking(cfg);
    CHECK_FALSE(rt.IsActive());
    CHECK(rt.socket.Get() == nullptr);
    CHECK(rt.manager.Get() == nullptr);
}

TEST_CASE(
    "net-startup: config -> socket -> role connects a server + client over real localhost UDP")
{
    net::NetworkStartup serverCfg;
    serverCfg.role = net::NetworkRole::Server;
    serverCfg.dedicated = true;
    serverCfg.listenPort = 0; // OS-assigned
    net::NetworkRuntime server = net::StartNetworking(serverCfg);
    REQUIRE(server.IsActive());
    REQUIRE(server.socket->IsOpen());
    CHECK(server.manager->Session().IsServer());

    net::NetworkStartup clientCfg;
    clientCfg.role = net::NetworkRole::Client;
    clientCfg.serverHost = String(u8"127.0.0.1");
    clientCfg.serverPort = server.socket->BoundPort(); // connect to the server's actual port
    net::NetworkRuntime client = net::StartNetworking(clientCfg);
    REQUIRE(client.IsActive());
    CHECK(client.manager->Session().IsClient());

    // Drive both on the fixed lane (16 ms) until the handshake lands.
    for (int i = 0; i < 300 && server.manager->Session().PeerCount() == 0u; ++i)
    {
        server.manager->Update(16.0f);
        client.manager->Update(16.0f);
    }
    CHECK(server.manager->Session().PeerCount() == 1u);
}
