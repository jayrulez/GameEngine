// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net.websocket: handshake math (RFC vectors), the frame codec (including the
// malformed cases the accept path must survive, since its bytes are untrusted), and the gateway
// and hybrid socket over real localhost sockets, as TcpSocketTests does.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.net.websocket;
import foundation.http;

using namespace foundation::core;
namespace net = foundation::net;
namespace http = foundation::http;

namespace
{
    [[nodiscard]] String HexOf(Span<const byte> bytes)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        String out;
        for (byte b : bytes)
        {
            out.PushBack(static_cast<utf8char>(kHex[(static_cast<u8>(b) >> 4) & 0xF]));
            out.PushBack(static_cast<utf8char>(kHex[static_cast<u8>(b) & 0xF]));
        }
        return out;
    }

    [[nodiscard]] Array<byte> BytesOf(StringView text)
    {
        Array<byte> out;
        for (usize i = 0; i < text.Size(); ++i)
        {
            out.PushBack(static_cast<byte>(text[i]));
        }
        return out;
    }
}

TEST_CASE("websocket: SHA-1 matches the RFC 3174 vectors")
{
    const Array<byte> abc = BytesOf(u8"abc");
    byte digest[20];
    net::Sha1(Span<const byte>{abc.Data(), abc.Size()}, digest);
    CHECK(HexOf(Span<const byte>{digest, 20}) == u8"a9993e364706816aba3e25717850c26c9cd0d89d");

    const Array<byte> longer =
        BytesOf(u8"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    net::Sha1(Span<const byte>{longer.Data(), longer.Size()}, digest);
    CHECK(HexOf(Span<const byte>{digest, 20}) == u8"84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    net::Sha1({}, digest); // empty message
    CHECK(HexOf(Span<const byte>{digest, 20}) == u8"da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("websocket: Base64 matches the RFC 4648 vectors")
{
    CHECK(net::Base64Encode({}) == u8"");
    const Array<byte> f = BytesOf(u8"f");
    CHECK(net::Base64Encode(Span<const byte>{f.Data(), f.Size()}) == u8"Zg==");
    const Array<byte> fo = BytesOf(u8"fo");
    CHECK(net::Base64Encode(Span<const byte>{fo.Data(), fo.Size()}) == u8"Zm8=");
    const Array<byte> foo = BytesOf(u8"foo");
    CHECK(net::Base64Encode(Span<const byte>{foo.Data(), foo.Size()}) == u8"Zm9v");
    const Array<byte> foob = BytesOf(u8"foobar");
    CHECK(net::Base64Encode(Span<const byte>{foob.Data(), foob.Size()}) == u8"Zm9vYmFy");
}

TEST_CASE("websocket: the accept key matches the RFC 6455 example")
{
    CHECK(net::ComputeWebSocketAccept(u8"dGhlIHNhbXBsZSBub25jZQ==") ==
          u8"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("websocket: frame codec round-trips a masked client frame")
{
    const Array<byte> payload = BytesOf(u8"hello frame");
    Array<byte> wire;
    net::EncodeWebSocketFrame(net::WsOpcode::Binary,
                              Span<const byte>{payload.Data(), payload.Size()},
                              /*mask*/ true, 0xA1B2C3D4u, wire);

    net::WebSocketFrameParser parser;
    // Feed in two arbitrary chunks: the parser is incremental.
    parser.Push(Span<const byte>{wire.Data(), 3});
    parser.Push(Span<const byte>{wire.Data() + 3, wire.Size() - 3});
    REQUIRE(!parser.Failed());
    net::WsFrame frame;
    REQUIRE(parser.Next(frame));
    CHECK(frame.opcode == net::WsOpcode::Binary);
    REQUIRE(frame.payload.Size() == payload.Size());
    for (usize i = 0; i < payload.Size(); ++i)
    {
        CHECK(frame.payload[i] == payload[i]);
    }
    CHECK(!parser.Next(frame)); // exactly one
}

TEST_CASE("websocket: 16-bit extended lengths round-trip")
{
    Array<byte> payload;
    for (usize i = 0; i < 300; ++i)
    {
        payload.PushBack(static_cast<byte>(i & 0xFF));
    }
    Array<byte> wire;
    net::EncodeWebSocketFrame(net::WsOpcode::Binary,
                              Span<const byte>{payload.Data(), payload.Size()}, true, 7u, wire);
    net::WebSocketFrameParser parser;
    parser.Push(Span<const byte>{wire.Data(), wire.Size()});
    net::WsFrame frame;
    REQUIRE(parser.Next(frame));
    REQUIRE(frame.payload.Size() == 300u);
    CHECK(frame.payload[299] == static_cast<byte>(299 & 0xFF));
}

TEST_CASE("websocket: the parser rejects what the RFC forbids from clients")
{
    // Unmasked client frame.
    {
        Array<byte> wire;
        const Array<byte> payload = BytesOf(u8"x");
        net::EncodeWebSocketFrame(net::WsOpcode::Binary,
                                  Span<const byte>{payload.Data(), payload.Size()},
                                  /*mask*/ false, 0, wire);
        net::WebSocketFrameParser parser;
        parser.Push(Span<const byte>{wire.Data(), wire.Size()});
        CHECK(parser.Failed());
    }
    // Fragmentation (FIN = 0).
    {
        Array<byte> wire;
        wire.PushBack(byte{0x02}); // no FIN, Binary
        wire.PushBack(byte{0x80}); // masked, len 0
        wire.PushBack(byte{0});
        wire.PushBack(byte{0});
        wire.PushBack(byte{0});
        wire.PushBack(byte{0});
        net::WebSocketFrameParser parser;
        parser.Push(Span<const byte>{wire.Data(), wire.Size()});
        CHECK(parser.Failed());
    }
    // Oversized declared length.
    {
        Array<byte> wire;
        wire.PushBack(static_cast<byte>(0x82)); // FIN, Binary
        wire.PushBack(static_cast<byte>(0x80u | 127u));
        for (usize i = 0; i < 8; ++i)
        {
            wire.PushBack(byte{0x7F}); // absurd 64-bit length
        }
        net::WebSocketFrameParser parser;
        parser.Push(Span<const byte>{wire.Data(), wire.Size()});
        CHECK(parser.Failed());
    }
    // RSV bits set (no extension negotiated).
    {
        Array<byte> wire;
        wire.PushBack(static_cast<byte>(0xC2)); // FIN, RSV1, Binary
        wire.PushBack(static_cast<byte>(0x80));
        net::WebSocketFrameParser parser;
        parser.Push(Span<const byte>{wire.Data(), wire.Size()});
        CHECK(parser.Failed());
    }
}

namespace
{
    [[nodiscard]] http::HttpMessageParser ParseRequestText(StringView text)
    {
        http::HttpMessageParser parser(http::HttpMessageParser::Mode::Request, 4096);
        const Array<byte> bytes = BytesOf(text);
        (void)parser.Push(Span<const byte>{bytes.Data(), bytes.Size()});
        return parser;
    }
}

TEST_CASE("websocket: handshake validation - the good request and each broken variant")
{
    const StringView good = u8"GET /game HTTP/1.1\r\n"
                            u8"Host: localhost\r\n"
                            u8"Upgrade: websocket\r\n"
                            u8"Connection: keep-alive, Upgrade\r\n"
                            u8"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            u8"Sec-WebSocket-Version: 13\r\n\r\n";
    {
        http::HttpMessageParser parser = ParseRequestText(good);
        String key;
        REQUIRE(net::ValidateWebSocketUpgrade(parser, key));
        CHECK(key == u8"dGhlIHNhbXBsZSBub25jZQ==");
        const Array<byte> response = net::BuildWebSocketUpgradeResponse(key.AsView());
        const StringView text{reinterpret_cast<const utf8char*>(response.Data()),
                              response.Size()};
        CHECK(text.SubStr(0, 12) == u8"HTTP/1.1 101");
        bool hasAccept = false;
        const StringView accept = u8"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        for (usize i = 0; i + accept.Size() <= text.Size(); ++i)
        {
            hasAccept = hasAccept || text.SubStr(i, accept.Size()) == accept;
        }
        CHECK(hasAccept);
    }
    const StringView broken[] = {
        // POST instead of GET
        u8"POST /game HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        u8"Sec-WebSocket-Key: aaaa\r\nSec-WebSocket-Version: 13\r\n\r\n",
        // missing key
        u8"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        u8"Sec-WebSocket-Version: 13\r\n\r\n",
        // wrong version
        u8"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        u8"Sec-WebSocket-Key: aaaa\r\nSec-WebSocket-Version: 8\r\n\r\n",
        // no upgrade header
        u8"GET / HTTP/1.1\r\nConnection: Upgrade\r\n"
        u8"Sec-WebSocket-Key: aaaa\r\nSec-WebSocket-Version: 13\r\n\r\n",
        // connection without the Upgrade token
        u8"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: close\r\n"
        u8"Sec-WebSocket-Key: aaaa\r\nSec-WebSocket-Version: 13\r\n\r\n",
    };
    for (const StringView request : broken)
    {
        http::HttpMessageParser parser = ParseRequestText(request);
        String key;
        CHECK(!net::ValidateWebSocketUpgrade(parser, key));
    }
}

namespace
{
    // Drive a real localhost client through connect and upgrade against a gateway.
    struct WsTestClient
    {
        net::TcpSocket socket;
        net::WebSocketFrameParser frames; // unused for server frames, which are unmasked;
                                          // they are read raw and decoded by hand below.

        [[nodiscard]] bool ConnectAndUpgrade(net::WebSocketServerGateway& gateway, u16 port)
        {
            socket = net::TcpSocket::Connect(u8"127.0.0.1", port);
            if (!socket.IsOpen())
            {
                return false;
            }
            for (int i = 0; i < 300 && socket.ConnectStatus() == 0; ++i)
            {
                gateway.Pump();
                SleepMilliseconds(1);
            }
            if (socket.ConnectStatus() != 1)
            {
                return false;
            }
            const StringView request = u8"GET /game HTTP/1.1\r\n"
                                       u8"Host: localhost\r\n"
                                       u8"Upgrade: websocket\r\n"
                                       u8"Connection: Upgrade\r\n"
                                       u8"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                       u8"Sec-WebSocket-Version: 13\r\n\r\n";
            Array<byte> bytes;
            for (usize i = 0; i < request.Size(); ++i)
            {
                bytes.PushBack(static_cast<byte>(request[i]));
            }
            if (socket.Send(Span<const byte>{bytes.Data(), bytes.Size()}) !=
                static_cast<i64>(bytes.Size()))
            {
                return false;
            }
            // Read until the response head terminator arrives.
            Array<byte> response;
            for (int i = 0; i < 500; ++i)
            {
                gateway.Pump();
                byte chunk[512];
                const i64 got = socket.Receive(Span<byte>{chunk, sizeof(chunk)});
                for (i64 j = 0; j < got; ++j)
                {
                    response.PushBack(chunk[j]);
                }
                if (response.Size() >= 4)
                {
                    const usize n = response.Size();
                    if (static_cast<u8>(response[n - 4]) == '\r' &&
                        static_cast<u8>(response[n - 3]) == '\n' &&
                        static_cast<u8>(response[n - 2]) == '\r' &&
                        static_cast<u8>(response[n - 1]) == '\n')
                    {
                        break;
                    }
                }
                SleepMilliseconds(1);
            }
            const StringView text{reinterpret_cast<const utf8char*>(response.Data()),
                                  response.Size()};
            return text.Size() >= 12 && text.SubStr(0, 12) == u8"HTTP/1.1 101";
        }

        void SendBinary(Span<const byte> payload, u32 maskKey = 0xDEADBEEFu)
        {
            Array<byte> wire;
            net::EncodeWebSocketFrame(net::WsOpcode::Binary, payload, true, maskKey, wire);
            (void)socket.Send(Span<const byte>{wire.Data(), wire.Size()});
        }

        // Receive one unmasked server frame (blocking pump loop).
        [[nodiscard]] bool ReceiveBinary(net::WebSocketServerGateway& gateway,
                                         Array<byte>& outPayload)
        {
            Array<byte> wire;
            for (int i = 0; i < 500; ++i)
            {
                gateway.Pump();
                byte chunk[512];
                const i64 got = socket.Receive(Span<byte>{chunk, sizeof(chunk)});
                for (i64 j = 0; j < got; ++j)
                {
                    wire.PushBack(chunk[j]);
                }
                if (wire.Size() >= 2)
                {
                    const usize len = static_cast<u8>(wire[1]) & 0x7Fu; // small test frames
                    if (wire.Size() >= 2 + len)
                    {
                        if ((static_cast<u8>(wire[0]) & 0x0Fu) != 0x2u)
                        {
                            return false; // not binary
                        }
                        outPayload.Clear();
                        for (usize j = 0; j < len; ++j)
                        {
                            outPayload.PushBack(wire[2 + j]);
                        }
                        return true;
                    }
                }
                SleepMilliseconds(1);
            }
            return false;
        }
    };
}

TEST_CASE("websocket: gateway - a client upgrades, exchanges binary frames, and pings")
{
    net::WebSocketServerGateway gateway(DefaultAllocator(), 0);
    REQUIRE(gateway.IsOpen());
    REQUIRE(gateway.BoundPort() != 0u);

    WsTestClient client;
    REQUIRE(client.ConnectAndUpgrade(gateway, gateway.BoundPort()));

    // The upgrade produced a Connected event.
    net::WsGatewayEvent event;
    bool connected = false;
    u32 clientId = 0;
    for (int i = 0; i < 100 && !connected; ++i)
    {
        gateway.Pump();
        while (gateway.Poll(event))
        {
            if (event.kind == net::WsGatewayEventKind::Connected)
            {
                connected = true;
                clientId = event.client;
            }
        }
        SleepMilliseconds(1);
    }
    REQUIRE(connected);
    CHECK(gateway.ClientCount() == 1u);

    // Client to gateway binary frame.
    const Array<byte> hello = BytesOf(u8"hi server");
    client.SendBinary(Span<const byte>{hello.Data(), hello.Size()});
    bool received = false;
    for (int i = 0; i < 300 && !received; ++i)
    {
        gateway.Pump();
        while (gateway.Poll(event))
        {
            if (event.kind == net::WsGatewayEventKind::Message)
            {
                received = true;
                CHECK(event.client == clientId);
                REQUIRE(event.payload.Size() == hello.Size());
                for (usize j = 0; j < hello.Size(); ++j)
                {
                    CHECK(event.payload[j] == hello[j]);
                }
            }
        }
        SleepMilliseconds(1);
    }
    REQUIRE(received);

    // Gateway to client binary frame.
    const Array<byte> reply = BytesOf(u8"hi browser");
    gateway.SendBinary(clientId, Span<const byte>{reply.Data(), reply.Size()});
    Array<byte> got;
    REQUIRE(client.ReceiveBinary(gateway, got));
    REQUIRE(got.Size() == reply.Size());
    for (usize j = 0; j < reply.Size(); ++j)
    {
        CHECK(got[j] == reply[j]);
    }

    // A ping is answered with a pong carrying the same payload.
    {
        const Array<byte> pingBody = BytesOf(u8"ka");
        Array<byte> wire;
        net::EncodeWebSocketFrame(net::WsOpcode::Ping,
                                  Span<const byte>{pingBody.Data(), pingBody.Size()}, true,
                                  0x1234u, wire);
        (void)client.socket.Send(Span<const byte>{wire.Data(), wire.Size()});
        Array<byte> pongWire;
        bool pong = false;
        for (int i = 0; i < 300 && !pong; ++i)
        {
            gateway.Pump();
            byte chunk[64];
            const i64 gotBytes = client.socket.Receive(Span<byte>{chunk, sizeof(chunk)});
            for (i64 j = 0; j < gotBytes; ++j)
            {
                pongWire.PushBack(chunk[j]);
            }
            pong = pongWire.Size() >= 2 + pingBody.Size() &&
                   (static_cast<u8>(pongWire[0]) & 0x0Fu) == 0xAu;
            if (!pong)
            {
                SleepMilliseconds(1);
            }
        }
        CHECK(pong);
    }
}

TEST_CASE("websocket: garbage on the accept socket is rejected, not crashed on")
{
    net::WebSocketServerGateway gateway(DefaultAllocator(), 0);
    REQUIRE(gateway.IsOpen());

    net::TcpSocket rogue = net::TcpSocket::Connect(u8"127.0.0.1", gateway.BoundPort());
    REQUIRE(rogue.IsOpen());
    for (int i = 0; i < 300 && rogue.ConnectStatus() == 0; ++i)
    {
        gateway.Pump();
        SleepMilliseconds(1);
    }
    REQUIRE(rogue.ConnectStatus() == 1);

    const StringView junk = u8"NOT-HTTP \x01\x02 garbage\r\nmore trash\r\n\r\n";
    Array<byte> bytes;
    for (usize i = 0; i < junk.Size(); ++i)
    {
        bytes.PushBack(static_cast<byte>(junk[i]));
    }
    (void)rogue.Send(Span<const byte>{bytes.Data(), bytes.Size()});
    for (int i = 0; i < 100; ++i)
    {
        gateway.Pump();
        SleepMilliseconds(1);
    }
    CHECK(gateway.ClientCount() == 0u); // dropped, no upgrade
}

TEST_CASE("websocket: hybrid socket - UDP and WS peers arrive as distinct datagram endpoints")
{
    net::WebSocketHybridSocket hybrid(DefaultAllocator(), 0, 0);
    REQUIRE(hybrid.IsOpen());
    REQUIRE(hybrid.BoundPort() != 0u);
    REQUIRE(hybrid.WebSocketBoundPort() != 0u);

    // A WebSocket peer: upgrade, then a binary frame becomes a datagram with the WebSocket endpoint bit.
    WsTestClient browser;
    REQUIRE(browser.ConnectAndUpgrade(hybrid.Gateway(), hybrid.WebSocketBoundPort()));
    const Array<byte> wsMsg = BytesOf(u8"from-browser");
    browser.SendBinary(Span<const byte>{wsMsg.Data(), wsMsg.Size()});

    net::DatagramEndpoint wsFrom{};
    Array<byte> wsPayload;
    bool wsGot = false;
    for (int i = 0; i < 500 && !wsGot; ++i)
    {
        net::DatagramEndpoint from{};
        Array<byte> payload;
        while (hybrid.Receive(from, payload))
        {
            if (net::IsWebSocketEndpoint(from))
            {
                wsFrom = from;
                wsPayload = static_cast<Array<byte>&&>(payload);
                wsGot = true;
            }
        }
        if (!wsGot)
        {
            SleepMilliseconds(1);
        }
    }
    REQUIRE(wsGot);
    REQUIRE(wsPayload.Size() == wsMsg.Size());
    CHECK(net::IsWebSocketEndpoint(wsFrom));

    // Replying to that endpoint routes back through the gateway as a frame.
    const Array<byte> wsReply = BytesOf(u8"to-browser");
    hybrid.Send(wsFrom, Span<const byte>{wsReply.Data(), wsReply.Size()});
    Array<byte> browserGot;
    REQUIRE(browser.ReceiveBinary(hybrid.Gateway(), browserGot));
    CHECK(browserGot.Size() == wsReply.Size());

    // A UDP peer on the same socket keeps its normal endpoint (no WebSocket bit).
    net::UdpSocket udpPeer(0);
    REQUIRE(udpPeer.IsOpen());
    const Array<byte> udpMsg = BytesOf(u8"from-native");
    const net::DatagramEndpoint hostEndpoint =
        net::MakeEndpoint(0x7F000001u, hybrid.BoundPort()); // 127.0.0.1
    udpPeer.Send(hostEndpoint, Span<const byte>{udpMsg.Data(), udpMsg.Size()});
    bool udpGot = false;
    for (int i = 0; i < 500 && !udpGot; ++i)
    {
        net::DatagramEndpoint from{};
        Array<byte> payload;
        while (hybrid.Receive(from, payload))
        {
            if (!net::IsWebSocketEndpoint(from) && payload.Size() == udpMsg.Size())
            {
                udpGot = true;
            }
        }
        if (!udpGot)
        {
            SleepMilliseconds(1);
        }
    }
    CHECK(udpGot);
}
