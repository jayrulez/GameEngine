/// Draconic::NetworkManager - the `draconic.net.manager` module (docs/design/networking.md §6).
///
/// A NETWORKED ENDPOINT, owned per running game (a GameInstance): NetworkManager owns a live
/// NetSession + RpcTable over an IDatagramSocket (real UDP or the sim), is driven each fixed step,
/// and installs a script service so the `Net` facade can query the session and fire RPCs from
/// Wren / AngelScript. NOT a subsystem - there is no once-per-context networking state, so an
/// endpoint lives with the instance that runs it (N instances = N independent endpoints). The Net
/// facade resolves the CURRENT script context's endpoint, so each instance's script sees its own.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.net.manager;

import draconic.foundation;
import draconic.net;
import draconic.net.replication; // StateReplication / InterpolationBuffer (same draconic::net namespace)
import draconic.scene;           // Scene (the replicated world)
import draconic.script;          // Object / IScriptContext / CurrentScriptContext / SetService

using namespace draconic::foundation;
using namespace draconic::script; // Object, IScriptContext, CurrentScriptContext (the facade base)
namespace scene = draconic::scene;

export namespace draconic::net
{

    // The service key the Net facade resolves per script context (distinct from script.runtime).
    inline constexpr StringView kNetScriptService = u8"net.runtime";

    // Reserved channel for StateReplication deltas (alongside kControlChannel=255, kRpcChannel=254).
    inline constexpr u8 kReplicationChannel = 253;

    class NetworkManager; // defined below; the facade + binding reference it

    // The owner of a running game's networking (a GameInstance): the Net facade drives roles (start
    // server / connect / disconnect) and reaches the live endpoint THROUGH it. Behind an interface so
    // draconic.net.manager never depends on the runtime layer - the dependency points DOWN (runtime
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
    };

    // A networked endpoint: owns the session + RPC table, driven each fixed step. Owned per running
    // game (a GameInstance), NOT app-wide - N instances hold N independent endpoints (module header).
    class NetworkManager
    {
    public:
        // Sim/test: BORROW an external socket (SimDatagramNetwork or a shared UDP socket). The session
        // is live immediately; the manager does not own the socket, so it must outlive the manager.
        explicit NetworkManager(IDatagramSocket& socket, const ReliableConfig& config = {})
            : m_session(socket, config)
        {
        }

        // Runtime: OWN an already-opened UDP socket (m_ownedSocket destructs after m_session, which
        // borrows it - declaration order below guarantees it). Used by the HostServer/JoinServer
        // factories; a bare game never constructs this directly.
        explicit NetworkManager(foundation::UniquePtr<UdpSocket> ownedSocket,
                                const ReliableConfig& config = {})
            : m_ownedSocket(static_cast<foundation::UniquePtr<UdpSocket>&&>(ownedSocket)),
              m_session(*m_ownedSocket, config)
        {
        }

        // Runtime endpoint construction: open a real UDP socket and enter a role in one step. Returns
        // null if the socket fails to open (the caller logs + runs offline). HostServer binds `port`
        // (0 = OS-assigned; read BoundPort() after); JoinServer binds ephemeral and connects to
        // host:port. These are how a running game goes online (the Net facade calls them - phase 3).
        [[nodiscard]] static foundation::UniquePtr<NetworkManager>
        HostServer(u16 port, bool dedicated = false, const ReliableConfig& config = {});
        [[nodiscard]] static foundation::UniquePtr<NetworkManager>
        JoinServer(foundation::StringView host, u16 port, const ReliableConfig& config = {});

        // The port this endpoint's owned UDP socket is bound to (0 when borrowing a sim/shared socket).
        // A HostServer opened with port 0 reports its OS-assigned port here so a client can reach it.
        [[nodiscard]] u16 BoundPort() const noexcept
        {
            return m_ownedSocket ? m_ownedSocket->BoundPort() : 0;
        }

        // Roles (see NetSession).
        void StartServer(bool dedicated = false) { m_session.StartServer(dedicated); }
        PeerId ConnectTo(const DatagramEndpoint& server) { return m_session.Connect(server); }

        // Drive the session + route received messages by reserved channel: RPCs (254) to the table,
        // replication deltas (253) into the scene, everything else to `onEvent` (e.g. Connected). On the
        // server, push each connected peer its replication delta this tick (reliable-ordered - the delta
        // baseline assumes delivery). Call on the FIXED lane.
        void Update(f32 deltaMs, const Function<void(const NetEvent&)>& onEvent = {})
        {
            m_session.Update(deltaMs);

            NetEvent ev;
            while (m_session.PollEvent(ev))
            {
                if (ev.kind == NetEventKind::Received && ev.channel == kRpcChannel)
                {
                    m_rpc.Dispatch(ev.peer, ev.payload.AsSpan());
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
                    }
                    if (onEvent)
                    {
                        onEvent(ev);
                    }
                }
            }

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
        // How far behind synced network time the client renders (interpolation delay). ~2x the server
        // send interval hides one lost/late update. Default 100 ms.
        void SetInterpolationDelayMs(f64 ms) noexcept { m_interpDelayMs = ms; }

        [[nodiscard]] NetSession& Session() noexcept { return m_session; }
        [[nodiscard]] RpcTable& Rpc() noexcept { return m_rpc; }

    private:
        // Declared FIRST so it constructs before (and destructs after) m_session, which borrows it.
        // Null when the manager borrows an external socket (the sim/test ctor); non-null when it owns
        // a UDP socket (the HostServer/JoinServer factories).
        foundation::UniquePtr<UdpSocket> m_ownedSocket;
        NetSession m_session;
        RpcTable m_rpc;
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
        DRACONIC_OBJECT(Net, Object)
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
        static void disconnect()
        {
            INetworkController* c = Controller();
            if (c != nullptr)
            {
                c->StopNetworking();
            }
        }

        // Fire an RPC (no args). Client -> the server; server -> broadcast to all clients.
        static void rpc(String name) { CallRpc(name.AsView(), Function<void(BitWriter&)>{}); }
        // Fire an RPC with one number / one text argument (the common turn-based-order shapes).
        static void rpcNumber(String name, f64 value)
        {
            CallRpc(name.AsView(),
                    [value](BitWriter& w)
                    {
                        u64 bits = 0;
                        MemCopy(&bits, &value, sizeof(bits));
                        w.WriteU64(bits);
                    });
        }
        static void rpcText(String name, String text)
        {
            String owned(text);
            CallRpc(name.AsView(),
                    [owned](BitWriter& w)
                    {
                        w.WriteVarU32(static_cast<u32>(owned.Size()));
                        w.WriteBytes(Span<const byte>(reinterpret_cast<const byte*>(owned.Data()),
                                                      owned.Size()));
                    });
        }

    private:
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
    // audio-engine-settings preset). IPv4 for v1: serverHost is a dotted-quad (no DNS yet).
    struct NetworkStartup
    {
        NetworkRole role = NetworkRole::None;
        u16 listenPort = 0; // server: bind port; client: 0 = OS-assigned
        foundation::String serverHost =
            foundation::String(u8"127.0.0.1"); // client: server address to connect to
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
        foundation::UniquePtr<UdpSocket> socket;
        foundation::UniquePtr<NetworkManager> manager;

        [[nodiscard]] bool IsActive() const noexcept { return manager.Get() != nullptr; }
    };

    // Open the socket, build the manager, and enter the role (StartServer / ConnectTo). Returns an
    // empty NetworkRuntime for role==None. Registers the Net script facade as a side effect when a
    // role is entered (idempotent), so the facade is bound wherever networking is actually used.
    [[nodiscard]] NetworkRuntime StartNetworking(const NetworkStartup& config);

    /// Register the Net facade type (call before a script manager is created, like
    /// RegisterScriptFacadeReflection). Idempotent.
    void RegisterNetScriptFacade();

}
