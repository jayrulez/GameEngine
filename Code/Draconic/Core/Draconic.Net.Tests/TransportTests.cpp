// draconic.net:transport - the loopback/sim transport (deterministic latency/loss/reorder/dup).
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
    // Drain every currently-queued event from a transport.
    Array<net::NetEvent> Drain(net::INetTransport& t)
    {
        Array<net::NetEvent> out;
        net::NetEvent ev;
        while (t.Poll(ev))
        {
            out.PushBack(static_cast<net::NetEvent&&>(ev));
        }
        return out;
    }
}

TEST_CASE("transport: both sides see Connected on link creation")
{
    net::LoopbackLink link;
    Array<net::NetEvent> a = Drain(link.A());
    Array<net::NetEvent> b = Drain(link.B());
    REQUIRE(a.Size() == 1u);
    REQUIRE(b.Size() == 1u);
    CHECK(a[0].kind == net::NetEventKind::Connected);
    CHECK(b[0].kind == net::NetEventKind::Connected);
    CHECK(a[0].peer == net::LoopbackLink::kRemotePeer);
}

TEST_CASE("transport: a reliable message arrives after the latency, not before")
{
    net::SimConditions sim;
    sim.latencyMs = 50.0f;
    net::LoopbackLink link(sim);
    (void)Drain(link.A());
    (void)Drain(link.B()); // clear Connected

    const Array<byte> msg = Bytes({0xDE, 0xAD, 0xBE, 0xEF});
    link.A().Send(net::LoopbackLink::kRemotePeer, 0, msg.AsSpan(),
                  net::Reliability::ReliableOrdered);

    link.Advance(30.0f); // not yet
    CHECK(Drain(link.B()).Size() == 0u);
    link.Advance(30.0f); // now 60ms >= 50ms latency
    Array<net::NetEvent> got = Drain(link.B());
    REQUIRE(got.Size() == 1u);
    CHECK(got[0].kind == net::NetEventKind::Received);
    REQUIRE(got[0].payload.Size() == 4u);
    CHECK(static_cast<u8>(got[0].payload[0]) == 0xDEu);
    CHECK(static_cast<u8>(got[0].payload[3]) == 0xEFu);
}

TEST_CASE("transport: reliable channel is never dropped, even at 100% loss")
{
    net::SimConditions sim;
    sim.lossPct = 1.0f; // drop everything droppable
    net::LoopbackLink link(sim);
    (void)Drain(link.A());
    (void)Drain(link.B());

    for (int i = 0; i < 10; ++i)
    {
        link.A().Send(net::LoopbackLink::kRemotePeer, 0, Bytes({static_cast<u8>(i)}).AsSpan(),
                      net::Reliability::ReliableOrdered);
    }
    link.Advance(1.0f);
    CHECK(Drain(link.B()).Size() == 10u); // all reliable messages survive
}

TEST_CASE("transport: unreliable channel drops packets under loss (deterministic)")
{
    net::SimConditions sim;
    sim.lossPct = 0.5f;
    sim.seed = 42;
    net::LoopbackLink link(sim);
    (void)Drain(link.A());
    (void)Drain(link.B());

    for (int i = 0; i < 100; ++i)
    {
        link.A().Send(net::LoopbackLink::kRemotePeer, 0, Bytes({static_cast<u8>(i)}).AsSpan(),
                      net::Reliability::Unreliable);
    }
    link.Advance(1.0f);
    const usize delivered = Drain(link.B()).Size();
    CHECK(delivered > 0u);
    CHECK(delivered < 100u); // ~half dropped

    // Same seed => same outcome (determinism, the whole point of the sim).
    net::LoopbackLink link2(sim);
    (void)Drain(link2.A());
    (void)Drain(link2.B());
    for (int i = 0; i < 100; ++i)
    {
        link2.A().Send(net::LoopbackLink::kRemotePeer, 0, Bytes({static_cast<u8>(i)}).AsSpan(),
                       net::Reliability::Unreliable);
    }
    link2.Advance(1.0f);
    CHECK(Drain(link2.B()).Size() == delivered);
}

TEST_CASE("transport: reorder produces out-of-order delivery on unreliable channels")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.reorderPct = 1.0f;
    sim.seed = 7;
    net::LoopbackLink link(sim);
    (void)Drain(link.A());
    (void)Drain(link.B());

    for (u8 i = 0; i < 8; ++i)
    {
        link.A().Send(net::LoopbackLink::kRemotePeer, 0, Bytes({i}).AsSpan(),
                      net::Reliability::Unreliable);
    }
    link.Advance(1000.0f); // deliver everything
    Array<net::NetEvent> got = Drain(link.B());
    REQUIRE(got.Size() == 8u);
    bool anyOutOfOrder = false;
    for (usize i = 1; i < got.Size(); ++i)
    {
        if (static_cast<u8>(got[i].payload[0]) < static_cast<u8>(got[i - 1].payload[0]))
        {
            anyOutOfOrder = true;
            break;
        }
    }
    CHECK(anyOutOfOrder);
}

TEST_CASE("transport: reliable channel stays in order despite reorder settings")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.reorderPct = 1.0f;
    sim.dupPct = 1.0f;
    sim.seed = 7;
    net::LoopbackLink link(sim);
    (void)Drain(link.A());
    (void)Drain(link.B());

    for (u8 i = 0; i < 8; ++i)
    {
        link.A().Send(net::LoopbackLink::kRemotePeer, 0, Bytes({i}).AsSpan(),
                      net::Reliability::ReliableOrdered);
    }
    link.Advance(1000.0f);
    Array<net::NetEvent> got = Drain(link.B());
    REQUIRE(got.Size() == 8u); // no dup, no drop
    for (u8 i = 0; i < 8; ++i)
    {
        CHECK(static_cast<u8>(got[i].payload[0]) == i);
    } // strictly in order
}

TEST_CASE("transport: Disconnect delivers a Disconnected event to the peer")
{
    net::LoopbackLink link;
    (void)Drain(link.A());
    (void)Drain(link.B());
    link.A().Disconnect(net::LoopbackLink::kRemotePeer);
    Array<net::NetEvent> got = Drain(link.B());
    REQUIRE(got.Size() == 1u);
    CHECK(got[0].kind == net::NetEventKind::Disconnected);
}

TEST_CASE("transport: Update on an endpoint advances the shared clock")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    net::LoopbackLink link(sim);
    (void)Drain(link.A());
    (void)Drain(link.B());
    link.A().Send(net::LoopbackLink::kRemotePeer, 0, Bytes({1}).AsSpan(),
                  net::Reliability::ReliableOrdered);
    link.B().Update(15.0f); // driving time from either endpoint works
    CHECK(Drain(link.B()).Size() == 1u);
}
