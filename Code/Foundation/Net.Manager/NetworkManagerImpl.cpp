// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Net.Manager - implementation unit: the REFLECT_* body for the Net facade +
// the registration (kept out of the interface unit per the GCC gcm-cluster rule).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"
#if PLATFORM_WEB
#include <emscripten/emscripten.h> // EM_ASM_INT: is the page https
#endif

module foundation.net.manager;

import foundation.core;
import foundation.net;
import foundation.script;
import foundation.script.facades; // RegisterExtraFacadeName (the behavior prelude hook)

using namespace foundation::core;
using namespace foundation::script;
namespace core = foundation::core;

namespace foundation::net
{
    REFLECT_MEMBERS(Net, "rtti::net")
    {
        builder.Method<&Net::isServer>("isServer");
        builder.Method<&Net::isClient>("isClient");
        builder.Method<&Net::peerCount>("peerCount");
        builder.Method<&Net::networkTick>("networkTick");
        builder.Method<&Net::networkTimeMs>("networkTimeMs");
        builder.Method<&Net::canHost>("canHost"); // false in a browser: offer no Host there
        builder.Method<&Net::startServer>("startServer");
        builder.Method<&Net::startDedicatedServer>("startDedicatedServer");
        builder.Method<&Net::connect>("connect");
        builder.Method<&Net::disconnect>("disconnect");
        builder.Method<&Net::waitForReady>("waitForReady", {"wait"}); // host: joiners wait for ready()
        builder.Method<&Net::ready>("ready");                         // client: its scene is the host's
        builder.Method<&Net::rpc>("rpc");
        builder.Method<&Net::rpcNumber>("rpcNumber");
        builder.Method<&Net::rpcText>("rpcText");
        builder.Method<&Net::on>("on", {"name", "handler"}); // receive: handler(sender[, value])
        builder.Method<&Net::spawn>("spawn", {"prefab", "x", "y", "z"}); // host: a prefab every peer sees
        builder.Method<&Net::spawnFor>("spawnFor", {"player", "prefab", "x", "y", "z"}); // owned by a player
        builder.Method<&Net::rpcTo>("rpcTo", {"player", "name"});                         // host: one player
        builder.Method<&Net::rpcNumberTo>("rpcNumberTo", {"player", "name", "value"});
        builder.Method<&Net::rpcTextTo>("rpcTextTo", {"player", "name", "text"});
        builder.Method<&Net::localPlayer>("localPlayer"); // 0 = the host, -1 offline / not welcomed yet
        builder.Method<&Net::isMine>("isMine", {"entity"});
        builder.Method<&Net::onJoin>("onJoin", {"handler"});
        builder.Method<&Net::onLeave>("onLeave", {"handler"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    void RegisterNetScriptFacade()
    {
        static const bool once = []()
        {
            GlobalTypeRegistry().Register(Net::StaticType());
            // Net.on's handler shapes, so an AngelScript game passes one without declaring it.
            RegisterDelegateShape(u8"void NetHandler(int sender)"); // onJoin / onLeave take one too (the player)
            RegisterDelegateShape(u8"void NetNumberHandler(int sender, double value)");
            RegisterDelegateShape(u8"void NetTextHandler(int sender, const string &in text)");
            RegisterExtraFacadeName(
                u8"Net"); // so the behavior prelude imports it (AngelScript binds by registry)
            return true;
        }();
        (void)once;
    }

    core::UniquePtr<NetworkManager> NetworkManager::HostServer(IAllocator& allocator, u16 port, bool dedicated,
                                                               const ReliableConfig& config,
                                                               u16 webSocketPort, const WebSocketTls* webSocketTls)
    {
        if (!CanHost())
        {
            return {}; // browsers cannot listen (no UDP bind, no TCP accept) - reject cleanly
        }
        if (webSocketPort != 0)
        {
            // Hybrid host: native peers over UDP + browser peers through the WS gateway,
            // one session behind one datagram socket.
            UniquePtr<WebSocketHybridSocket> hybrid =
                MakeUnique<WebSocketHybridSocket>(allocator, allocator, port, webSocketPort, webSocketTls);
            if (!hybrid->IsOpen())
            {
                if (!hybrid->Gateway().Failure().IsEmpty())
                {
                    LOG_ERROR(u8"Net", u8"the wss gateway did not open: {}", hybrid->Gateway().Failure());
                }
                return {};
            }
            UniquePtr<NetworkManager> manager = MakeUnique<NetworkManager>(
                allocator, static_cast<UniquePtr<WebSocketHybridSocket>&&>(hybrid),
                config);
            manager->StartServer(dedicated);
            return manager;
        }
        UniquePtr<UdpSocket> socket = MakeUnique<UdpSocket>(allocator, port);
        if (!socket->IsOpen())
        {
            return {};
        } // caller logs + runs offline
        UniquePtr<NetworkManager> manager = MakeUnique<NetworkManager>(
            allocator, static_cast<UniquePtr<UdpSocket>&&>(socket), config);
        manager->StartServer(dedicated);
        return manager;
    }

    namespace
    {
        UniquePtr<NetworkManager> JoinWebSocket(IAllocator& allocator, const WebSocketClientConfig& webSocket,
                                                const ReliableConfig& config)
        {
            // Sends buffer through the async handshake, so the session's immediate connect
            // packet is safe.
            UniquePtr<WebSocketClientSocket> socket = MakeUnique<WebSocketClientSocket>(allocator, allocator, webSocket);
            if (!socket->IsOpen())
            {
                LOG_ERROR(u8"Net", u8"could not join {}: {}", webSocket.host.AsView(), socket->Failure());
                return {};
            }
            UniquePtr<NetworkManager> manager =
                MakeUnique<NetworkManager>(allocator, static_cast<UniquePtr<WebSocketClientSocket>&&>(socket), config);
            manager->ConnectTo(kWebSocketServerEndpoint);
            return manager;
        }
    }

    core::UniquePtr<NetworkManager> NetworkManager::JoinServer(IAllocator& allocator, StringView host, u16 port,
                                                               const ReliableConfig& config, ITlsBackend* tls,
                                                               TlsTrust trust)
    {
        WebSocketClientConfig webSocket;
        const bool addressed = ParseWebSocketAddress(host, port, webSocket);
#if PLATFORM_WEB
        // Browsers have no UDP: a bare host is joined over a WebSocket to the host's gateway
        // (`port` here is the host's WEB SOCKET port), secure when the page is.
        (void)tls;
        (void)trust;
        if (!addressed)
        {
            webSocket.host = String(host);
            webSocket.port = port;
            webSocket.secure = EM_ASM_INT({ return location.protocol === 'https:' ? 1 : 0; }) != 0;
        }
        return JoinWebSocket(allocator, webSocket, config);
#else
        if (addressed)
        {
            webSocket.tls = tls;
            webSocket.trust = static_cast<TlsTrust&&>(trust);
            return JoinWebSocket(allocator, webSocket, config);
        }
        UniquePtr<UdpSocket> socket =
            MakeUnique<UdpSocket>(allocator, u16{0}); // ephemeral
        if (!socket->IsOpen())
        {
            return {};
        }
        UniquePtr<NetworkManager> manager = MakeUnique<NetworkManager>(
            allocator, static_cast<UniquePtr<UdpSocket>&&>(socket), config);
        manager->ConnectTo(ResolveEndpoint(host, port));
        return manager;
#endif
    }

    NetworkRuntime StartNetworking(IAllocator& allocator, const NetworkStartup& config)
    {
        NetworkRuntime runtime;
        if (config.role == NetworkRole::None)
        {
            return runtime;
        }

        // The facade is bound wherever networking actually runs (idempotent).
        RegisterNetScriptFacade();

        // Server binds the listen port; a client binds ephemeral (0) unless a port is forced.
        runtime.socket = MakeUnique<UdpSocket>(allocator, config.listenPort);
        runtime.manager =
            MakeUnique<NetworkManager>(allocator, *runtime.socket, config.reliable);

        if (config.role == NetworkRole::Server)
        {
            runtime.manager->StartServer(config.dedicated);
        }
        else // Client
        {
            const DatagramEndpoint server =
                ResolveEndpoint(config.serverHost.AsView(), config.serverPort);
            runtime.manager->ConnectTo(server);
        }
        return runtime;
    }
}
