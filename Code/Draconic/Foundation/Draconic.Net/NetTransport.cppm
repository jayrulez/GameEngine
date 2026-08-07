/// Draconic::Net - `draconic.net:transport` partition.
///
/// The transport seam (docs/design/networking.md §4). `INetTransport` is the swappable-backend
/// abstraction the rest of the stack (session, reliability, RPC, replication) rides on - because
/// browsers cannot open raw UDP, multiple backends are MANDATORY (reliable-UDP, loopback/sim, later
/// websocket/webrtc). This slice ships the interface + the in-memory `LoopbackLink` sim, which is
/// what makes rolling our own transport safe: injectable latency / jitter / loss / reorder /
/// duplication with a SEEDED rng and a MANUAL logical clock, so reliability and replication become
/// deterministic headless unit tests (no sockets, no flake).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.net:transport;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::net
{

    // A connected remote. 0 = invalid/none.
    using PeerId = u32;
    inline constexpr PeerId kInvalidPeer = 0;

    // Delivery guarantee for a Send. Channels are independent ordering domains.
    enum class Reliability : u8
    {
        Unreliable, // fire-and-forget: may drop, reorder, duplicate (cosmetic/continuous data)
        UnreliableSequenced, // may drop; never delivered out of order (stale ones discarded)
        ReliableOrdered, // guaranteed, in order (commands/orders - the turn-based primary channel)
    };

    enum class NetEventKind : u8
    {
        Connected,
        Disconnected,
        Received
    };

    // A transport event drained via Poll. `payload` is owned (moved out on Received).
    struct NetEvent
    {
        NetEventKind kind = NetEventKind::Received;
        PeerId peer = kInvalidPeer;
        u8 channel = 0;
        Array<byte> payload; // Received only
    };

    // Per-peer link telemetry (net profiler / replication bandwidth budgeting later).
    struct TransportStats
    {
        f32 rttMs = 0.0f;
        f32 lossPct = 0.0f;
        u32 sentBytes = 0;
        u32 recvBytes = 0;
        u32 queuedBytes = 0;
    };

    // The swappable transport backend. Time is driven by Update(dtMs) so a deterministic sim (loopback)
    // and a real socket transport share the same shape.
    class INetTransport
    {
    public:
        virtual ~INetTransport() = default;

        virtual void Send(PeerId peer, u8 channel, Span<const byte> data,
                          Reliability reliability) = 0;
        virtual void Disconnect(PeerId peer) = 0;

        // Drain one queued event; false when none remain this tick. Call in a loop.
        [[nodiscard]] virtual bool Poll(NetEvent& out) = 0;

        // Advance internal time (deliver due packets, run timeouts/resends). dtMs in milliseconds.
        virtual void Update(f32 deltaMs) = 0;

        [[nodiscard]] virtual TransportStats Stats(PeerId peer) const = 0;
    };

    // Injectable network conditions for the loopback sim. All probabilities are 0..1.
    struct SimConditions
    {
        f32 latencyMs = 0.0f;  // base one-way latency
        f32 jitterMs = 0.0f;   // +/- uniform jitter added to latency
        f32 lossPct = 0.0f;    // drop probability (UNRELIABLE channels only)
        f32 dupPct = 0.0f;     // duplicate probability (UNRELIABLE channels only)
        f32 reorderPct = 0.0f; // extra-random-delay probability (UNRELIABLE channels only)
        u64 seed = 0x9E3779B97F4A7C15ull;
    };

    class LoopbackLink;

    // One side of an in-memory connected pair. Send() hands the packet to the shared link (which applies
    // the sim on Update); Poll() drains what the link has delivered to this side.
    class LoopbackEndpoint final : public INetTransport
    {
    public:
        void Send(PeerId peer, u8 channel, Span<const byte> data, Reliability reliability) override;
        void Disconnect(PeerId peer) override;
        [[nodiscard]] bool Poll(NetEvent& out) override
        {
            if (m_recvHead >= m_recv.Size())
            {
                return false;
            }
            out = static_cast<NetEvent&&>(m_recv[m_recvHead++]);
            if (m_recvHead >= m_recv.Size())
            {
                m_recv.Clear();
                m_recvHead = 0;
            }
            return true;
        }
        void Update(f32 deltaMs) override; // delegates the clock to the shared link
        [[nodiscard]] TransportStats Stats(PeerId peer) const override;

    private:
        friend class LoopbackLink;
        LoopbackLink* m_link = nullptr;
        u32 m_side = 0; // 0 or 1
        Array<NetEvent> m_recv;
        usize m_recvHead = 0;
        TransportStats m_stats;
    };

    // A deterministic in-memory link connecting two endpoints (A <-> B) with injectable sim conditions.
    // The MANUAL clock (Advance / either endpoint's Update) + SEEDED rng make delivery reproducible.
    // The remote PeerId on each side is 1 (a pair has exactly one remote).
    class LoopbackLink
    {
    public:
        explicit LoopbackLink(const SimConditions& sim = {}) : m_sim(sim), m_rng(sim.seed)
        {
            m_a.m_link = this;
            m_a.m_side = 0;
            m_b.m_link = this;
            m_b.m_side = 1;
            // Both sides observe the connection immediately (a real handshake lands in the session layer).
            Deliver(0, MakeEvent(NetEventKind::Connected, kRemotePeer, 0, {}));
            Deliver(1, MakeEvent(NetEventKind::Connected, kRemotePeer, 0, {}));
        }

        [[nodiscard]] INetTransport& A() noexcept { return m_a; }
        [[nodiscard]] INetTransport& B() noexcept { return m_b; }

        [[nodiscard]] const SimConditions& Sim() const noexcept { return m_sim; }
        void SetSim(const SimConditions& sim) { m_sim = sim; }

        // Advance the logical clock and deliver every packet whose time has come (in delivery-time order).
        void Advance(f32 deltaMs)
        {
            m_nowMs += static_cast<f64>(deltaMs);
            while (!m_inflight.IsEmpty() && m_inflight[0].deliverAt <= m_nowMs)
            {
                InFlight pkt = static_cast<InFlight&&>(m_inflight[0]);
                m_inflight.RemoveAt(0);
                NetEvent ev;
                ev.kind = NetEventKind::Received;
                ev.peer = kRemotePeer;
                ev.channel = pkt.channel;
                ev.payload = static_cast<Array<byte>&&>(pkt.data);
                Deliver(pkt.dstSide, static_cast<NetEvent&&>(ev));
            }
        }

        static constexpr PeerId kRemotePeer = 1;

    private:
        friend class LoopbackEndpoint;

        struct InFlight
        {
            u32 dstSide = 0;
            u8 channel = 0;
            Array<byte> data;
            f64 deliverAt = 0.0;
            u64 order = 0; // FIFO tiebreak for equal deliverAt
        };

        static NetEvent MakeEvent(NetEventKind kind, PeerId peer, u8 channel, Array<byte>&& payload)
        {
            NetEvent ev;
            ev.kind = kind;
            ev.peer = peer;
            ev.channel = channel;
            ev.payload = static_cast<Array<byte>&&>(payload);
            return ev;
        }

        void Deliver(u32 side, NetEvent&& ev)
        {
            (side == 0 ? m_a : m_b).m_recv.PushBack(static_cast<NetEvent&&>(ev));
        }

        // Insert an in-flight packet keeping m_inflight ordered by (deliverAt, order).
        void Enqueue(u32 dstSide, u8 channel, Span<const byte> data, f64 deliverAt)
        {
            InFlight pkt;
            pkt.dstSide = dstSide;
            pkt.channel = channel;
            pkt.data.Resize(data.Size());
            if (data.Size() > 0)
            {
                MemCopy(pkt.data.Data(), data.Data(), data.Size());
            }
            pkt.deliverAt = deliverAt;
            pkt.order = m_orderCounter++;
            usize i = m_inflight.Size();
            while (i > 0)
            {
                const InFlight& prev = m_inflight[i - 1];
                if (prev.deliverAt < deliverAt ||
                    (prev.deliverAt == deliverAt && prev.order < pkt.order))
                {
                    break;
                }
                --i;
            }
            m_inflight.Insert(i, static_cast<InFlight&&>(pkt));
        }

        // The heart of the sim: apply loss/dup/reorder (unreliable channels) + latency/jitter, then queue.
        void Submit(u32 srcSide, u8 channel, Span<const byte> data, Reliability reliability)
        {
            const u32 dstSide = (srcSide == 0) ? 1u : 0u;
            (srcSide == 0 ? m_a : m_b).m_stats.sentBytes += static_cast<u32>(data.Size());

            const bool unreliable = (reliability != Reliability::ReliableOrdered);
            // Loss (unreliable only) - reliable channels are a guaranteed-delivery abstraction here; the
            // real ack/resend implementation is the reliable-UDP transport (a later slice).
            if (unreliable && Chance(m_sim.lossPct))
            {
                return;
            }

            const f64 latency = static_cast<f64>(Max(0.0f, m_sim.latencyMs + Jitter()));
            f64 deliverAt = m_nowMs + latency;
            // Reorder (unreliable only): a random extra delay so it can arrive after a later packet.
            if (unreliable && Chance(m_sim.reorderPct))
            {
                deliverAt += static_cast<f64>(m_rng.NextFloat() * Max(1.0f, m_sim.latencyMs));
            }
            Enqueue(dstSide, channel, data, deliverAt);
            // Duplicate (unreliable only): a second copy at a slightly different time.
            if (unreliable && Chance(m_sim.dupPct))
            {
                Enqueue(dstSide, channel, data, deliverAt + static_cast<f64>(m_rng.NextFloat()));
            }
        }

        [[nodiscard]] bool Chance(f32 pct) { return pct > 0.0f && m_rng.NextFloat() < pct; }
        [[nodiscard]] f32 Jitter()
        {
            return (m_sim.jitterMs > 0.0f) ? (m_rng.NextFloat() * 2.0f - 1.0f) * m_sim.jitterMs
                                           : 0.0f;
        }

        SimConditions m_sim;
        Random m_rng;
        f64 m_nowMs = 0.0;
        u64 m_orderCounter = 0;
        Array<InFlight> m_inflight; // ordered by (deliverAt, order)
        LoopbackEndpoint m_a;
        LoopbackEndpoint m_b;
    };

    // --- LoopbackEndpoint methods that need the full LoopbackLink ---

    inline void LoopbackEndpoint::Send(PeerId peer, u8 channel, Span<const byte> data,
                                       Reliability reliability)
    {
        (void)peer; // a pair has one remote
        if (m_link != nullptr)
        {
            m_link->Submit(m_side, channel, data, reliability);
        }
    }
    inline void LoopbackEndpoint::Disconnect(PeerId peer)
    {
        (void)peer;
        if (m_link != nullptr)
        {
            const u32 dst = (m_side == 0) ? 1u : 0u;
            m_link->Deliver(dst, LoopbackLink::MakeEvent(NetEventKind::Disconnected,
                                                         LoopbackLink::kRemotePeer, 0, {}));
        }
    }
    inline void LoopbackEndpoint::Update(f32 deltaMs)
    {
        if (m_link != nullptr)
        {
            m_link->Advance(deltaMs);
        }
    }
    inline TransportStats LoopbackEndpoint::Stats(PeerId peer) const
    {
        (void)peer;
        TransportStats s = m_stats;
        if (m_link != nullptr)
        {
            s.rttMs = 2.0f * m_link->Sim().latencyMs;
        }
        return s;
    }

}
