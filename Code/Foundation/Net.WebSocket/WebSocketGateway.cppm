// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net.WebSocket - the `foundation.net.websocket` module.
///
/// The native WebSocket ACCEPT path: lets a desktop host accept BROWSER clients (browsers
/// cannot open raw UDP - their only game-usable socket is a TCP-based WebSocket). A WS
/// connection begins as an HTTP request, so the handshake parses through foundation.http
/// (the recorded front-door design); after the 101 upgrade the connection speaks RFC 6455
/// binary frames, which are message-oriented and map 1:1 onto the datagram seam the whole
/// net stack rides (`IDatagramSocket`).
///
/// `WebSocketHybridSocket` is the drop-in: ONE IDatagramSocket wrapping the host's UDP
/// socket AND a WS gateway - native peers arrive over UDP, browser peers over WS, and the
/// session/reliability/replication layers cannot tell the difference. WS endpoints are
/// synthetic (bit 63 set + client id; real UDP endpoints use only the low 48 bits).
///
/// TLS (wss://) is the gateway's when the host hands it a backend and a certificate
/// (`WebSocketTls`): each connection then runs a TLS session before the upgrade, through
/// foundation.net.tls's streams. Deliberate v1 bounds: no fragmented frames (FIN=0 is a protocol error; game packets are far below
/// any fragmentation threshold), text frames rejected (the wire is binary), 1 MiB frame
/// cap. The reliability layer runs UNCHANGED over WS: TCP already guarantees delivery, so
/// resends simply never fire (acks flow; redundant but correct - the keep-v1-simple
/// ruling).
///
/// The module holds both halves: the native accept path, the client socket
/// (`WebSocketClientSocket`: the browser's WebSocket in a web build, this module's own client
/// natively), and what they share (frame codec, handshake math), unit-tested on desktop where
/// sockets work headless.

module;
#include "Core/Prelude.h"

export module foundation.net.websocket;

import foundation.core;
import foundation.net;  // TcpListener/TcpSocket, IDatagramSocket, UdpSocket, endpoints
import foundation.http; // HttpMessageParser - the upgrade request IS an HTTP request
import foundation.net.tls; // the streams each connection rides, plain or TLS

using namespace foundation::core;

export namespace foundation::net
{
    // ==================================================================================
    // Handshake math (RFC 6455 4.2.2): Accept = Base64(SHA1(key + magic GUID)).
    // Hand-rolled - tiny, RFC-fixed, and the no-new-third-party rule applies.
    // ==================================================================================

    /// SHA-1 of `data` into out20 (20 bytes). Handshake-sized inputs only - not a general
    /// hashing service (and SHA-1 is fine here: the accept key is an echo, not security).
    void Sha1(Span<const byte> data, byte* out20);

    [[nodiscard]] String Base64Encode(Span<const byte> data);

    /// The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key.
    [[nodiscard]] String ComputeWebSocketAccept(StringView clientKey);

    // ==================================================================================
    // Frame codec (RFC 6455 5.2)
    // ==================================================================================

    enum class WsOpcode : u8
    {
        Continuation = 0x0,
        Text = 0x1,
        Binary = 0x2,
        Close = 0x8,
        Ping = 0x9,
        Pong = 0xA,
    };

    inline constexpr usize kWsMaxFrameBytes = 1024 * 1024; // parser cap (protocol error above)

    /// Append one FIN frame to `out`. `mask` = client->server direction (RFC: client frames
    /// MUST be masked, server frames MUST NOT); the tests build client frames with it.
    void EncodeWebSocketFrame(WsOpcode opcode, Span<const byte> payload, bool mask,
                              u32 maskingKey, Array<byte>& out);

    struct WsFrame
    {
        WsOpcode opcode = WsOpcode::Binary;
        Array<byte> payload;
    };

    /// Incremental frame decoder for one side of a connection. Push bytes as they arrive;
    /// drain complete frames via Next(). Enforces: the server reads masked frames (a client's)
    /// and the client unmasked ones (a server's), FIN only (no fragmentation), size cap.
    /// Failed() latches - close the connection.
    class WebSocketFrameParser
    {
    public:
        /// `serverSide`: reading what a client sent (masked), else what a server sent.
        explicit WebSocketFrameParser(bool serverSide = true) noexcept : m_serverSide(serverSide) {}
        void Push(Span<const byte> bytes);
        [[nodiscard]] bool Next(WsFrame& out);
        [[nodiscard]] bool Failed() const noexcept { return m_failed; }

    private:
        void Parse();

        Array<byte> m_buffer;
        usize m_head = 0;
        Array<WsFrame> m_frames;
        usize m_frameHead = 0;
        bool m_failed = false;
        bool m_serverSide = true;
    };

    // ==================================================================================
    // Handshake validation + responses
    // ==================================================================================

    /// True when the COMPLETE parsed request is a valid RFC 6455 upgrade (GET, Upgrade:
    /// websocket, Connection contains the Upgrade token, version 13, a key present); fills
    /// `outKey` with the client's Sec-WebSocket-Key.
    [[nodiscard]] bool ValidateWebSocketUpgrade(const http::HttpMessageParser& request,
                                                String& outKey);

    /// The 101 Switching Protocols response bytes for a validated key.
    [[nodiscard]] Array<byte> BuildWebSocketUpgradeResponse(StringView clientKey);
    /// The 400 rejection bytes for anything else.
    [[nodiscard]] Array<byte> BuildWebSocketRejectResponse();

    // ==================================================================================
    // The server gateway: accept browser connections, upgrade, speak frames.
    // ==================================================================================

    enum class WsGatewayEventKind : u8
    {
        Connected,    // a client completed the upgrade
        Disconnected, // a client closed / errored
        Message,      // one binary frame arrived
    };

    struct WsGatewayEvent
    {
        WsGatewayEventKind kind = WsGatewayEventKind::Message;
        u32 client = 0;
        Array<byte> payload; // Message only
    };

    /// What a gateway needs to speak wss://: the TLS backend (the application's, outliving
    /// the gateway) and the host's certificate chain and key.
    struct WebSocketTls
    {
        ITlsBackend* backend = nullptr;
        TlsServerConfig config;
    };

    /// The pump-model WS server: Pump() accepts pending TCP connections, progresses their
    /// HTTP upgrade handshakes, decodes frames (answering pings), and queues events; drain
    /// with Poll(). Single-threaded, like HttpServer.
    class WebSocketServerGateway
    {
    public:
        /// Bind the listener (port 0 = OS-assigned; read BoundPort()). Check IsOpen().
        /// With `tls`, every connection is TLS first (wss://); a certificate or key that does
        /// not load leaves the gateway closed, Failure() saying why.
        // The allocator (required - the owner decides) backs the impl + per-client
        // handshake parsers.
        WebSocketServerGateway(IAllocator& allocator, u16 port, const WebSocketTls* tls = nullptr);
        ~WebSocketServerGateway();
        WebSocketServerGateway(const WebSocketServerGateway&) = delete;
        WebSocketServerGateway& operator=(const WebSocketServerGateway&) = delete;

        [[nodiscard]] bool IsOpen() const noexcept;
        [[nodiscard]] u16 BoundPort() const noexcept;
        [[nodiscard]] bool IsSecure() const noexcept;
        /// Why the gateway is not open, when its TLS configuration is the reason.
        [[nodiscard]] StringView Failure() const noexcept;

        void Pump();
        [[nodiscard]] bool Poll(WsGatewayEvent& out);

        /// Send one binary frame to an upgraded client (unknown/closed ids no-op).
        void SendBinary(u32 client, Span<const byte> data);
        void DisconnectClient(u32 client);

        [[nodiscard]] usize ClientCount() const noexcept;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };

    // ==================================================================================
    // The hybrid datagram socket: UDP natives + WS browsers behind ONE IDatagramSocket.
    // ==================================================================================

    /// WS client endpoints: bit 63 + the gateway client id (UDP endpoints pack
    /// (ip << 16) | port and never touch the high bits).
    inline constexpr u64 kWebSocketEndpointBit = 1ull << 63;

    [[nodiscard]] inline DatagramEndpoint MakeWebSocketEndpoint(u32 client) noexcept
    {
        return DatagramEndpoint{kWebSocketEndpointBit | static_cast<u64>(client)};
    }
    [[nodiscard]] inline bool IsWebSocketEndpoint(const DatagramEndpoint& e) noexcept
    {
        return (e.value & kWebSocketEndpointBit) != 0;
    }
    [[nodiscard]] inline u32 WebSocketEndpointClient(const DatagramEndpoint& e) noexcept
    {
        return static_cast<u32>(e.value & 0xFFFFFFFFull);
    }

    /// The single remote a WebSocketClientSocket talks to: the host's gateway.
    inline constexpr DatagramEndpoint kWebSocketServerEndpoint{kWebSocketEndpointBit | 1ull};

    /// Where a WebSocketClientSocket connects, and how.
    struct WebSocketClientConfig
    {
        String host;                ///< a name or a dotted quad
        u16 port = 0;
        bool secure = false;        ///< wss:// (TLS); ws:// otherwise
        String path = String(u8"/");
        /// Native wss only: the TLS backend (the application's, outliving the socket) and what
        /// the host's certificate is checked against. A browser does its own TLS.
        ITlsBackend* tls = nullptr;
        TlsTrust trust;
        /// Native only: a host that has not upgraded the connection by then is given up on.
        f32 upgradeTimeoutSeconds = 10.0f;
    };

    /// One WebSocket connection to a host's gateway, presented as an IDatagramSocket (each
    /// binary frame = one datagram; the single remote is kWebSocketServerEndpoint). In a
    /// browser it is the browser's WebSocket; natively it is this module's own client over a
    /// plain or TLS stream, so a desktop or Deck client can join a host that only speaks
    /// wss. The handshake is ASYNC either way - sends queue until the upgrade, so the
    /// session's immediate connect packet survives the open latency. Natively the name
    /// lookup in the constructor blocks, as a UDP join's does.
    class WebSocketClientSocket final : public IDatagramSocket
    {
    public:
        WebSocketClientSocket(IAllocator& allocator, const WebSocketClientConfig& config);
        ~WebSocketClientSocket() override;
        WebSocketClientSocket(const WebSocketClientSocket&) = delete;
        WebSocketClientSocket& operator=(const WebSocketClientSocket&) = delete;

        [[nodiscard]] bool IsOpen() const noexcept;      // created and not failed (may still be connecting)
        [[nodiscard]] bool IsConnected() const noexcept; // the upgrade done: sends go at once
        /// Why it failed or closed, said for a person; empty while it lives. (A browser keeps
        /// the reason from the page: there it says only that the connection failed.)
        [[nodiscard]] StringView Failure() const noexcept;

        void Send(const DatagramEndpoint& to, Span<const byte> data) override;
        [[nodiscard]] bool Receive(DatagramEndpoint& from, Array<byte>& out) override;
        [[nodiscard]] DatagramEndpoint LocalEndpoint() const override
        {
            return DatagramEndpoint{kWebSocketEndpointBit | 2ull};
        }

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };

    class WebSocketHybridSocket final : public IDatagramSocket
    {
    public:
        /// Bind the UDP socket on `udpPort` and the WS listener on `webSocketPort`
        /// (either may be 0 = OS-assigned). Check IsOpen().
        WebSocketHybridSocket(IAllocator& allocator, u16 udpPort, u16 webSocketPort, const WebSocketTls* tls = nullptr)
            : m_udp(udpPort), m_gateway(allocator, webSocketPort, tls)
        {
        }

        [[nodiscard]] bool IsOpen() const noexcept
        {
            return m_udp.IsOpen() && m_gateway.IsOpen();
        }
        [[nodiscard]] u16 BoundPort() const noexcept { return m_udp.BoundPort(); }
        [[nodiscard]] u16 WebSocketBoundPort() const noexcept { return m_gateway.BoundPort(); }

        void Send(const DatagramEndpoint& to, Span<const byte> data) override
        {
            if (IsWebSocketEndpoint(to))
            {
                m_gateway.SendBinary(WebSocketEndpointClient(to), data);
            }
            else
            {
                m_udp.Send(to, data);
            }
        }

        [[nodiscard]] bool Receive(DatagramEndpoint& from, Array<byte>& out) override
        {
            if (m_udp.Receive(from, out))
            {
                return true;
            }
            // The session drains Receive to exhaustion every frame - pumping here keeps the
            // gateway serviced without a separate update hook on the datagram seam.
            m_gateway.Pump();
            WsGatewayEvent event;
            while (m_gateway.Poll(event))
            {
                if (event.kind != WsGatewayEventKind::Message)
                {
                    continue; // session-level presence comes from session packets, not TCP
                }
                from = MakeWebSocketEndpoint(event.client);
                out = static_cast<Array<byte>&&>(event.payload);
                return true;
            }
            return false;
        }

        [[nodiscard]] DatagramEndpoint LocalEndpoint() const override
        {
            return m_udp.LocalEndpoint();
        }

        [[nodiscard]] WebSocketServerGateway& Gateway() noexcept { return m_gateway; }

    private:
        UdpSocket m_udp;
        WebSocketServerGateway m_gateway;
    };
}
