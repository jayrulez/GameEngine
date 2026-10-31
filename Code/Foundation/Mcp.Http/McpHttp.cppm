// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp.Http - the `foundation.mcp.http` module.
//
// The MCP streamable-HTTP binding: the same McpServer registry the stdio host serves, over
// foundation.http, bound to localhost and guarded by a bearer token. It is a separate module from
// foundation.mcp (as Mcp.Reflection and Mcp.Script are), so the protocol core stays free of
// transports.
//
// Endpoints:
//   POST /mcp     one JSON-RPC message per request (the body is the line); the JSON-RPC
//                 response is the 200 body. A notification (no response) answers 202.
//   GET  /events  the Server-Sent Events channel for server-initiated traffic (progress
//                 notifications, resource changes). Events are broadcast with
//                 McpHttpHost::Broadcast; a `: connected` comment confirms the stream.
//   Otherwise     404; a wrong method on /mcp is 405; a missing or wrong bearer token is 401
//                 on both endpoints (localhost is the boundary, the token is the lock).
//
// Threading: call Pump() from one thread (the editor's main thread, per frame). Handlers run
// synchronously there, so tools may touch main-thread state.
// Broadcast is safe from that same thread; SseStream itself serializes its writes. A tool that
// is not finished leaves its request waiting on the connection (the handler answers
// foundation.http's "not yet") and is re-entered by the next Pump; HasPendingRequest says a
// caller is waiting, so a host that idles must keep pumping until it is answered.

module;
#include "Core/Prelude.h"

export module foundation.mcp.http;

import foundation.core;
import foundation.http;
import foundation.mcp;

using namespace foundation::core;

export namespace foundation::mcp
{
    struct McpHttpConfig
    {
        u16 port = 0;   // 0 = OS-assigned (read BoundPort after Start)
        String token;   // required bearer token; Start refuses an empty one
    };

    /// Hosts an McpServer over HTTP and SSE. The McpServer must outlive the host.
    class McpHttpHost
    {
    public:
        McpHttpHost(core::IAllocator& allocator, McpServer& server)
            : m_server(&server), m_http(allocator)
        {
        }
        ~McpHttpHost() { Stop(); }
        McpHttpHost(const McpHttpHost&) = delete;
        McpHttpHost& operator=(const McpHttpHost&) = delete;

        [[nodiscard]] bool Start(const McpHttpConfig& config);
        void Stop();
        [[nodiscard]] bool IsRunning() const noexcept { return m_http.IsRunning(); }
        [[nodiscard]] u16 BoundPort() const noexcept { return m_http.BoundPort(); }
        /// A tool call is waiting on a not-finished tool: keep pumping until it is answered.
        [[nodiscard]] bool HasPendingRequest() const noexcept
        {
            return m_http.PendingRequestCount() > 0;
        }

        /// One pump of the underlying HTTP server (accept, read, dispatch, write). Call per
        /// frame or in a loop. Returns completed requests this call.
        usize Pump();

        /// Broadcast one event to every connected /events listener. Returns how many
        /// listeners received it (closed streams are dropped).
        usize Broadcast(StringView eventName, StringView data);
        [[nodiscard]] usize ListenerCount() const noexcept { return m_listeners.Size(); }

    private:
        [[nodiscard]] Optional<http::HttpResponse> Handle(const http::HttpRequest& request);
        [[nodiscard]] bool Authorized(const http::HttpRequest& request) const;

        McpServer* m_server;
        http::HttpServer m_http;
        String m_token;
        Array<RefPtr<http::SseStream>> m_listeners;
    };
}
