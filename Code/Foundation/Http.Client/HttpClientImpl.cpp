// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.http.client - implementation: URLs, the native client's worker, and the browser
// client over fetch.

module;
#include "Core/Prelude.h"
#if PLATFORM_WEB
#include <emscripten/fetch.h>
#endif

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
        struct Delivery
        {
            HttpRequestId id = 0;
            HttpClientResponse response;
            Function<void(const HttpClientResponse&)> done;
        };
    }

#if !PLATFORM_WEB
    namespace
    {
        void Append(Array<byte>& out, StringView text)
        {
            for (const utf8char c : text)
                out.PushBack(static_cast<byte>(c));
        }

        struct Job
        {
            HttpRequestId id = 0;
            HttpClientRequest request;
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

    UniquePtr<IHttpClient> CreateHttpClient(IAllocator& allocator, foundation::net::ITlsBackend* tls, foundation::net::TlsTrust trust)
    {
        return CreateNativeHttpClient(allocator, tls, Move(trust));
    }
#else
    namespace
    {
        // The browser's fetch (emscripten's, over XMLHttpRequest). Without pthreads its callbacks
        // run on this thread between frames; each finished request waits for Pump.
        class BrowserHttpClient final : public IHttpClient
        {
        public:
            explicit BrowserHttpClient(IAllocator& allocator) : m_allocator(&allocator) {}
            ~BrowserHttpClient() override
            {
                for (UniquePtr<Flight>& f : m_flights)
                    Abort(*f);
            }
            BrowserHttpClient(const BrowserHttpClient&) = delete;
            BrowserHttpClient& operator=(const BrowserHttpClient&) = delete;

            HttpRequestId Send(HttpClientRequest request, Function<void(const HttpClientResponse&)> done) override
            {
                const HttpRequestId id = ++m_nextId;
                UniquePtr<Flight> f = MakeUnique<Flight>(*m_allocator);
                f->owner = this;
                f->id = id;
                f->request = Move(request);
                f->done = Move(done);
                f->clock = Stopwatch::StartNew();

                HttpUrl at;
                String error;
                if (!ParseHttpUrl(f->request.url.AsView(), at, error))
                {
                    Deliver(*f, Failed(Move(error)));
                    return id;
                }

                emscripten_fetch_attr_t attr;
                emscripten_fetch_attr_init(&attr);
                const String method = f->request.method;
                const usize length = Min(method.Size(), sizeof(attr.requestMethod) - 1);
                for (usize i = 0; i < length; ++i)
                    attr.requestMethod[i] = static_cast<char>(method[i]);
                attr.requestMethod[length] = '\0';
                attr.userData = f.Get();
                attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
                attr.timeoutMSecs = static_cast<u32>(f->request.timeoutSeconds * 1000.0f);
                attr.onsuccess = &BrowserHttpClient::OnDone;
                attr.onerror = &BrowserHttpClient::OnDone;
                // The caller's headers, less those a browser sets itself and refuses from a page.
                for (const HttpHeader& h : f->request.headers)
                {
                    const StringView name = h.name.AsView();
                    if (EqualsIgnoreCase(name, u8"Host") || EqualsIgnoreCase(name, u8"Content-Length") ||
                        EqualsIgnoreCase(name, u8"Connection") || EqualsIgnoreCase(name, u8"User-Agent") ||
                        EqualsIgnoreCase(name, u8"Accept-Encoding"))
                        continue;
                    f->headerText.PushBack(h.name);
                    f->headerText.PushBack(h.value);
                }
                for (const String& text : f->headerText)
                    f->headerPointers.PushBack(reinterpret_cast<const char*>(text.CStr()));
                f->headerPointers.PushBack(nullptr);
                attr.requestHeaders = f->headerPointers.Data();
                if (!f->request.body.IsEmpty())
                {
                    attr.requestData = reinterpret_cast<const char*>(f->request.body.Data());
                    attr.requestDataSize = f->request.body.Size();
                }
                Flight* flight = f.Get();
                m_flights.PushBack(Move(f));
                emscripten_fetch_t* fetch = emscripten_fetch(&attr, reinterpret_cast<const char*>(flight->request.url.CStr()));
                // A callback may already have answered it (and freed the flight): look it up again.
                for (UniquePtr<Flight>& live : m_flights)
                    if (live->id == id && !live->finished)
                    {
                        live->fetch = fetch;
                        if (fetch == nullptr)
                        {
                            live->finished = true;
                            Finish(*live, Failed(String(u8"the browser refused the request")));
                        }
                        break;
                    }
                return id;
            }

            void Cancel(HttpRequestId id) override
            {
                for (usize i = 0; i < m_flights.Size(); ++i)
                    if (m_flights[i]->id == id)
                    {
                        Abort(*m_flights[i]);
                        m_flights.RemoveAt(i);
                        return;
                    }
                for (usize i = 0; i < m_delivered.Size(); ++i)
                    if (m_delivered[i].id == id)
                    {
                        m_delivered.RemoveAt(i);
                        return;
                    }
            }

            void Pump() override
            {
                Array<Delivery> ready = Move(m_delivered);
                m_delivered.Clear();
                for (Delivery& d : ready)
                    if (d.done)
                        d.done(d.response);
            }

            usize Pending() const override { return m_flights.Size() + m_delivered.Size(); }

        private:
            struct Flight
            {
                BrowserHttpClient* owner = nullptr;
                HttpRequestId id = 0;
                HttpClientRequest request; // its body and headers live as long as the fetch
                Function<void(const HttpClientResponse&)> done;
                Stopwatch clock;
                Array<String> headerText;
                Array<const char*> headerPointers;
                emscripten_fetch_t* fetch = nullptr;
                bool finished = false; // answered or cancelled: later callbacks are ignored
            };

            static HttpClientResponse Failed(String why)
            {
                HttpClientResponse r;
                r.error = Move(why);
                return r;
            }

            // A fetch still running is closed, its error callback (called by the close) ignored.
            void Abort(Flight& f)
            {
                if (f.finished)
                    return;
                f.finished = true;
                if (f.fetch != nullptr)
                    (void)emscripten_fetch_close(f.fetch);
                f.fetch = nullptr;
            }

            static void OnDone(emscripten_fetch_t* fetch)
            {
                auto* f = static_cast<Flight*>(fetch->userData);
                if (f->finished)
                    return; // the close of a cancelled or answered fetch
                f->finished = true;
                HttpClientResponse response = Read(*f, fetch);
                f->fetch = nullptr;
                (void)emscripten_fetch_close(fetch); // may call back here: ignored above, f still alive
                f->owner->Finish(*f, Move(response)); // frees f
            }

            static HttpClientResponse Read(const Flight& f, emscripten_fetch_t* fetch)
            {
                HttpClientResponse r;
                // A status of 0 is no response: the network, a timeout, or a cross-origin refusal
                // (CORS; the browser keeps the reason from the page).
                if (fetch->status == 0)
                {
                    const f32 elapsed = f.clock.Elapsed().AsSecondsF();
                    r.error = elapsed >= f.request.timeoutSeconds - 0.05f
                                  ? Format(u8"timed out after {} s", f.request.timeoutSeconds)
                                  : String(u8"the request failed (the network, or a cross-origin request the server's CORS headers do not allow)");
                    return r;
                }
                if (fetch->numBytes > f.request.maxResponseBytes)
                {
                    r.error = Format(u8"the response is larger than {} bytes", f.request.maxResponseBytes);
                    return r;
                }
                r.status = fetch->status;
                r.url = f.request.url;
                r.body.Resize(static_cast<usize>(fetch->numBytes));
                for (usize i = 0; i < r.body.Size(); ++i)
                    r.body[i] = static_cast<byte>(fetch->data[i]);
                const size_t length = emscripten_fetch_get_response_headers_length(fetch);
                if (length > 0)
                {
                    Array<char> text;
                    text.Resize(length + 1);
                    (void)emscripten_fetch_get_response_headers(fetch, text.Data(), length + 1);
                    char** pairs = emscripten_fetch_unpack_response_headers(text.Data());
                    for (char** p = pairs; p != nullptr && p[0] != nullptr && p[1] != nullptr; p += 2)
                    {
                        StringView value(reinterpret_cast<const utf8char*>(p[1]));
                        while (!value.IsEmpty() && (value[0] == u8' ' || value[0] == u8'\t'))
                            value = value.SubStr(1, value.Size() - 1);
                        while (!value.IsEmpty() && (value[value.Size() - 1] == u8'\r' || value[value.Size() - 1] == u8'\n' ||
                                                    value[value.Size() - 1] == u8' '))
                            value = value.SubStr(0, value.Size() - 1);
                        r.headers.PushBack(HttpHeader{String(StringView(reinterpret_cast<const utf8char*>(p[0]))), String(value)});
                    }
                    emscripten_fetch_free_unpacked_response_headers(pairs);
                }
                return r;
            }

            // The answer queued for Pump, the flight gone.
            void Finish(Flight& f, HttpClientResponse response)
            {
                Delivery d;
                d.id = f.id;
                d.response = Move(response);
                d.done = Move(f.done);
                m_delivered.PushBack(Move(d));
                for (usize i = 0; i < m_flights.Size(); ++i)
                    if (m_flights[i].Get() == &f)
                    {
                        m_flights.RemoveAt(i); // f is freed here; nothing touches it after
                        return;
                    }
            }

            // Failed before any fetch began: queued for Pump like any answer.
            void Deliver(Flight& f, HttpClientResponse response)
            {
                Delivery d;
                d.id = f.id;
                d.response = Move(response);
                d.done = Move(f.done);
                m_delivered.PushBack(Move(d));
            }

            IAllocator* m_allocator;
            Array<UniquePtr<Flight>> m_flights;
            Array<Delivery> m_delivered;
            HttpRequestId m_nextId = 0;
        };
    }

    UniquePtr<IHttpClient> CreateBrowserHttpClient(IAllocator& allocator) { return MakeUnique<BrowserHttpClient>(allocator, allocator); }

    UniquePtr<IHttpClient> CreateHttpClient(IAllocator& allocator, foundation::net::ITlsBackend*, foundation::net::TlsTrust)
    {
        return CreateBrowserHttpClient(allocator);
    }
#endif
}
