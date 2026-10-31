// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Http.Client.WebCheck - the browser HTTP client checked by hand: a wasm build runs no tests, so
// this page sends a few requests through the fetch backend and prints what came back. Serve its
// folder (python3 -m http.server) and open the page:
//   1. this page itself: 200, its body's size and Content-Type
//   2. a file that is not there: 404 (a response, not an error)
//   3. a POST to the page: the server's answer (python's http.server says 501)
//   4. a cross-origin site without CORS headers: an error, saying CORS may be why
//   5. a request cancelled at once: never answered
// "all answered" ends it.
#include "Core/Prelude.h"
#include <cstdio>
#include <emscripten/emscripten.h>

import foundation.core;
import foundation.http;
import foundation.http.client;

using namespace foundation::core;
using namespace foundation::http;

namespace
{
    UniquePtr<IHttpClient> g_client;
    bool g_cancelledHeard = false;
    bool g_ended = false;

    void Report(const char* what, const HttpClientResponse& r)
    {
        if (!r.Ok())
        {
            std::printf("%s: error: %s\n", what, reinterpret_cast<const char*>(r.error.CStr()));
            return;
        }
        const String type(r.Header(u8"Content-Type"));
        std::printf("%s: %u, %zu bytes, Content-Type '%s'\n", what, static_cast<unsigned>(r.status),
                    static_cast<size_t>(r.body.Size()), reinterpret_cast<const char*>(type.CStr()));
    }

    void Frame()
    {
        g_client->Pump();
        if (!g_ended && g_client->Pending() == 0)
        {
            g_ended = true;
            std::printf(g_cancelledHeard ? "FAIL: the cancelled request was answered\n" : "the cancelled request was never answered\n");
            std::printf("all answered\n");
            emscripten_cancel_main_loop();
        }
    }
}

int main()
{
    const String origin(StringView(reinterpret_cast<const utf8char*>(emscripten_run_script_string("location.origin"))));
    const String page(StringView(reinterpret_cast<const utf8char*>(emscripten_run_script_string("location.href"))));
    std::printf("Http.Client.WebCheck at %s\n", reinterpret_cast<const char*>(page.CStr()));
    g_client = CreateHttpClient(DefaultAllocator(), nullptr);

    (void)g_client->Send(HttpClientRequest::Get(page.AsView()), [](const HttpClientResponse& r) { Report("1. this page", r); });
    (void)g_client->Send(HttpClientRequest::Get(Format(u8"{}/not-there.txt", origin.AsView()).AsView()),
                         [](const HttpClientResponse& r) { Report("2. a missing file", r); });
    (void)g_client->Send(HttpClientRequest::Post(page.AsView(), u8"text/plain", u8"hello"),
                         [](const HttpClientResponse& r) { Report("3. a POST", r); });
    HttpClientRequest cross = HttpClientRequest::Get(u8"https://example.com/");
    cross.timeoutSeconds = 10.0f;
    (void)g_client->Send(Move(cross), [](const HttpClientResponse& r) { Report("4. cross-origin, no CORS", r); });
    const HttpRequestId cancelled = g_client->Send(HttpClientRequest::Get(page.AsView()), [](const HttpClientResponse&) { g_cancelledHeard = true; });
    g_client->Cancel(cancelled);

    emscripten_set_main_loop(&Frame, 0, false);
    return 0;
}
