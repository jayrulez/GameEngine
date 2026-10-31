// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Http - the `foundation.http` module.
//
// A small HTTP/1.1 layer over the Core/System TCP sockets (via foundation.net's
// TcpSocket/TcpListener): the substrate for the MCP streamable-HTTP host, the WebSocket
// upgrade path (a WebSocket connection begins as an HTTP request), and loopback tooling and
// tests. It is not a general web stack:
//   - HTTP/1.1 only, one request per connection (Connection: close), the subset the MCP
//     transport needs. No pipelining, no keep-alive, no chunked request bodies, no TLS
//     (localhost trust domain; auth is a bearer token at the consumer layer).
//   - Two exceptions to answer-and-close. A handler may answer with an event stream
//     (Server-Sent Events): the connection then stays open and the consumer writes events
//     through a ref-counted SseStream for as long as the peer listens. Or it may answer not
//     yet (an empty answer): the request stays on its connection, the peer keeps waiting, and
//     the server hands the same request to the handler again on every Pump until it answers
//     (for a handler that must let its host make progress, e.g. an MCP tool waiting on a
//     background cook). A response is only ever a response; "not yet" is the absence of one.
//   - The client is blocking with a timeout and localhost-focused (a host name or dotted
//     quad; no TLS): tests, local tooling and the MCP acceptance loop. It is not the
//     game-facing client (that is foundation.http.client).
//
// Threading: HttpServer is single-threaded; call Start, Pump and Stop from one thread (pump it
// from a dedicated thread or a frame loop). SseStream::WriteEvent is safe from any one writer
// thread at a time (the stream serializes writes internally against Close).

module;
#include "Core/Prelude.h"

export module foundation.http;

import foundation.core;
import foundation.net;

using namespace foundation::core;

export namespace foundation::http
{
    struct HttpHeader
    {
        String name;
        String value;
    };

    // Case-insensitive (ASCII) header lookup; empty view when absent.
    [[nodiscard]] StringView FindHeader(const Array<HttpHeader>& headers, StringView name);

    struct HttpRequest
    {
        String method; // "GET", "POST", ... (verbatim, upper-case per spec)
        String target; // origin-form target, verbatim ("/mcp", "/events?since=5")
        Array<HttpHeader> headers;
        Array<byte> body;
        /// The server's number for this request, unique for the server's life and never 0; the
        /// same every time a request answered not yet is handed back to the handler.
        u64 sequence = 0;

        [[nodiscard]] StringView Header(StringView name) const
        {
            return FindHeader(headers, name);
        }
        [[nodiscard]] StringView BodyText() const
        {
            return StringView(reinterpret_cast<const utf8char*>(body.Data()), body.Size());
        }
    };

    struct HttpResponse
    {
        i32 status = 200;
        Array<HttpHeader> headers; // Content-Length and Connection are written by the server
        Array<byte> body;
        bool eventStream = false; // true = SSE: headers go out, the connection stays open

        [[nodiscard]] StringView Header(StringView name) const
        {
            return FindHeader(headers, name);
        }
        [[nodiscard]] StringView BodyText() const
        {
            return StringView(reinterpret_cast<const utf8char*>(body.Data()), body.Size());
        }

        [[nodiscard]] static HttpResponse Text(i32 status, StringView contentType,
                                               StringView text);
        [[nodiscard]] static HttpResponse Json(i32 status, StringView jsonText);
        /// The SSE marker response: the server writes `text/event-stream` headers and hands
        /// the held connection to the stream handler instead of closing.
        [[nodiscard]] static HttpResponse EventStream();
    };

    [[nodiscard]] StringView HttpStatusText(i32 status) noexcept;

    // ---- incremental message parser ------------------------------------------------------

    enum class HttpParseState : u8
    {
        NeedMore, // feed more bytes
        Complete, // a full message is available
        Failed,   // malformed or over limits: close the connection (the server answers 400)
    };

    /// Incremental HTTP/1.1 message parser (request or response mode). Push bytes as they
    /// arrive; when Complete, the parsed pieces are readable. Strict subset: CRLF framing,
    /// 16 KiB header cap, caller body cap. A request's body is Content-Length only (chunked is
    /// Failed: the server's contract); a response's is Content-Length, chunked (the client reads
    /// what servers send), or read-until-close (caller calls OnPeerClosed). A response that
    /// carries no body (204, 304, 1xx, or one to a HEAD: SetNoBody) is Complete at its head.
    class HttpMessageParser
    {
    public:
        enum class Mode : u8
        {
            Request,
            Response
        };

        explicit HttpMessageParser(Mode mode, usize maxBodyBytes = 16 * 1024 * 1024)
            : m_mode(mode), m_maxBody(maxBodyBytes)
        {
        }

        [[nodiscard]] HttpParseState Push(Span<const byte> bytes);
        /// Response mode: the request was a HEAD, so the response has headers alone.
        void SetNoBody() noexcept { m_noBody = true; }
        /// Response mode: the peer closed, so a close-delimited body is complete.
        [[nodiscard]] HttpParseState OnPeerClosed();
        [[nodiscard]] HttpParseState State() const noexcept { return m_state; }
        /// Failed because the body is larger than the cap (not because it is malformed).
        [[nodiscard]] bool TooLarge() const noexcept { return m_tooLarge; }

        // Request mode results:
        [[nodiscard]] const String& Method() const noexcept { return m_method; }
        [[nodiscard]] const String& Target() const noexcept { return m_target; }
        // Response mode result:
        [[nodiscard]] i32 Status() const noexcept { return m_status; }
        // Both:
        [[nodiscard]] const Array<HttpHeader>& Headers() const noexcept { return m_headers; }
        [[nodiscard]] Array<byte>& Body() noexcept { return m_body; }

    private:
        [[nodiscard]] bool ParseHead();
        [[nodiscard]] HttpParseState DecodeChunks(); // the chunked bytes so far into m_body

        Mode m_mode;
        usize m_maxBody;
        HttpParseState m_state = HttpParseState::NeedMore;
        bool m_headParsed = false;
        bool m_bodyUntilClose = false;
        bool m_noBody = false;
        bool m_chunked = false;
        bool m_tooLarge = false;
        enum class Chunk : u8
        {
            Size,     // a chunk's size line next
            Data,     // m_chunkLeft bytes of data, then CRLF
            Trailers, // after the last (0) chunk: trailer lines to the blank one
        };
        Chunk m_chunk = Chunk::Size;
        usize m_chunkLeft = 0;
        Array<byte> m_chunkRaw; // encoded bytes not yet decoded
        usize m_bodyExpected = 0;
        Array<byte> m_buffer; // head accumulation (capped), then body bytes append to m_body
        String m_method;
        String m_target;
        i32 m_status = 0;
        Array<HttpHeader> m_headers;
        Array<byte> m_body;
    };

    // ---- server --------------------------------------------------------------------------

    /// A held Server-Sent Events connection. Ref-counted: the server keeps one ref (dropped
    /// when the peer disconnects or the server stops), the consumer keeps another and writes
    /// events until IsOpen() turns false. Writes after close are safe no-ops returning false.
    class SseStream final : public RefCounted
    {
    public:
        explicit SseStream(foundation::net::TcpSocket socket) : m_socket(Move(socket)) {}

        /// Send one event: `event: <name>` (omitted when empty), per-line `data:` fields and
        /// the blank-line terminator. False when the peer is gone (the stream closes itself).
        bool WriteEvent(StringView eventName, StringView data);
        /// A comment line (": <text>"), the SSE keep-alive idiom.
        bool WriteComment(StringView text);
        [[nodiscard]] bool IsOpen() const;
        /// Liveness probe: reads (and discards) any client chatter, so a closed peer is detected
        /// promptly. A write into a freshly closed socket can still succeed (TCP buffers it until
        /// the reset arrives), so sweeps poll this instead. False when the peer is gone (the
        /// stream closes itself).
        [[nodiscard]] bool PollLive();
        void Close();

    private:
        bool SendAll(Span<const byte> bytes);

        mutable Mutex m_mutex;
        foundation::net::TcpSocket m_socket;
    };

    struct HttpServerConfig
    {
        u16 port = 0; // 0 = OS-assigned (read BoundPort after Start)
        usize maxBodyBytes = 16 * 1024 * 1024;
        usize maxConnections = 32; // accepted-but-unanswered connections beyond this are refused
        /// Bind 127.0.0.1 alone, so nothing off this machine can connect. On by default: every
        /// server here is a local tool's (the editor's MCP host), whose bearer token should not
        /// be the only thing between it and the network.
        bool loopbackOnly = true;
    };

    /// The pump-model HTTP server. Start binds the listener; each Pump() accepts pending
    /// connections, reads, and for every complete request calls the handler and writes the
    /// response (Connection: close). For an EventStream() response it writes the SSE headers
    /// and hands the connection to the stream handler instead; when the handler answers not
    /// yet (an empty Optional), it keeps the request and calls the handler with it again next
    /// Pump (a peer that leaves while waiting is dropped). Malformed input answers 400 and
    /// closes; a missing handler answers 404. Single-threaded (see the module header).
    class HttpServer
    {
    public:
        // The allocator (required: the owner decides) backs the listener,
        // connections, and SSE streams.
        explicit HttpServer(IAllocator& allocator) noexcept : m_allocator(&allocator) {}
        ~HttpServer() { Stop(); }
        HttpServer(const HttpServer&) = delete;
        HttpServer& operator=(const HttpServer&) = delete;

        [[nodiscard]] bool Start(const HttpServerConfig& config);
        void Stop();
        [[nodiscard]] bool IsRunning() const noexcept { return m_listener.Get() != nullptr; }
        [[nodiscard]] u16 BoundPort() const noexcept;
        /// The bound IPv4 address in host order; 0 is every interface (or not running).
        [[nodiscard]] u32 BoundIp() const noexcept;

        /// The request handler: a one-shot response, the EventStream() marker, or not yet
        /// (an empty Optional), in which case the request stays pending on its connection and
        /// the handler sees it again on the next Pump. A handler that waits this way keeps its
        /// own progress state and its own timeout: a server that stops pumping and a
        /// handler that never answers look the same to the peer.
        void SetHandler(Function<Optional<HttpResponse>(const HttpRequest&)> handler)
        {
            m_handler = Move(handler);
        }
        /// Called when a handler answered EventStream(): the consumer takes its ref and
        /// writes events for as long as it likes.
        void SetStreamHandler(Function<void(const HttpRequest&, RefPtr<SseStream>)> handler)
        {
            m_streamHandler = Move(handler);
        }
        /// Told of a request answered not yet that will never be answered: its peer left while
        /// it waited, or the server stopped. A handler keeping state for it lets go.
        void SetAbandonHandler(Function<void(const HttpRequest&)> handler)
        {
            m_abandonHandler = Move(handler);
        }

        /// One pump: accept, read, dispatch and write. Returns the number of requests
        /// answered this call (0: nothing happened; callers may sleep briefly on 0). A
        /// request answered not yet counts when it is finally answered, not on the pumps it waits.
        usize Pump();

        /// Requests whose handler answered not yet and that are still waiting. A host whose
        /// loop sleeps when idle must keep pumping while this is non-zero.
        [[nodiscard]] usize PendingRequestCount() const noexcept;

    private:
        struct Connection
        {
            foundation::net::TcpSocket socket;
            HttpMessageParser parser{HttpMessageParser::Mode::Request};
            HttpRequest request;  // the parsed request, valid while `pending`
            bool pending = false; // a complete request awaits its answer (not yet given)
            Connection(foundation::net::TcpSocket s, usize maxBody)
                : socket(Move(s)), parser(HttpMessageParser::Mode::Request, maxBody)
            {
            }
        };

        /// Hands the connection's pending request to the handler. True when it was answered
        /// (or handed to a stream) and the connection is finished with; false when the
        /// handler answered not yet and the request stays pending.
        bool Dispatch(Connection& connection);
        /// A pending connection that will not be answered: the abandon handler is told.
        void NotifyAbandoned(const Connection& connection);
        static void WriteResponse(foundation::net::TcpSocket& socket, const HttpResponse& r);

        HttpServerConfig m_config;
        IAllocator* m_allocator;
        UniquePtr<foundation::net::TcpListener> m_listener;
        Array<UniquePtr<Connection>> m_connections;
        Array<RefPtr<SseStream>> m_streams; // server-side refs; swept when closed
        Function<Optional<HttpResponse>(const HttpRequest&)> m_handler;
        Function<void(const HttpRequest&, RefPtr<SseStream>)> m_streamHandler;
        Function<void(const HttpRequest&)> m_abandonHandler;
        u64 m_nextSequence = 0;
    };

    // ---- client --------------------------------------------------------------------------

    /// Blocking one-shot HTTP request against `address` (a host name or a dotted quad; no
    /// TLS: localhost tooling). Connects, sends, reads the full response (Content-Length or until
    /// close), enforcing `timeoutMilliseconds` across the whole exchange. The error is a
    /// human-readable reason (connect failed, timeout, malformed response).
    [[nodiscard]] Result<HttpResponse, String>
    HttpFetch(StringView address, u16 port, const HttpRequest& request,
              u32 timeoutMilliseconds = 5000);
}
