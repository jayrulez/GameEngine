// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp.Http - implementation unit.

module;
#include "Core/Prelude.h"

module foundation.mcp.http;

import foundation.core;
import foundation.http;
import foundation.mcp;

using namespace foundation::core;

namespace foundation::mcp
{
    bool McpHttpHost::Start(const McpHttpConfig& config)
    {
        if (config.token.IsEmpty())
        {
            return false; // the token is the lock: never serve without one
        }
        m_token = config.token;
        http::HttpServerConfig httpConfig;
        httpConfig.port = config.port;
        if (!m_http.Start(httpConfig))
        {
            return false;
        }
        m_http.SetHandler([this](const http::HttpRequest& request)
                          { return Handle(request); });
        // A call waiting on a tool whose caller left ends there: the tool's state for it goes.
        m_http.SetAbandonHandler([this](const http::HttpRequest& request)
                                 { m_server->AbandonCall(request.sequence); });
        m_http.SetStreamHandler(
            [this](const http::HttpRequest&, RefPtr<http::SseStream> stream)
            {
                // Confirm the stream immediately (clients treat the first bytes as "live").
                (void)stream->WriteComment(u8"connected");
                m_listeners.PushBack(Move(stream));
            });
        return true;
    }

    void McpHttpHost::Stop()
    {
        for (RefPtr<http::SseStream>& listener : m_listeners)
        {
            listener->Close();
        }
        m_listeners.Clear();
        m_http.Stop();
    }

    usize McpHttpHost::Pump()
    {
        const usize completed = m_http.Pump();
        // Sweep listeners whose peer went away (polled for liveness; see SseStream::PollLive).
        for (usize i = 0; i < m_listeners.Size();)
        {
            if (!m_listeners[i]->PollLive())
            {
                m_listeners.RemoveAt(i);
            }
            else
            {
                ++i;
            }
        }
        return completed;
    }

    usize McpHttpHost::Broadcast(StringView eventName, StringView data)
    {
        usize delivered = 0;
        for (usize i = 0; i < m_listeners.Size();)
        {
            if (m_listeners[i]->WriteEvent(eventName, data))
            {
                ++delivered;
                ++i;
            }
            else
            {
                m_listeners.RemoveAt(i); // peer gone: the write closed the stream
            }
        }
        return delivered;
    }

    bool McpHttpHost::Authorized(const http::HttpRequest& request) const
    {
        const StringView auth = request.Header(u8"Authorization");
        const StringView prefix = u8"Bearer ";
        if (!auth.StartsWith(prefix))
        {
            return false;
        }
        return auth.SubStr(prefix.Size(), auth.Size() - prefix.Size()) == m_token.AsView();
    }

    Optional<http::HttpResponse> McpHttpHost::Handle(const http::HttpRequest& request)
    {
        if (!Authorized(request))
        {
            return http::HttpResponse::Json(
                401, u8"{\"error\":\"missing or invalid bearer token\"}");
        }
        if (request.target.AsView() == StringView(u8"/mcp"))
        {
            if (request.method.AsView() != StringView(u8"POST"))
            {
                return http::HttpResponse::Json(
                    405, u8"{\"error\":\"POST one JSON-RPC message per request\"}");
            }
            // The request's number is the call's identity: the same each time an unfinished call
            // comes back, and different for two identical calls in flight at once.
            LineOutcome outcome = m_server->HandleLine(request.BodyText(), request.sequence);
            switch (outcome.state)
            {
            case LineState::Notification:
            {
                // A notification: accepted, nothing to say.
                http::HttpResponse accepted;
                accepted.status = 202;
                return accepted;
            }
            case LineState::NotFinished:
                // The tool asked to be re-entered, so there is no answer yet: the request waits on
                // its connection and comes back through Handle on the next Pump.
                return {};
            case LineState::Answered:
                break;
            }
            return http::HttpResponse::Json(200, outcome.response.AsView());
        }
        if (request.target.AsView() == StringView(u8"/events"))
        {
            if (request.method.AsView() != StringView(u8"GET"))
            {
                return http::HttpResponse::Json(
                    405, u8"{\"error\":\"GET opens the event stream\"}");
            }
            return http::HttpResponse::EventStream();
        }
        return http::HttpResponse::Json(
            404, u8"{\"error\":\"unknown endpoint - POST /mcp or GET /events\"}");
    }
}
