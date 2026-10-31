// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Http.Client - the `foundation.http.client` module.
///
/// The game-facing HTTP client: requests go out, and their responses come back by callback on the
/// caller's thread when it pumps. `IHttpClient` is the interface; the native backend here runs each
/// request on a worker thread (a name lookup blocks), speaking HTTP/1.1 over a PlainStream for
/// http:// and a TlsStream for https:// (foundation.net.tls, the backend the application
/// composes). A browser build has its own backend over the browser's
/// fetch (emscripten's), which does TLS, redirects and certificates itself; there `url` is the URL
/// asked for, maxRedirects is the browser's, and a cross-origin request needs the server's CORS
/// headers (without them it fails, and the browser does not say why).
///
/// What it does: GET, POST and any method; bodies by Content-Length, chunked, or until close;
/// redirects within a limit (an https request is never followed to http); a timeout over the whole
/// request; cancel; a cap on the response's size. What it does not do: keep-alive or HTTP/2 (each
/// request has its own connection), cookies, proxies, IPv6 (the socket layer has none).
///
/// Threading: Send, Cancel and Pump from one thread (the game's); callbacks run inside Pump.

module;
#include "Core/Prelude.h"

export module foundation.http.client;

import foundation.core;
import foundation.http;
import foundation.net.tls;

using namespace foundation::core;

export namespace foundation::http
{
    using HttpRequestId = u64;

    struct HttpClientRequest
    {
        String method = String(u8"GET");
        String url; ///< http://host[:port]/path?query or https://...
        Array<HttpHeader> headers;
        Array<byte> body;
        f32 timeoutSeconds = 30.0f;            ///< over the whole request, redirects included
        u32 maxRedirects = 5;                  ///< 0: a redirect comes back as it is
        usize maxResponseBytes = 16 * 1024 * 1024; ///< a body past this fails the request

        [[nodiscard]] static HttpClientRequest Get(StringView url)
        {
            HttpClientRequest r;
            r.url = String(url);
            return r;
        }
        [[nodiscard]] static HttpClientRequest Post(StringView url, StringView contentType, StringView body)
        {
            HttpClientRequest r;
            r.method = String(u8"POST");
            r.url = String(url);
            r.headers.PushBack(HttpHeader{String(u8"Content-Type"), String(contentType)});
            for (const utf8char c : body)
                r.body.PushBack(static_cast<byte>(c));
            return r;
        }
    };

    struct HttpClientResponse
    {
        u16 status = 0; ///< 0 when no response arrived (`error` says why)
        Array<HttpHeader> headers;
        Array<byte> body;
        String error; ///< why the request failed, said for a person; empty when a response came
        String url;   ///< the URL that answered (after redirects)

        [[nodiscard]] bool Ok() const noexcept { return error.IsEmpty(); }
        [[nodiscard]] StringView Header(StringView name) const { return FindHeader(headers, name); }
        [[nodiscard]] StringView BodyText() const { return StringView(reinterpret_cast<const utf8char*>(body.Data()), body.Size()); }
    };

    class IHttpClient
    {
    public:
        virtual ~IHttpClient() = default;
        /// Sends `request`; `done` hears its response (or why it failed) inside a later Pump.
        virtual HttpRequestId Send(HttpClientRequest request, Function<void(const HttpClientResponse&)> done) = 0;
        /// Its callback is never called; a request under way is dropped.
        virtual void Cancel(HttpRequestId id) = 0;
        /// Delivers the responses that have arrived, on this thread.
        virtual void Pump() = 0;
        /// Requests sent and not yet delivered or cancelled.
        [[nodiscard]] virtual usize Pending() const = 0;
    };

    /// A URL the client can fetch.
    struct HttpUrl
    {
        bool secure = false; ///< https
        String host;
        u16 port = 0;        ///< 80 or 443 when the URL names none
        String target;       ///< the path and query, "/" at the least
    };

    /// `url` split for a request; false, `error` saying why, when it is not an http(s) URL this
    /// client fetches (another scheme, no host, an IPv6 literal, a bad port).
    [[nodiscard]] bool ParseHttpUrl(StringView url, HttpUrl& out, String& error);

    /// A redirect's Location made absolute against the URL that sent it.
    [[nodiscard]] String ResolveHttpLocation(const HttpUrl& base, StringView location);

#if !PLATFORM_WEB
    /// The native client: requests on a worker; https through `tls` (null: http only, an https
    /// request failing to say so), checked against `trust`.
    [[nodiscard]] UniquePtr<IHttpClient> CreateNativeHttpClient(IAllocator& allocator, foundation::net::ITlsBackend* tls,
                                                                foundation::net::TlsTrust trust = foundation::net::TlsTrust::System());
#else
    /// The browser client: the browser's fetch, its callbacks between frames.
    [[nodiscard]] UniquePtr<IHttpClient> CreateBrowserHttpClient(IAllocator& allocator);
#endif

    /// The platform's client: the native one (`tls` and `trust` as there), or in a browser the
    /// browser's (which ignores them: the browser does TLS).
    [[nodiscard]] UniquePtr<IHttpClient> CreateHttpClient(IAllocator& allocator, foundation::net::ITlsBackend* tls,
                                                          foundation::net::TlsTrust trust = foundation::net::TlsTrust::System());
}
