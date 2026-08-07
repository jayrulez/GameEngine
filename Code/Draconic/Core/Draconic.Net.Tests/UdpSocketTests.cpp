// draconic.net:socket - the REAL UDP backend over localhost (integration: uses the OS network stack).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.net;

using namespace draconic::foundation;
namespace net = draconic::net;

TEST_CASE("udp: endpoint pack/unpack and IPv4 parsing round-trip")
{
    const net::DatagramEndpoint e = net::ResolveEndpoint(u8"192.168.1.42", 7777);
    CHECK(e.IsValid());
    CHECK(net::EndpointPort(e) == 7777u);
    // 192.168.1.42 host-order = 0xC0A8012A
    CHECK(net::EndpointIp(e) == 0xC0A8012Au);

    CHECK_FALSE(net::ResolveEndpoint(u8"not.an.ip", 80).IsValid());
}

TEST_CASE("udp: two sockets bind distinct OS ports and exchange a raw datagram over localhost")
{
    net::UdpSocket a(0);
    net::UdpSocket b(0);
    REQUIRE(a.IsOpen());
    REQUIRE(b.IsOpen());
    CHECK(a.BoundPort() != 0u);
    CHECK(a.BoundPort() != b.BoundPort());

    const u8 msg[] = {0xDE, 0xAD};
    a.Send(b.LocalEndpoint(), Span<const byte>(reinterpret_cast<const byte*>(msg), sizeof(msg)));

    // Give the OS a moment, then poll.
    net::DatagramEndpoint from;
    Array<byte> got;
    bool received = false;
    for (int i = 0; i < 100 && !received; ++i)
    {
        if (b.Receive(from, got))
        {
            received = true;
            break;
        }
        SleepMilliseconds(1);
    }
    REQUIRE(received);
    REQUIRE(got.Size() == 2u);
    CHECK(static_cast<u8>(got[0]) == 0xDEu);
    CHECK(net::EndpointPort(from) == a.BoundPort()); // sender identified
}

TEST_CASE("udp: reliable transport handshakes + delivers a reliable message over real sockets")
{
    net::UdpSocket serverSock(0);
    net::UdpSocket clientSock(0);
    REQUIRE(serverSock.IsOpen());
    REQUIRE(clientSock.IsOpen());

    net::ReliableTransport server(serverSock);
    net::ReliableTransport client(clientSock);
    server.SetAccepting(true);
    const net::PeerId sp = client.Connect(serverSock.LocalEndpoint());

    const auto pump = [&](int n)
    {
        for (int i = 0; i < n; ++i)
        {
            client.Update(10.0f);
            server.Update(10.0f);
            SleepMilliseconds(1);
        }
    };
    pump(30); // handshake over the OS stack

    bool serverConnected = false;
    {
        net::NetEvent ev;
        while (server.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Connected)
                serverConnected = true;
        }
    }
    CHECK(serverConnected);

    const u8 payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
    client.Send(sp, 0, Span<const byte>(reinterpret_cast<const byte*>(payload), sizeof(payload)),
                net::Reliability::ReliableOrdered);

    bool delivered = false;
    for (int i = 0; i < 100 && !delivered; ++i)
    {
        pump(2);
        net::NetEvent ev;
        while (server.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Received && ev.payload.Size() == 4 &&
                static_cast<u8>(ev.payload[0]) == 0xCAu && static_cast<u8>(ev.payload[3]) == 0xBEu)
            {
                delivered = true;
            }
        }
    }
    CHECK(delivered);
}
