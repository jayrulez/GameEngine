// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net.websocket - implementation: handshake math, frame codec, the gateway.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#if PLATFORM_WEB
#include <cstdio> // snprintf (the ws:// URL)
#include <emscripten/websocket.h>
#endif

module foundation.net.websocket;

import foundation.core;
import foundation.net;
import foundation.http;
import foundation.net.tls;

using namespace foundation::core;

namespace foundation::net
{
    // ---- SHA-1 (RFC 3174) ------------------------------------------------------------

    namespace
    {
        [[nodiscard]] u32 Rol32(u32 v, u32 bits) noexcept
        {
            return (v << bits) | (v >> (32u - bits));
        }
    }

    void Sha1(Span<const byte> data, byte* out20)
    {
        u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

        // Padded message: data + 0x80 + zeros + 64-bit big-endian bit length.
        Array<byte> padded;
        padded.Reserve(data.Size() + 72);
        for (byte b : data)
        {
            padded.PushBack(b);
        }
        padded.PushBack(byte{0x80});
        while ((padded.Size() % 64) != 56)
        {
            padded.PushBack(byte{0});
        }
        const u64 bitLength = static_cast<u64>(data.Size()) * 8ull;
        for (i32 shift = 56; shift >= 0; shift -= 8)
        {
            padded.PushBack(static_cast<byte>((bitLength >> shift) & 0xFFull));
        }

        for (usize block = 0; block < padded.Size(); block += 64)
        {
            u32 w[80];
            for (usize i = 0; i < 16; ++i)
            {
                const usize at = block + i * 4;
                w[i] = (static_cast<u32>(padded[at]) << 24) |
                       (static_cast<u32>(padded[at + 1]) << 16) |
                       (static_cast<u32>(padded[at + 2]) << 8) | static_cast<u32>(padded[at + 3]);
            }
            for (usize i = 16; i < 80; ++i)
            {
                w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }
            u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (usize i = 0; i < 80; ++i)
            {
                u32 f = 0, k = 0;
                if (i < 20)
                {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999u;
                }
                else if (i < 40)
                {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1u;
                }
                else if (i < 60)
                {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDCu;
                }
                else
                {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6u;
                }
                const u32 temp = Rol32(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = Rol32(b, 30);
                b = a;
                a = temp;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
        }

        for (usize i = 0; i < 5; ++i)
        {
            out20[i * 4 + 0] = static_cast<byte>((h[i] >> 24) & 0xFFu);
            out20[i * 4 + 1] = static_cast<byte>((h[i] >> 16) & 0xFFu);
            out20[i * 4 + 2] = static_cast<byte>((h[i] >> 8) & 0xFFu);
            out20[i * 4 + 3] = static_cast<byte>(h[i] & 0xFFu);
        }
    }

    // ---- Base64 (RFC 4648) -----------------------------------------------------------

    String Base64Encode(Span<const byte> data)
    {
        static constexpr char kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        String out;
        usize i = 0;
        for (; i + 3 <= data.Size(); i += 3)
        {
            const u32 v = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8) |
                          static_cast<u32>(data[i + 2]);
            out.PushBack(static_cast<utf8char>(kTable[(v >> 18) & 0x3F]));
            out.PushBack(static_cast<utf8char>(kTable[(v >> 12) & 0x3F]));
            out.PushBack(static_cast<utf8char>(kTable[(v >> 6) & 0x3F]));
            out.PushBack(static_cast<utf8char>(kTable[v & 0x3F]));
        }
        const usize rest = data.Size() - i;
        if (rest == 1)
        {
            const u32 v = static_cast<u32>(data[i]) << 16;
            out.PushBack(static_cast<utf8char>(kTable[(v >> 18) & 0x3F]));
            out.PushBack(static_cast<utf8char>(kTable[(v >> 12) & 0x3F]));
            out.PushBack(utf8char('='));
            out.PushBack(utf8char('='));
        }
        else if (rest == 2)
        {
            const u32 v = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8);
            out.PushBack(static_cast<utf8char>(kTable[(v >> 18) & 0x3F]));
            out.PushBack(static_cast<utf8char>(kTable[(v >> 12) & 0x3F]));
            out.PushBack(static_cast<utf8char>(kTable[(v >> 6) & 0x3F]));
            out.PushBack(utf8char('='));
        }
        return out;
    }

    String ComputeWebSocketAccept(StringView clientKey)
    {
        // RFC 6455 magic GUID, concatenated to the key VERBATIM (no decode).
        static constexpr StringView kMagic = u8"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        Array<byte> input;
        input.Reserve(clientKey.Size() + kMagic.Size());
        for (usize i = 0; i < clientKey.Size(); ++i)
        {
            input.PushBack(static_cast<byte>(clientKey[i]));
        }
        for (usize i = 0; i < kMagic.Size(); ++i)
        {
            input.PushBack(static_cast<byte>(kMagic[i]));
        }
        byte digest[20];
        Sha1(Span<const byte>{input.Data(), input.Size()}, digest);
        return Base64Encode(Span<const byte>{digest, 20});
    }

    // ---- frame codec -------------------------------------------------------------------

    void EncodeWebSocketFrame(WsOpcode opcode, Span<const byte> payload, bool mask,
                              u32 maskingKey, Array<byte>& out)
    {
        out.PushBack(static_cast<byte>(0x80u | static_cast<u8>(opcode))); // FIN + opcode
        const u8 maskBit = mask ? 0x80u : 0x00u;
        if (payload.Size() < 126)
        {
            out.PushBack(static_cast<byte>(maskBit | static_cast<u8>(payload.Size())));
        }
        else if (payload.Size() <= 0xFFFF)
        {
            out.PushBack(static_cast<byte>(maskBit | 126u));
            out.PushBack(static_cast<byte>((payload.Size() >> 8) & 0xFF));
            out.PushBack(static_cast<byte>(payload.Size() & 0xFF));
        }
        else
        {
            out.PushBack(static_cast<byte>(maskBit | 127u));
            for (i32 shift = 56; shift >= 0; shift -= 8)
            {
                out.PushBack(static_cast<byte>((static_cast<u64>(payload.Size()) >> shift) & 0xFF));
            }
        }
        byte key[4] = {static_cast<byte>((maskingKey >> 24) & 0xFF),
                       static_cast<byte>((maskingKey >> 16) & 0xFF),
                       static_cast<byte>((maskingKey >> 8) & 0xFF),
                       static_cast<byte>(maskingKey & 0xFF)};
        if (mask)
        {
            out.PushBack(key[0]);
            out.PushBack(key[1]);
            out.PushBack(key[2]);
            out.PushBack(key[3]);
            for (usize i = 0; i < payload.Size(); ++i)
            {
                out.PushBack(static_cast<byte>(static_cast<u8>(payload[i]) ^
                                               static_cast<u8>(key[i % 4])));
            }
        }
        else
        {
            for (byte b : payload)
            {
                out.PushBack(b);
            }
        }
    }

    void WebSocketFrameParser::Push(Span<const byte> bytes)
    {
        if (m_failed)
        {
            return;
        }
        for (byte b : bytes)
        {
            m_buffer.PushBack(b);
        }
        Parse();
    }

    bool WebSocketFrameParser::Next(WsFrame& out)
    {
        if (m_frameHead >= m_frames.Size())
        {
            return false;
        }
        out = static_cast<WsFrame&&>(m_frames[m_frameHead++]);
        if (m_frameHead >= m_frames.Size())
        {
            m_frames.Clear();
            m_frameHead = 0;
        }
        return true;
    }

    void WebSocketFrameParser::Parse()
    {
        for (;;)
        {
            const usize available = m_buffer.Size() - m_head;
            if (available < 2)
            {
                break;
            }
            const u8 b0 = static_cast<u8>(m_buffer[m_head]);
            const u8 b1 = static_cast<u8>(m_buffer[m_head + 1]);
            const bool fin = (b0 & 0x80u) != 0;
            if ((b0 & 0x70u) != 0 || !fin)
            {
                // RSV bits (no extensions negotiated) and fragmentation are protocol errors
                // in this deliberately-bounded server.
                m_failed = true;
                return;
            }
            const auto opcode = static_cast<WsOpcode>(b0 & 0x0Fu);
            const bool masked = (b1 & 0x80u) != 0;
            if (!masked)
            {
                m_failed = true; // RFC 6455 5.1: client frames MUST be masked
                return;
            }
            u64 length = b1 & 0x7Fu;
            usize cursor = m_head + 2;
            if (length == 126)
            {
                if (available < 4)
                {
                    break;
                }
                length = (static_cast<u64>(static_cast<u8>(m_buffer[cursor])) << 8) |
                         static_cast<u64>(static_cast<u8>(m_buffer[cursor + 1]));
                cursor += 2;
            }
            else if (length == 127)
            {
                if (available < 10)
                {
                    break;
                }
                length = 0;
                for (usize i = 0; i < 8; ++i)
                {
                    length = (length << 8) | static_cast<u64>(static_cast<u8>(m_buffer[cursor + i]));
                }
                cursor += 8;
            }
            if (length > kWsMaxFrameBytes)
            {
                m_failed = true;
                return;
            }
            if (m_buffer.Size() - cursor < 4 + length)
            {
                break; // need the masking key + full payload
            }
            byte key[4] = {m_buffer[cursor], m_buffer[cursor + 1], m_buffer[cursor + 2],
                           m_buffer[cursor + 3]};
            cursor += 4;

            WsFrame frame;
            frame.opcode = opcode;
            frame.payload.Reserve(static_cast<usize>(length));
            for (usize i = 0; i < length; ++i)
            {
                frame.payload.PushBack(static_cast<byte>(
                    static_cast<u8>(m_buffer[cursor + i]) ^ static_cast<u8>(key[i % 4])));
            }
            m_frames.PushBack(static_cast<WsFrame&&>(frame));
            m_head = cursor + static_cast<usize>(length);

            if (m_head == m_buffer.Size())
            {
                m_buffer.Clear();
                m_head = 0;
            }
        }
    }

    // ---- handshake ---------------------------------------------------------------------

    namespace
    {
        // Case-insensitive ASCII contains-token (Connection: keep-alive, Upgrade).
        [[nodiscard]] bool ContainsTokenNoCase(StringView haystack, StringView token)
        {
            if (token.IsEmpty() || haystack.Size() < token.Size())
            {
                return false;
            }
            const auto lower = [](utf8char c) -> utf8char
            {
                return (c >= utf8char('A') && c <= utf8char('Z'))
                           ? static_cast<utf8char>(c + 32)
                           : c;
            };
            for (usize i = 0; i + token.Size() <= haystack.Size(); ++i)
            {
                bool match = true;
                for (usize j = 0; j < token.Size(); ++j)
                {
                    if (lower(haystack[i + j]) != lower(token[j]))
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool ValidateWebSocketUpgrade(const http::HttpMessageParser& request, String& outKey)
    {
        if (request.State() != http::HttpParseState::Complete ||
            request.Method() != u8"GET")
        {
            return false;
        }
        const Array<http::HttpHeader>& headers = request.Headers();
        const StringView upgrade = http::FindHeader(headers, u8"Upgrade");
        const StringView connection = http::FindHeader(headers, u8"Connection");
        const StringView version = http::FindHeader(headers, u8"Sec-WebSocket-Version");
        const StringView key = http::FindHeader(headers, u8"Sec-WebSocket-Key");
        if (!ContainsTokenNoCase(upgrade, u8"websocket") ||
            !ContainsTokenNoCase(connection, u8"upgrade") || version != u8"13" || key.IsEmpty())
        {
            return false;
        }
        outKey = String(key);
        return true;
    }

    namespace
    {
        void AppendText(Array<byte>& out, StringView text)
        {
            for (usize i = 0; i < text.Size(); ++i)
            {
                out.PushBack(static_cast<byte>(text[i]));
            }
        }
    }

    Array<byte> BuildWebSocketUpgradeResponse(StringView clientKey)
    {
        Array<byte> out;
        AppendText(out, u8"HTTP/1.1 101 Switching Protocols\r\n"
                        u8"Upgrade: websocket\r\n"
                        u8"Connection: Upgrade\r\n"
                        u8"Sec-WebSocket-Accept: ");
        AppendText(out, ComputeWebSocketAccept(clientKey).AsView());
        AppendText(out, u8"\r\n\r\n");
        return out;
    }

    Array<byte> BuildWebSocketRejectResponse()
    {
        Array<byte> out;
        AppendText(out, u8"HTTP/1.1 400 Bad Request\r\n"
                        u8"Connection: close\r\n"
                        u8"Content-Length: 0\r\n\r\n");
        return out;
    }

    // ---- the gateway ---------------------------------------------------------------------

    struct WebSocketServerGateway::Impl
    {
        struct Client
        {
            u32 id = 0;
            UniquePtr<INetByteStream> stream; // plain, or TLS (wss://); it buffers what would block
            bool upgraded = false;
            UniquePtr<http::HttpMessageParser> handshake; // until upgraded
            WebSocketFrameParser frames;                  // after
            bool dead = false;
        };

        Impl(IAllocator& alloc, u16 port) : allocator(&alloc), listener(port) {}
        IAllocator* allocator;

        TcpListener listener;
        bool secure = false;
        ITlsBackend* tls = nullptr;
        TlsServerConfig tlsConfig;
        String failure;
        Array<Client> clients;
        Array<WsGatewayEvent> events;
        usize eventHead = 0;
        u32 nextClientId = 1;
        static constexpr usize kMaxClients = 64;

        void PushEvent(WsGatewayEventKind kind, u32 id, Array<byte> payload = {})
        {
            WsGatewayEvent event;
            event.kind = kind;
            event.client = id;
            event.payload = static_cast<Array<byte>&&>(payload);
            events.PushBack(static_cast<WsGatewayEvent&&>(event));
        }

        // The stream takes all of it, holding what the socket would not take yet.
        void SendRaw(Client& client, Span<const byte> bytes)
        {
            if (client.stream->Send(bytes) < 0)
            {
                client.dead = true;
            }
        }

        void PumpClient(Client& client)
        {
            // Moves the stream's bytes (and a TLS handshake) on; what it already holds is still
            // read below before a closed stream counts.
            const bool alive = client.stream->Pump();
            ReadClient(client);
            if (!alive && !client.dead)
            {
                client.dead = true;
                if (client.upgraded)
                {
                    PushEvent(WsGatewayEventKind::Disconnected, client.id);
                }
            }
        }

        void ReadClient(Client& client)
        {
            byte chunk[4096];
            for (;;)
            {
                const i64 got = client.stream->Receive(Span<byte>{chunk, sizeof(chunk)});
                if (got < 0)
                {
                    client.dead = true;
                    if (client.upgraded)
                    {
                        PushEvent(WsGatewayEventKind::Disconnected, client.id);
                    }
                    return;
                }
                if (got == 0)
                {
                    break; // would-block: nothing more this pump
                }
                usize offset = 0;
                if (!client.upgraded)
                {
                    // Feed the handshake BYTE-WISE so bytes past the request head (an eager
                    // client's first frame in the same segment) land in the frame parser,
                    // not the HTTP parser - Push reports no consumed count.
                    while (offset < static_cast<usize>(got))
                    {
                        const http::HttpParseState state =
                            client.handshake->Push(Span<const byte>{chunk + offset, 1});
                        ++offset;
                        if (state == http::HttpParseState::Failed)
                        {
                            const Array<byte> reject = BuildWebSocketRejectResponse();
                            SendRaw(client,
                                    Span<const byte>{reject.Data(), reject.Size()});
                            client.dead = true;
                            return;
                        }
                        if (state == http::HttpParseState::Complete)
                        {
                            String key;
                            if (!ValidateWebSocketUpgrade(*client.handshake, key))
                            {
                                const Array<byte> reject = BuildWebSocketRejectResponse();
                                SendRaw(client,
                                        Span<const byte>{reject.Data(), reject.Size()});
                                client.dead = true;
                                return;
                            }
                            const Array<byte> ok = BuildWebSocketUpgradeResponse(key.AsView());
                            SendRaw(client, Span<const byte>{ok.Data(), ok.Size()});
                            client.upgraded = true;
                            client.handshake = {};
                            PushEvent(WsGatewayEventKind::Connected, client.id);
                            break; // rest of the chunk is frame bytes
                        }
                    }
                    if (!client.upgraded)
                    {
                        continue; // handshake still incomplete - read more
                    }
                }
                client.frames.Push(
                    Span<const byte>{chunk + offset, static_cast<usize>(got) - offset});
                if (client.frames.Failed())
                {
                    client.dead = true;
                    PushEvent(WsGatewayEventKind::Disconnected, client.id);
                    return;
                }
                WsFrame frame;
                while (client.frames.Next(frame))
                {
                    switch (frame.opcode)
                    {
                    case WsOpcode::Binary:
                        PushEvent(WsGatewayEventKind::Message, client.id,
                                  static_cast<Array<byte>&&>(frame.payload));
                        break;
                    case WsOpcode::Ping:
                    {
                        Array<byte> pong;
                        EncodeWebSocketFrame(WsOpcode::Pong,
                                             Span<const byte>{frame.payload.Data(),
                                                              frame.payload.Size()},
                                             false, 0, pong);
                        SendRaw(client, Span<const byte>{pong.Data(), pong.Size()});
                        break;
                    }
                    case WsOpcode::Pong:
                        break; // keep-alive answer; nothing to do
                    case WsOpcode::Close:
                    {
                        Array<byte> close;
                        EncodeWebSocketFrame(WsOpcode::Close, {}, false, 0, close);
                        SendRaw(client, Span<const byte>{close.Data(), close.Size()});
                        client.dead = true;
                        PushEvent(WsGatewayEventKind::Disconnected, client.id);
                        return;
                    }
                    default:
                        client.dead = true; // Text/Continuation: not this wire's protocol
                        PushEvent(WsGatewayEventKind::Disconnected, client.id);
                        return;
                    }
                }
            }
        }
    };

    WebSocketServerGateway::WebSocketServerGateway(IAllocator& allocator, u16 port, const WebSocketTls* tls)
        : m_impl(MakeUnique<Impl>(allocator, allocator, port))
    {
        if (tls == nullptr)
        {
            return;
        }
        m_impl->secure = true;
        m_impl->tls = tls->backend;
        m_impl->tlsConfig = tls->config;
        if (tls->backend == nullptr)
        {
            m_impl->failure = String(u8"wss needs a TLS backend, and none was given");
            return;
        }
        // A certificate or key that does not load fails every connection: say so now, once.
        const UniquePtr<ITlsSession> probe = tls->backend->CreateServer(tls->config);
        if (probe->State() == TlsState::Failed)
        {
            m_impl->failure = String(probe->Failure());
        }
    }
    WebSocketServerGateway::~WebSocketServerGateway()
    {
        for (Impl::Client& client : m_impl->clients)
        {
            client.stream->Close();
        }
    }

    bool WebSocketServerGateway::IsOpen() const noexcept { return m_impl->listener.IsOpen() && m_impl->failure.IsEmpty(); }
    bool WebSocketServerGateway::IsSecure() const noexcept { return m_impl->secure; }
    StringView WebSocketServerGateway::Failure() const noexcept { return m_impl->failure.AsView(); }
    u16 WebSocketServerGateway::BoundPort() const noexcept { return m_impl->listener.BoundPort(); }
    usize WebSocketServerGateway::ClientCount() const noexcept { return m_impl->clients.Size(); }

    void WebSocketServerGateway::Pump()
    {
        Impl& impl = *m_impl;
        if (!impl.failure.IsEmpty())
        {
            return;
        }
        for (;;)
        {
            TcpSocket accepted = impl.listener.Accept();
            if (!accepted.IsOpen())
            {
                break;
            }
            if (impl.clients.Size() >= Impl::kMaxClients)
            {
                continue; // over cap: drop (RAII closes)
            }
            Impl::Client client;
            client.id = impl.nextClientId++;
            if (impl.secure)
            {
                client.stream = MakeUnique<TlsStream>(*impl.allocator, static_cast<TcpSocket&&>(accepted),
                                                      impl.tls->CreateServer(impl.tlsConfig));
            }
            else
            {
                client.stream = MakeUnique<PlainStream>(*impl.allocator, static_cast<TcpSocket&&>(accepted));
            }
            client.handshake = MakeUnique<http::HttpMessageParser>(
                *impl.allocator, http::HttpMessageParser::Mode::Request, usize{4096});
            impl.clients.PushBack(static_cast<Impl::Client&&>(client));
        }
        for (Impl::Client& client : impl.clients)
        {
            if (!client.dead)
            {
                impl.PumpClient(client);
            }
        }
        for (usize i = impl.clients.Size(); i > 0; --i)
        {
            if (impl.clients[i - 1].dead)
            {
                impl.clients[i - 1].stream->Close(); // what is still held goes (a close frame, close_notify)
                impl.clients.RemoveAt(i - 1);
            }
        }
    }

    bool WebSocketServerGateway::Poll(WsGatewayEvent& out)
    {
        Impl& impl = *m_impl;
        if (impl.eventHead >= impl.events.Size())
        {
            return false;
        }
        out = static_cast<WsGatewayEvent&&>(impl.events[impl.eventHead++]);
        if (impl.eventHead >= impl.events.Size())
        {
            impl.events.Clear();
            impl.eventHead = 0;
        }
        return true;
    }

    void WebSocketServerGateway::SendBinary(u32 clientId, Span<const byte> data)
    {
        for (Impl::Client& client : m_impl->clients)
        {
            if (client.id == clientId && client.upgraded && !client.dead)
            {
                Array<byte> frame;
                EncodeWebSocketFrame(WsOpcode::Binary, data, false, 0, frame);
                m_impl->SendRaw(client, Span<const byte>{frame.Data(), frame.Size()});
                return;
            }
        }
    }

    void WebSocketServerGateway::DisconnectClient(u32 clientId)
    {
        for (Impl::Client& client : m_impl->clients)
        {
            if (client.id == clientId)
            {
                Array<byte> close;
                EncodeWebSocketFrame(WsOpcode::Close, {}, false, 0, close);
                m_impl->SendRaw(client, Span<const byte>{close.Data(), close.Size()});
                client.dead = true;
                return;
            }
        }
    }

#if PLATFORM_WEB
    // ---- the browser client socket -------------------------------------------------------
    // Callback-driven (emscripten/websocket.h); without pthreads the callbacks run on the
    // main-thread event loop, so the queues below need no locking. Sends before onopen
    // BUFFER - the session fires its connect packet immediately, well inside the WS
    // handshake latency.

    struct WebSocketClientSocket::Impl
    {
        EMSCRIPTEN_WEBSOCKET_T socket = 0;
        bool open = false;
        bool failed = false;
        Array<Array<byte>> pendingSends; // queued until onopen
        Array<Array<byte>> received;
        usize receivedHead = 0;

        static EM_BOOL OnOpen(int, const EmscriptenWebSocketOpenEvent*, void* user)
        {
            auto* impl = static_cast<Impl*>(user);
            impl->open = true;
            for (const Array<byte>& payload : impl->pendingSends)
            {
                (void)emscripten_websocket_send_binary(
                    impl->socket, const_cast<byte*>(payload.Data()),
                    static_cast<u32>(payload.Size()));
            }
            impl->pendingSends.Clear();
            return EM_TRUE;
        }

        static EM_BOOL OnMessage(int, const EmscriptenWebSocketMessageEvent* event, void* user)
        {
            auto* impl = static_cast<Impl*>(user);
            if (event->isText)
            {
                return EM_TRUE; // the wire is binary; ignore stray text
            }
            Array<byte> payload;
            payload.Reserve(event->numBytes);
            for (u32 i = 0; i < event->numBytes; ++i)
            {
                payload.PushBack(static_cast<byte>(event->data[i]));
            }
            impl->received.PushBack(static_cast<Array<byte>&&>(payload));
            return EM_TRUE;
        }

        static EM_BOOL OnError(int, const EmscriptenWebSocketErrorEvent*, void* user)
        {
            static_cast<Impl*>(user)->failed = true;
            return EM_TRUE;
        }

        static EM_BOOL OnClose(int, const EmscriptenWebSocketCloseEvent*, void* user)
        {
            auto* impl = static_cast<Impl*>(user);
            impl->open = false;
            impl->failed = true;
            return EM_TRUE;
        }
    };

    WebSocketClientSocket::WebSocketClientSocket(IAllocator& allocator, StringView host, u16 port)
        : m_impl(MakeUnique<Impl>(allocator))
    {
        if (!emscripten_websocket_is_supported())
        {
            m_impl->failed = true;
            return;
        }
        char url[256];
        usize cursor = 0;
        const auto append = [&](StringView text)
        {
            for (usize i = 0; i < text.Size() && cursor + 1 < sizeof(url); ++i)
            {
                url[cursor++] = static_cast<char>(text[i]);
            }
        };
        append(u8"ws://");
        append(host);
        append(u8":");
        char portText[8];
        const int written = snprintf(portText, sizeof(portText), "%u",
                                     static_cast<unsigned>(port));
        for (int i = 0; i < written && cursor + 1 < sizeof(url); ++i)
        {
            url[cursor++] = portText[i];
        }
        url[cursor] = '\0';
        EmscriptenWebSocketCreateAttributes attributes = {url, nullptr, EM_TRUE};
        m_impl->socket = emscripten_websocket_new(&attributes);
        if (m_impl->socket <= 0)
        {
            m_impl->failed = true;
            return;
        }
        emscripten_websocket_set_onopen_callback(m_impl->socket, m_impl.Get(), Impl::OnOpen);
        emscripten_websocket_set_onmessage_callback(m_impl->socket, m_impl.Get(),
                                                    Impl::OnMessage);
        emscripten_websocket_set_onerror_callback(m_impl->socket, m_impl.Get(), Impl::OnError);
        emscripten_websocket_set_onclose_callback(m_impl->socket, m_impl.Get(), Impl::OnClose);
    }

    WebSocketClientSocket::~WebSocketClientSocket()
    {
        if (m_impl->socket > 0)
        {
            emscripten_websocket_close(m_impl->socket, 1000, "bye");
            emscripten_websocket_delete(m_impl->socket);
        }
    }

    bool WebSocketClientSocket::IsOpen() const noexcept
    {
        return m_impl->socket > 0 && !m_impl->failed;
    }

    void WebSocketClientSocket::Send(const DatagramEndpoint&, Span<const byte> data)
    {
        if (m_impl->failed || m_impl->socket <= 0)
        {
            return;
        }
        if (!m_impl->open)
        {
            Array<byte> copy;
            copy.Reserve(data.Size());
            for (byte b : data)
            {
                copy.PushBack(b);
            }
            m_impl->pendingSends.PushBack(static_cast<Array<byte>&&>(copy));
            return;
        }
        (void)emscripten_websocket_send_binary(m_impl->socket,
                                               const_cast<byte*>(data.Data()),
                                               static_cast<u32>(data.Size()));
    }

    bool WebSocketClientSocket::Receive(DatagramEndpoint& from, Array<byte>& out)
    {
        Impl& impl = *m_impl;
        if (impl.receivedHead >= impl.received.Size())
        {
            return false;
        }
        from = kWebSocketServerEndpoint;
        out = static_cast<Array<byte>&&>(impl.received[impl.receivedHead++]);
        if (impl.receivedHead >= impl.received.Size())
        {
            impl.received.Clear();
            impl.receivedHead = 0;
        }
        return true;
    }
#endif // PLATFORM_WEB
}
