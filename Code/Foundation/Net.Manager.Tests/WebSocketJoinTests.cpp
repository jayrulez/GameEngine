// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Joining a host's WebSocket gateway by address: a native client given "ws://..." or "wss://..."
// joins over a WebSocket instead of UDP, the session and its RPCs unchanged; wss with the host's
// certificate trusted.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.net.manager;
import foundation.net.tls;
import foundation.net.tls.mbedtls;
import foundation.net.websocket;

using namespace foundation::core;
namespace net = foundation::net;

namespace
{
    u16 FreePort()
    {
        net::TcpListener probe(0, true);
        return probe.BoundPort(); // closed on return: free for the host to bind (as other tests do)
    }

    // The client joins, then calls an RPC the host hears.
    void JoinAndCall(net::NetworkManager& server, net::NetworkManager& client)
    {
        for (int i = 0; i < 2000 && server.Session().PeerCount() == 0u; ++i)
        {
            server.Update(16.0f);
            client.Update(16.0f);
            SleepMilliseconds(1);
        }
        REQUIRE(server.Session().PeerCount() == 1u);
        bool got = false;
        server.Rpc().On(u8"wave", [&](net::PeerId, net::BitReader&) { got = true; });
        client.Rpc().Call(client.Session(), client.Session().ServerPeer(), u8"wave", [](net::BitWriter&) {});
        for (int i = 0; i < 2000 && !got; ++i)
        {
            server.Update(16.0f);
            client.Update(16.0f);
            SleepMilliseconds(1);
        }
        CHECK(got);
    }
}

TEST_CASE("net-manager: a ws:// address joins the host's gateway natively")
{
    const u16 webSocketPort = FreePort();
    UniquePtr<net::NetworkManager> server =
        net::NetworkManager::HostServer(DefaultAllocator(), 0, true, {}, webSocketPort);
    REQUIRE(static_cast<bool>(server));
    UniquePtr<net::NetworkManager> client = net::NetworkManager::JoinServer(
        DefaultAllocator(), Format(u8"ws://127.0.0.1:{}", webSocketPort).AsView(), 0);
    REQUIRE(static_cast<bool>(client));
    CHECK(client->BoundPort() == 0u); // no UDP socket of its own
    JoinAndCall(*server, *client);
}

TEST_CASE("net-manager: a wss:// address joins a host with a certificate, trusted")
{
    net::TlsCertificate cert;
    String error;
    REQUIRE(net::CreateSelfSignedCertificate(u8"localhost", net::CertificateTime(-1).AsView(), net::CertificateTime(30).AsView(), cert, error));
    UniquePtr<net::ITlsBackend> backend = net::CreateMbedTlsBackend(DefaultAllocator());
    net::WebSocketTls tls;
    tls.backend = backend.Get();
    tls.config.certificateChainPem = cert.certificatePem;
    tls.config.privateKeyPem = cert.privateKeyPem;
    const u16 webSocketPort = FreePort();
    UniquePtr<net::NetworkManager> server =
        net::NetworkManager::HostServer(DefaultAllocator(), 0, true, {}, webSocketPort, &tls);
    REQUIRE(static_cast<bool>(server));
    UniquePtr<net::NetworkManager> client = net::NetworkManager::JoinServer(
        DefaultAllocator(), u8"wss://localhost", webSocketPort, {}, backend.Get(),
        net::TlsTrust::FromPem(cert.certificatePem.AsView()));
    REQUIRE(static_cast<bool>(client));
    JoinAndCall(*server, *client);

    // wss with no backend: no join, and HostServer with a key that does not load: no host.
    CHECK_FALSE(static_cast<bool>(net::NetworkManager::JoinServer(DefaultAllocator(), u8"wss://localhost", webSocketPort)));
    tls.config.privateKeyPem = String(u8"not a key");
    CHECK_FALSE(static_cast<bool>(net::NetworkManager::HostServer(DefaultAllocator(), 0, true, {}, FreePort(), &tls)));
}
