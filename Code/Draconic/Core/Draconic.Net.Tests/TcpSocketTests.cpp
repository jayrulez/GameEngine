// draconic.net:socket - the TCP stream backend over localhost (integration: uses the OS TCP stack).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.net;

using namespace draconic::foundation;
namespace net = draconic::net;

TEST_CASE("tcp: connect + accept + bidirectional stream over localhost")
{
    net::TcpListener listener(0);
    REQUIRE(listener.IsOpen());
    CHECK(listener.BoundPort() != 0u);

    net::TcpSocket client = net::TcpSocket::Connect(u8"127.0.0.1", listener.BoundPort());
    REQUIRE(client.IsOpen());

    // Drive the non-blocking connect + accept to completion.
    net::TcpSocket server;
    for (int i = 0; i < 300 && !(server.IsOpen() && client.ConnectStatus() == 1); ++i)
    {
        if (!server.IsOpen())
        {
            net::TcpSocket s = listener.Accept();
            if (s.IsOpen())
            {
                server = static_cast<net::TcpSocket&&>(s);
            }
        }
        SleepMilliseconds(1);
    }
    REQUIRE(server.IsOpen());
    CHECK(client.ConnectStatus() == 1);

    // client -> server
    const u8 out[] = {0x01, 0x02, 0x03, 0x04};
    CHECK(client.Send(Span<const byte>(reinterpret_cast<const byte*>(out), sizeof(out))) == 4);

    byte buf[64] = {};
    i64 got = 0;
    for (int i = 0; i < 300 && got <= 0; ++i)
    {
        got = server.Receive(Span<byte>(buf, sizeof(buf)));
        if (got <= 0)
            SleepMilliseconds(1);
    }
    REQUIRE(got == 4);
    CHECK(static_cast<u8>(buf[0]) == 0x01u);
    CHECK(static_cast<u8>(buf[3]) == 0x04u);

    // server -> client
    CHECK(server.Send(Span<const byte>(buf, 4)) == 4);
    byte rbuf[64] = {};
    i64 back = 0;
    for (int i = 0; i < 300 && back <= 0; ++i)
    {
        back = client.Receive(Span<byte>(rbuf, sizeof(rbuf)));
        if (back <= 0)
            SleepMilliseconds(1);
    }
    REQUIRE(back == 4);
    CHECK(static_cast<u8>(rbuf[0]) == 0x01u);

    // Closing the client is observed by the server as a closed stream (-1).
    client.Close();
    i64 afterClose = 0;
    for (int i = 0; i < 300; ++i)
    {
        afterClose = server.Receive(Span<byte>(rbuf, sizeof(rbuf)));
        if (afterClose != 0)
            break;
        SleepMilliseconds(1);
    }
    CHECK(afterClose == -1); // peer closed
}
