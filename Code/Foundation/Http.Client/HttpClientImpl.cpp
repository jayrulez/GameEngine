// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.http.client - implementation: URLs, and the native client's worker.

module;
#include "Core/Prelude.h"

module foundation.http.client;

import foundation.core;
import foundation.http;
import foundation.net;
import foundation.net.tls;

using namespace foundation::core;

namespace foundation::http
{
    namespace
    {
        bool EqualsIgnoreCase(StringView a, StringView b) { return a.Size() == b.Size() && a.ContainsIgnoreCase(b); }

        void Append(Array<byte>& out, StringView text)
        {
            for (const utf8char c : text)
                out.PushBack(static_cast<byte>(c));
        }
    }

    bool ParseHttpUrl(StringView url, HttpUrl& out, String& error)
    {
        out = HttpUrl{};
        StringView rest;
        if (url.Size() >= 8 && EqualsIgnoreCase(url.SubStr(0, 8), u8"https://"))
        {
            out.secure = true;
            rest = url.SubStr(8, url.Size() - 8);
        }
        else if (url.Size() >= 7 && EqualsIgnoreCase(url.SubStr(0, 7), u8"http://"))
            rest = url.SubStr(7, url.Size() - 7);
        else
        {
            error = Format(u8"'{}' is not an http:// or https:// URL", url);
            return false;
        }
        usize end = 0;
        while (end < rest.Size() && rest[end] != u8'/' && rest[end] != u8'?' && rest[end] != u8'#')
            ++end;
        StringView authority = rest.SubStr(0, end);
        StringView target = rest.SubStr(end, rest.Size() - end);
        for (usize i = 0; i < target.Size(); ++i)
            if (target[i] == u8'#')
            {
                target = target.SubStr(0, i); // a fragment never goes to the server
                break;
            }
        for (usize i = authority.Size(); i > 0; --i)
            if (authority[i - 1] == u8'@')
            {
                authority = authority.SubStr(i, authority.Size() - i); // credentials are not sent
                break;
            }
        if (!authority.IsEmpty() && authority[0] == u8'[')
        {
            error = String(u8"an IPv6 address is not supported (the sockets are IPv4)");
            return false;
        }
        usize colon = authority.Size();
        for (usize i = 0; i < authority.Size(); ++i)
            if (authority[i] == u8':')
                colon = i;
        out.host = String(authority.SubStr(0, colon));
        if (out.host.IsEmpty())
        {
            error = Format(u8"'{}' names no host", url);
            return false;
        }
        out.port = out.secure ? 443 : 80;
        if (colon < authority.Size())
        {
            const StringView port = authority.SubStr(colon + 1, authority.Size() - colon - 1);
            u32 value = 0;
            for (const utf8char c : port)
            {
                if (c < u8'0' || c > u8'9' || value > 65535)
                {
                    error = Format(u8"'{}' has a bad port", url);
                    return false;
                }
                value = value * 10 + static_cast<u32>(c - u8'0');
            }
            if (port.IsEmpty() || value == 0 || value > 65535)
            {
                error = Format(u8"'{}' has a bad port", url);
                return false;
            }
            out.port = static_cast<u16>(value);
        }
        out.target = target.IsEmpty() ? String(u8"/") : (target[0] == u8'?' ? Format(u8"/{}", target) : String(target));
        return true;
    }

    String ResolveHttpLocation(const HttpUrl& base, StringView location)
    {
        const StringView scheme = base.secure ? StringView(u8"https") : StringView(u8"http");
        if ((location.Size() >= 7 && EqualsIgnoreCase(location.SubStr(0, 7), u8"http://")) ||
            (location.Size() >= 8 && EqualsIgnoreCase(location.SubStr(0, 8), u8"https://")))
            return String(location);
        if (location.StartsWith(u8"//"))
            return Format(u8"{}:{}", scheme, location);
        const bool defaultPort = base.port == (base.secure ? 443 : 80);
        const String origin = defaultPort ? Format(u8"{}://{}", scheme, base.host.AsView())
                                          : Format(u8"{}://{}:{}", scheme, base.host.AsView(), base.port);
        if (location.StartsWith(u8"/"))
            return Format(u8"{}{}", origin.AsView(), location);
        // Relative: against the base's directory.
        StringView path = base.target.AsView();
        for (usize i = 0; i < path.Size(); ++i)
            if (path[i] == u8'?')
            {
                path = path.SubStr(0, i);
                break;
            }
        usize slash = path.Size();
        while (slash > 0 && path[slash - 1] != u8'/')
            --slash;
        return Format(u8"{}{}{}", origin.AsView(), path.SubStr(0, slash), location);
    }

    namespace
    {
        struct Job
        {
            HttpRequestId id = 0;
            HttpClientRequest request;
            Function<void(const HttpClientResponse&)> done;
        };

        struct Delivery
        {
            HttpRequestId id = 0;
            HttpClientResponse response;
            Function<void(const HttpClientResponse&)> done;
        };

        class NativeHttpClient final : public IHttpClient
        {
        public:
            NativeHttpClient(IAllocator& allocator, foundation::net::ITlsBackend* tls, foundation::net::TlsTrust trust)
                : m_allocator(&allocator), m_tls(tls), m_trust(Move(trust))
            {
                m_worker = Thread([this]() { Work(); });
            }
            ~NativeHttpClient() override
            {
                {
                    ScopedLock lock(m_mutex);
                    m_stopping = true;
                    m_queue.Clear();
                }
                m_wake.NotifyAll();
                m_worker.Join();
            }
            NativeHttpClient(const NativeHttpClient&) = delete;
            NativeHttpClient& operator=(const NativeHttpClient&) = delete;

            HttpRequestId Send(HttpClientRequest request, Function<void(const HttpClientResponse&)> done) override
            {
                const HttpRequestId id = ++m_nextId;
                {
                    ScopedLock lock(m_mutex);
                    Job job;
                    job.id = id;
                    job.request = Move(request);
                    job.done = Move(done);
                    m_queue.PushBack(Move(job));
                    ++m_pending;
                }
                m_wake.NotifyOne();
                return id;
            }

            void Cancel(HttpRequestId id) override
            {
                ScopedLock lock(m_mutex);
                for (usize i = 0; i < m_queue.Size(); ++i)
                    if (m_queue[i].id == id)
                    {
                        m_queue.RemoveAt(i);
                        --m_pending;
                        return;
                    }
                if (m_current == id)
                    m_cancelCurrent = true; // the worker drops it at its next step
                for (usize i = 0; i < m_delivered.Size(); ++i)
                    if (m_delivered[i].id == id)
                    {
                        m_delivered.RemoveAt(i);
                        --m_pending;
                        return;
                    }
            }

            void Pump() override
            {
                Array<Delivery> ready;
                {
                    ScopedLock lock(m_mutex);
                    ready = Move(m_delivered);
                    m_delivered.Clear();
                    m_pending -= ready.Size();
                }
                for (Delivery& d : ready)
                    if (d.done)
                        d.done(d.response);
            }

            usize Pending() const override
            {
                ScopedLock lock(m_mutex);
                return m_pending;
            }

        private:
            void Work()
            {
                for (;;)
                {
                    Job job;
                    {
                        ScopedLock lock(m_mutex);
                        while (!m_stopping && m_queue.IsEmpty())
                            m_wake.Wait(m_mutex);
                        if (m_stopping)
                            return;
                        job = Move(m_queue[0]);
                        m_queue.RemoveAt(0);
                        m_current = job.id;
                        m_cancelCurrent = false;
                    }
                    HttpClientResponse response = Run(job.request);
                    ScopedLock lock(m_mutex);
                    const bool cancelled = m_cancelCurrent;
                    m_current = 0;
                    m_cancelCurrent = false;
                    if (cancelled)
                    {
                        --m_pending;
                        continue;
                    }
                    Delivery d;
                    d.id = job.id;
                    d.response = Move(response);
                    d.done = Move(job.done);
                    m_delivered.PushBack(Move(d));
                }
            }

            bool Cancelled()
            {
                ScopedLock lock(m_mutex);
                return m_cancelCurrent || m_stopping;
            }

            // One request to its end: each redirect a connection of its own.
            HttpClientResponse Run(const HttpClientRequest& request)
            {
                HttpClientResponse response;
                const Stopwatch clock = Stopwatch::StartNew();
                String url = request.url;
                String method = request.method;
                Array<byte> body = request.body;
                for (u32 hop = 0;; ++hop)
                {
                    HttpUrl at;
                    String error;
                    if (!ParseHttpUrl(url.AsView(), at, error))
                    {
                        response.error = error;
                        return response;
                    }
                    HttpMessageParser parser(HttpMessageParser::Mode::Response, request.maxResponseBytes);
                    if (EqualsIgnoreCase(method.AsView(), u8"HEAD"))
                        parser.SetNoBody();
                    if (!Exchange(at, method.AsView(), request.headers, body.AsSpan(), request, clock, parser, response.error))
                        return response;
                    const u16 status = static_cast<u16>(parser.Status());
                    const StringView location = FindHeader(parser.Headers(), u8"Location");
                    const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
                    if (redirect && request.maxRedirects > 0 && !location.IsEmpty())
                    {
                        if (hop >= request.maxRedirects)
                        {
                            response.error = Format(u8"too many redirects (more than {})", request.maxRedirects);
                            return response;
                        }
                        const String next = ResolveHttpLocation(at, location);
                        HttpUrl nextAt;
                        if (ParseHttpUrl(next.AsView(), nextAt, error) && at.secure && !nextAt.secure)
                        {
                            response.error = Format(u8"an https request was redirected to http ({}): refused", next.AsView());
                            return response;
                        }
                        // 303, and 301/302 after a POST (as browsers do): the next request is a GET.
                        if (status == 303 || ((status == 301 || status == 302) && EqualsIgnoreCase(method.AsView(), u8"POST")))
                        {
                            method = String(u8"GET");
                            body.Clear();
                        }
                        url = next;
                        continue;
                    }
                    response.status = status;
                    response.headers = parser.Headers();
                    response.body = Move(parser.Body());
                    response.url = url;
                    return response;
                }
            }

            // One connection: the request out, the response read into `parser`. False, `error`
            // saying why, when no whole response arrived.
            bool Exchange(const HttpUrl& at, StringView method, const Array<HttpHeader>& headers, Span<const byte> body,
                          const HttpClientRequest& request, const Stopwatch& clock, HttpMessageParser& parser, String& error)
            {
                using namespace foundation::net;
                if (at.secure && m_tls == nullptr)
                {
                    error = String(u8"https needs a TLS backend, and this client has none");
                    return false;
                }
                TcpSocket socket = TcpSocket::Connect(at.host.AsView(), at.port);
                if (!socket.IsOpen())
                {
                    error = Format(u8"'{}' could not be found", at.host.AsView());
                    return false;
                }
                UniquePtr<INetByteStream> stream;
                if (at.secure)
                {
                    TlsClientConfig config;
                    config.serverName = at.host;
                    config.trust = m_trust;
                    config.alpn.PushBack(String(u8"http/1.1"));
                    stream = MakeUnique<TlsStream>(*m_allocator, Move(socket), m_tls->CreateClient(config));
                }
                else
                    stream = MakeUnique<PlainStream>(*m_allocator, Move(socket));

                // The request: its line, Host, what this client always says, the caller's own.
                Array<byte> out;
                const bool defaultPort = at.port == (at.secure ? 443 : 80);
                Append(out, Format(u8"{} {} HTTP/1.1\r\n", method, at.target.AsView()).AsView());
                Append(out, (defaultPort ? Format(u8"Host: {}\r\n", at.host.AsView()) : Format(u8"Host: {}:{}\r\n", at.host.AsView(), at.port)).AsView());
                bool userAgent = false;
                for (const HttpHeader& h : headers)
                    userAgent = userAgent || EqualsIgnoreCase(h.name.AsView(), u8"User-Agent");
                if (!userAgent)
                    Append(out, u8"User-Agent: AssiduousEngine\r\n");
                Append(out, u8"Accept-Encoding: identity\r\nConnection: close\r\n");
                if (!body.IsEmpty() || EqualsIgnoreCase(method, u8"POST") || EqualsIgnoreCase(method, u8"PUT") || EqualsIgnoreCase(method, u8"PATCH"))
                    Append(out, Format(u8"Content-Length: {}\r\n", body.Size()).AsView());
                for (const HttpHeader& h : headers)
                {
                    if (EqualsIgnoreCase(h.name.AsView(), u8"Host") || EqualsIgnoreCase(h.name.AsView(), u8"Content-Length") ||
                        EqualsIgnoreCase(h.name.AsView(), u8"Connection"))
                        continue; // the client's own
                    Append(out, Format(u8"{}: {}\r\n", h.name.AsView(), h.value.AsView()).AsView());
                }
                Append(out, u8"\r\n");
                for (const byte b : body)
                    out.PushBack(b);
                if (stream->Send(out.AsSpan()) < 0)
                {
                    error = String(u8"the connection closed before the request went");
                    return false;
                }

                byte buffer[16 * 1024];
                for (;;)
                {
                    if (Cancelled())
                    {
                        error = String(u8"cancelled");
                        return false;
                    }
                    if (clock.Elapsed().AsSeconds() > static_cast<f64>(request.timeoutSeconds))
                    {
                        error = Format(u8"timed out after {} s", request.timeoutSeconds);
                        return false;
                    }
                    const bool alive = stream->Pump();
                    bool moved = false;
                    for (;;)
                    {
                        const i64 n = stream->Receive(Span<byte>(buffer, sizeof(buffer)));
                        if (n > 0)
                        {
                            moved = true;
                            (void)parser.Push(Span<const byte>(buffer, static_cast<usize>(n)));
                            if (parser.State() != HttpParseState::NeedMore)
                                break;
                            continue;
                        }
                        if (n < 0)
                            (void)parser.OnPeerClosed();
                        break;
                    }
                    if (parser.State() == HttpParseState::Complete)
                        return true;
                    if (parser.State() == HttpParseState::Failed)
                    {
                        error = parser.TooLarge()
                                    ? Format(u8"the response is larger than {} bytes", request.maxResponseBytes)
                                    : String(u8"the response could not be read (it ended early, or is not HTTP)");
                        return false;
                    }
                    if (!alive)
                    {
                        const StringView why = stream->Failure();
                        error = why.IsEmpty() ? String(u8"the connection closed before a response") : String(why);
                        return false;
                    }
                    if (!moved)
                        SleepMilliseconds(1);
                }
            }

            IAllocator* m_allocator;
            foundation::net::ITlsBackend* m_tls;
            foundation::net::TlsTrust m_trust;
            mutable Mutex m_mutex;
            ConditionVariable m_wake;
            Array<Job> m_queue;
            Array<Delivery> m_delivered;
            usize m_pending = 0;
            HttpRequestId m_current = 0;
            bool m_cancelCurrent = false;
            bool m_stopping = false;
            HttpRequestId m_nextId = 0;
            Thread m_worker; // last: it starts once the rest is made
        };
    }

    UniquePtr<IHttpClient> CreateNativeHttpClient(IAllocator& allocator, foundation::net::ITlsBackend* tls, foundation::net::TlsTrust trust)
    {
        return MakeUnique<NativeHttpClient>(allocator, allocator, tls, Move(trust));
    }
}
