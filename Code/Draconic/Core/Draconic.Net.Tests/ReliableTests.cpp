// draconic.net:reliable - the reliable-UDP transport over the lossy datagram sim.
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

    // A client+server pair over one sim network, with a step pump.
    struct Fixture
    {
        net::SimDatagramNetwork net;
        net::IDatagramSocket* sa;
        net::IDatagramSocket* sb;
        net::ReliableTransport client;
        net::ReliableTransport server;

        explicit Fixture(const net::SimConditions& sim, const net::ReliableConfig& cfg = {})
            : net(sim), sa(net.CreateSocket()), sb(net.CreateSocket()), client(*sa, cfg),
              server(*sb, cfg)
        {
        }

        void Step(f32 dt = 10.0f)
        {
            net.Advance(dt);
            client.Update(dt);
            server.Update(dt);
        }
        void Pump(int steps, f32 dt = 10.0f)
        {
            for (int i = 0; i < steps; ++i)
            {
                Step(dt);
            }
        }
    };

    // Drain all Received first-bytes from a transport (call inside/after pumping).
    Array<u8> DrainReceived(net::INetTransport& t)
    {
        Array<u8> out;
        net::NetEvent ev;
        while (t.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Received && ev.payload.Size() > 0)
            {
                out.PushBack(static_cast<u8>(ev.payload[0]));
            }
        }
        return out;
    }
    bool AnyKind(net::INetTransport& t, net::NetEventKind kind)
    {
        net::NetEvent ev;
        bool found = false;
        while (t.Poll(ev))
        {
            if (ev.kind == kind)
            {
                found = true;
            }
        }
        return found;
    }
}

TEST_CASE("reliable: handshake connects both sides")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    CHECK(sp != net::kInvalidPeer);
    fx.Pump(20);
    CHECK(AnyKind(fx.client, net::NetEventKind::Connected));
    CHECK(AnyKind(fx.server, net::NetEventKind::Connected));
}

TEST_CASE("reliable: all reliable messages arrive in order despite 50% loss")
{
    net::SimConditions sim;
    sim.latencyMs = 30.0f;
    sim.jitterMs = 10.0f;
    sim.lossPct = 0.5f;
    sim.reorderPct = 0.3f;
    sim.seed = 123;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20); // establish
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 30; ++i)
    {
        Array<byte> m = Bytes({i});
        fx.client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    }
    Array<u8> got;
    for (int s = 0; s < 400; ++s)
    {
        fx.Step();
        Array<u8> chunk = DrainReceived(fx.server);
        for (u8 b : chunk)
        {
            got.PushBack(b);
        }
        if (got.Size() >= 30u)
        {
            break;
        }
    }
    REQUIRE(got.Size() == 30u); // none lost
    for (u8 i = 0; i < 30; ++i)
    {
        CHECK(got[i] == i);
    } // strictly in order, no dups
}

TEST_CASE("reliable: duplication never double-delivers a reliable message")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.dupPct = 1.0f;
    sim.seed = 5;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 8; ++i)
    {
        Array<byte> m = Bytes({i});
        fx.client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    }
    Array<u8> got;
    for (int s = 0; s < 200; ++s)
    {
        fx.Step();
        Array<u8> c = DrainReceived(fx.server);
        for (u8 b : c)
            got.PushBack(b);
        if (got.Size() >= 8u)
            break;
    }
    REQUIRE(got.Size() == 8u); // exactly once each despite 100% duplication
    for (u8 i = 0; i < 8; ++i)
    {
        CHECK(got[i] == i);
    }
}

TEST_CASE("reliable: unreliable messages can drop under loss (no resend)")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    sim.lossPct = 0.6f;
    sim.seed = 77;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)DrainReceived(fx.server);

    for (u8 i = 0; i < 50; ++i)
    {
        Array<byte> m = Bytes({i});
        fx.client.Send(sp, 1, m.AsSpan(), net::Reliability::Unreliable);
        fx.Step();
    }
    fx.Pump(30);
    const usize delivered = DrainReceived(fx.server).Size();
    CHECK(delivered > 0u);
    CHECK(delivered < 50u); // lossy + no resend => some are gone forever
}

TEST_CASE("reliable: RTT is estimated from acks")
{
    net::SimConditions sim;
    sim.latencyMs = 40.0f; // ~80ms round trip
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(10);
    for (int i = 0; i < 5; ++i)
    {
        Array<byte> m = Bytes({1});
        fx.client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
        fx.Pump(10);
    }
    const net::TransportStats st = fx.client.Stats(sp);
    CHECK(st.rttMs > 40.0f); // at least one one-way; realistically ~80ms
    CHECK(st.rttMs < 200.0f);
}

TEST_CASE("reliable: a silent peer times out into Disconnected")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    net::ReliableConfig cfg;
    cfg.timeoutMs = 500.0f;
    Fixture fx(sim, cfg);
    fx.server.SetAccepting(true);
    (void)fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    CHECK(AnyKind(fx.client, net::NetEventKind::Connected));

    // Server goes dark: only tick the client + the network (client hears nothing).
    bool disconnected = false;
    for (int s = 0; s < 100; ++s)
    {
        fx.net.Advance(10.0f);
        fx.client.Update(10.0f);
        if (AnyKind(fx.client, net::NetEventKind::Disconnected))
        {
            disconnected = true;
            break;
        }
    }
    CHECK(disconnected);
}

TEST_CASE("reliable: a large message fragments + reassembles intact over loss")
{
    net::SimConditions sim;
    sim.latencyMs = 20.0f;
    sim.lossPct = 0.3f;
    sim.reorderPct = 0.2f;
    sim.seed = 31;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    {
        net::NetEvent ev;
        while (fx.server.Poll(ev))
        {
        }
    }

    // 5000 bytes => several fragments (FragPayloadLimit ~1136).
    Array<byte> big;
    big.Resize(5000);
    for (usize i = 0; i < big.Size(); ++i)
    {
        big[i] = static_cast<byte>((i * 7u + 3u) & 0xFFu);
    }
    fx.client.Send(sp, 0, big.AsSpan(), net::Reliability::ReliableOrdered);

    Array<byte> got;
    for (int s = 0; s < 600; ++s)
    {
        fx.Step();
        net::NetEvent ev;
        while (fx.server.Poll(ev))
        {
            if (ev.kind == net::NetEventKind::Received)
            {
                got = static_cast<Array<byte>&&>(ev.payload);
            }
        }
        if (got.Size() == 5000u)
        {
            break;
        }
    }
    REQUIRE(got.Size() == 5000u); // reassembled whole
    bool exact = true;
    for (usize i = 0; i < got.Size(); ++i)
    {
        if (static_cast<u8>(got[i]) != static_cast<u8>((i * 7u + 3u) & 0xFFu))
        {
            exact = false;
            break;
        }
    }
    CHECK(exact); // byte-exact after reassembly
}

namespace
{
    // Decorator that counts datagrams sent (to prove resend timing isn't per-tick).
    struct CountingSocket final : net::IDatagramSocket
    {
        net::IDatagramSocket* inner;
        int sends = 0;
        explicit CountingSocket(net::IDatagramSocket* i) : inner(i) {}
        void Send(const net::DatagramEndpoint& to, Span<const byte> data) override
        {
            ++sends;
            inner->Send(to, data);
        }
        bool Receive(net::DatagramEndpoint& from, Array<byte>& out) override
        {
            return inner->Receive(from, out);
        }
        net::DatagramEndpoint LocalEndpoint() const override { return inner->LocalEndpoint(); }
    };
}

TEST_CASE("reliable: resend is RTT-timed, not per-tick (no flooding for an unacked message)")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    net::ReliableConfig cfg;
    cfg.minResendMs = 50.0f;
    cfg.keepAliveMs = 100.0f;
    net::SimDatagramNetwork network(sim);
    net::IDatagramSocket* sa = network.CreateSocket();
    net::IDatagramSocket* sb = network.CreateSocket();
    CountingSocket counting(sa);
    net::ReliableTransport client(counting, cfg);
    net::ReliableTransport server(*sb, cfg);
    server.SetAccepting(true);
    const net::PeerId sp = client.Connect(sb->LocalEndpoint());
    for (int i = 0; i < 20; ++i)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
        server.Update(10.0f);
    }

    // Send a reliable message, then let the server go DARK so it never acks. Over 200ms with a 50ms
    // resend floor, the client should resend ~4 times (+ ~2 keepalives), NOT ~20 (once per 10ms tick).
    Array<byte> m = Bytes({0x42});
    client.Send(sp, 0, m.AsSpan(), net::Reliability::ReliableOrdered);
    const int before = counting.sends;
    for (int i = 0; i < 20; ++i)
    {
        network.Advance(10.0f);
        client.Update(10.0f);
    } // 200ms, server dark
    const int sent = counting.sends - before;
    CHECK(sent >= 2);  // it does keep retrying
    CHECK(sent <= 10); // but RTT-timed (~4 resends + ~2 keepalives), not per-tick (~20)
}

TEST_CASE("reliable: explicit Disconnect notifies the peer")
{
    net::SimConditions sim;
    sim.latencyMs = 10.0f;
    Fixture fx(sim);
    fx.server.SetAccepting(true);
    const net::PeerId sp = fx.client.Connect(fx.sb->LocalEndpoint());
    fx.Pump(20);
    (void)AnyKind(fx.server, net::NetEventKind::Connected);
    fx.client.Disconnect(sp);
    fx.Pump(10);
    CHECK(AnyKind(fx.server, net::NetEventKind::Disconnected));
}
