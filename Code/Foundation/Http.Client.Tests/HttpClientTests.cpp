// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The native HTTP client against real loopback servers: the engine's HttpServer for GET, POST,
// redirects (followed, a POST turned GET by 303, a loop stopped), a timeout, a cancel and the size cap; a raw server for a chunked response, and for
// https through the Mbed TLS backend with a self-signed certificate (trusted, and an https
// request redirected to http refused). Also URL parsing and redirect resolution alone, the only
// part a wasm build compiles (its client is the browser's: Http.Client.WebCheck checks it by hand).
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.net.tls;
import foundation.net.tls.mbedtls;
import foundation.http;
import foundation.http.client;

using namespace foundation::core;
using namespace foundation::http;
using namespace foundation::net;

#if !PLATFORM_WEB
namespace
{
    Span<const byte> Bytes(StringView text) { return Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()); }

    doctest::String C(const String& s) { return doctest::String(reinterpret_cast<const char*>(s.CStr())); }

    // A server that answers each connection once, with what `answer` makes of the request's
    // head, then closes; plain or TLS. Pumped on the test thread.
    class RawServer
    {
    public:
        RawServer(Function<String(StringView head)> answer, ITlsBackend* tls = nullptr, const TlsCertificate* cert = nullptr)
            : m_listener(0, /*loopbackOnly*/ true), m_answer(Move(answer)), m_tls(tls), m_cert(cert)
        {
        }
        [[nodiscard]] u16 Port() const { return m_listener.BoundPort(); }

        void Pump()
        {
            for (TcpSocket socket = m_listener.Accept(); socket.IsOpen(); socket = m_listener.Accept())
            {
                Connection c;
                if (m_tls != nullptr)
                {
                    TlsServerConfig config;
                    config.certificateChainPem = m_cert->certificatePem;
                    config.privateKeyPem = m_cert->privateKeyPem;
                    c.stream = MakeUnique<TlsStream>(DefaultAllocator(), Move(socket), m_tls->CreateServer(config));
                }
                else
                    c.stream = MakeUnique<PlainStream>(DefaultAllocator(), Move(socket));
                m_connections.PushBack(Move(c));
            }
            for (Connection& c : m_connections)
            {
                if (c.answered)
                    continue;
                (void)c.stream->Pump();
                byte buffer[4096];
                for (i64 n = c.stream->Receive(Span<byte>(buffer, sizeof(buffer))); n > 0; n = c.stream->Receive(Span<byte>(buffer, sizeof(buffer))))
                    c.request += StringView(reinterpret_cast<const utf8char*>(buffer), static_cast<usize>(n));
                if (c.request.AsView().ContainsIgnoreCase(u8"\r\n\r\n"))
                {
                    const String reply = m_answer(c.request.AsView());
                    (void)c.stream->Send(Bytes(reply.AsView()));
                    (void)c.stream->Pump();
                    c.stream->Close();
                    c.answered = true;
                }
            }
        }

    private:
        struct Connection
        {
            UniquePtr<INetByteStream> stream;
            String request;
            bool answered = false;
        };
        TcpListener m_listener;
        Function<String(StringView)> m_answer;
        ITlsBackend* m_tls;
        const TlsCertificate* m_cert;
        Array<Connection> m_connections;
    };

    // Sends `request` and pumps the client (and `serve`) until its answer arrives, ~10 s at most.
    HttpClientResponse Fetch(IHttpClient& client, HttpClientRequest request, const Function<void()>& serve)
    {
        HttpClientResponse got;
        bool done = false;
        (void)client.Send(Move(request), [&](const HttpClientResponse& r)
                          {
                              got = r;
                              done = true;
                          });
        for (u32 i = 0; i < 10000 && !done; ++i)
        {
            serve();
            client.Pump();
            if (!done)
                SleepMilliseconds(1);
        }
        REQUIRE(done);
        return got;
    }

    String Url(u16 port, StringView path, bool secure = false)
    {
        return Format(u8"{}://{}:{}{}", secure ? StringView(u8"https") : StringView(u8"http"),
                      secure ? StringView(u8"localhost") : StringView(u8"127.0.0.1"), port, path);
    }
}

#endif

TEST_CASE("http client: URLs split for a request; other schemes, no host, IPv6 and bad ports refused")
{
    HttpUrl u;
    String error;
    REQUIRE(ParseHttpUrl(u8"https://Example.com/a/b?x=1#frag", u, error));
    CHECK(u.secure);
    CHECK(u.host == StringView(u8"Example.com"));
    CHECK(u.port == 443);
    CHECK(u.target == StringView(u8"/a/b?x=1"));
    REQUIRE(ParseHttpUrl(u8"http://user:pw@host:8080", u, error));
    CHECK_FALSE(u.secure);
    CHECK(u.host == StringView(u8"host"));
    CHECK(u.port == 8080);
    CHECK(u.target == StringView(u8"/"));
    REQUIRE(ParseHttpUrl(u8"http://host?q", u, error));
    CHECK(u.target == StringView(u8"/?q"));
    CHECK_FALSE(ParseHttpUrl(u8"ftp://host/", u, error));
    CHECK_FALSE(ParseHttpUrl(u8"http:///path", u, error));
    CHECK_FALSE(ParseHttpUrl(u8"http://[::1]/", u, error));
    CHECK_FALSE(ParseHttpUrl(u8"http://host:99999/", u, error));
    CHECK_FALSE(ParseHttpUrl(u8"http://host:/", u, error));
    CHECK_FALSE(error.IsEmpty());
}

TEST_CASE("http client: a redirect's Location made absolute")
{
    HttpUrl base;
    String error;
    REQUIRE(ParseHttpUrl(u8"http://host:8080/dir/page?q=1", base, error));
    CHECK(ResolveHttpLocation(base, u8"https://other/x") == StringView(u8"https://other/x"));
    CHECK(ResolveHttpLocation(base, u8"//other/x") == StringView(u8"http://other/x"));
    CHECK(ResolveHttpLocation(base, u8"/root") == StringView(u8"http://host:8080/root"));
    CHECK(ResolveHttpLocation(base, u8"next") == StringView(u8"http://host:8080/dir/next"));
    REQUIRE(ParseHttpUrl(u8"https://host/", base, error));
    CHECK(ResolveHttpLocation(base, u8"/a") == StringView(u8"https://host/a"));
}

#if !PLATFORM_WEB
TEST_CASE("http client: GET, POST, redirects followed and stopped, against the engine's server")
{
    HttpServer server(DefaultAllocator());
    REQUIRE(server.Start(HttpServerConfig{}));
    const u16 port = server.BoundPort();
    String lastMethod;
    server.SetHandler(
        [&](const HttpRequest& request) -> Optional<HttpResponse>
        {
            lastMethod = request.method;
            const StringView target = request.target.AsView();
            if (target == StringView(u8"/hello"))
            {
                HttpResponse r = HttpResponse::Text(200, u8"text/plain", u8"hello there");
                r.headers.PushBack(HttpHeader{String(u8"X-Kind"), String(u8"greeting")});
                return r;
            }
            if (target == StringView(u8"/echo"))
                return HttpResponse::Text(200, u8"text/plain",
                                          Format(u8"{} {} {}", request.method.AsView(), request.Header(u8"Content-Type"), request.BodyText()).AsView());
            if (target == StringView(u8"/old"))
            {
                HttpResponse r = HttpResponse::Text(302, u8"text/plain", u8"");
                r.headers.PushBack(HttpHeader{String(u8"Location"), String(u8"/hello")});
                return r;
            }
            if (target == StringView(u8"/after-post"))
            {
                HttpResponse r = HttpResponse::Text(303, u8"text/plain", u8"");
                r.headers.PushBack(HttpHeader{String(u8"Location"), String(u8"hello")});
                return r;
            }
            if (target == StringView(u8"/loop"))
            {
                HttpResponse r = HttpResponse::Text(301, u8"text/plain", u8"");
                r.headers.PushBack(HttpHeader{String(u8"Location"), String(u8"/loop")});
                return r;
            }
            if (target == StringView(u8"/big"))
            {
                String big;
                for (u32 i = 0; i < 100; ++i)
                    big += StringView(u8"x");
                return HttpResponse::Text(200, u8"text/plain", big.AsView());
            }
            if (target == StringView(u8"/never"))
                return {}; // not yet, for as long as it is asked
            return HttpResponse::Text(404, u8"text/plain", u8"not here");
        });
    UniquePtr<IHttpClient> client = CreateNativeHttpClient(DefaultAllocator(), nullptr);
    const Function<void()> serve = [&]() { (void)server.Pump(); };

    SUBCASE("a GET")
    {
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(Url(port, u8"/hello").AsView()), serve);
        INFO(C(r.error));
        CHECK(r.Ok());
        CHECK(r.status == 200);
        CHECK(r.BodyText() == StringView(u8"hello there"));
        CHECK(r.Header(u8"x-kind") == StringView(u8"greeting"));
    }
    SUBCASE("a POST, its body and type sent")
    {
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Post(Url(port, u8"/echo").AsView(), u8"text/plain", u8"payload"), serve);
        CHECK(r.status == 200);
        CHECK(r.BodyText() == StringView(u8"POST text/plain payload"));
    }
    SUBCASE("a status that is not 2xx is still a response")
    {
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(Url(port, u8"/nothing").AsView()), serve);
        CHECK(r.Ok());
        CHECK(r.status == 404);
    }
    SUBCASE("a redirect followed, the answering URL reported")
    {
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(Url(port, u8"/old").AsView()), serve);
        CHECK(r.status == 200);
        CHECK(r.BodyText() == StringView(u8"hello there"));
        CHECK(r.url == Url(port, u8"/hello"));
    }
    SUBCASE("a 303 after a POST becomes a GET")
    {
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Post(Url(port, u8"/after-post").AsView(), u8"text/plain", u8"x"), serve);
        CHECK(r.status == 200);
        CHECK(lastMethod == StringView(u8"GET"));
    }
    SUBCASE("with redirects off, the redirect comes back as it is")
    {
        HttpClientRequest request = HttpClientRequest::Get(Url(port, u8"/old").AsView());
        request.maxRedirects = 0;
        const HttpClientResponse r = Fetch(*client, Move(request), serve);
        CHECK(r.status == 302);
        CHECK(r.Header(u8"Location") == StringView(u8"/hello"));
    }
    SUBCASE("a redirect loop stopped, saying why")
    {
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(Url(port, u8"/loop").AsView()), serve);
        CHECK_FALSE(r.Ok());
        CHECK(r.status == 0);
        CHECK(r.error.AsView().ContainsIgnoreCase(u8"too many redirects"));
    }
    SUBCASE("a body past the cap fails the request")
    {
        HttpClientRequest request = HttpClientRequest::Get(Url(port, u8"/big").AsView());
        request.maxResponseBytes = 8;
        const HttpClientResponse r = Fetch(*client, Move(request), serve);
        CHECK_FALSE(r.Ok());
        CHECK(r.error.AsView().ContainsIgnoreCase(u8"larger than 8 bytes"));
    }
    SUBCASE("a server that never answers times out")
    {
        HttpClientRequest request = HttpClientRequest::Get(Url(port, u8"/never").AsView());
        request.timeoutSeconds = 0.3f;
        const HttpClientResponse r = Fetch(*client, Move(request), serve);
        CHECK_FALSE(r.Ok());
        CHECK(r.error.AsView().ContainsIgnoreCase(u8"timed out"));
    }
    SUBCASE("a cancelled request is never answered, queued or under way")
    {
        bool heard = false;
        const HttpRequestId running = client->Send(HttpClientRequest::Get(Url(port, u8"/never").AsView()), [&](const HttpClientResponse&) { heard = true; });
        const HttpRequestId queued = client->Send(HttpClientRequest::Get(Url(port, u8"/hello").AsView()), [&](const HttpClientResponse&) { heard = true; });
        CHECK(client->Pending() == 2);
        for (u32 i = 0; i < 50; ++i)
        {
            serve();
            SleepMilliseconds(1);
        }
        client->Cancel(queued);
        client->Cancel(running);
        for (u32 i = 0; i < 5000 && client->Pending() > 0; ++i)
        {
            serve();
            client->Pump();
            SleepMilliseconds(1);
        }
        CHECK(client->Pending() == 0);
        CHECK_FALSE(heard);
        // The client still works after.
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(Url(port, u8"/hello").AsView()), serve);
        CHECK(r.status == 200);
    }
    SUBCASE("https with no TLS backend fails, saying so")
    {
        const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(u8"https://localhost:1/"), serve);
        CHECK_FALSE(r.Ok());
        CHECK(r.error.AsView().ContainsIgnoreCase(u8"TLS backend"));
    }
}

TEST_CASE("http client: a refused connection and a bad URL come back as errors")
{
    u16 port = 0;
    {
        TcpListener gone(0, true);
        port = gone.BoundPort();
    }
    UniquePtr<IHttpClient> client = CreateNativeHttpClient(DefaultAllocator(), nullptr);
    const HttpClientResponse refused = Fetch(*client, HttpClientRequest::Get(Url(port, u8"/").AsView()), []() {});
    CHECK_FALSE(refused.Ok());
    CHECK(refused.status == 0);
    const HttpClientResponse bad = Fetch(*client, HttpClientRequest::Get(u8"gopher://x/"), []() {});
    CHECK(bad.error.AsView().ContainsIgnoreCase(u8"not an http"));
}

TEST_CASE("http client: a chunked response")
{
    RawServer server([](StringView) {
        return String(u8"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                      u8"4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    });
    UniquePtr<IHttpClient> client = CreateNativeHttpClient(DefaultAllocator(), nullptr);
    const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(Url(server.Port(), u8"/").AsView()), [&]() { server.Pump(); });
    INFO(C(r.error));
    CHECK(r.status == 200);
    CHECK(r.BodyText() == StringView(u8"Wikipedia"));
}

TEST_CASE("http client: https through the TLS backend, and an https request redirected to http refused")
{
    TlsCertificate cert;
    String error;
    REQUIRE(CreateSelfSignedCertificate(u8"localhost", CertificateTime(-1).AsView(), CertificateTime(30).AsView(), cert, error));
    UniquePtr<ITlsBackend> tls = CreateMbedTlsBackend(DefaultAllocator());
    String seen;
    RawServer server(
        [&](StringView head) {
            seen = String(head);
            if (head.StartsWith(u8"GET /down"))
                return String(u8"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/\r\nContent-Length: 0\r\n\r\n");
            return String(u8"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecure");
        },
        tls.Get(), &cert);
    UniquePtr<IHttpClient> client = CreateNativeHttpClient(DefaultAllocator(), tls.Get(), TlsTrust::FromPem(cert.certificatePem.AsView()));
    const Function<void()> serve = [&]() { server.Pump(); };

    const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(Url(server.Port(), u8"/x", true).AsView()), serve);
    INFO(C(r.error));
    CHECK(r.status == 200);
    CHECK(r.BodyText() == StringView(u8"secure"));
    CHECK(seen.AsView().ContainsIgnoreCase(Format(u8"Host: localhost:{}", server.Port()).AsView()));

    const HttpClientResponse down = Fetch(*client, HttpClientRequest::Get(Url(server.Port(), u8"/down", true).AsView()), serve);
    CHECK_FALSE(down.Ok());
    CHECK(down.error.AsView().ContainsIgnoreCase(u8"refused"));

    // A client that does not trust the certificate fails, saying why.
    UniquePtr<IHttpClient> wary = CreateNativeHttpClient(DefaultAllocator(), tls.Get(), TlsTrust::FromPem(u8""));
    const HttpClientResponse untrusted = Fetch(*wary, HttpClientRequest::Get(Url(server.Port(), u8"/x", true).AsView()), serve);
    CHECK_FALSE(untrusted.Ok());
    CHECK_FALSE(untrusted.error.IsEmpty());
}

// A real site over the internet with the system's roots: only when ENGINE_HTTP_LIVE is set (a test
// run must not need the network).
TEST_CASE("http client: a real https site with the system's roots (ENGINE_HTTP_LIVE)")
{
    if (!GetEnvironmentVariable(u8"ENGINE_HTTP_LIVE").HasValue())
        return;
    UniquePtr<ITlsBackend> tls = CreateMbedTlsBackend(DefaultAllocator());
    UniquePtr<IHttpClient> client = CreateNativeHttpClient(DefaultAllocator(), tls.Get());
    const HttpClientResponse r = Fetch(*client, HttpClientRequest::Get(u8"https://example.com/"), []() {});
    INFO(C(r.error));
    CHECK(r.status == 200);
    CHECK(r.BodyText().ContainsIgnoreCase(u8"<html"));
}
#endif // !PLATFORM_WEB (a browser build has the fetch client: Http.Client.WebCheck, by hand)
