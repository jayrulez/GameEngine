// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net.Manager - the `foundation.net.manager` module.
///
/// A NETWORKED ENDPOINT, owned per running game (a GameInstance): NetworkManager owns a live
/// NetSession + RpcTable over an IDatagramSocket (real UDP or the sim), is driven each fixed step,
/// and installs a script service so the `Net` facade can query the session and fire RPCs from
/// AngelScript / Luau. NOT a subsystem - there is no once-per-context networking state, so an
/// endpoint lives with the instance that runs it (N instances = N independent endpoints). The Net
/// facade resolves the CURRENT script context's endpoint, so each instance's script sees its own.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module foundation.net.manager;

import foundation.core;
import foundation.profiler;
import foundation.net;
import foundation.net.replication; // StateReplication / InterpolationBuffer (same foundation::net namespace)
import foundation.net.websocket;   // WebSocketHybridSocket (UDP natives + WS browser clients)
import foundation.net.tls;         // the backend and trust a wss join checks the host with
import foundation.scene;           // Scene (the replicated world)
import foundation.script;          // Object / IScriptContext / CurrentScriptContext / SetService
import foundation.script.facades;  // the Scene and Entity handles Net.spawn spawns through and returns

using namespace foundation::core;
namespace core = foundation::core;
using namespace foundation::script; // Object, IScriptContext, CurrentScriptContext (the facade base)
namespace scene = foundation::scene;

export namespace foundation::net
{

    // The service key the Net facade resolves per script context (distinct from script.runtime).
    inline constexpr StringView kNetScriptService = u8"net.runtime";

    // Reserved channel for StateReplication deltas (alongside kControlChannel=255, kRpcChannel=254).
    inline constexpr u8 kReplicationChannel = 253;

    class NetworkManager; // defined below; the facade + binding reference it

    // The owner of a running game's networking (a GameInstance): the Net facade drives roles (start
    // server / connect / disconnect) and reaches the live endpoint THROUGH it. Behind an interface so
    // foundation.net.manager never depends on the runtime layer - the dependency points DOWN (runtime
    // implements this). The controller outlives every endpoint it creates, so the binding never dangles.
    class INetworkController
    {
    public:
        virtual ~INetworkController() = default;
        virtual bool StartServer(u16 port, bool dedicated) = 0;
        virtual bool Connect(StringView host, u16 port) = 0;
        virtual void StopNetworking() = 0;
        // The live endpoint, or null when offline (before startServer/connect, after disconnect).
        [[nodiscard]] virtual NetworkManager* NetEndpoint() const = 0;
    };

    // Installed as a per-context script service; the Net facade resolves it. Holds the CONTROLLER (the
    // GameInstance) - stable for the instance's life. The endpoint it points at may come and go, but
    // the binding itself never dangles (approach A - the dangling-service bug made structurally
    // impossible rather than merely avoided). Install/clear with the free helpers below.
    struct NetScriptBinding
    {
        INetworkController* controller = nullptr;

        /// Net.on's handlers by RPC name. A script registers them whenever it likes, usually before
        /// it hosts or joins; they live here, beside the controller, so every endpoint the controller
        /// opens gets them (InstallScriptRpcHandlers) and a handler survives a disconnect.
        struct ScriptRpcHandler
        {
            String name;
            RefPtr<IScriptDelegate> handler;
        };
        Array<ScriptRpcHandler> rpcHandlers;
        /// Net.onJoin / Net.onLeave: called with the player id (SetPlayerEvents says who that is).
        RefPtr<IScriptDelegate> joinHandler;
        RefPtr<IScriptDelegate> leaveHandler;
    };

    /// What a script RPC carries after its id: the Net facade writes the kind first, so Net.on's
    /// handler knows how to read the rest (nothing, a number, or a text).
    enum class ScriptRpcKind : u8
    {
        None = 0,
        Number = 1,
        Text = 2,
    };

    // A networked endpoint: owns the session + RPC table, driven each fixed step. Owned per running
    // game (a GameInstance), NOT app-wide - N instances hold N independent endpoints (module header).
    class NetworkManager
    {
    public:
        // Closing an endpoint (a game leaving, a host stopping) tells every peer, so a host frees a
        // leaver's seat at once and a client sees its host go, instead of each waiting out the
        // transport's timeout. The socket is still open here: an owned one goes after the session,
        // and a borrowed one outlives the manager.
        ~NetworkManager() { m_session.Transport().DisconnectAll(); }

        // Sim/test: BORROW an external socket (SimDatagramNetwork or a shared UDP socket). The session
        // is live immediately; the manager does not own the socket, so it must outlive the manager.
        explicit NetworkManager(IDatagramSocket& socket, const ReliableConfig& config = {})
            : m_session(socket, config)
        {
        }

        // Runtime: OWN an already-opened UDP socket (m_ownedSocket destructs after m_session, which
        // borrows it - declaration order below guarantees it). Used by the HostServer/JoinServer
        // factories; a bare game never constructs this directly.
        explicit NetworkManager(core::UniquePtr<UdpSocket> ownedSocket,
                                const ReliableConfig& config = {})
            : m_ownedSocket(static_cast<core::UniquePtr<UdpSocket>&&>(ownedSocket)),
              m_session(*m_ownedSocket, config)
        {
            m_boundPort = static_cast<UdpSocket*>(m_ownedSocket.Get())->BoundPort();
        }

        // Runtime host with a WEB GATEWAY: owns the hybrid UDP+WS socket, so browser clients
        // join the same session native peers do (HostServer with webSocketPort).
        explicit NetworkManager(core::UniquePtr<WebSocketHybridSocket> hybrid,
                                const ReliableConfig& config = {})
            : m_boundPort(hybrid->BoundPort()), m_webSocketPort(hybrid->WebSocketBoundPort()),
              m_ownedSocket(static_cast<core::UniquePtr<WebSocketHybridSocket>&&>(hybrid)),
              m_session(*m_ownedSocket, config)
        {
        }

        // WebSocket client: owns the connection to a host's gateway (JoinServer on web, or with a
        // ws:// or wss:// address anywhere).
        explicit NetworkManager(core::UniquePtr<WebSocketClientSocket> socket,
                                const ReliableConfig& config = {})
            : m_ownedSocket(static_cast<core::UniquePtr<WebSocketClientSocket>&&>(socket)),
              m_session(*m_ownedSocket, config)
        {
        }

        // Runtime endpoint construction: open a real UDP socket and enter a role in one step. Returns
        // null if the socket fails to open (the caller logs + runs offline). HostServer binds `port`
        // (0 = OS-assigned; read BoundPort() after); JoinServer binds ephemeral and connects to
        // host:port. These are how a running game goes online (the Net facade calls them).
        [[nodiscard]] static core::UniquePtr<NetworkManager>
        HostServer(core::IAllocator& allocator, u16 port, bool dedicated = false,
                   const ReliableConfig& config = {},
                   u16 webSocketPort = 0, // != 0: ALSO accept browser clients over ws://
                   const WebSocketTls* webSocketTls = nullptr); // and with it over wss:// instead
        // JoinServer's `host` may be an address with a scheme: "ws://host[:port][/path]" or
        // "wss://..." joins the host's WebSocket gateway (`port` when the address names none),
        // natively too, wss through `tls` checked against `trust`. A bare host joins over UDP
        // natively; in a browser over a WebSocket to `port`, wss when the page is https (a
        // browser refuses ws:// from an https page).
        [[nodiscard]] static core::UniquePtr<NetworkManager>
        JoinServer(core::IAllocator& allocator, core::StringView host, u16 port,
                   const ReliableConfig& config = {}, ITlsBackend* tls = nullptr,
                   TlsTrust trust = TlsTrust::System());

        /// Whether THIS build can host a session at all. Browsers cannot listen on any
        /// socket - the web build's Host paths reject cleanly instead of hanging.
        [[nodiscard]] static constexpr bool CanHost() noexcept
        {
#if PLATFORM_WEB
            return false;
#else
            return true;
#endif
        }

        // The port this endpoint's owned UDP socket is bound to (0 when borrowing a sim/shared socket).
        // A HostServer opened with port 0 reports its OS-assigned port here so a client can reach it.
        [[nodiscard]] u16 BoundPort() const noexcept { return m_boundPort; }
        // The WS gateway's bound port (0 = this endpoint accepts no browser clients).
        [[nodiscard]] u16 WebSocketBoundPort() const noexcept { return m_webSocketPort; }

        // Roles (see NetSession).
        void StartServer(bool dedicated = false) { m_session.StartServer(dedicated); }
        PeerId ConnectTo(const DatagramEndpoint& server) { return m_session.Connect(server); }

        // Transport HALF (per-frame): drive the session + route received
        // messages by reserved channel: RPCs (254) to the table, replication deltas (253) buffered into
        // the scene's interpolation state, everything else to `onEvent` (e.g. Connected). No state
        // capture/send here - that is the replication half, on the scene fixed lane. Pumps regardless of
        // scene time (connections stay alive while a scene is paused).
        void UpdateTransport(f32 deltaMs, const Function<void(const NetEvent&)>& onEvent = {})
        {
            PROFILE_SCOPE("Net.Transport");
            m_session.Update(deltaMs);

            NetEvent ev;
            while (m_session.PollEvent(ev))
            {
                if (ev.kind == NetEventKind::Received && ev.channel == kRpcChannel)
                {
                    m_rpc.Dispatch(ev.peer, ev.payload.AsSpan());
                }
                else if (ev.kind == NetEventKind::Received && ev.channel == kReplicationChannel && m_session.IsServer())
                {
                    // The one message a client sends on this channel: it is ready (its scene is the
                    // host's, loaded). Its state starts now, from a full snapshot.
                    ReleasePeer(ev.peer);
                }
                else if (ev.kind == NetEventKind::Received && ev.channel == kReplicationChannel)
                {
                    if (m_scene != nullptr)
                    {
                        // Packet = server capture time (for interpolation) + the delta.
                        BitReader reader(ev.payload.AsSpan());
                        const u64 bits = reader.ReadU64();
                        f64 serverTimeMs = 0.0;
                        MemCopy(&serverTimeMs, &bits, sizeof(serverTimeMs));
                        m_replication.ApplyDelta(*m_scene, reader, m_interp, serverTimeMs);
                    }
                }
                else
                {
                    if (ev.kind == NetEventKind::Disconnected)
                    {
                        m_replication.ForgetPeer(ev.peer);
                        (void)m_held.Remove(ev.peer);
                    }
                    else if (ev.kind == NetEventKind::Connected && m_session.IsServer() && m_waitForReady)
                    {
                        m_held.Insert(ev.peer); // no state until it says it is ready
                    }
                    ReportPlayer(ev);
                    if (onEvent)
                    {
                        onEvent(ev);
                    }
                }
            }
            // A client has joined once the server has said who it is (the session's welcome).
            if (m_session.IsClient() && !m_welcomed && m_session.LocalPeer() != kInvalidPeer)
            {
                m_welcomed = true;
                if (m_playerEvents)
                {
                    m_playerEvents(true, m_session.LocalPeer());
                }
            }
        }

        /// Host: whether a client that joins waits for its ready (SendReady) before it hears any state:
        /// a game whose joiner loads the host's map after connecting holds the state off until the
        /// map's networked entities are there to take it. Off by default (state flows at once). RPCs
        /// are never held: the host's welcome (naming its map, say) reaches a held joiner.
        void SetWaitForReady(bool wait) noexcept { m_waitForReady = wait; }
        [[nodiscard]] bool WaitsForReady() const noexcept { return m_waitForReady; }
        /// Host: whether a client is held (joined, not yet ready).
        [[nodiscard]] bool IsHeld(PeerId peer) const { return m_held.Contains(peer); }
        /// Client: tell the host this end is ready for state (its scene loaded). The host forgets
        /// what it last sent it, so the state starts from a full snapshot. Harmless to a host that
        /// does not wait (its baseline restarts: one full snapshot more).
        void SendReady()
        {
            if (!m_session.IsClient())
                return;
            const byte ready[1] = {byte{1}};
            for (const NetPeer& peer : m_session.Peers())
                m_session.Send(peer.id, kReplicationChannel, Span<const byte>(ready, 1), Reliability::ReliableOrdered);
        }

        /// Players joining and leaving, by player id (0 = the host, else a client's PeerId on the
        /// server). On the host: a client joined or left. On a client: it joined itself (its own id,
        /// once the server welcomed it), or the host left (0).
        void SetPlayerEvents(Function<void(bool joined, u32 player)> handler) { m_playerEvents = Move(handler); }

        // Replication HALF (per-scene FIXED lane): server captures the
        // scene's networked state + pushes a per-peer delta; client samples buffered interpolation into
        // the scene. Gated on the scene's fixed lane, so a paused/slow-mo scene replicates at its scaled
        // rate (a paused sim produces no deltas). Does not touch the socket - transport keeps that.
        void UpdateReplication()
        {
            PROFILE_SCOPE("Net.Replication");
            if (m_session.IsServer() && m_scene != nullptr)
            {
                // Pull each networked entity's authoritative LOCAL transform into its NetworkedTransform
                // component, so the capture below sends the current pose (the reflected-component pipe).
                CaptureEntityTransforms(*m_scene);
                const f64 now = m_session.NetworkTimeMs();
                u64 bits = 0;
                MemCopy(&bits, &now, sizeof(bits));
                for (const NetPeer& peer : m_session.Peers())
                {
                    if (m_held.Contains(peer.id))
                        continue; // joined, not ready: its scene is not the host's yet
                    BitWriter writer;
                    writer.WriteU64(bits); // server capture time, ahead of the delta
                    if (m_replication.CaptureDelta(*m_scene, peer.id, writer) > 0)
                    {
                        m_session.Send(peer.id, kReplicationChannel, writer.Data(),
                                       Reliability::ReliableOrdered);
                    }
                }
            }

            // Client: render each interpolatable networked component at (synced network time - delay),
            // playing the buffered states back smoothly between the low-rate updates, then push the
            // (interpolated) NetworkedTransform components back onto their entities' local transforms.
            if (m_session.IsClient() && m_scene != nullptr)
            {
                m_replication.SampleInterpolation(*m_scene, m_interp,
                                                  m_session.NetworkTimeMs() - m_interpDelayMs);
                ApplyEntityTransforms(*m_scene);
            }
        }

        // Convenience for callers that drive both halves together (tests + any single-lane consumer):
        // transport then replication, the same order the pre-split Update ran. Production drives the two
        // halves on their own lanes (transport per-frame, replication on the scene fixed lane).
        void Update(f32 deltaMs, const Function<void(const NetEvent&)>& onEvent = {})
        {
            UpdateTransport(deltaMs, onEvent);
            UpdateReplication();
        }

        // The scene this manager replicates (server captures from it, client applies into it). Null =
        // no replication (session + RPC still run). The host sets the gameplay scene here. On a SERVER,
        // authored-networked entities in the scene are assigned NetworkIds now (so a designer marks an
        // entity networked and it "just replicates" on host); the client receives ids over the wire.
        void SetReplicatedScene(scene::Scene* scene)
        {
            m_scene = scene;
            if (scene == nullptr)
            {
                return;
            }
            if (m_session.IsServer())
            {
                m_replication.AssignSceneNetworkIds(*scene); // mint + register authored entities
            }
            else
            {
                m_replication.RegisterAuthoredEntities(
                    *scene); // match by stable authored id (no duplicate)
            }
        }
        [[nodiscard]] StateReplication& Replication() noexcept { return m_replication; }
        /// The replicated world (null = no replication).
        [[nodiscard]] scene::Scene* ReplicatedScene() const noexcept { return m_scene; }
        // How far behind synced network time the client renders (interpolation delay). ~2x the server
        // send interval hides one lost/late update. Default 100 ms.
        void SetInterpolationDelayMs(f64 ms) noexcept { m_interpDelayMs = ms; }

        [[nodiscard]] NetSession& Session() noexcept { return m_session; }
        [[nodiscard]] RpcTable& Rpc() noexcept { return m_rpc; }

    private:
        u16 m_boundPort = 0;
        u16 m_webSocketPort = 0;
        // Declared FIRST (before m_session) so it constructs before (and destructs after)
        // m_session, which borrows it. Null when the manager borrows an external socket (the
        // sim/test ctor); a UdpSocket or WebSocketHybridSocket from the factories otherwise.
        core::UniquePtr<IDatagramSocket> m_ownedSocket;
        NetSession m_session;
        // The player a session event is about, to SetPlayerEvents' handler (see there).
        // A held client said it is ready: its baseline goes, so its first delta is everything.
        void ReleasePeer(PeerId peer)
        {
            (void)m_held.Remove(peer);
            m_replication.ForgetPeer(peer);
        }

        void ReportPlayer(const NetEvent& ev)
        {
            if (!m_playerEvents)
            {
                return;
            }
            if (m_session.IsServer() && ev.kind == NetEventKind::Connected)
            {
                m_playerEvents(true, ev.peer);
            }
            else if (m_session.IsServer() && ev.kind == NetEventKind::Disconnected)
            {
                m_playerEvents(false, ev.peer);
            }
            else if (m_session.IsClient() && ev.kind == NetEventKind::Disconnected)
            {
                m_playerEvents(false, 0u); // the host went
            }
        }

        RpcTable m_rpc;
        Function<void(bool joined, u32 player)> m_playerEvents;
        bool m_welcomed = false; // client: the welcome has been reported
        bool m_waitForReady = false; // host: joiners wait for their ready before any state
        HashSet<PeerId> m_held;      // host: joined, not yet ready
        StateReplication m_replication;
        InterpolationBuffer m_interp;    // client-side smoothing of received states
        scene::Scene* m_scene = nullptr; // the replicated world (null = no replication)
        f64 m_interpDelayMs = 100.0;
    };

    // Point / clear a script context's Net facade at a binding. The binding (a GameInstance member)
    // must outlive the context; the owner clears it before teardown. The facade reads binding.controller.
    inline void InstallNetScriptService(IScriptContext& context, NetScriptBinding& binding)
    {
        context.SetService(kNetScriptService, &binding);
    }
    inline void ClearNetScriptService(IScriptContext& context)
    {
        context.SetService(kNetScriptService, nullptr);
    }

    // Script facade: read the live session, drive roles (server/connect/disconnect), and fire RPCs.
    // Static methods resolve the per-context NetScriptBinding -> its controller -> the live endpoint
    // (null-safe when offline). Ports are natural i32 (the corrected numerics basis). Both backends
    // bind it via reflection. Defined after NetworkManager so the read paths see the full type.
    class Net final : public Object
    {
        RTTI_OBJECT(Net, Object)
    public:
        [[nodiscard]] static NetScriptBinding* Resolve()
        {
            IScriptContext* context = CurrentScriptContext();
            return context != nullptr
                       ? static_cast<NetScriptBinding*>(context->GetService(kNetScriptService))
                       : nullptr;
        }
        // The live endpoint for the current script context, or null when offline.
        [[nodiscard]] static NetworkManager* Endpoint()
        {
            NetScriptBinding* b = Resolve();
            return (b != nullptr && b->controller != nullptr) ? b->controller->NetEndpoint()
                                                              : nullptr;
        }

        [[nodiscard]] static bool isServer()
        {
            NetworkManager* m = Endpoint();
            return m != nullptr && m->Session().IsServer();
        }
        [[nodiscard]] static bool isClient()
        {
            NetworkManager* m = Endpoint();
            return m != nullptr && m->Session().IsClient();
        }
        [[nodiscard]] static i32 peerCount()
        {
            NetworkManager* m = Endpoint();
            return m != nullptr ? static_cast<i32>(m->Session().PeerCount()) : 0;
        }
        [[nodiscard]] static f64 networkTick()
        {
            NetworkManager* m = Endpoint();
            return m != nullptr ? static_cast<f64>(m->Session().NetworkTick()) : 0.0;
        }
        [[nodiscard]] static f64 networkTimeMs()
        {
            NetworkManager* m = Endpoint();
            return m != nullptr ? m->Session().NetworkTimeMs() : 0.0;
        }

        /// Whether this build can host at all (a browser cannot listen): a title offers Host only
        /// when it can, rather than failing once it is chosen.
        [[nodiscard]] static bool canHost() { return NetworkManager::CanHost(); }

        // Runtime role control - a game's menu calls these. Port is a natural i32 (0..65535). Return
        // false when offline-support is absent or the socket fails to open (the game shows an error).
        static bool startServer(i32 port)
        {
            INetworkController* c = Controller();
            return c != nullptr && c->StartServer(static_cast<u16>(port), /*dedicated=*/false);
        }
        static bool startDedicatedServer(i32 port)
        {
            INetworkController* c = Controller();
            return c != nullptr && c->StartServer(static_cast<u16>(port), /*dedicated=*/true);
        }
        static bool connect(String host, i32 port)
        {
            INetworkController* c = Controller();
            return c != nullptr && c->Connect(host.AsView(), static_cast<u16>(port));
        }
        /// Host: joiners wait for `ready` before they hear any state (see SetWaitForReady).
        static void waitForReady(bool wait)
        {
            if (NetworkManager* m = Endpoint())
                m->SetWaitForReady(wait);
        }
        /// Client: its scene is the host's, loaded; the host's state starts, from a full snapshot.
        static void ready()
        {
            if (NetworkManager* m = Endpoint())
                m->SendReady();
        }
        static void disconnect()
        {
            INetworkController* c = Controller();
            if (c != nullptr)
            {
                c->StopNetworking();
            }
        }

        // Fire an RPC (no args). Client -> the server; server -> broadcast to all clients.
        static void rpc(String name) { CallRpc(name.AsView(), WriteNothing()); }
        // Fire an RPC with one number / one text argument (the common turn-based-order shapes).
        static void rpcNumber(String name, f64 value) { CallRpc(name.AsView(), WriteNumber(value)); }
        static void rpcText(String name, String text) { CallRpc(name.AsView(), WriteText(Move(text))); }

        /// Host only: send to one player (a client's player id), as rpc / rpcNumber / rpcText send
        /// to all. Nothing for the host itself (0), an unknown player, or on a client.
        static void rpcTo(i32 player, String name) { CallRpcTo(player, name.AsView(), WriteNothing()); }
        static void rpcNumberTo(i32 player, String name, f64 value)
        {
            CallRpcTo(player, name.AsView(), WriteNumber(value));
        }
        static void rpcTextTo(i32 player, String name, String text)
        {
            CallRpcTo(player, name.AsView(), WriteText(Move(text)));
        }

        /// This game's own player id: 0 on the host, the id the host gave it on a client (from the
        /// host's welcome), -1 offline or before the welcome.
        [[nodiscard]] static i32 localPlayer()
        {
            NetworkManager* m = Endpoint();
            if (m == nullptr)
            {
                return -1;
            }
            if (m->Session().IsServer())
            {
                return 0;
            }
            const PeerId self = m->Session().LocalPeer();
            return self != kInvalidPeer ? static_cast<i32>(self) : -1;
        }

        /// Whether `entity` is this player's: its owner is localPlayer(). False for an entity that
        /// is not networked, or offline.
        [[nodiscard]] static bool isMine(foundation::script::Entity entity)
        {
            if (!entity.Live())
            {
                return false;
            }
            auto* networked = entity.scene->GetSystem<NetworkComponentManager>();
            const NetworkComponent* nc = networked != nullptr ? networked->Get(entity.Handle()) : nullptr;
            const i32 self = localPlayer();
            return nc != nullptr && self >= 0 && nc->owner == static_cast<u32>(self);
        }

        /// Called with the player id when a player joins, and when one leaves: on the host, a
        /// client; on a client, itself once the host has welcomed it, and the host (0) if it goes.
        static void onJoin(RefPtr<IScriptDelegate> handler) { SetPlayerHandler(handler, /*join=*/true); }
        static void onLeave(RefPtr<IScriptDelegate> handler) { SetPlayerHandler(handler, /*join=*/false); }

        /// Host only: spawn `prefab` at (x, y, z) in the replicated scene, as `scene.spawn` does,
        /// and give it a network id with the prefab, so every client spawns its own copy (and a
        /// player who joins later gets it in the snapshot). The prefab's root needs a Networked
        /// Transform for its movement to follow. A null entity on a client, offline, or for a
        /// prefab the scene cannot spawn.
        [[nodiscard]] static foundation::script::Entity spawn(Guid prefab, f32 x, f32 y, f32 z)
        {
            return spawnFor(0, prefab, x, y, z);
        }

        /// Host only: spawn as `spawn` does, owned by `player` (0 = the host), so that player's
        /// isMine says yes for it and its owner shows on every peer.
        [[nodiscard]] static foundation::script::Entity spawnFor(i32 player, Guid prefab, f32 x, f32 y, f32 z)
        {
            NetworkManager* m = Endpoint();
            if (m == nullptr || !m->Session().IsServer() || m->ReplicatedScene() == nullptr)
            {
                return foundation::script::Entity{};
            }
            foundation::script::Scene world;
            world.scene = m->ReplicatedScene();
            foundation::script::Entity entity = world.spawn(prefab, x, y, z);
            if (entity.Live())
            {
                (void)m->Replication().AssignNetworkId(*world.scene, entity.Handle(), prefab,
                                                       player > 0 ? static_cast<u32>(player) : 0u);
            }
            return entity;
        }

        /// Receive the RPC `name`: `handler` is called with the sender's peer id, then the number or
        /// the text it carries, if any (`void f(int sender)`, `void f(int sender, double value)` or
        /// `void f(int sender, const string &in text)`; a handler may take fewer arguments than
        /// arrive). On the host the sender is the client that called; on a client, the host. Register
        /// any time, before hosting or joining too; a later registration of the name replaces it.
        static void on(String name, RefPtr<IScriptDelegate> handler)
        {
            NetScriptBinding* b = Resolve();
            if (b == nullptr || !handler)
            {
                return;
            }
            bool replaced = false;
            for (NetScriptBinding::ScriptRpcHandler& entry : b->rpcHandlers)
            {
                if (entry.name == name)
                {
                    entry.handler = handler;
                    replaced = true;
                }
            }
            if (!replaced)
            {
                b->rpcHandlers.PushBack(NetScriptBinding::ScriptRpcHandler{String(name), handler});
            }
            if (NetworkManager* m = Endpoint())
            {
                InstallScriptRpcHandler(*m, name.AsView(), handler);
            }
        }

        /// Point the RPC `name` on `endpoint` at a script handler (Net.on, and every endpoint a
        /// controller opens: InstallScriptRpcHandlers).
        static void InstallScriptRpcHandler(NetworkManager& endpoint, StringView name, RefPtr<IScriptDelegate> handler)
        {
            NetworkManager* at = &endpoint; // the table belongs to it, so it outlives the handler
            endpoint.Rpc().On(name,
                              [handler, at](PeerId sender, BitReader& r)
                              {
                                  // A client hears only from the host: player 0 (its own id for
                                  // the host connection means nothing to a script).
                                  if (at->Session().IsClient())
                                  {
                                      sender = 0;
                                  }
                                  const u8 kind = r.ReadU8();
                                  if (!r.Ok())
                                  {
                                      return;
                                  }
                                  Variant args[2];
                                  args[0] = Variant::From(static_cast<i32>(sender));
                                  usize count = 1;
                                  if (kind == static_cast<u8>(ScriptRpcKind::Number))
                                  {
                                      const u64 bits = r.ReadU64();
                                      f64 value = 0.0;
                                      MemCopy(&value, &bits, sizeof(value));
                                      args[count++] = Variant::From(value);
                                  }
                                  else if (kind == static_cast<u8>(ScriptRpcKind::Text))
                                  {
                                      const u32 size = r.ReadVarU32();
                                      if (!r.Ok() || size > (1u << 20))
                                      {
                                          return;
                                      }
                                      Array<byte> bytes;
                                      bytes.Resize(size);
                                      r.ReadBytes(bytes.AsSpan());
                                      args[count++] = Variant::From(
                                          String(StringView(reinterpret_cast<const utf8char*>(bytes.Data()), size)));
                                  }
                                  if (!r.Ok())
                                  {
                                      return; // truncated: nothing half-read reaches the script
                                  }
                                  (void)handler->Invoke(Span<Variant>(args, count));
                              });
        }

        /// Point `endpoint`'s join and leave reporting at the binding's script handlers (Net.onJoin,
        /// Net.onLeave, and every endpoint a controller opens).
        static void InstallPlayerHandlers(NetworkManager& endpoint, const NetScriptBinding& binding)
        {
            RefPtr<IScriptDelegate> join = binding.joinHandler;
            RefPtr<IScriptDelegate> leave = binding.leaveHandler;
            endpoint.SetPlayerEvents(
                [join, leave](bool joined, u32 player)
                {
                    const RefPtr<IScriptDelegate>& handler = joined ? join : leave;
                    if (handler)
                    {
                        Variant args[1] = {Variant::From(static_cast<i32>(player))};
                        (void)handler->Invoke(Span<Variant>(args, 1));
                    }
                });
        }

    private:
        static Function<void(BitWriter&)> WriteNothing()
        {
            return [](BitWriter& w) { w.WriteU8(static_cast<u8>(ScriptRpcKind::None)); };
        }
        static Function<void(BitWriter&)> WriteNumber(f64 value)
        {
            return [value](BitWriter& w)
            {
                w.WriteU8(static_cast<u8>(ScriptRpcKind::Number));
                u64 bits = 0;
                MemCopy(&bits, &value, sizeof(bits));
                w.WriteU64(bits);
            };
        }
        static Function<void(BitWriter&)> WriteText(String text)
        {
            return [owned = Move(text)](BitWriter& w)
            {
                w.WriteU8(static_cast<u8>(ScriptRpcKind::Text));
                w.WriteVarU32(static_cast<u32>(owned.Size()));
                w.WriteBytes(Span<const byte>(reinterpret_cast<const byte*>(owned.Data()), owned.Size()));
            };
        }

        static void CallRpcTo(i32 player, StringView name, const Function<void(BitWriter&)>& writeArgs)
        {
            NetworkManager* m = Endpoint();
            if (m == nullptr || !m->Session().IsServer() || player <= 0)
            {
                return;
            }
            for (const NetPeer& peer : m->Session().Peers())
            {
                if (peer.id == static_cast<PeerId>(player))
                {
                    m->Rpc().Call(m->Session(), peer.id, name, writeArgs);
                    return;
                }
            }
        }

        static void SetPlayerHandler(RefPtr<IScriptDelegate> handler, bool join)
        {
            NetScriptBinding* b = Resolve();
            if (b == nullptr)
            {
                return;
            }
            (join ? b->joinHandler : b->leaveHandler) = handler;
            if (NetworkManager* m = Endpoint())
            {
                InstallPlayerHandlers(*m, *b);
            }
        }

        [[nodiscard]] static INetworkController* Controller()
        {
            NetScriptBinding* b = Resolve();
            return (b != nullptr) ? b->controller : nullptr;
        }
        static void CallRpc(StringView name, const Function<void(BitWriter&)>& writeArgs)
        {
            NetworkManager* m = Endpoint();
            if (m == nullptr)
            {
                return;
            }
            NetSession& session = m->Session();
            if (session.IsClient())
            {
                m->Rpc().Call(session, session.ServerPeer(), name, writeArgs);
            }
            else if (session.IsServer())
            {
                m->Rpc().CallAll(session, name, writeArgs);
            }
        }
    };

    // ---- runtime startup: how a host (DefaultApplication) enters a networked role from config ----

    // The role a networked run starts in. None = single-player (no socket opened, no facade service).
    enum class NetworkRole
    {
        None,
        Server,
        Client
    };

    // Declarative startup config an app presets before it configures its subsystems (mirrors the
    // audio-engine-settings preset). IPv4 for v1: serverHost is a dotted-quad or a DNS name.
    struct NetworkStartup
    {
        NetworkRole role = NetworkRole::None;
        u16 listenPort = 0; // server: bind port; client: 0 = OS-assigned
        core::String serverHost =
            core::String(u8"127.0.0.1"); // client: server address to connect to
        u16 serverPort = 0;              // client: the server's port
        bool dedicated = false;          // server: dedicated (no local player) vs listen-server
        ReliableConfig reliable = {};    // protocol tuning (keepalive/timeout/resend)
    };

    // A started net home: the socket the manager borrows + the manager itself. Both must outlive the
    // run; the manager holds a reference to the socket, so keep/destroy the manager FIRST. socket/
    // manager are null when role==None; a non-null socket that failed to open reports !IsOpen() (the
    // caller logs). Bundled so the socket-open + role-entry logic stays in this lib.
    struct NetworkRuntime
    {
        core::UniquePtr<UdpSocket> socket;
        core::UniquePtr<NetworkManager> manager;

        [[nodiscard]] bool IsActive() const noexcept { return manager.Get() != nullptr; }
    };

    // Open the socket, build the manager, and enter the role (StartServer / ConnectTo). Returns an
    // empty NetworkRuntime for role==None. Registers the Net script facade as a side effect when a
    // role is entered (idempotent), so the facade is bound wherever networking is actually used.
    [[nodiscard]] NetworkRuntime StartNetworking(core::IAllocator& allocator,
                                                 const NetworkStartup& config);

    /// Every Net.on, Net.onJoin and Net.onLeave handler a binding holds, on a new endpoint (the
    /// controller calls it when it hosts or joins).
    inline void InstallScriptRpcHandlers(const NetScriptBinding& binding, NetworkManager& endpoint)
    {
        for (const NetScriptBinding::ScriptRpcHandler& entry : binding.rpcHandlers)
        {
            Net::InstallScriptRpcHandler(endpoint, entry.name.AsView(), entry.handler);
        }
        Net::InstallPlayerHandlers(endpoint, binding);
    }

    /// Register the Net facade type (call before a script manager is created, like
    /// RegisterScriptFacadeReflection), and the shapes of Net.on's handlers. Idempotent.
    void RegisterNetScriptFacade();

}
