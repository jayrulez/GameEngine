/// Draconic::Net - `draconic.net:session` partition.
///
/// The session layer (docs/design/networking.md §3 `draconic.net.session`, P1): roles, a peer
/// registry, and the connect/disconnect lifecycle over the reliable-UDP transport. A NetSession is
/// the API the game (and the replication/RPC layers) talk to - the server tracks its connected
/// clients, the client tracks its one server, and Broadcast fans a message out to every peer.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.net:session;

import draconic.foundation;
import :transport;
import :datagram;
import :reliable;
import :wire;

using namespace draconic::foundation;

export namespace draconic::net
{

    // A session's role. A listen-server is also a local player; a dedicated server has no local player
    // (the headless `Server` runtime config, docs/design/networking.md §6).
    enum class NetRole : u8
    {
        None,
        Client,
        ListenServer,
        DedicatedServer
    };

    // Channel 255 is reserved for session-internal control messages (clock sync); it never surfaces as
    // a user Received event. Games should use channels 0..254.
    inline constexpr u8 kControlChannel = 255;

    // A connected remote in the session.
    struct NetPeer
    {
        PeerId id = kInvalidPeer;
        f64 connectedAtMs = 0.0;
    };

    // Roles + peers + lifecycle over a ReliableTransport (which rides an IDatagramSocket - real UDP or
    // the sim). Owns the transport; drives it via Update, surfaces events via PollEvent, and keeps the
    // peer registry current. Higher layers (RPC, replication) build on this.
    class NetSession
    {
    public:
        explicit NetSession(IDatagramSocket& socket, const ReliableConfig& config = {})
            : m_transport(socket, config)
        {
        }

        // Server: begin accepting connections. `dedicated` = headless (no local player).
        void StartServer(bool dedicated = false)
        {
            m_role = dedicated ? NetRole::DedicatedServer : NetRole::ListenServer;
            m_transport.SetAccepting(true);
        }
        // Client: connect to `server`. Returns the server's PeerId (a Connected event confirms it).
        PeerId Connect(const DatagramEndpoint& server)
        {
            m_role = NetRole::Client;
            m_serverPeer = m_transport.Connect(server);
            return m_serverPeer;
        }

        // --- messaging ---
        void Send(PeerId peer, u8 channel, Span<const byte> data, Reliability reliability)
        {
            m_transport.Send(peer, channel, data, reliability);
        }
        // Server: send to every connected peer.
        void Broadcast(u8 channel, Span<const byte> data, Reliability reliability)
        {
            for (const NetPeer& p : m_peers)
            {
                m_transport.Send(p.id, channel, data, reliability);
            }
        }
        // Server: send to every connected peer EXCEPT `except` (e.g. echo an order to everyone but its author).
        void BroadcastExcept(PeerId except, u8 channel, Span<const byte> data,
                             Reliability reliability)
        {
            for (const NetPeer& p : m_peers)
            {
                if (p.id != except)
                {
                    m_transport.Send(p.id, channel, data, reliability);
                }
            }
        }
        void Disconnect(PeerId peer) { m_transport.Disconnect(peer); }

        // --- pump ---
        void Update(f32 deltaMs)
        {
            m_nowMs += static_cast<f64>(deltaMs);
            m_transport.Update(deltaMs);
            NetEvent ev;
            while (m_transport.Poll(ev))
            {
                if (ev.kind == NetEventKind::Connected)
                {
                    AddPeer(ev.peer);
                }
                else if (ev.kind == NetEventKind::Disconnected)
                {
                    RemovePeer(ev.peer);
                }
                else if (ev.kind == NetEventKind::Received && ev.channel == kControlChannel)
                {
                    HandleControl(ev);
                    continue;
                }
                m_events.PushBack(static_cast<NetEvent&&>(ev));
            }
            // Server: broadcast the authoritative clock so clients can sync their tick to it.
            if (IsServer() && !m_peers.IsEmpty() &&
                (m_nowMs - m_lastSyncSendMs) >= static_cast<f64>(m_syncIntervalMs))
            {
                m_lastSyncSendMs = m_nowMs;
                BitWriter w;
                w.WriteU8(static_cast<u8>(ControlType::TimeSync));
                u64 bits = 0;
                MemCopy(&bits, &m_nowMs, sizeof(bits));
                w.WriteU64(bits); // server time (ms), bit-exact
                const Span<const byte> payload = w.Data();
                Broadcast(kControlChannel, payload,
                          Reliability::Unreliable); // frequent, latest-wins
            }
        }

        // --- clock / tick sync (server-authoritative) ---
        void SetFixedStepMs(f32 stepMs) noexcept
        {
            m_fixedStepMs = (stepMs > 0.0f) ? stepMs : 1.0f;
        }
        void SetTimeSyncIntervalMs(f32 ms) noexcept { m_syncIntervalMs = ms; }
        // The network clock: the server's authoritative time, in ms. On the server it IS the local time;
        // on a client it's the local time plus the RTT-adjusted offset estimated from the server's syncs.
        [[nodiscard]] f64 NetworkTimeMs() const noexcept
        {
            return IsServer() ? m_nowMs : (m_nowMs + m_timeOffsetMs);
        }
        // The synced network tick number (NetworkTimeMs / fixed step). Aligns clients to the server's lane.
        [[nodiscard]] u64 NetworkTick() const noexcept
        {
            const f64 t = NetworkTimeMs();
            return (t > 0.0) ? static_cast<u64>(t / static_cast<f64>(m_fixedStepMs)) : 0;
        }
        // Client: has at least one server sync been applied? (NetworkTimeMs is local-only until then.)
        [[nodiscard]] bool HasClockSync() const noexcept { return m_hasSync; }
        // Drain one session event (Connected / Disconnected / Received). False when none remain.
        [[nodiscard]] bool PollEvent(NetEvent& out)
        {
            if (m_eventHead >= m_events.Size())
            {
                return false;
            }
            out = static_cast<NetEvent&&>(m_events[m_eventHead++]);
            if (m_eventHead >= m_events.Size())
            {
                m_events.Clear();
                m_eventHead = 0;
            }
            return true;
        }

        // --- queries ---
        [[nodiscard]] NetRole Role() const noexcept { return m_role; }
        [[nodiscard]] bool IsServer() const noexcept
        {
            return m_role == NetRole::ListenServer || m_role == NetRole::DedicatedServer;
        }
        [[nodiscard]] bool IsClient() const noexcept { return m_role == NetRole::Client; }
        [[nodiscard]] Span<const NetPeer> Peers() const noexcept { return m_peers.AsSpan(); }
        [[nodiscard]] usize PeerCount() const noexcept { return m_peers.Size(); }
        [[nodiscard]] PeerId ServerPeer() const noexcept { return m_serverPeer; } // client-side
        [[nodiscard]] TransportStats Stats(PeerId peer) const { return m_transport.Stats(peer); }
        [[nodiscard]] ReliableTransport& Transport() noexcept { return m_transport; }

    private:
        enum class ControlType : u8
        {
            TimeSync = 0
        };

        void AddPeer(PeerId id)
        {
            for (const NetPeer& p : m_peers)
            {
                if (p.id == id)
                {
                    return;
                }
            }
            m_peers.PushBack(NetPeer{id, m_nowMs});
        }
        void RemovePeer(PeerId id)
        {
            for (usize i = 0; i < m_peers.Size(); ++i)
            {
                if (m_peers[i].id == id)
                {
                    m_peers.RemoveAt(i);
                    return;
                }
            }
        }

        // A session-internal control message (channel 255). Client-side: apply clock sync.
        void HandleControl(const NetEvent& ev)
        {
            BitReader r(ev.payload.AsSpan());
            const u8 type = r.ReadU8();
            if (!r.Ok())
            {
                return;
            }
            if (static_cast<ControlType>(type) == ControlType::TimeSync && !IsServer())
            {
                const u64 bits = r.ReadU64();
                if (!r.Ok())
                {
                    return;
                }
                f64 serverTime = 0.0;
                MemCopy(&serverTime, &bits, sizeof(serverTime));
                // The sync took ~RTT/2 to arrive, so the server's clock has advanced ~RTT/2 since.
                const f64 rtt = static_cast<f64>(m_transport.Stats(ev.peer).rttMs);
                const f64 estimatedServerNow = serverTime + rtt * 0.5;
                const f64 sample =
                    estimatedServerNow - m_nowMs; // offset: server clock vs our local clock
                m_timeOffsetMs = m_hasSync ? (m_timeOffsetMs * 0.9 + sample * 0.1) : sample;
                m_hasSync = true;
            }
        }

        ReliableTransport m_transport;
        NetRole m_role = NetRole::None;
        PeerId m_serverPeer = kInvalidPeer;
        f64 m_nowMs = 0.0;
        Array<NetPeer> m_peers;
        Array<NetEvent> m_events;
        usize m_eventHead = 0;
        // clock sync
        f32 m_fixedStepMs = 50.0f;     // 20 Hz network tick (turn-based-appropriate)
        f32 m_syncIntervalMs = 250.0f; // server broadcasts its clock this often
        f64 m_lastSyncSendMs = -1.0e9; // server
        f64 m_timeOffsetMs = 0.0;      // client: NetworkTime = local + offset
        bool m_hasSync = false;        // client
    };

}
