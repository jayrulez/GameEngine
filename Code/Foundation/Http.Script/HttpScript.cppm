// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Http.Script - the `foundation.http.script` module.
///
/// The `Http` facade (Documentation/Specs/tls-http-wss.md, P5): a game's scripts send requests and
/// hear the answers by callback on the game's thread, through the run's IHttpClient (native, or the
/// browser's in a web build). The run owns an HttpScriptBinding and installs it on its script
/// contexts; the client is made on the first request, so a game that never asks opens nothing, and
/// requests leave only when the game asks. A game in the editor's play mode is a game like any
/// other.
///
///     Http.get("https://example.com/scores", function (status, body, error) ... end)
///
/// A handler hears (status, body, error): the status and body text of the response (a 404 is a
/// response), or status 0 and why in `error` when none came (no network, a timeout, a refused
/// certificate). Bodies are text both ways in v1.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // the Http facade (RTTI_OBJECT)

export module foundation.http.script;

import foundation.core;
import foundation.http.client;
import foundation.script;

using namespace foundation::core;

export namespace foundation::http
{
    namespace script = foundation::script;

    inline constexpr StringView kHttpScriptService = u8"http.runtime";

    /// A run's HTTP for its scripts: the client, made on the first request by the owner's
    /// factory, pumped by the owner each frame.
    class HttpScriptBinding
    {
    public:
        using Factory = Function<UniquePtr<IHttpClient>()>;

        void SetFactory(Factory factory) { m_factory = Move(factory); }
        /// The client, made now if this is the first request; null when the run has no factory.
        [[nodiscard]] IHttpClient* Client()
        {
            if (!m_client && m_factory)
                m_client = m_factory();
            return m_client.Get();
        }
        /// Delivers the answers that have arrived (their handlers run here). Each frame.
        void Pump()
        {
            if (m_client)
                m_client->Pump();
        }
        /// Drops the client and every request under way, their handlers never called: when the
        /// scripts that sent them stop.
        void Reset() { m_client = {}; }
        [[nodiscard]] usize Pending() const { return m_client ? m_client->Pending() : 0; }

    private:
        Factory m_factory;
        UniquePtr<IHttpClient> m_client;
    };

    inline void InstallHttpScriptService(script::IScriptContext& context, HttpScriptBinding& binding)
    {
        context.SetService(kHttpScriptService, &binding);
    }
    inline void ClearHttpScriptService(script::IScriptContext& context) { context.SetService(kHttpScriptService, nullptr); }

    class Http final : public Object
    {
        RTTI_OBJECT(Http, Object)
    public:
        [[nodiscard]] static HttpScriptBinding* Resolve()
        {
            script::IScriptContext* context = script::CurrentScriptContext();
            return context != nullptr ? static_cast<HttpScriptBinding*>(context->GetService(kHttpScriptService)) : nullptr;
        }

        /// Each returns the request's id (for cancel), or 0 when this run has no HTTP (its handler
        /// is then never called).
        static i32 get(String url, RefPtr<script::IScriptDelegate> handler);
        static i32 post(String url, String contentType, String body, RefPtr<script::IScriptDelegate> handler);
        /// Any method; `headers` is "Name: value" lines (empty for none).
        static i32 send(String method, String url, String headers, String body, RefPtr<script::IScriptDelegate> handler);
        /// Its handler is never called; a request under way is dropped.
        static void cancel(i32 id);
    };

    /// Registers the `Http` facade and its handler shape. Idempotent; the run domain calls it.
    void RegisterHttpScriptFacade();
}
