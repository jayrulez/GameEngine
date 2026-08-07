/// Draconic::Net - `draconic.net:datagram` partition.
///
/// The UNRELIABLE datagram substrate the reliability layer is built on (docs/design/networking.md
/// §4). `IDatagramSocket` is a connectionless send/recv over an opaque `DatagramEndpoint`; its
/// backends are a real UDP socket (Foundation/System, a later slice) and the in-memory `SimDatagramNetwork`
/// here. Splitting reliability from the socket is what lets the reliable-UDP protocol be tested
/// against deterministic packet loss/reorder with no OS sockets.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.net:datagram;

import draconic.foundation;
import :transport; // SimConditions (shared sim knobs)

using namespace draconic::foundation;

export namespace draconic::net
{

    // An opaque datagram address. Real UDP packs IPv4+port; the sim uses a socket index. Comparable +
    // hashable so a transport can key per-remote connection state off it.
    struct DatagramEndpoint
    {
        u64 value = 0;
        [[nodiscard]] bool operator==(const DatagramEndpoint& o) const noexcept
        {
            return value == o.value;
        }
        [[nodiscard]] bool operator!=(const DatagramEndpoint& o) const noexcept
        {
            return value != o.value;
        }
        [[nodiscard]] bool IsValid() const noexcept { return value != 0; }
    };

    // Connectionless, unreliable datagram I/O - the substrate reliability rides on. No ordering, no
    // delivery guarantee; a datagram arrives whole or not at all.
    class IDatagramSocket
    {
    public:
        virtual ~IDatagramSocket() = default;
        virtual void Send(const DatagramEndpoint& to, Span<const byte> data) = 0;
        // Dequeue one received datagram; false when none. Sets `from` to the sender.
        [[nodiscard]] virtual bool Receive(DatagramEndpoint& from, Array<byte>& out) = 0;
        [[nodiscard]] virtual DatagramEndpoint LocalEndpoint() const = 0;
    };

    class SimDatagramNetwork;

    // A sim socket: Send routes through the shared network (which applies loss/latency on Advance);
    // Receive drains what the network has delivered here.
    class SimDatagramSocket final : public IDatagramSocket
    {
    public:
        void Send(const DatagramEndpoint& to, Span<const byte> data) override;
        [[nodiscard]] bool Receive(DatagramEndpoint& from, Array<byte>& out) override
        {
            if (m_recvHead >= m_recv.Size())
            {
                return false;
            }
            from = m_recv[m_recvHead].from;
            out = static_cast<Array<byte>&&>(m_recv[m_recvHead].data);
            ++m_recvHead;
            if (m_recvHead >= m_recv.Size())
            {
                m_recv.Clear();
                m_recvHead = 0;
            }
            return true;
        }
        [[nodiscard]] DatagramEndpoint LocalEndpoint() const override { return m_endpoint; }

    private:
        friend class SimDatagramNetwork;
        struct Received
        {
            DatagramEndpoint from;
            Array<byte> data;
        };
        SimDatagramNetwork* m_net = nullptr;
        DatagramEndpoint m_endpoint;
        Array<Received> m_recv;
        usize m_recvHead = 0;
    };

    // A deterministic in-memory datagram network: hands out sockets with unique endpoints and routes
    // datagrams between them under injectable conditions (seeded rng + manual clock via Advance). This
    // is the reliability layer's test harness - lossy on purpose.
    class SimDatagramNetwork
    {
    public:
        explicit SimDatagramNetwork(const SimConditions& sim = {}) : m_sim(sim), m_rng(sim.seed) {}

        // Create a socket with a fresh endpoint (value = 1,2,3,...). The network owns it.
        [[nodiscard]] IDatagramSocket* CreateSocket()
        {
            auto sock = MakeUnique<SimDatagramSocket>(DefaultAllocator());
            sock->m_net = this;
            sock->m_endpoint = DatagramEndpoint{m_nextEndpoint++};
            SimDatagramSocket* raw = sock.Get();
            m_sockets.PushBack(static_cast<UniquePtr<SimDatagramSocket>&&>(sock));
            return raw;
        }

        // Advance the clock; deliver every datagram whose time has come (in delivery-time order).
        void Advance(f32 deltaMs)
        {
            m_nowMs += static_cast<f64>(deltaMs);
            while (!m_inflight.IsEmpty() && m_inflight[0].deliverAt <= m_nowMs)
            {
                InFlight pkt = static_cast<InFlight&&>(m_inflight[0]);
                m_inflight.RemoveAt(0);
                if (SimDatagramSocket* dst = Find(pkt.to))
                {
                    SimDatagramSocket::Received rec;
                    rec.from = pkt.from;
                    rec.data = static_cast<Array<byte>&&>(pkt.data);
                    dst->m_recv.PushBack(static_cast<SimDatagramSocket::Received&&>(rec));
                }
            }
        }

        [[nodiscard]] const SimConditions& Sim() const noexcept { return m_sim; }
        void SetSim(const SimConditions& sim) { m_sim = sim; }

    private:
        friend class SimDatagramSocket;
        struct InFlight
        {
            DatagramEndpoint from;
            DatagramEndpoint to;
            Array<byte> data;
            f64 deliverAt = 0.0;
            u64 order = 0;
        };

        [[nodiscard]] SimDatagramSocket* Find(const DatagramEndpoint& ep)
        {
            for (const UniquePtr<SimDatagramSocket>& s : m_sockets)
            {
                if (s->m_endpoint == ep)
                {
                    return s.Get();
                }
            }
            return nullptr;
        }

        void Route(const DatagramEndpoint& from, const DatagramEndpoint& to, Span<const byte> data)
        {
            // Every datagram is subject to loss/reorder/dup (this is the raw unreliable layer).
            if (Chance(m_sim.lossPct))
            {
                return;
            }
            f64 deliverAt = m_nowMs + static_cast<f64>(Max(0.0f, m_sim.latencyMs + Jitter()));
            if (Chance(m_sim.reorderPct))
            {
                deliverAt += static_cast<f64>(m_rng.NextFloat() * Max(1.0f, m_sim.latencyMs));
            }
            Enqueue(from, to, data, deliverAt);
            if (Chance(m_sim.dupPct))
            {
                Enqueue(from, to, data, deliverAt + static_cast<f64>(m_rng.NextFloat()));
            }
        }

        void Enqueue(const DatagramEndpoint& from, const DatagramEndpoint& to,
                     Span<const byte> data, f64 deliverAt)
        {
            InFlight pkt;
            pkt.from = from;
            pkt.to = to;
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

        [[nodiscard]] bool Chance(f32 pct) { return pct > 0.0f && m_rng.NextFloat() < pct; }
        [[nodiscard]] f32 Jitter()
        {
            return (m_sim.jitterMs > 0.0f) ? (m_rng.NextFloat() * 2.0f - 1.0f) * m_sim.jitterMs
                                           : 0.0f;
        }

        SimConditions m_sim;
        Random m_rng;
        f64 m_nowMs = 0.0;
        u64 m_nextEndpoint = 1;
        u64 m_orderCounter = 0;
        Array<InFlight> m_inflight;
        Array<UniquePtr<SimDatagramSocket>> m_sockets;
    };

    inline void SimDatagramSocket::Send(const DatagramEndpoint& to, Span<const byte> data)
    {
        if (m_net != nullptr)
        {
            m_net->Route(m_endpoint, to, data);
        }
    }

}
