// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The TLS seam over its Mbed TLS backend (Documentation/Specs/tls-http-wss.md, P0), headless: a
// client and a server session wired by an in-memory pipe, the server's certificate made by the
// test. A handshake and data both ways (TLS 1.3, and 1.2 when asked), ALPN agreed, plaintext
// written before the handshake held and delivered; the client refusing a certificate for another
// name, one it does not trust, and an expired one, each saying why; a close seen by the peer; a
// configuration that cannot work failing with its reason; the system's roots read where the box
// has them. P1: a TLS server and client over loopback sockets (TlsStream), and the plain stream.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.net.tls;
import foundation.net.tls.mbedtls;

using namespace foundation::core;
using namespace foundation::net;

namespace
{
    TlsCertificate Certificate(StringView name, i32 fromDays = -1, i32 toDays = 30)
    {
        TlsCertificate made;
        String error;
        const bool ok = CreateSelfSignedCertificate(name, CertificateTime(fromDays).AsView(), CertificateTime(toDays).AsView(), made, error);
        INFO(reinterpret_cast<const char*>(error.CStr()));
        REQUIRE(ok);
        return made;
    }

    // Moves each side's bytes to the other until neither has any left (or a bound is hit).
    void Pump(ITlsSession& client, ITlsSession& server)
    {
        for (u32 round = 0; round < 64; ++round)
        {
            Array<byte> toServer;
            Array<byte> toClient;
            const usize a = client.TakeOutgoing(toServer);
            if (a > 0)
                (void)server.Receive(toServer.AsSpan());
            const usize b = server.TakeOutgoing(toClient);
            if (b > 0)
                (void)client.Receive(toClient.AsSpan());
            if (a == 0 && b == 0)
                return;
        }
    }

    String ReadAll(ITlsSession& session)
    {
        String text;
        byte buffer[256];
        for (;;)
        {
            const usize n = session.Read(Span<byte>(buffer, sizeof(buffer)));
            if (n == 0)
                return text;
            text += StringView(reinterpret_cast<const utf8char*>(buffer), n);
        }
    }

    Span<const byte> Bytes(StringView text) { return Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()); }

    struct Pair
    {
        UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
        UniquePtr<ITlsSession> client;
        UniquePtr<ITlsSession> server;

        Pair(const TlsCertificate& served, TlsClientConfig clientConfig, TlsServerConfig serverConfig = {})
        {
            serverConfig.certificateChainPem = served.certificatePem;
            serverConfig.privateKeyPem = served.privateKeyPem;
            server = backend->CreateServer(serverConfig);
            client = backend->CreateClient(clientConfig);
            Pump(*client, *server);
        }
    };

    TlsClientConfig Trusting(const TlsCertificate& cert, StringView name)
    {
        TlsClientConfig c;
        c.serverName = String(name);
        c.trust = TlsTrust::FromPem(cert.certificatePem.AsView());
        return c;
    }
}

TEST_CASE("tls: a handshake over TLS 1.3, data both ways, ALPN agreed")
{
    const TlsCertificate cert = Certificate(u8"game.example");
    TlsClientConfig client = Trusting(cert, u8"game.example");
    client.alpn.PushBack(String(u8"h2"));
    client.alpn.PushBack(String(u8"http/1.1"));
    TlsServerConfig server;
    server.alpn.PushBack(String(u8"http/1.1"));
    Pair pair(cert, client, server);
    INFO(reinterpret_cast<const char*>(String(pair.client->Failure()).CStr()));
    REQUIRE(pair.client->State() == TlsState::Open);
    REQUIRE(pair.server->State() == TlsState::Open);
    CHECK(pair.client->Version() == TlsVersion::Tls13);
    CHECK(pair.client->AlpnChosen() == StringView(u8"http/1.1"));
    CHECK(pair.server->AlpnChosen() == StringView(u8"http/1.1"));

    REQUIRE(pair.client->Write(Bytes(u8"GET / HTTP/1.1\r\n\r\n")).IsOk());
    Pump(*pair.client, *pair.server);
    CHECK(ReadAll(*pair.server) == StringView(u8"GET / HTTP/1.1\r\n\r\n"));
    REQUIRE(pair.server->Write(Bytes(u8"HTTP/1.1 200 OK\r\n\r\nhello")).IsOk());
    Pump(*pair.client, *pair.server);
    CHECK(ReadAll(*pair.client) == StringView(u8"HTTP/1.1 200 OK\r\n\r\nhello"));

    // A larger message, past one TLS record.
    String big;
    for (u32 i = 0; i < 40000; ++i)
        big += StringView(i % 26 == 25 ? u8"\n" : u8"x");
    REQUIRE(pair.client->Write(Bytes(big.AsView())).IsOk());
    Pump(*pair.client, *pair.server);
    CHECK(ReadAll(*pair.server) == big);
}

TEST_CASE("tls: TLS 1.2 when the client asks no higher; plaintext written early waits for the handshake")
{
    const TlsCertificate cert = Certificate(u8"localhost");
    TlsClientConfig client = Trusting(cert, u8"localhost");
    client.maximum = TlsVersion::Tls12;
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    TlsServerConfig serverConfig;
    serverConfig.certificateChainPem = cert.certificatePem;
    serverConfig.privateKeyPem = cert.privateKeyPem;
    UniquePtr<ITlsSession> server = backend->CreateServer(serverConfig);
    UniquePtr<ITlsSession> session = backend->CreateClient(client);
    REQUIRE(session->State() == TlsState::Handshaking);
    REQUIRE(session->Write(Bytes(u8"early")).IsOk()); // held until Open
    Pump(*session, *server);
    REQUIRE(session->State() == TlsState::Open);
    CHECK(session->Version() == TlsVersion::Tls12);
    Pump(*session, *server);
    CHECK(ReadAll(*server) == StringView(u8"early"));
}

TEST_CASE("tls: the client refuses a certificate for another name, an untrusted one and an expired one, saying why")
{
    const TlsCertificate cert = Certificate(u8"localhost");
    {
        Pair pair(cert, Trusting(cert, u8"example.com"));
        CHECK(pair.client->State() == TlsState::Failed);
        CHECK(pair.client->Failure().ContainsIgnoreCase(u8"refused"));
        CHECK(pair.client->Failure().ContainsIgnoreCase(u8"CN"));
        CHECK(pair.server->State() != TlsState::Open);
    }
    {
        const TlsCertificate other = Certificate(u8"localhost");
        Pair pair(cert, Trusting(other, u8"localhost"));
        CHECK(pair.client->State() == TlsState::Failed);
        CHECK(pair.client->Failure().ContainsIgnoreCase(u8"trusted"));
    }
    {
        const TlsCertificate expired = Certificate(u8"localhost", -30, -1);
        Pair pair(expired, Trusting(expired, u8"localhost"));
        CHECK(pair.client->State() == TlsState::Failed);
        CHECK(pair.client->Failure().ContainsIgnoreCase(u8"expired"));
    }
    // Trusting nothing in particular (a test's own), it goes through whatever the name.
    {
        TlsClientConfig insecure;
        insecure.trust = TlsTrust::InsecureForTests();
        Pair pair(cert, insecure);
        CHECK(pair.client->State() == TlsState::Open);
    }
}

TEST_CASE("tls: a close is seen by the peer")
{
    const TlsCertificate cert = Certificate(u8"localhost");
    Pair pair(cert, Trusting(cert, u8"localhost"));
    REQUIRE(pair.client->State() == TlsState::Open);
    pair.client->Close();
    CHECK(pair.client->State() == TlsState::Closed);
    Pump(*pair.client, *pair.server);
    CHECK(ReadAll(*pair.server).IsEmpty());
    CHECK(pair.server->State() == TlsState::Closed);
    CHECK_FALSE(pair.client->Write(Bytes(u8"after")).IsOk());
}

TEST_CASE("tls: a configuration that cannot work fails at once, saying why")
{
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    CHECK(backend->Name() == StringView(u8"Mbed TLS"));
    TlsServerConfig noKey;
    noKey.certificateChainPem = String(u8"not a certificate");
    UniquePtr<ITlsSession> server = backend->CreateServer(noKey);
    CHECK(server->State() == TlsState::Failed);
    CHECK(server->Failure().ContainsIgnoreCase(u8"certificate chain"));

    TlsClientConfig nameless = TlsClientConfig{};
    nameless.trust = TlsTrust::FromPem(Certificate(u8"localhost").certificatePem.AsView());
    UniquePtr<ITlsSession> client = backend->CreateClient(nameless);
    CHECK(client->State() == TlsState::Failed);
    CHECK(client->Failure().ContainsIgnoreCase(u8"name"));

    TlsClientConfig noRoots;
    noRoots.serverName = String(u8"localhost");
    noRoots.trust = TlsTrust::FromPem(u8"nothing to trust");
    UniquePtr<ITlsSession> rootless = backend->CreateClient(noRoots);
    CHECK(rootless->State() == TlsState::Failed);
    CHECK(rootless->Failure().ContainsIgnoreCase(u8"no trusted roots"));
}

TEST_CASE("tls: the system's roots, where the box has them")
{
    String where;
    const TlsRoots roots = LoadSystemRoots(&where);
    MESSAGE(doctest::String(reinterpret_cast<const char*>(where.CStr())));
    if (roots.IsEmpty())
        return; // a box without a bundle: said above
    // A client trusting them starts its handshake (it has roots to check a server against).
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    TlsClientConfig config;
    config.serverName = String(u8"example.com");
    UniquePtr<ITlsSession> client = backend->CreateClient(config);
    INFO(reinterpret_cast<const char*>(String(client->Failure()).CStr()));
    CHECK(client->State() == TlsState::Handshaking);
    Array<byte> hello;
    CHECK(client->TakeOutgoing(hello) > 0); // its ClientHello
}

namespace
{
    // Pumps both streams until `done` holds or about two seconds pass.
    template <typename Done>
    bool PumpUntil(INetByteStream& a, INetByteStream& b, Done done)
    {
        for (u32 i = 0; i < 2000; ++i)
        {
            (void)a.Pump();
            (void)b.Pump();
            if (done())
                return true;
            SleepMilliseconds(1);
        }
        return false;
    }

    String ReceiveAll(INetByteStream& stream, usize want, INetByteStream& other)
    {
        String text;
        byte buffer[4096];
        (void)PumpUntil(stream, other, [&]()
                        {
                            for (;;)
                            {
                                const i64 n = stream.Receive(Span<byte>(buffer, sizeof(buffer)));
                                if (n <= 0)
                                    break;
                                text += StringView(reinterpret_cast<const utf8char*>(buffer), static_cast<usize>(n));
                            }
                            return text.Size() >= want;
                        });
        return text;
    }

    // A client connected to a loopback listener, and the server's side of it accepted.
    bool Connect(TcpListener& listener, TcpSocket& client, TcpSocket& server)
    {
        client = TcpSocket::Connect(u8"127.0.0.1", listener.BoundPort());
        for (u32 i = 0; i < 2000 && !server.IsOpen(); ++i)
        {
            server = listener.Accept();
            if (!server.IsOpen())
                SleepMilliseconds(1);
        }
        return client.IsOpen() && server.IsOpen();
    }
}

TEST_CASE("tls stream: a TLS server and client over loopback sockets, data both ways, a close")
{
    const TlsCertificate cert = Certificate(u8"localhost");
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    TcpListener listener(0, /*loopbackOnly*/ true);
    REQUIRE(listener.IsOpen());
    TcpSocket clientSocket;
    TcpSocket serverSocket;
    REQUIRE(Connect(listener, clientSocket, serverSocket));

    TlsServerConfig serverConfig;
    serverConfig.certificateChainPem = cert.certificatePem;
    serverConfig.privateKeyPem = cert.privateKeyPem;
    TlsStream server(Move(serverSocket), backend->CreateServer(serverConfig));
    TlsStream client(Move(clientSocket), backend->CreateClient(Trusting(cert, u8"localhost")));

    // Written before the handshake: held, then sent.
    REQUIRE(client.Send(Bytes(u8"ping")) == 4);
    REQUIRE(PumpUntil(client, server, [&]() { return client.Ready() && server.Ready(); }));
    CHECK(ReceiveAll(server, 4, client) == StringView(u8"ping"));

    String big;
    for (u32 i = 0; i < 100000; ++i)
        big += StringView(i % 61 == 60 ? u8"\n" : u8"y");
    REQUIRE(server.Send(Bytes(big.AsView())) == static_cast<i64>(big.Size()));
    CHECK(ReceiveAll(client, big.Size(), server) == big);

    client.Close();
    byte b[16];
    CHECK(PumpUntil(server, client, [&]() { return server.Receive(Span<byte>(b, sizeof(b))) < 0; }));
}

TEST_CASE("tls stream: a client refusing the server's certificate fails over sockets too, saying why")
{
    const TlsCertificate cert = Certificate(u8"localhost");
    UniquePtr<ITlsBackend> backend = CreateMbedTlsBackend(DefaultAllocator());
    TcpListener listener(0, true);
    TcpSocket clientSocket;
    TcpSocket serverSocket;
    REQUIRE(Connect(listener, clientSocket, serverSocket));
    TlsServerConfig serverConfig;
    serverConfig.certificateChainPem = cert.certificatePem;
    serverConfig.privateKeyPem = cert.privateKeyPem;
    TlsStream server(Move(serverSocket), backend->CreateServer(serverConfig));
    TlsStream client(Move(clientSocket), backend->CreateClient(Trusting(cert, u8"example.com")));
    CHECK(PumpUntil(client, server, [&]() { return !client.Pump(); }));
    CHECK_FALSE(client.Ready());
    CHECK(client.Failure().ContainsIgnoreCase(u8"refused"));
}

TEST_CASE("plain stream: a bare socket as a stream, the same conventions")
{
    TcpListener listener(0, true);
    TcpSocket clientSocket;
    TcpSocket serverSocket;
    REQUIRE(Connect(listener, clientSocket, serverSocket));
    PlainStream client(Move(clientSocket));
    PlainStream server(Move(serverSocket));
    REQUIRE(client.Send(Bytes(u8"hello")) == 5);
    REQUIRE(PumpUntil(client, server, [&]() { return client.Ready() && server.Ready(); }));
    CHECK(ReceiveAll(server, 5, client) == StringView(u8"hello"));
    client.Close();
    byte b[16];
    CHECK(PumpUntil(server, client, [&]() { return server.Receive(Span<byte>(b, sizeof(b))) < 0; }));

    // Nothing listening (a port whose listener has gone): the connect fails, said so.
    u16 port = 0;
    {
        TcpListener gone(0, true);
        port = gone.BoundPort();
    }
    PlainStream nowhere(TcpSocket::Connect(u8"127.0.0.1", port));
    bool ended = false;
    for (u32 i = 0; i < 2000 && !ended; ++i)
    {
        ended = !nowhere.Pump();
        if (!ended)
            SleepMilliseconds(1);
    }
    CHECK(ended);
    CHECK(nowhere.Failure().ContainsIgnoreCase(u8"could not be made"));
}
