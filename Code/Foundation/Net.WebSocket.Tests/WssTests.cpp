// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The gateway over TLS: with a backend and a certificate every connection is TLS first, then the
// upgrade, then frames, all through the same code as ws://.
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

// ---- the native client socket ------------------------------------------------------------

namespace
{
    bool SameBytes(const Array<byte>& a, const Array<byte>& b)
    {
        if (a.Size() != b.Size())
            return false;
        for (usize i = 0; i < a.Size(); ++i)
            if (a[i] != b[i])
                return false;
        return true;
    }

    // Pumps the gateway and drains the client until `done` or ~3 s; what the client receives
    // collects in `got`, the gateway's messages in `atGateway`.
    template <typename Fn>
    bool Exchange(WebSocketServerGateway& gateway, WebSocketClientSocket& client, Array<Array<byte>>& got,
                  Array<Array<byte>>& atGateway, u32& clientId, Fn done)
    {
        for (u32 i = 0; i < 3000; ++i)
        {
            gateway.Pump();
            WsGatewayEvent event;
            while (gateway.Poll(event))
            {
                if (event.kind == WsGatewayEventKind::Connected)
                    clientId = event.client;
                else if (event.kind == WsGatewayEventKind::Message)
                    atGateway.PushBack(event.payload);
            }
            DatagramEndpoint from;
            Array<byte> datagram;
            while (client.Receive(from, datagram))
            {
                CHECK(from == kWebSocketServerEndpoint);
                got.PushBack(datagram);
            }
            if (done())
                return true;
            SleepMilliseconds(1);
        }
        return false;
    }

    void JoinAndTalk(WebSocketServerGateway& gateway, const WebSocketClientConfig& config)
    {
        WebSocketClientSocket client(DefaultAllocator(), config);
        REQUIRE(client.IsOpen());
        // Sent before the upgrade: held, then delivered in order.
        const byte first[] = {byte{7}};
        const byte second[] = {byte{8}, byte{9}};
        client.Send(kWebSocketServerEndpoint, Span<const byte>(first, 1));
        client.Send(kWebSocketServerEndpoint, Span<const byte>(second, 2));
        Array<Array<byte>> got;
        Array<Array<byte>> atGateway;
        u32 id = 0;
        REQUIRE(Exchange(gateway, client, got, atGateway, id, [&]() { return atGateway.Size() == 2; }));
        CHECK(client.IsConnected());
        CHECK(atGateway[0].Size() == 1);
        CHECK(atGateway[1][1] == byte{9});

        // A big one each way (16-bit and 64-bit lengths), then a small one back.
        Array<byte> big;
        for (u32 i = 0; i < 70000; ++i)
            big.PushBack(static_cast<byte>(i * 31));
        client.Send(kWebSocketServerEndpoint, big.AsSpan());
        gateway.SendBinary(id, big.AsSpan());
        gateway.SendBinary(id, Span<const byte>(second, 2));
        REQUIRE(Exchange(gateway, client, got, atGateway, id, [&]() { return atGateway.Size() == 3 && got.Size() == 2; }));
        CHECK(SameBytes(atGateway[2], big));
        CHECK(SameBytes(got[0], big));
        CHECK(got[1].Size() == 2);

        // The gateway drops it: the client sees the close.
        gateway.DisconnectClient(id);
        REQUIRE(Exchange(gateway, client, got, atGateway, id, [&]() { return !client.IsOpen(); }));
        CHECK(client.Failure().ContainsIgnoreCase(u8"closed"));
    }
}

TEST_CASE("websocket client: joins the gateway over ws, datagrams both ways, sends before the upgrade held")
{
    WebSocketServerGateway gateway(DefaultAllocator(), 0);
    REQUIRE(gateway.IsOpen());
    WebSocketClientConfig config;
    config.host = String(u8"127.0.0.1");
    config.port = gateway.BoundPort();
    JoinAndTalk(gateway, config);
}

TEST_CASE("websocket client: joins the gateway over wss with the host's certificate trusted")
{
    const TlsCertificate cert = Certificate();
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    const WebSocketTls tls = GatewayTls(*backend, cert);
    WebSocketServerGateway gateway(DefaultAllocator(), 0, &tls);
    REQUIRE(gateway.IsOpen());
    WebSocketClientConfig config;
    config.host = String(u8"localhost");
    config.port = gateway.BoundPort();
    config.secure = true;
    config.tls = backend.Get();
    config.trust = TlsTrust::FromPem(cert.certificatePem.AsView());
    JoinAndTalk(gateway, config);
}

TEST_CASE("websocket client: a certificate it does not trust, wss with no backend, and a plain gateway refused")
{
    const TlsCertificate cert = Certificate();
    const TlsCertificate other = Certificate();
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    const WebSocketTls tls = GatewayTls(*backend, cert);
    WebSocketServerGateway gateway(DefaultAllocator(), 0, &tls);
    REQUIRE(gateway.IsOpen());

    WebSocketClientConfig config;
    config.host = String(u8"localhost");
    config.port = gateway.BoundPort();
    config.secure = true;
    config.tls = backend.Get();
    config.trust = TlsTrust::FromPem(other.certificatePem.AsView());
    {
        WebSocketClientSocket client(DefaultAllocator(), config);
        Array<Array<byte>> got;
        Array<Array<byte>> atGateway;
        u32 id = 0;
        REQUIRE(Exchange(gateway, client, got, atGateway, id, [&]() { return !client.IsOpen(); }));
        CHECK_FALSE(client.IsConnected());
        CHECK(client.Failure().ContainsIgnoreCase(u8"refused"));
        CHECK(id == 0);
    }
    config.tls = nullptr;
    {
        WebSocketClientSocket client(DefaultAllocator(), config);
        CHECK_FALSE(client.IsOpen());
        CHECK(client.Failure().ContainsIgnoreCase(u8"TLS backend"));
    }
    // wss to a plain gateway: the TLS hello is not an upgrade request; refused.
    WebSocketServerGateway plain(DefaultAllocator(), 0);
    config.port = plain.BoundPort();
    config.tls = backend.Get();
    config.trust = TlsTrust::FromPem(cert.certificatePem.AsView());
    {
        WebSocketClientSocket client(DefaultAllocator(), config);
        Array<Array<byte>> got;
        Array<Array<byte>> atGateway;
        u32 id = 0;
        REQUIRE(Exchange(plain, client, got, atGateway, id, [&]() { return !client.IsOpen(); }));
        CHECK_FALSE(client.Failure().IsEmpty());
    }
}

TEST_CASE("websocket client: a host that never upgrades is given up on, saying so")
{
    TcpListener silent(0, true); // accepts (the OS does), never answers
    WebSocketClientConfig config;
    config.host = String(u8"127.0.0.1");
    config.port = silent.BoundPort();
    config.upgradeTimeoutSeconds = 0.3f;
    WebSocketClientSocket client(DefaultAllocator(), config);
    REQUIRE(client.IsOpen());
    DatagramEndpoint from;
    Array<byte> datagram;
    for (u32 i = 0; i < 3000 && client.IsOpen(); ++i)
    {
        (void)client.Receive(from, datagram);
        SleepMilliseconds(1);
    }
    CHECK_FALSE(client.IsOpen());
    CHECK(client.Failure().ContainsIgnoreCase(u8"in time"));
}

TEST_CASE("websocket: the client side's parser takes unmasked frames and refuses masked ones")
{
    const byte payload[] = {byte{1}, byte{2}};
    Array<byte> unmasked;
    EncodeWebSocketFrame(WsOpcode::Binary, Span<const byte>(payload, 2), false, 0, unmasked);
    WebSocketFrameParser client(false);
    client.Push(unmasked.AsSpan());
    WsFrame frame;
    REQUIRE(client.Next(frame));
    CHECK(frame.payload.Size() == 2);
    Array<byte> masked;
    EncodeWebSocketFrame(WsOpcode::Binary, Span<const byte>(payload, 2), true, 0x01020304u, masked);
    WebSocketFrameParser strict(false);
    strict.Push(masked.AsSpan());
    CHECK_FALSE(strict.Next(frame));
    CHECK(strict.Failed());
}

TEST_CASE("websocket: join addresses with a scheme are read; a bare host is not one")
{
    WebSocketClientConfig c;
    REQUIRE(ParseWebSocketAddress(u8"wss://play.example.com/ball", 7000, c));
    CHECK(c.secure);
    CHECK(c.host == StringView(u8"play.example.com"));
    CHECK(c.port == 7000);
    CHECK(c.path == StringView(u8"/ball"));
    REQUIRE(ParseWebSocketAddress(u8"WS://10.0.0.2:7010", 7000, c));
    CHECK_FALSE(c.secure);
    CHECK(c.host == StringView(u8"10.0.0.2"));
    CHECK(c.port == 7010);
    CHECK(c.path == StringView(u8"/"));
    REQUIRE(ParseWebSocketAddress(u8"wss://host", 0, c));
    CHECK(c.port == 443);
    CHECK_FALSE(ParseWebSocketAddress(u8"10.0.0.2", 7000, c));
    CHECK_FALSE(ParseWebSocketAddress(u8"https://host/", 7000, c));
    CHECK_FALSE(ParseWebSocketAddress(u8"wss://", 7000, c));
    CHECK_FALSE(ParseWebSocketAddress(u8"ws://host:99999", 7000, c));
}
