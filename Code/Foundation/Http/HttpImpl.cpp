// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Http - implementation unit (parser, server pump, SSE writes, blocking client).

module;
#include "Core/Prelude.h"
#include <cstring>

module foundation.http;

import foundation.core;
import foundation.net;

using namespace foundation::core;
namespace net = foundation::net;

namespace foundation::http
{
    namespace
    {
        inline constexpr usize kMaxHeadBytes = 16 * 1024;

        [[nodiscard]] bool AsciiEqualsIgnoreCase(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
            {
                return false;
            }
            for (usize i = 0; i < a.Size(); ++i)
            {
                utf8char ca = a[i];
                utf8char cb = b[i];
                if (ca >= u8'A' && ca <= u8'Z')
                {
                    ca = static_cast<utf8char>(ca + (u8'a' - u8'A'));
                }
                if (cb >= u8'A' && cb <= u8'Z')
                {
                    cb = static_cast<utf8char>(cb + (u8'a' - u8'A'));
                }
                if (ca != cb)
                {
                    return false;
                }
            }
            return true;
        }

        void AppendBytes(Array<byte>& out, const byte* data, usize size)
        {
            if (size == 0)
            {
                return;
            }
            const usize oldSize = out.Size();
            out.Resize(oldSize + size);
            std::memcpy(out.Data() + oldSize, data, size);
        }

        void AppendText(Array<byte>& out, StringView text)
        {
            AppendBytes(out, reinterpret_cast<const byte*>(text.Data()), text.Size());
        }

        // Digits-only parse; false on empty/garbage/overflowing values.
        [[nodiscard]] bool ParseUnsigned(StringView text, usize& out)
        {
            if (text.IsEmpty())
            {
                return false;
            }
            usize value = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                const utf8char c = text[i];
                if (c < u8'0' || c > u8'9')
                {
                    return false;
                }
                if (value > (~usize(0) - 9) / 10)
                {
                    return false;
                }
                value = value * 10 + static_cast<usize>(c - u8'0');
            }
            out = value;
            return true;
        }
    }

    StringView FindHeader(const Array<HttpHeader>& headers, StringView name)
    {
        for (const HttpHeader& h : headers)
        {
            if (AsciiEqualsIgnoreCase(h.name.AsView(), name))
            {
                return h.value.AsView();
            }
        }
        return {};
    }

    HttpResponse HttpResponse::Text(i32 status, StringView contentType, StringView text)
    {
        HttpResponse r;
        r.status = status;
        r.headers.PushBack(HttpHeader{String(u8"Content-Type"), String(contentType)});
        AppendText(r.body, text);
        return r;
    }

    HttpResponse HttpResponse::Json(i32 status, StringView jsonText)
    {
        return Text(status, u8"application/json", jsonText);
    }

    HttpResponse HttpResponse::EventStream()
    {
        HttpResponse r;
        r.status = 200;
        r.eventStream = true;
        return r;
    }

    StringView HttpStatusText(i32 status) noexcept
    {
        switch (status)
        {
        case 200: return u8"OK";
        case 202: return u8"Accepted";
        case 204: return u8"No Content";
        case 400: return u8"Bad Request";
        case 401: return u8"Unauthorized";
        case 404: return u8"Not Found";
        case 405: return u8"Method Not Allowed";
        case 413: return u8"Content Too Large";
        case 500: return u8"Internal Server Error";
        case 501: return u8"Not Implemented";
        case 503: return u8"Service Unavailable";
        default: return u8"Status";
        }
    }

    // ---- parser --------------------------------------------------------------------------

    bool HttpMessageParser::ParseHead()
    {
        const StringView head(reinterpret_cast<const utf8char*>(m_buffer.Data()),
                              m_buffer.Size());
        usize lineStart = 0;
        bool firstLine = true;
        while (lineStart < head.Size())
        {
            usize lineEnd = lineStart;
            while (lineEnd + 1 < head.Size() &&
                   !(head[lineEnd] == u8'\r' && head[lineEnd + 1] == u8'\n'))
            {
                ++lineEnd;
            }
            const StringView line = head.SubStr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 2;
            if (line.IsEmpty())
            {
                break;
            }
            if (firstLine)
            {
                firstLine = false;
                // Request:  METHOD SP TARGET SP HTTP/1.x
                // Response: HTTP/1.x SP STATUS SP REASON
                usize firstSpace = 0;
                while (firstSpace < line.Size() && line[firstSpace] != u8' ')
                {
                    ++firstSpace;
                }
                usize secondSpace = firstSpace + 1;
                while (secondSpace < line.Size() && line[secondSpace] != u8' ')
                {
                    ++secondSpace;
                }
                // Both separators must be present with something after them: "GET /path"
                // (no version) stops the second scan exactly at the end of the line, and the
                // version read below would then start one past it. A response may omit the
                // reason phrase ("HTTP/1.1 200"): only the request form needs the third part.
                if (firstSpace >= line.Size() ||
                    (m_mode == Mode::Request && secondSpace >= line.Size()))
                {
                    return false;
                }
                if (m_mode == Mode::Request)
                {
                    const StringView version =
                        line.SubStr(secondSpace + 1, line.Size() - secondSpace - 1);
                    if (!version.StartsWith(u8"HTTP/1."))
                    {
                        return false;
                    }
                    m_method = String(line.SubStr(0, firstSpace));
                    m_target =
                        String(line.SubStr(firstSpace + 1, secondSpace - firstSpace - 1));
                    if (m_method.IsEmpty() || m_target.IsEmpty())
                    {
                        return false;
                    }
                }
                else
                {
                    if (!line.StartsWith(u8"HTTP/1."))
                    {
                        return false;
                    }
                    usize statusValue = 0;
                    if (!ParseUnsigned(
                            line.SubStr(firstSpace + 1, secondSpace - firstSpace - 1),
                            statusValue) ||
                        statusValue > 999)
                    {
                        return false;
                    }
                    m_status = static_cast<i32>(statusValue);
                }
                continue;
            }
            // Header line: name ":" OWS value.
            usize colon = 0;
            while (colon < line.Size() && line[colon] != u8':')
            {
                ++colon;
            }
            if (colon == 0 || colon >= line.Size())
            {
                return false;
            }
            StringView value = line.SubStr(colon + 1, line.Size() - colon - 1);
            value = Trim(value);
            m_headers.PushBack(HttpHeader{String(line.SubStr(0, colon)), String(value)});
        }

        // A response with no body: complete at its head (Push sees m_noBody).
        if (m_mode == Mode::Response && (m_noBody || m_status == 204 || m_status == 304 || (m_status >= 100 && m_status < 200)))
        {
            m_noBody = true;
            return true;
        }
        // Body framing: Content-Length, or for a response chunked (what servers send); a chunked
        // request stays out of the server's subset.
        const StringView transfer = FindHeader(m_headers, u8"Transfer-Encoding");
        if (!transfer.IsEmpty())
        {
            if (m_mode != Mode::Response || !transfer.ContainsIgnoreCase(u8"chunked"))
            {
                return false;
            }
            m_chunked = true;
            return true;
        }
        const StringView contentLength = FindHeader(m_headers, u8"Content-Length");
        if (!contentLength.IsEmpty())
        {
            if (!ParseUnsigned(contentLength, m_bodyExpected))
            {
                return false;
            }
            if (m_bodyExpected > m_maxBody)
            {
                m_tooLarge = true;
                return false;
            }
        }
        else if (m_mode == Mode::Response)
        {
            m_bodyUntilClose = true; // close-delimited response body
        }
        return true;
    }

    HttpParseState HttpMessageParser::Push(Span<const byte> bytes)
    {
        if (m_state != HttpParseState::NeedMore)
        {
            return m_state;
        }
        usize offset = 0;
        if (!m_headParsed)
        {
            AppendBytes(m_buffer, bytes.Data(), bytes.Size());
            if (m_buffer.Size() > kMaxHeadBytes)
            {
                m_state = HttpParseState::Failed;
                return m_state;
            }
            // Find the CRLFCRLF head terminator.
            usize headEnd = 0;
            bool found = false;
            for (usize i = 0; i + 3 < m_buffer.Size(); ++i)
            {
                if (m_buffer[i] == byte{'\r'} && m_buffer[i + 1] == byte{'\n'} &&
                    m_buffer[i + 2] == byte{'\r'} && m_buffer[i + 3] == byte{'\n'})
                {
                    headEnd = i + 4;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return m_state; // NeedMore
            }
            const usize bodyStart = headEnd;
            const usize extra = m_buffer.Size() - bodyStart;
            Array<byte> spill;
            AppendBytes(spill, m_buffer.Data() + bodyStart, extra);
            m_buffer.Resize(headEnd);
            if (!ParseHead())
            {
                m_state = HttpParseState::Failed;
                return m_state;
            }
            m_headParsed = true;
            m_buffer.Clear();
            if (m_noBody)
            {
                m_state = HttpParseState::Complete;
                return m_state;
            }
            AppendBytes(m_chunked ? m_chunkRaw : m_body, spill.Data(), spill.Size());
            offset = bytes.Size(); // everything already consumed via m_buffer
        }
        else
        {
            AppendBytes(m_chunked ? m_chunkRaw : m_body, bytes.Data(), bytes.Size());
            offset = bytes.Size();
        }
        (void)offset;

        if (m_chunked)
        {
            return DecodeChunks();
        }

        if (m_bodyUntilClose)
        {
            if (m_body.Size() > m_maxBody)
            {
                m_tooLarge = true;
                m_state = HttpParseState::Failed;
            }
            return m_state; // NeedMore until the peer closes
        }
        if (m_body.Size() > m_maxBody)
        {
            m_tooLarge = true;
            m_state = HttpParseState::Failed;
            return m_state;
        }
        if (m_body.Size() >= m_bodyExpected)
        {
            m_body.Resize(m_bodyExpected); // drop any over-read (Connection: close model)
            m_state = HttpParseState::Complete;
        }
        return m_state;
    }

    HttpParseState HttpMessageParser::DecodeChunks()
    {
        usize at = 0;
        // The CRLF-ended line starting at `at`, or false when it has not all arrived.
        auto line = [&](StringView& out) -> bool
        {
            for (usize i = at; i + 1 < m_chunkRaw.Size(); ++i)
            {
                if (m_chunkRaw[i] == byte{'\r'} && m_chunkRaw[i + 1] == byte{'\n'})
                {
                    out = StringView(reinterpret_cast<const utf8char*>(m_chunkRaw.Data() + at), i - at);
                    at = i + 2;
                    return true;
                }
            }
            return false;
        };
        while (m_state == HttpParseState::NeedMore)
        {
            if (m_chunk == Chunk::Size)
            {
                StringView sizeLine;
                if (!line(sizeLine))
                {
                    break;
                }
                // Hex digits, then any extension after ';' (ignored).
                usize size = 0;
                usize digits = 0;
                for (usize i = 0; i < sizeLine.Size() && sizeLine[i] != u8';'; ++i)
                {
                    const utf8char c = sizeLine[i];
                    u32 v = 0;
                    if (c >= u8'0' && c <= u8'9')
                        v = static_cast<u32>(c - u8'0');
                    else if (c >= u8'a' && c <= u8'f')
                        v = static_cast<u32>(c - u8'a' + 10);
                    else if (c >= u8'A' && c <= u8'F')
                        v = static_cast<u32>(c - u8'A' + 10);
                    else if (c == u8' ' || c == u8'\t')
                        continue;
                    else
                    {
                        m_state = HttpParseState::Failed;
                        return m_state;
                    }
                    if (++digits > 15)
                    {
                        m_state = HttpParseState::Failed; // a size no body here reaches
                        return m_state;
                    }
                    size = size * 16 + v;
                }
                if (digits == 0)
                {
                    m_state = HttpParseState::Failed;
                    return m_state;
                }
                if (size == 0)
                {
                    m_chunk = Chunk::Trailers;
                    continue;
                }
                if (m_body.Size() + size > m_maxBody)
                {
                    m_tooLarge = true;
                    m_state = HttpParseState::Failed;
                    return m_state;
                }
                m_chunk = Chunk::Data;
                m_chunkLeft = size;
            }
            else if (m_chunk == Chunk::Data)
            {
                if (m_chunkRaw.Size() - at < m_chunkLeft + 2)
                {
                    break; // the chunk and its CRLF not all here yet
                }
                AppendBytes(m_body, m_chunkRaw.Data() + at, m_chunkLeft);
                at += m_chunkLeft;
                if (m_chunkRaw[at] != byte{'\r'} || m_chunkRaw[at + 1] != byte{'\n'})
                {
                    m_state = HttpParseState::Failed;
                    return m_state;
                }
                at += 2;
                m_chunk = Chunk::Size;
            }
            else
            {
                StringView trailer;
                if (!line(trailer))
                {
                    break;
                }
                if (trailer.IsEmpty())
                {
                    m_state = HttpParseState::Complete; // trailers themselves are dropped
                }
            }
        }
        // Keep what is not decoded yet.
        if (at > 0)
        {
            Array<byte> rest;
            AppendBytes(rest, m_chunkRaw.Data() + at, m_chunkRaw.Size() - at);
            m_chunkRaw = Move(rest);
        }
        if (m_chunkRaw.Size() > kMaxHeadBytes + m_maxBody)
        {
            m_state = HttpParseState::Failed;
        }
        return m_state;
    }

    HttpParseState HttpMessageParser::OnPeerClosed()
    {
        if (m_state != HttpParseState::NeedMore)
        {
            return m_state;
        }
        if (m_headParsed && m_bodyUntilClose)
        {
            m_state = HttpParseState::Complete;
        }
        else
        {
            m_state = HttpParseState::Failed; // closed mid-message
        }
        return m_state;
    }

    // ---- SSE stream ----------------------------------------------------------------------

    bool SseStream::SendAll(Span<const byte> bytes)
    {
        usize sent = 0;
        u32 patience = 2000; // ~2 s of 1 ms would-block waits (localhost: effectively never)
        while (sent < bytes.Size())
        {
            const i64 n = m_socket.Send(
                Span<const byte>(bytes.Data() + sent, bytes.Size() - sent));
            if (n < 0)
            {
                m_socket.Close();
                return false;
            }
            if (n == 0)
            {
                if (patience-- == 0)
                {
                    m_socket.Close();
                    return false;
                }
                SleepMilliseconds(1);
                continue;
            }
            sent += static_cast<usize>(n);
        }
        return true;
    }

    bool SseStream::WriteEvent(StringView eventName, StringView data)
    {
        ScopedLock lock(m_mutex);
        if (!m_socket.IsOpen())
        {
            return false;
        }
        Array<byte> out;
        if (!eventName.IsEmpty())
        {
            AppendText(out, u8"event: ");
            AppendText(out, eventName);
            AppendText(out, u8"\n");
        }
        // Multi-line data becomes one `data:` field per line (the SSE framing rule).
        usize lineStart = 0;
        for (usize i = 0; i <= data.Size(); ++i)
        {
            if (i == data.Size() || data[i] == u8'\n')
            {
                AppendText(out, u8"data: ");
                AppendText(out, data.SubStr(lineStart, i - lineStart));
                AppendText(out, u8"\n");
                lineStart = i + 1;
            }
        }
        AppendText(out, u8"\n");
        return SendAll(Span<const byte>(out.Data(), out.Size()));
    }

    bool SseStream::WriteComment(StringView text)
    {
        ScopedLock lock(m_mutex);
        if (!m_socket.IsOpen())
        {
            return false;
        }
        Array<byte> out;
        AppendText(out, u8": ");
        AppendText(out, text);
        AppendText(out, u8"\n\n");
        return SendAll(Span<const byte>(out.Data(), out.Size()));
    }

    bool SseStream::IsOpen() const
    {
        ScopedLock lock(m_mutex);
        return m_socket.IsOpen();
    }

    bool SseStream::PollLive()
    {
        ScopedLock lock(m_mutex);
        if (!m_socket.IsOpen())
        {
            return false;
        }
        byte buffer[256];
        for (;;)
        {
            const i64 n = m_socket.Receive(Span<byte>(buffer, sizeof(buffer)));
            if (n == 0)
            {
                return true; // would-block: the peer is quiet but alive
            }
            if (n < 0)
            {
                m_socket.Close(); // closed or errored: the stream is dead
                return false;
            }
            // n > 0: an SSE client should not send; discard and keep probing.
        }
    }

    void SseStream::Close()
    {
        ScopedLock lock(m_mutex);
        m_socket.Close();
    }

    // ---- server --------------------------------------------------------------------------

    bool HttpServer::Start(const HttpServerConfig& config)
    {
        Stop();
        m_config = config;
        m_listener = MakeUnique<net::TcpListener>(*m_allocator, config.port, config.loopbackOnly);
        if (!m_listener->IsOpen())
        {
            m_listener = nullptr;
            return false;
        }
        return true;
    }

    void HttpServer::Stop()
    {
        for (const UniquePtr<Connection>& connection : m_connections)
        {
            NotifyAbandoned(*connection);
        }
        m_connections.Clear();
        for (RefPtr<SseStream>& stream : m_streams)
        {
            stream->Close();
        }
        m_streams.Clear();
        m_listener = nullptr;
    }

    u16 HttpServer::BoundPort() const noexcept
    {
        return m_listener.Get() != nullptr ? m_listener->BoundPort() : 0;
    }

    u32 HttpServer::BoundIp() const noexcept
    {
        return m_listener.Get() != nullptr ? m_listener->BoundIp() : 0;
    }

    void HttpServer::WriteResponse(net::TcpSocket& socket, const HttpResponse& r)
    {
        StringBuilder head;
        head.Append(u8"HTTP/1.1 ");
        head.Append(Format(u8"{}", r.status).AsView());
        head.Append(u8' ');
        head.Append(HttpStatusText(r.status));
        head.Append(u8"\r\n");
        for (const HttpHeader& h : r.headers)
        {
            head.Append(h.name.AsView());
            head.Append(u8": ");
            head.Append(h.value.AsView());
            head.Append(u8"\r\n");
        }
        head.Append(Format(u8"Content-Length: {}\r\n", r.body.Size()).AsView());
        head.Append(u8"Connection: close\r\n\r\n");
        const String headText = head.Take();

        Array<byte> wire;
        AppendText(wire, headText.AsView());
        AppendBytes(wire, r.body.Data(), r.body.Size());
        usize sent = 0;
        u32 patience = 2000;
        while (sent < wire.Size())
        {
            const i64 n =
                socket.Send(Span<const byte>(wire.Data() + sent, wire.Size() - sent));
            if (n < 0)
            {
                return;
            }
            if (n == 0)
            {
                if (patience-- == 0)
                {
                    return;
                }
                SleepMilliseconds(1);
                continue;
            }
            sent += static_cast<usize>(n);
        }
    }

    bool HttpServer::Dispatch(Connection& connection)
    {
        const HttpRequest& request = connection.request;
        Optional<HttpResponse> answer =
            m_handler ? m_handler(request)
                      : Optional<HttpResponse>(
                            HttpResponse::Text(404, u8"text/plain", u8"no handler registered"));
        if (!answer.HasValue())
        {
            return false; // not yet: the handler sees the same request next Pump
        }
        connection.pending = false;
        const HttpResponse& response = answer.Value();
        if (response.eventStream)
        {
            // SSE: write the stream headers, then hand the held connection over.
            StringBuilder head;
            head.Append(u8"HTTP/1.1 200 OK\r\n"
                        u8"Content-Type: text/event-stream\r\n"
                        u8"Cache-Control: no-store\r\n"
                        u8"Connection: close\r\n\r\n");
            const String headText = head.Take();
            const i64 sent = connection.socket.Send(Span<const byte>(
                reinterpret_cast<const byte*>(headText.Data()), headText.Size()));
            if (sent == static_cast<i64>(headText.Size()))
            {
                RefPtr<SseStream> stream =
                    MakeRef<SseStream>(*m_allocator, Move(connection.socket));
                m_streams.PushBack(stream);
                if (m_streamHandler)
                {
                    m_streamHandler(request, stream);
                }
            }
            return true;
        }
        WriteResponse(connection.socket, response);
        return true;
    }

    void HttpServer::NotifyAbandoned(const Connection& connection)
    {
        if (connection.pending && m_abandonHandler)
        {
            m_abandonHandler(connection.request);
        }
    }

    usize HttpServer::PendingRequestCount() const noexcept
    {
        usize pending = 0;
        for (const UniquePtr<Connection>& connection : m_connections)
        {
            if (connection->pending)
            {
                ++pending;
            }
        }
        return pending;
    }

    usize HttpServer::Pump()
    {
        if (m_listener.Get() == nullptr)
        {
            return 0;
        }
        // Accept everything pending (refusing beyond the connection cap by dropping).
        for (;;)
        {
            net::TcpSocket accepted = m_listener->Accept();
            if (!accepted.IsOpen())
            {
                break;
            }
            if (m_connections.Size() >= m_config.maxConnections)
            {
                continue; // the socket closes on scope exit: refused
            }
            m_connections.PushBack(MakeUnique<Connection>(*m_allocator, Move(accepted),
                                                          m_config.maxBodyBytes));
        }

        usize completed = 0;
        for (usize i = 0; i < m_connections.Size();)
        {
            Connection& connection = *m_connections[i];
            bool done = false;
            byte buffer[4096];
            if (connection.pending)
            {
                // A request answered "not yet". The peer has nothing more to say on a one-shot
                // connection, so the read only probes whether it is still there; then the
                // handler is asked again.
                if (connection.socket.Receive(Span<byte>(buffer, sizeof(buffer))) < 0)
                {
                    NotifyAbandoned(connection);
                    done = true; // the peer left while waiting: nobody to answer
                }
                else if (Dispatch(connection))
                {
                    ++completed;
                    done = true;
                }
            }
            else
            {
                for (;;)
                {
                    const i64 n =
                        connection.socket.Receive(Span<byte>(buffer, sizeof(buffer)));
                    if (n > 0)
                    {
                        const HttpParseState state = connection.parser.Push(
                            Span<const byte>(buffer, static_cast<usize>(n)));
                        if (state == HttpParseState::Complete)
                        {
                            connection.request.method = connection.parser.Method();
                            connection.request.target = connection.parser.Target();
                            connection.request.headers = connection.parser.Headers();
                            connection.request.body = Move(connection.parser.Body());
                            connection.request.sequence = ++m_nextSequence;
                            connection.pending = true;
                            if (Dispatch(connection))
                            {
                                ++completed;
                                done = true;
                            }
                            break;
                        }
                        if (state == HttpParseState::Failed)
                        {
                            WriteResponse(connection.socket,
                                          HttpResponse::Text(400, u8"text/plain",
                                                             u8"malformed HTTP request"));
                            done = true;
                            break;
                        }
                        continue;
                    }
                    if (n == 0)
                    {
                        break; // would-block: keep the connection, try next pump
                    }
                    done = true; // peer closed before a complete request
                    break;
                }
            }
            if (done)
            {
                m_connections.RemoveAt(i);
            }
            else
            {
                ++i;
            }
        }

        // Sweep dead event streams, polled for liveness: a write into a freshly closed peer can
        // still succeed, so IsOpen alone lags. The consumer's ref stays valid; its writes no-op.
        for (usize i = 0; i < m_streams.Size();)
        {
            if (!m_streams[i]->PollLive())
            {
                m_streams.RemoveAt(i);
            }
            else
            {
                ++i;
            }
        }
        return completed;
    }

    // ---- client --------------------------------------------------------------------------

    Result<HttpResponse, String> HttpFetch(StringView address, u16 port,
                                           const HttpRequest& request,
                                           u32 timeoutMilliseconds)
    {
        net::TcpSocket socket = net::TcpSocket::Connect(address, port);
        if (!socket.IsOpen())
        {
            return Err(Format(u8"could not open a socket to {}:{}", address, port));
        }
        u32 budget = timeoutMilliseconds;
        for (;;)
        {
            const int status = socket.ConnectStatus();
            if (status > 0)
            {
                break;
            }
            if (status < 0)
            {
                return Err(Format(u8"connect to {}:{} failed", address, port));
            }
            if (budget-- == 0)
            {
                return Err(Format(u8"connect to {}:{} timed out", address, port));
            }
            SleepMilliseconds(1);
        }

        // Request wire form.
        StringBuilder head;
        head.Append(request.method.IsEmpty() ? StringView(u8"GET") : request.method.AsView());
        head.Append(u8' ');
        head.Append(request.target.IsEmpty() ? StringView(u8"/") : request.target.AsView());
        head.Append(u8" HTTP/1.1\r\nHost: ");
        head.Append(address);
        head.Append(u8"\r\n");
        for (const HttpHeader& h : request.headers)
        {
            head.Append(h.name.AsView());
            head.Append(u8": ");
            head.Append(h.value.AsView());
            head.Append(u8"\r\n");
        }
        head.Append(Format(u8"Content-Length: {}\r\n", request.body.Size()).AsView());
        head.Append(u8"Connection: close\r\n\r\n");
        const String headText = head.Take();

        Array<byte> wire;
        AppendText(wire, headText.AsView());
        AppendBytes(wire, request.body.Data(), request.body.Size());
        usize sent = 0;
        while (sent < wire.Size())
        {
            const i64 n =
                socket.Send(Span<const byte>(wire.Data() + sent, wire.Size() - sent));
            if (n < 0)
            {
                return Err(String(u8"connection closed while sending the request"));
            }
            if (n == 0)
            {
                if (budget-- == 0)
                {
                    return Err(String(u8"request send timed out"));
                }
                SleepMilliseconds(1);
                continue;
            }
            sent += static_cast<usize>(n);
        }

        // Read the response until Complete (Content-Length) or the peer closes.
        HttpMessageParser parser(HttpMessageParser::Mode::Response);
        byte buffer[4096];
        for (;;)
        {
            const i64 n = socket.Receive(Span<byte>(buffer, sizeof(buffer)));
            if (n > 0)
            {
                const HttpParseState state =
                    parser.Push(Span<const byte>(buffer, static_cast<usize>(n)));
                if (state == HttpParseState::Complete)
                {
                    break;
                }
                if (state == HttpParseState::Failed)
                {
                    return Err(String(u8"malformed HTTP response"));
                }
                continue;
            }
            if (n == 0)
            {
                if (budget-- == 0)
                {
                    return Err(String(u8"response read timed out"));
                }
                SleepMilliseconds(1);
                continue;
            }
            if (parser.OnPeerClosed() == HttpParseState::Complete)
            {
                break;
            }
            return Err(String(u8"connection closed mid-response"));
        }

        HttpResponse response;
        response.status = parser.Status();
        response.headers = parser.Headers();
        response.body = Move(parser.Body());
        return response;
    }
}
