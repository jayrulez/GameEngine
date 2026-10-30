// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.http.script - the `Http` facade: requests from scripts, answers to their handlers.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // the Http facade reflection body

module foundation.http.script;

import foundation.core;
import foundation.http;
import foundation.http.client;
import foundation.script;
import foundation.script.facades; // RegisterExtraFacadeName

using namespace foundation::core;

namespace foundation::http
{
    namespace
    {
        // Sends `request`; the handler hears (status, body, error) when it is answered.
        i32 Send(HttpClientRequest request, RefPtr<script::IScriptDelegate> handler)
        {
            HttpScriptBinding* binding = Http::Resolve();
            IHttpClient* client = binding != nullptr ? binding->Client() : nullptr;
            if (client == nullptr)
                return 0;
            const HttpRequestId id = client->Send(Move(request),
                                                  [handler](const HttpClientResponse& response)
                                                  {
                                                      if (!handler)
                                                          return;
                                                      Variant args[3] = {Variant::From(static_cast<i32>(response.status)),
                                                                         Variant::From(String(response.BodyText())),
                                                                         Variant::From(response.error)};
                                                      (void)handler->Invoke(Span<Variant>(args, 3));
                                                  });
            return static_cast<i32>(id);
        }

        void SetBody(HttpClientRequest& request, StringView body)
        {
            for (const utf8char c : body)
                request.body.PushBack(static_cast<byte>(c));
        }
    }

    i32 Http::get(String url, RefPtr<script::IScriptDelegate> handler)
    {
        return Send(HttpClientRequest::Get(url.AsView()), Move(handler));
    }

    i32 Http::post(String url, String contentType, String body, RefPtr<script::IScriptDelegate> handler)
    {
        return Send(HttpClientRequest::Post(url.AsView(), contentType.AsView(), body.AsView()), Move(handler));
    }

    i32 Http::send(String method, String url, String headers, String body, RefPtr<script::IScriptDelegate> handler)
    {
        HttpClientRequest request;
        request.method = Move(method);
        request.url = Move(url);
        SetBody(request, body.AsView());
        // "Name: value" lines; a line without a colon is skipped.
        const StringView text = headers.AsView();
        usize start = 0;
        while (start < text.Size())
        {
            usize end = start;
            while (end < text.Size() && text[end] != u8'\n')
                ++end;
            StringView line = text.SubStr(start, end - start);
            if (!line.IsEmpty() && line[line.Size() - 1] == u8'\r')
                line = line.SubStr(0, line.Size() - 1);
            for (usize i = 0; i < line.Size(); ++i)
                if (line[i] == u8':')
                {
                    StringView value = line.SubStr(i + 1, line.Size() - i - 1);
                    while (!value.IsEmpty() && value[0] == u8' ')
                        value = value.SubStr(1, value.Size() - 1);
                    request.headers.PushBack(HttpHeader{String(line.SubStr(0, i)), String(value)});
                    break;
                }
            start = end + 1;
        }
        return Send(Move(request), Move(handler));
    }

    void Http::cancel(i32 id)
    {
        HttpScriptBinding* binding = Resolve();
        if (binding != nullptr && id > 0)
            if (IHttpClient* client = binding->Client())
                client->Cancel(static_cast<HttpRequestId>(id));
    }

    REFLECT_MEMBERS(Http, "rtti::http")
    {
        builder.Method<&Http::get>("get", {"url", "handler"});
        builder.Method<&Http::post>("post", {"url", "contentType", "body", "handler"});
        builder.Method<&Http::send>("send", {"method", "url", "headers", "body", "handler"});
        builder.Method<&Http::cancel>("cancel", {"id"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    void RegisterHttpScriptFacade()
    {
        static const bool once = []()
        {
            GlobalTypeRegistry().Register(Http::StaticType());
            // The handler's shape, so an AngelScript game passes one without declaring it.
            script::RegisterDelegateShape(u8"void HttpHandler(int status, const string &in body, const string &in error)");
            script::RegisterExtraFacadeName(u8"Http"); // so the behavior prelude imports it
            return true;
        }();
        (void)once;
    }
}
