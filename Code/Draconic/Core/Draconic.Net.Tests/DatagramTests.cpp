// draconic.net:datagram - the in-memory unreliable datagram sim.
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
}

TEST_CASE("datagram: a datagram routes A->B after latency, carrying the sender endpoint")
{
    net::SimConditions sim;
    sim.latencyMs = 25.0f;
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* a = network.CreateSocket();
    net::IDatagramSocket* b = network.CreateSocket();

    Array<byte> msg = Bytes({0x11, 0x22, 0x33});
    a->Send(b->LocalEndpoint(), msg.AsSpan());

    net::DatagramEndpoint from;
    Array<byte> got;
    network.Advance(10.0f);
    CHECK_FALSE(b->Receive(from, got)); // not yet
    network.Advance(20.0f);             // now past 25ms
    REQUIRE(b->Receive(from, got));
    CHECK(from == a->LocalEndpoint());
    REQUIRE(got.Size() == 3u);
    CHECK(static_cast<u8>(got[0]) == 0x11u);
}

TEST_CASE("datagram: loss drops datagrams (deterministic under a fixed seed)")
{
    net::SimConditions sim;
    sim.lossPct = 0.5f;
    sim.seed = 999;
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* a = network.CreateSocket();
    net::IDatagramSocket* b = network.CreateSocket();

    for (int i = 0; i < 100; ++i)
    {
        Array<byte> m = Bytes({static_cast<u8>(i)});
        a->Send(b->LocalEndpoint(), m.AsSpan());
    }
    network.Advance(1.0f);

    usize received = 0;
    net::DatagramEndpoint from;
    Array<byte> got;
    while (b->Receive(from, got))
    {
        ++received;
    }
    CHECK(received > 0u);
    CHECK(received < 100u);
}

TEST_CASE("datagram: sockets get distinct endpoints")
{
    net::SimDatagramNetwork network;
    net::IDatagramSocket* a = network.CreateSocket();
    net::IDatagramSocket* b = network.CreateSocket();
    net::IDatagramSocket* c = network.CreateSocket();
    CHECK(a->LocalEndpoint() != b->LocalEndpoint());
    CHECK(b->LocalEndpoint() != c->LocalEndpoint());
    CHECK(a->LocalEndpoint().IsValid());
}
