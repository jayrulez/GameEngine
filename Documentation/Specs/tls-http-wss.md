# TLS, an HTTP client and wss

> Status: approved 2026-10-30 with the recommendations below (the host terminates TLS itself, a proxy documented beside it; the system's roots plus a game's own PEM, no vendored CA list; the backend chosen at composition). The engine's networking has no TLS: `foundation.http` is a
> small HTTP/1.1 server with a blocking localhost client (MCP, tests), and
> `foundation.net.websocket` accepts `ws://` only. Mbed TLS 4.2.0 is vendored (326ea5204,
> `ThirdParty::MbedTLS`). This spec puts TLS behind a seam, builds a game-facing HTTP client on
> it, and brings `wss://` to both ends of the WebSocket path: Masquerade's gap 8 and P7, and the
> HTTP calls a shipped game makes (leaderboards, news, a backend).

## What is there now

- `foundation.net` `TcpSocket` / `TcpListener`: non-blocking IPv4 streams; `Connect` resolves a
  name, the lookup blocking (`core::ResolveHostIPv4`).
- `foundation.http`: `HttpServer` (one request per connection, SSE, "not yet") and a BLOCKING
  client for loopback tooling, documented as not a game-facing fetch.
- `foundation.net.websocket`: `WebSocketGateway` accepts browser clients on the host (HTTP upgrade,
  RFC 6455 binary frames, an `IDatagramSocket` so the session layer cannot tell UDP from WS);
  `WebSocketClientSocket` is the browser's client (emscripten), always building a `ws://` URL.
- Nothing native can open a WebSocket as a client.

## The seams

### TLS: `foundation.net.tls`

A TLS session as a byte engine that never touches a socket ("memory BIO"): the caller feeds it
what arrived from the peer, takes what it must send, and reads and writes plaintext. So any
transport carries it (a `TcpSocket`, a test's in-memory pipe), two sessions wired together test the
whole handshake headlessly, and a backend is only the crypto.

```cpp
struct TlsClientConfig { String serverName; TlsTrust trust; Array<String> alpn; };
struct TlsServerConfig { String certificateChainPem; String privateKeyPem; Array<String> alpn; };
enum class TlsState : u8 { Handshaking, Open, Closed, Failed };

class ITlsSession {                       // one connection's TLS, either end
    virtual Status Receive(Span<const byte> fromPeer) = 0;   // bytes off the wire
    virtual usize  TakeOutgoing(Array<byte>& toPeer) = 0;    // bytes for the wire
    virtual usize  Read(Span<byte> plaintext) = 0;           // what the peer sent, decrypted
    virtual Status Write(Span<const byte> plaintext) = 0;    // to send, encrypted
    virtual void   Close() = 0;                              // close_notify
    virtual TlsState State() const = 0;
    virtual StringView Failure() const = 0;                  // why, said for a person
    virtual StringView AlpnChosen() const = 0;
};
class ITlsBackend {
    virtual Result<UniquePtr<ITlsSession>> CreateClient(const TlsClientConfig&) = 0;
    virtual Result<UniquePtr<ITlsSession>> CreateServer(const TlsServerConfig&) = 0;
};
```

- `TlsTrust` is what a client checks a server's certificate against: the system's roots (the
  default), a PEM bundle the game ships (a private backend's own CA), or, for a test only, none
  (named `InsecureForTests`, never the default, never reachable from scripts).
- The system's roots: Linux (and SteamOS) read the distribution's bundle (the usual paths:
  `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`, ...); Windows reads the
  `ROOT` certificate store. macOS comes with its backend. No CA list is vendored.
- `TlsStream` adapts a session to a `TcpSocket`: it pumps the socket into the session and the
  session's output into the socket, non-blocking, so the HTTP client and the WebSocket paths speak
  plaintext over it as they do over a bare socket.

### The backend: `foundation.net.tls.mbedtls`

The only module that links `ThirdParty::MbedTLS` and includes its headers. TLS 1.2 and 1.3, the
client verifying the server's name and chain, the server presenting its chain. The application
chooses the backend at composition (desktop and Deck: Mbed TLS); a web build has none, since the
browser does TLS.

### The HTTP client: `foundation.http.client`

```cpp
struct HttpRequest  { String method; String url; Array<HttpHeader> headers; Array<byte> body;
                      f32 timeoutSeconds = 30; u32 maxRedirects = 5; usize maxResponseBytes; };
struct HttpResponse { u16 status = 0; Array<HttpHeader> headers; Array<byte> body; String error; };
class IHttpClient {
    virtual HttpRequestId Send(HttpRequest, Function<void(const HttpResponse&)> done) = 0;
    virtual void Cancel(HttpRequestId) = 0;
    virtual void Pump() = 0;   // completions delivered here, on the caller's thread
};
```

- Native backend: requests run on a worker (the name lookup blocks), HTTP/1.1 over a `TcpSocket`,
  `https` through a `TlsStream`; bodies by Content-Length or chunked; redirects followed within the
  limit (an https page never redirected to http); timeouts; cancel; a response size cap.
- Web backend: the browser's `fetch` (`emscripten_fetch`), the same interface.
- v1 bounds: no keep-alive or HTTP/2, no cookies, no proxies, IPv4 (the socket layer's), one
  worker. The existing blocking localhost client stays for MCP and tests.

### wss

- The browser client takes the scheme it is given: a `wss://` address opens a secure WebSocket
  (Masquerade gap 8).
- The host's gateway takes an optional `TlsServerConfig`: each connection runs a TLS session before
  the HTTP upgrade, so a host with a certificate for its domain accepts browsers from an https page
  directly. A reverse proxy (Caddy, nginx) in front of a plain `ws://` gateway stays the documented
  alternative, and the only one for a host without its own certificate.
- A native WebSocket client (`ws://` and `wss://`, an `IDatagramSocket` like the browser's), so a
  desktop or Deck client can join a host that only speaks wss, and the gateway's TLS path is
  tested end to end in process.

### Scripts

`Http.get` / `Http.post` / `Http.send` in both languages, answering by callback on the game's
thread, through `IHttpClient` (native or browser alike). Requests leave from the game only when it
asks; a game in the editor's play mode is a game like any other.

## Phases (one commit each, with tests)

- **P0 The TLS seam and the Mbed TLS backend.** `ITlsSession` / `ITlsBackend`, `TlsTrust`, the Mbed
  TLS sessions. Tests, headless (two sessions over an in-memory pipe, the test making a self-signed
  certificate): a handshake and data both ways (TLS 1.3, and 1.2 when asked); the client refusing a
  certificate for another name, one it does not trust, and an expired one; a close seen by the
  peer; ALPN chosen. Built 2026-10-30: `foundation.net.tls` (the seam, `LoadSystemRoots`: the
  distribution's bundle on Linux and SteamOS, the ROOT store on Windows) and
  `foundation.net.tls.mbedtls` (the sessions; `CreateSelfSignedCertificate`, a P-256 key with
  ECDSA, for tests and a LAN host), 6 cases green on clang and gcc, the libraries built for wasm.
- **P1 `TlsStream`.** A session over a `TcpSocket`. Tests: a TLS server and client over loopback
  sockets. (The system's roots, first planned here, came with P0 so the seam's default trust worked
  from the start.) Built 2026-10-30: `INetByteStream` with `PlainStream` and `TlsStream` (the
  socket's conventions: bytes moved, 0 for would-block, -1 for done), so the HTTP and WebSocket
  clients are written once for both schemes.
- **P2 The HTTP client, native.** `IHttpClient` and its native backend. Tests against the in-repo
  `HttpServer` (http) and a TLS-wrapped one (https): GET and POST, Content-Length and chunked
  bodies, a redirect followed and a loop stopped, an https-to-http redirect refused, a timeout, a
  cancel, the size cap. A test against a real site only when an environment variable asks.
  Built 2026-10-30: `foundation.http.client` (`HttpClientRequest` / `HttpClientResponse`, named
  apart from the server's `HttpRequest` / `HttpResponse`; `ParseHttpUrl`, `ResolveHttpLocation`;
  `CreateNativeHttpClient`, one worker), the response parser taught chunked bodies, bodiless
  statuses and HEAD, and a `TooLarge()` that tells the size cap from a malformed body. The live
  test runs when `ENGINE_HTTP_LIVE` is set.
- **P3 The HTTP client in a browser.** The `fetch` backend. Built on the wasm lane; checked by hand
  in a browser (no wasm tests run). Built 2026-10-30: `CreateBrowserHttpClient` over emscripten's
  fetch (`-sFETCH=1`, an INTERFACE link option of Http.Client), and `CreateHttpClient`, the
  platform's client for a composition root. The browser follows redirects and does TLS itself, so
  there `url` is the URL asked for and `maxRedirects` is not ours; a status of 0 is no response
  (the network, a timeout timed here, or a cross-origin refusal the browser does not explain).
  `Http.Client.WebCheck` is the hand check: a page that sends five requests and prints the
  answers.
- **P4 wss.** The gateway's TLS, the native WebSocket client, the browser client's `wss://`. Tests:
  the native client joins the gateway over ws and over wss in process, packets both ways;
  a wrong certificate refused.
- **P5 Scripts and docs.** The `Http` facade in AngelScript and Luau (tested in
  `Code/Integration`), the docs pages (the HTTP client; hosting over wss, with a certificate or
  behind a proxy), Masquerade P7.

## Open questions

1. **The host's own TLS.** Proposed: the gateway terminates TLS itself when given a certificate,
   the reverse proxy documented beside it. The alternative is proxy only (no server half in P0).
2. **Trust.** Proposed: the system's roots, plus a game's own PEM bundle; no vendored CA list. A
   vendored list (Mozilla's, as curl ships it, MPL-2.0) would also cover a box with no bundle
   installed, at the cost of keeping it current.
3. **Who chooses the backend.** Proposed: the engine's composition, Mbed TLS on desktop and Deck,
   none on the web. A future console or platform backend slots in there.
