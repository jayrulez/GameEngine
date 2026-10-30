// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Http tests - the parser edges (split pushes, case-insensitive headers, limits,
// close-delimited bodies) and REAL loopback exchanges: a pumped server on an OS-assigned port,
// the blocking client from a worker thread, one-shot requests, refusals, and an SSE stream
// read raw off the socket.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.http;

using namespace foundation::core;
using namespace foundation::http;
namespace net = foundation::net;

namespace
{
    Span<const byte> Bytes(StringView text)
    {
        return Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size());
    }

    // Pump `server` until `done` flips or ~5 s passes (the test-side serve loop).
    void PumpUntil(HttpServer& server, const bool& done)
    {
        for (u32 i = 0; i < 5000 && !done; ++i)
        {
            if (server.Pump() == 0)
            {
                SleepMilliseconds(1);
            }
        }
    }
}

TEST_CASE("http: parser - a request in arbitrarily split pushes")
{
    const StringView wire = u8"POST /mcp?x=1 HTTP/1.1\r\n"
                            u8"Host: local\r\n"
                            u8"content-type: application/json\r\n"
                            u8"Content-Length: 11\r\n"
                            u8"\r\n"
                            u8"{\"ok\":true}";
    // Push one byte at a time - the harshest framing.
    HttpMessageParser parser(HttpMessageParser::Mode::Request);
    HttpParseState state = HttpParseState::NeedMore;
    for (usize i = 0; i < wire.Size(); ++i)
    {
        state = parser.Push(Bytes(wire.SubStr(i, 1)));
    }
    REQUIRE(state == HttpParseState::Complete);
    CHECK(parser.Method() == StringView(u8"POST"));
    CHECK(parser.Target() == StringView(u8"/mcp?x=1"));
    // Case-insensitive lookup finds the lower-case spelling.
    CHECK(FindHeader(parser.Headers(), u8"Content-Type") ==
          StringView(u8"application/json"));
    CHECK(parser.Body().Size() == 11u);
}

TEST_CASE("http: parser - failures (garbage, chunked, oversized head, body cap)")
{
    {
        HttpMessageParser p(HttpMessageParser::Mode::Request);
        CHECK(p.Push(Bytes(u8"this is not http\r\n\r\n")) == HttpParseState::Failed);
    }
    // A first line missing its second separator (no version), or ending on it: refused,
    // never read past (the second scan stops exactly at the end of the line).
    {
        HttpMessageParser p(HttpMessageParser::Mode::Request);
        CHECK(p.Push(Bytes(u8"GET /path\r\n\r\n")) == HttpParseState::Failed);
    }
    {
        HttpMessageParser p(HttpMessageParser::Mode::Request);
        CHECK(p.Push(Bytes(u8"GET\r\n\r\n")) == HttpParseState::Failed);
    }
    {
        HttpMessageParser p(HttpMessageParser::Mode::Request);
        CHECK(p.Push(Bytes(u8"GET /path \r\n\r\n")) == HttpParseState::Failed); // empty version
    }
    {
        // A response without a reason phrase is legal and still parses.
        HttpMessageParser p(HttpMessageParser::Mode::Response);
        CHECK(p.Push(Bytes(u8"HTTP/1.1 204\r\nContent-Length: 0\r\n\r\n")) != HttpParseState::Failed);
        CHECK(p.Status() == 204);
    }
    {
        HttpMessageParser p(HttpMessageParser::Mode::Request);
        CHECK(p.Push(Bytes(u8"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")) ==
              HttpParseState::Failed);
    }
    {
        HttpMessageParser p(HttpMessageParser::Mode::Request);
        String huge(u8"GET / HTTP/1.1\r\nX: ");
        for (usize i = 0; i < 20000; ++i)
        {
            huge.PushBack(u8'a');
        }
        CHECK(p.Push(Bytes(huge.AsView())) == HttpParseState::Failed);
    }
    {
        HttpMessageParser p(HttpMessageParser::Mode::Request, /*maxBodyBytes=*/8);
        CHECK(p.Push(Bytes(u8"POST / HTTP/1.1\r\nContent-Length: 9\r\n\r\n")) ==
              HttpParseState::Failed);
    }
}

TEST_CASE("http: parser - close-delimited response body")
{
    HttpMessageParser p(HttpMessageParser::Mode::Response);
    CHECK(p.Push(Bytes(u8"HTTP/1.1 200 OK\r\nX-Kind: raw\r\n\r\nhello")) ==
          HttpParseState::NeedMore);
    CHECK(p.Push(Bytes(u8" world")) == HttpParseState::NeedMore);
    CHECK(p.OnPeerClosed() == HttpParseState::Complete);
    CHECK(p.Status() == 200);
    CHECK(p.Body().Size() == 11u);
}

TEST_CASE("http: parser - a chunked response body, split anywhere, extensions and trailers dropped")
{
    const StringView wire = u8"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            u8"5\r\nhello\r\n7;ext=1\r\n, world\r\nA\r\n0123456789\r\n0\r\nX-Trailer: t\r\n\r\n";
    // Whole, then a byte at a time: the same body.
    const usize steps[] = {wire.Size(), 1, 3};
    for (const usize step : steps)
    {
        HttpMessageParser p(HttpMessageParser::Mode::Response);
        HttpParseState state = HttpParseState::NeedMore;
        for (usize at = 0; at < wire.Size(); at += step)
        {
            const usize n = at + step <= wire.Size() ? step : wire.Size() - at;
            state = p.Push(Bytes(wire.SubStr(at, n)));
            if (at + n < wire.Size())
                REQUIRE(state == HttpParseState::NeedMore);
        }
        CHECK(state == HttpParseState::Complete);
        CHECK(p.Status() == 200);
        CHECK(StringView(reinterpret_cast<const utf8char*>(p.Body().Data()), p.Body().Size()) ==
              StringView(u8"hello, world0123456789"));
    }
    // A bad size line, and chunk data not ended by CRLF: refused.
    {
        HttpMessageParser p(HttpMessageParser::Mode::Response);
        CHECK(p.Push(Bytes(u8"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n")) == HttpParseState::Failed);
        CHECK_FALSE(p.TooLarge());
    }
    {
        HttpMessageParser p(HttpMessageParser::Mode::Response);
        CHECK(p.Push(Bytes(u8"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabcd\r\n")) == HttpParseState::Failed);
    }
    // Past the cap: refused, and said to be too large.
    {
        HttpMessageParser p(HttpMessageParser::Mode::Response, /*maxBodyBytes=*/8);
        CHECK(p.Push(Bytes(u8"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n5\r\n")) == HttpParseState::Failed);
        CHECK(p.TooLarge());
    }
}

TEST_CASE("http: parser - responses with no body (204, 304, 1xx, a HEAD's) complete at their head")
{
    const StringView heads[] = {u8"HTTP/1.1 204 No Content\r\n\r\n", u8"HTTP/1.1 304 Not Modified\r\nContent-Length: 50\r\n\r\n"};
    for (const StringView head : heads)
    {
        HttpMessageParser p(HttpMessageParser::Mode::Response);
        CHECK(p.Push(Bytes(head)) == HttpParseState::Complete);
        CHECK(p.Body().IsEmpty());
    }
    HttpMessageParser head(HttpMessageParser::Mode::Response);
    head.SetNoBody();
    CHECK(head.Push(Bytes(u8"HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n")) == HttpParseState::Complete);
    CHECK(head.Status() == 200);
    // A Content-Length past the cap is too large, not malformed.
    HttpMessageParser capped(HttpMessageParser::Mode::Response, 8);
    CHECK(capped.Push(Bytes(u8"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n")) == HttpParseState::Failed);
    CHECK(capped.TooLarge());
}

TEST_CASE("http: loopback - one-shot request/response, 404, and bad request")
{
    HttpServer server(DefaultAllocator());
    REQUIRE(server.Start(HttpServerConfig{}));
    const u16 port = server.BoundPort();
    REQUIRE(port != 0);
    CHECK(server.BoundIp() == 0x7F000001u); // a local tool's server: loopback by default
    server.SetHandler(
        [](const HttpRequest& request) -> HttpResponse
        {
            if (request.target.AsView() == StringView(u8"/echo"))
            {
                return HttpResponse::Json(
                    200, Format(u8"{{\"method\":\"{}\",\"size\":{}}}",
                                request.method.AsView(), request.body.Size())
                             .AsView());
            }
            return HttpResponse::Text(404, u8"text/plain", u8"not here");
        });

    bool done = false;
    // Client on a worker; the test thread pumps the server.
    struct Outcome
    {
        Result<HttpResponse, String> echo = Err(String(u8"unset"));
        Result<HttpResponse, String> missing = Err(String(u8"unset"));
    } outcome;
    Thread client(
        [&]
        {
            HttpRequest post;
            post.method = String(u8"POST");
            post.target = String(u8"/echo");
            post.headers.PushBack(
                HttpHeader{String(u8"Content-Type"), String(u8"application/json")});
            for (usize i = 0; i < 5; ++i)
            {
                post.body.PushBack(byte{'x'});
            }
            outcome.echo = HttpFetch(u8"127.0.0.1", port, post);
            HttpRequest get;
            get.method = String(u8"GET");
            get.target = String(u8"/nope");
            outcome.missing = HttpFetch(u8"127.0.0.1", port, get);
            done = true;
        });
    PumpUntil(server, done);
    client.Join();

    REQUIRE(outcome.echo.HasValue());
    CHECK(outcome.echo.Value().status == 200);
    CHECK(outcome.echo.Value().Header(u8"content-type") ==
          StringView(u8"application/json"));
    const StringView echoBody(
        reinterpret_cast<const utf8char*>(outcome.echo.Value().body.Data()),
        outcome.echo.Value().body.Size());
    CHECK(echoBody == StringView(u8"{\"method\":\"POST\",\"size\":5}"));
    REQUIRE(outcome.missing.HasValue());
    CHECK(outcome.missing.Value().status == 404);

    // Raw garbage answers 400 and the server SURVIVES.
    bool done2 = false;
    bool got400 = false;
    Thread rawClient(
        [&]
        {
            net::TcpSocket raw = net::TcpSocket::Connect(u8"127.0.0.1", port);
            for (u32 i = 0; i < 5000 && raw.ConnectStatus() == 0; ++i)
            {
                SleepMilliseconds(1);
            }
            const StringView garbage = u8"complete garbage\r\n\r\n";
            (void)raw.Send(Bytes(garbage));
            HttpMessageParser p(HttpMessageParser::Mode::Response);
            byte buffer[1024];
            for (u32 i = 0; i < 5000; ++i)
            {
                const i64 n = raw.Receive(Span<byte>(buffer, sizeof(buffer)));
                if (n > 0)
                {
                    if (p.Push(Span<const byte>(buffer, static_cast<usize>(n))) ==
                        HttpParseState::Complete)
                    {
                        break;
                    }
                    continue;
                }
                if (n == 0)
                {
                    SleepMilliseconds(1);
                    continue;
                }
                (void)p.OnPeerClosed();
                break;
            }
            got400 = p.Status() == 400;
            done2 = true;
        });
    PumpUntil(server, done2);
    rawClient.Join();
    CHECK(got400);

    // And a normal request still works after the garbage one.
    bool done3 = false;
    Result<HttpResponse, String> after = Err(String(u8"unset"));
    Thread client3(
        [&]
        {
            HttpRequest get;
            get.method = String(u8"GET");
            get.target = String(u8"/echo");
            after = HttpFetch(u8"127.0.0.1", port, get);
            done3 = true;
        });
    PumpUntil(server, done3);
    client3.Join();
    REQUIRE(after.HasValue());
    CHECK(after.Value().status == 200);
}

TEST_CASE("http: loopback - a request answered not yet is re-dispatched every pump until it "
          "is answered; a peer that leaves while waiting is dropped")
{
    HttpServer server(DefaultAllocator());
    REQUIRE(server.Start(HttpServerConfig{}));
    const u16 port = server.BoundPort();
    REQUIRE(port != 0);

    u32 calls = 0;
    u32 answerOnCall = 3;
    Array<u64> sequences; // the request's number on each dispatch
    Array<u64> abandoned;
    server.SetAbandonHandler([&](const HttpRequest& request) { abandoned.PushBack(request.sequence); });
    server.SetHandler(
        [&](const HttpRequest& request) -> Optional<HttpResponse>
        {
            ++calls;
            sequences.PushBack(request.sequence);
            if (calls < answerOnCall)
            {
                return {}; // not yet
            }
            return HttpResponse::Json(200, Format(u8"{{\"calls\":{},\"target\":\"{}\"}}", calls,
                                                  request.target.AsView())
                                               .AsView());
        });

    // The client blocks on one GET; the handler defers twice and answers on the third pump.
    bool done = false;
    Result<HttpResponse, String> outcome = Err(String(u8"unset"));
    Thread client(
        [&]
        {
            HttpRequest get;
            get.method = String(u8"GET");
            get.target = String(u8"/wait");
            outcome = HttpFetch(u8"127.0.0.1", port, get);
            done = true;
        });
    // Pumped by hand so the wait is observable: a pump whose handler says not yet answers
    // nothing and leaves exactly one request pending.
    bool sawPending = false;
    usize answered = 0;
    for (u32 i = 0; i < 5000 && !done; ++i)
    {
        answered += server.Pump();
        if (server.PendingRequestCount() == 1)
        {
            sawPending = true;
        }
        SleepMilliseconds(1);
    }
    client.Join();

    REQUIRE(outcome.HasValue());
    CHECK(outcome.Value().status == 200);
    CHECK(outcome.Value().BodyText() == StringView(u8"{\"calls\":3,\"target\":\"/wait\"}"));
    CHECK(calls == 3);
    CHECK(sawPending);
    CHECK(answered == 1);
    CHECK(server.PendingRequestCount() == 0);
    // The request kept its number across its re-dispatches (Sedulous d509904a); answered, it
    // was not abandoned.
    REQUIRE(sequences.Size() == 3u);
    CHECK(sequences[0] != 0u);
    CHECK(sequences[1] == sequences[0]);
    CHECK(sequences[2] == sequences[0]);
    CHECK(abandoned.IsEmpty());

    // A peer that sends a request and leaves while the handler keeps saying not yet is dropped:
    // the pending count returns to zero without the handler ever answering.
    calls = 0;
    answerOnCall = 0xFFFFFFFFu; // never
    Thread departing(
        [&]
        {
            net::TcpSocket raw = net::TcpSocket::Connect(u8"127.0.0.1", port);
            for (u32 i = 0; i < 5000 && raw.ConnectStatus() == 0; ++i)
            {
                SleepMilliseconds(1);
            }
            (void)raw.Send(Bytes(u8"GET /gone HTTP/1.1\r\nHost: x\r\n\r\n"));
            // the socket closes on scope exit
        });
    departing.Join();
    bool dropped = false;
    for (u32 i = 0; i < 5000 && !dropped; ++i)
    {
        (void)server.Pump();
        dropped = calls > 0 && server.PendingRequestCount() == 0;
        SleepMilliseconds(1);
    }
    CHECK(dropped);
    CHECK(calls >= 1);
    // The handler is told the departed request will never be answered, by its own number,
    // a new one.
    REQUIRE(abandoned.Size() == 1u);
    CHECK(abandoned[0] == sequences[sequences.Size() - 1]);
    CHECK(abandoned[0] > sequences[0]);

    // A request still waiting when the server stops is abandoned too.
    bool stoppedDone = false;
    Thread waiting(
        [&]
        {
            HttpRequest get;
            get.method = String(u8"GET");
            get.target = String(u8"/stop");
            (void)HttpFetch(u8"127.0.0.1", port, get);
            stoppedDone = true;
        });
    for (u32 i = 0; i < 5000 && server.PendingRequestCount() == 0; ++i)
    {
        (void)server.Pump();
        SleepMilliseconds(1);
    }
    REQUIRE(server.PendingRequestCount() == 1u);
    server.Stop();
    waiting.Join();
    CHECK(stoppedDone);
    CHECK(abandoned.Size() == 2u);
}

TEST_CASE("http: loopback - a Server-Sent Events stream delivers events as they are written")
{
    HttpServer server(DefaultAllocator());
    REQUIRE(server.Start(HttpServerConfig{}));
    const u16 port = server.BoundPort();
    server.SetHandler([](const HttpRequest&) { return HttpResponse::EventStream(); });
    RefPtr<SseStream> held;
    server.SetStreamHandler([&held](const HttpRequest&, RefPtr<SseStream> stream)
                            { held = Move(stream); });

    // A raw client: send the GET, then read whatever the server pushes.
    String received;
    bool clientDone = false;
    bool sawHeaders = false;
    Mutex receivedMutex;
    Thread client(
        [&]
        {
            net::TcpSocket raw = net::TcpSocket::Connect(u8"127.0.0.1", port);
            for (u32 i = 0; i < 5000 && raw.ConnectStatus() == 0; ++i)
            {
                SleepMilliseconds(1);
            }
            const StringView get = u8"GET /events HTTP/1.1\r\nHost: local\r\n\r\n";
            (void)raw.Send(Bytes(get));
            byte buffer[2048];
            for (u32 i = 0; i < 5000; ++i)
            {
                const i64 n = raw.Receive(Span<byte>(buffer, sizeof(buffer)));
                if (n > 0)
                {
                    ScopedLock lock(receivedMutex);
                    received.Append(StringView(reinterpret_cast<const utf8char*>(buffer),
                                               static_cast<usize>(n)));
                    sawHeaders = true;
                    // Two full events end with two blank-line terminators.
                    usize terminators = 0;
                    for (usize c = 0; c + 1 < received.Size(); ++c)
                    {
                        if (received[c] == u8'\n' && received[c + 1] == u8'\n')
                        {
                            ++terminators;
                        }
                    }
                    if (terminators >= 2)
                    {
                        break;
                    }
                    continue;
                }
                if (n == 0)
                {
                    SleepMilliseconds(1);
                    continue;
                }
                break;
            }
            clientDone = true;
        });

    // Pump until the stream lands in the handler, write two events, then let the client read.
    for (u32 i = 0; i < 5000 && held.Get() == nullptr; ++i)
    {
        if (server.Pump() == 0)
        {
            SleepMilliseconds(1);
        }
    }
    REQUIRE(held.Get() != nullptr);
    CHECK(held->IsOpen());
    CHECK(held->WriteEvent(u8"progress", u8"cooking 1/2"));
    CHECK(held->WriteEvent(u8"", u8"line one\nline two"));
    for (u32 i = 0; i < 5000 && !clientDone; ++i)
    {
        (void)server.Pump();
        SleepMilliseconds(1);
    }
    client.Join();

    CHECK(sawHeaders);
    ScopedLock lock(receivedMutex);
    const StringView text = received.AsView();
    // Headers + both events arrived, with multi-line data split into per-line fields.
    CHECK(text.StartsWith(u8"HTTP/1.1 200 OK"));
    bool sawEventName = false;
    bool sawDataOne = false;
    bool sawDataTwo = false;
    for (usize i = 0; i < text.Size(); ++i)
    {
        if (text.SubStr(i, text.Size() - i).StartsWith(u8"event: progress"))
        {
            sawEventName = true;
        }
        if (text.SubStr(i, text.Size() - i).StartsWith(u8"data: line one"))
        {
            sawDataOne = true;
        }
        if (text.SubStr(i, text.Size() - i).StartsWith(u8"data: line two"))
        {
            sawDataTwo = true;
        }
    }
    CHECK(sawEventName);
    CHECK(sawDataOne);
    CHECK(sawDataTwo);

    // The consumer's ref outlives the peer: after the client is gone, writes turn false.
    held->Close();
    CHECK(!held->WriteEvent(u8"late", u8"nobody listening"));
}
