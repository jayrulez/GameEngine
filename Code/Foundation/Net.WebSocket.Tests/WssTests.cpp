// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The gateway over TLS (Documentation/Specs/tls-http-wss.md, P4): with a backend and a certificate
// every connection is TLS first, then the upgrade, then frames, all through the same code as ws://.
// A TLS client upgrades and exchanges binary frames both ways; a plain client is refused; a key
// that does not load leaves the gateway closed, saying why.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.net.tls;
import foundation.net.tls.mbedtls;
import foundation.net.websocket;

using namespace foundation::core;
using namespace foundation::net;

namespace
{
    Span<const byte> Bytes(StringView text) { return Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()); }

    TlsCertificate Certificate()
    {
        TlsCertificate cert;
        String error;
        const bool ok = CreateSelfSignedCertificate(u8"localhost", CertificateTime(-1).AsView(), CertificateTime(30).AsView(), cert, error);
        REQUIRE(ok);
        return cert;
    }

    WebSocketTls GatewayTls(ITlsBackend& backend, const TlsCertificate& cert)
    {
        WebSocketTls tls;
        tls.backend = &backend;
        tls.config.certificateChainPem = cert.certificatePem;
        tls.config.privateKeyPem = cert.privateKeyPem;
        return tls;
    }

    // Pumps the gateway and the client stream until `done` or ~3 s.
    template <typename Fn>
    bool PumpUntil(WebSocketServerGateway& gateway, INetByteStream& client, Array<byte>& received, Fn done)
    {
        for (u32 i = 0; i < 3000; ++i)
        {
            gateway.Pump();
            (void)client.Pump();
            byte chunk[1024];
            for (i64 n = client.Receive(Span<byte>(chunk, sizeof(chunk))); n > 0; n = client.Receive(Span<byte>(chunk, sizeof(chunk))))
                for (i64 j = 0; j < n; ++j)
                    received.PushBack(chunk[j]);
            if (done())
                return true;
            SleepMilliseconds(1);
        }
        return false;
    }

    bool StartsWith(const Array<byte>& bytes, StringView prefix)
    {
        return bytes.Size() >= prefix.Size() && StringView(reinterpret_cast<const utf8char*>(bytes.Data()), prefix.Size()) == prefix;
    }

    constexpr StringView kUpgrade = u8"GET /game HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    u8"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
}

TEST_CASE("wss: a TLS client upgrades through the gateway and exchanges binary frames both ways")
{
    const TlsCertificate cert = Certificate();
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    const WebSocketTls tls = GatewayTls(*backend, cert);
    WebSocketServerGateway gateway(DefaultAllocator(), 0, &tls);
    REQUIRE(gateway.IsOpen());
    CHECK(gateway.IsSecure());

    TlsClientConfig config;
    config.serverName = String(u8"localhost");
    config.trust = TlsTrust::FromPem(cert.certificatePem.AsView());
    TlsStream client(TcpSocket::Connect(u8"127.0.0.1", gateway.BoundPort()), backend->CreateClient(config));
    REQUIRE(client.Send(Bytes(kUpgrade)) > 0);
    Array<byte> received;
    REQUIRE(PumpUntil(gateway, client, received, [&]() { return received.Size() >= 4 && StartsWith(received, u8"HTTP/1.1 101"); }));

    WsGatewayEvent event;
    bool connected = false;
    while (gateway.Poll(event))
        connected = connected || event.kind == WsGatewayEventKind::Connected;
    CHECK(connected);

    // Client to gateway: one masked binary frame.
    const byte payload[] = {byte{1}, byte{2}, byte{3}, byte{250}};
    Array<byte> frame;
    EncodeWebSocketFrame(WsOpcode::Binary, Span<const byte>(payload, 4), true, 0x12345678u, frame);
    REQUIRE(client.Send(frame.AsSpan()) > 0);
    bool message = false;
    Array<byte> got;
    REQUIRE(PumpUntil(gateway, client, received, [&]() {
        while (gateway.Poll(event))
            if (event.kind == WsGatewayEventKind::Message)
            {
                message = true;
                got = event.payload;
            }
        return message;
    }));
    REQUIRE(got.Size() == 4);
    CHECK(got[3] == byte{250});

    // Gateway to client: an unmasked binary frame after the 101's head.
    received.Clear();
    gateway.SendBinary(1, Span<const byte>(payload, 3)); // the gateway's first client
    REQUIRE(PumpUntil(gateway, client, received, [&]() { return received.Size() >= 5; }));
    CHECK((static_cast<u8>(received[0]) & 0x0Fu) == 0x2u);
    CHECK(static_cast<u8>(received[1]) == 3u);
    CHECK(received[4] == byte{3});
}

TEST_CASE("wss: a plain client gets no upgrade from a TLS gateway")
{
    const TlsCertificate cert = Certificate();
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    const WebSocketTls tls = GatewayTls(*backend, cert);
    WebSocketServerGateway gateway(DefaultAllocator(), 0, &tls);
    REQUIRE(gateway.IsOpen());
    PlainStream client(TcpSocket::Connect(u8"127.0.0.1", gateway.BoundPort()));
    REQUIRE(client.Send(Bytes(kUpgrade)) > 0);
    Array<byte> received;
    // The gateway reads the HTTP text as a broken handshake and drops the connection.
    byte b[16];
    CHECK(PumpUntil(gateway, client, received, [&]() { return client.Receive(Span<byte>(b, sizeof(b))) < 0; }));
    CHECK_FALSE(StartsWith(received, u8"HTTP/1.1 101"));
    WsGatewayEvent event;
    while (gateway.Poll(event))
        CHECK(event.kind != WsGatewayEventKind::Connected);
}

TEST_CASE("wss: a key that does not load leaves the gateway closed, saying why")
{
    const TlsCertificate cert = Certificate();
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    WebSocketTls tls = GatewayTls(*backend, cert);
    tls.config.privateKeyPem = String(u8"not a key");
    WebSocketServerGateway gateway(DefaultAllocator(), 0, &tls);
    CHECK_FALSE(gateway.IsOpen());
    CHECK_FALSE(gateway.Failure().IsEmpty());

    WebSocketTls none;
    WebSocketServerGateway noBackend(DefaultAllocator(), 0, &none);
    CHECK_FALSE(noBackend.IsOpen());
    CHECK(noBackend.Failure().ContainsIgnoreCase(u8"TLS backend"));
}
