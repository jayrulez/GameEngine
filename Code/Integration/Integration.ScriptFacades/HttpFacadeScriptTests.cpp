// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.http.script - the Http facade on both script backends (Documentation/Specs/
// tls-http-wss.md, P5). A stand-in client records what the scripts send and answers when the test
// says, so the facade is checked without a network: get, post, and send with its headers reach
// the client as asked; handlers hear (status, body, error) on the pump; a cancelled request is
// never answered; the client is made only on the first request; a context with no Http service
// gets 0 back.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.http;
import foundation.http.client;
import foundation.http.script;
import foundation.script;
import foundation.script.facades;
#ifdef OPTION_HAS_ANGELSCRIPT
import foundation.script.angelscript;
#endif
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;
namespace http = foundation::http;

namespace
{
    struct Recorded
    {
        http::HttpRequestId id = 0;
        http::HttpClientRequest request;
        Function<void(const http::HttpClientResponse&)> done;
        bool cancelled = false;
    };

    // Answers only when told; Pump delivers what Answer queued, as a real client does.
    class FakeClient final : public http::IHttpClient
    {
    public:
        explicit FakeClient(Array<Recorded>& log) : m_log(&log) {}
        http::HttpRequestId Send(http::HttpClientRequest request, Function<void(const http::HttpClientResponse&)> done) override
        {
            Recorded r;
            r.id = ++m_next;
            r.request = Move(request);
            r.done = Move(done);
            m_log->PushBack(Move(r));
            return m_next;
        }
        void Cancel(http::HttpRequestId id) override
        {
            for (Recorded& r : *m_log)
                if (r.id == id)
                    r.cancelled = true;
        }
        void Pump() override
        {
            Array<usize> ready = Move(m_ready);
            m_ready.Clear();
            for (const usize i : ready)
                if (!(*m_log)[i].cancelled && (*m_log)[i].done)
                    (*m_log)[i].done(m_answers[i]);
        }
        [[nodiscard]] usize Pending() const override { return 0; }

        void Answer(usize index, http::HttpClientResponse response)
        {
            while (m_answers.Size() <= index)
                m_answers.PushBack(http::HttpClientResponse{});
            m_answers[index] = Move(response);
            m_ready.PushBack(index);
        }

    private:
        Array<Recorded>* m_log;
        Array<http::HttpClientResponse> m_answers;
        Array<usize> m_ready;
        http::HttpRequestId m_next = 0;
    };

    struct Fixture
    {
        Array<Recorded> log;
        FakeClient* client = nullptr;
        u32 made = 0;
        http::HttpScriptBinding binding;
        Fixture()
        {
            binding.SetFactory(
                [this]() -> UniquePtr<http::IHttpClient>
                {
                    ++made;
                    UniquePtr<FakeClient> c = MakeUnique<FakeClient>(DefaultAllocator(), log);
                    client = c.Get();
                    return c;
                });
        }
        static http::HttpClientResponse Response(u16 status, StringView body)
        {
            http::HttpClientResponse r;
            r.status = status;
            for (const utf8char c : body)
                r.body.PushBack(static_cast<byte>(c));
            return r;
        }
    };

    void Check(IScriptManager& manager, StringView script)
    {
        RegisterReflectedTypes(manager);
        RefPtr<IScriptContext> scriptContext = manager.CreateContext();
        Fixture fx;
        http::InstallHttpScriptService(*scriptContext, fx.binding);
        CHECK(fx.made == 0); // nothing until a script asks
        REQUIRE(scriptContext->Load(script, u8"http").IsOk());
        CHECK(fx.made == 1);
        REQUIRE(fx.log.Size() == 4);

        CHECK(fx.log[0].request.method == StringView(u8"GET"));
        CHECK(fx.log[0].request.url == StringView(u8"https://example.com/scores"));
        CHECK(fx.log[1].request.method == StringView(u8"POST"));
        CHECK(http::FindHeader(fx.log[1].request.headers, u8"Content-Type") == StringView(u8"application/json"));
        CHECK(StringView(reinterpret_cast<const utf8char*>(fx.log[1].request.body.Data()), fx.log[1].request.body.Size()) ==
              StringView(u8"{\"score\":9}"));
        CHECK(fx.log[2].request.method == StringView(u8"PUT"));
        REQUIRE(fx.log[2].request.headers.Size() == 2);
        CHECK(http::FindHeader(fx.log[2].request.headers, u8"X-Token") == StringView(u8"abc"));
        CHECK(http::FindHeader(fx.log[2].request.headers, u8"Accept") == StringView(u8"text/plain"));
        CHECK(fx.log[3].cancelled); // the script cancelled its fourth at once

        fx.client->Answer(0, Fixture::Response(200, u8"top: 42"));
        fx.client->Answer(1, Fixture::Response(404, u8"no"));
        http::HttpClientResponse failed;
        failed.error = String(u8"timed out after 30 s");
        fx.client->Answer(2, Move(failed));
        fx.client->Answer(3, Fixture::Response(200, u8"never"));
        CHECK(scriptContext->GetGlobal(u8"Calls").Get<f64>() == doctest::Approx(0.0)); // only on the pump
        fx.binding.Pump();

        CHECK(scriptContext->GetGlobal(u8"Calls").Get<f64>() == doctest::Approx(3.0));
        CHECK(scriptContext->GetGlobal(u8"Scores").Get<String>() == StringView(u8"200 top: 42"));
        CHECK(scriptContext->GetGlobal(u8"Posted").Get<f64>() == doctest::Approx(404.0));
        CHECK(scriptContext->GetGlobal(u8"Why").Get<String>() == StringView(u8"0 timed out after 30 s"));
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("http-facade: AngelScript sends, hears answers on the pump, cancels")
{
    RegisterCoreTypes();
    http::RegisterHttpScriptFacade();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(DefaultAllocator());
    Check(*manager,
          u8"double Calls = 0; string Scores; double Posted = 0; string Why;\n"
          u8"void scores(int status, const string &in body, const string &in error) { Calls++; Scores = status + \" \" + body; }\n"
          u8"void posted(int status, const string &in body, const string &in error) { Calls++; Posted = status; }\n"
          u8"void put(int status, const string &in body, const string &in error) { Calls++; Why = status + \" \" + error; }\n"
          u8"void never(int status, const string &in body, const string &in error) { Calls += 100; }\n"
          u8"void main() {\n"
          u8"  HttpHandler @a = scores; Http::get(\"https://example.com/scores\", a);\n"
          u8"  HttpHandler @b = posted; Http::post(\"https://example.com/scores\", \"application/json\", \"{\\\"score\\\":9}\", b);\n"
          u8"  HttpHandler @c = put; Http::send(\"PUT\", \"https://example.com/me\", \"X-Token: abc\\nAccept: text/plain\\n\", \"\", c);\n"
          u8"  HttpHandler @d = never; Http::cancel(Http::get(\"https://example.com/slow\", d));\n"
          u8"}\n");
}
#endif

#ifdef OPTION_HAS_LUAU
TEST_CASE("http-facade: Luau sends, hears answers on the pump, cancels")
{
    RegisterCoreTypes();
    http::RegisterHttpScriptFacade();
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    Check(*manager,
          u8"Calls = 0; Scores = ''; Posted = 0; Why = ''\n"
          u8"Http.get('https://example.com/scores', function(status, body, err) Calls = Calls + 1; Scores = status .. ' ' .. body end)\n"
          u8"Http.post('https://example.com/scores', 'application/json', '{\"score\":9}', function(status) Calls = Calls + 1; Posted = status end)\n"
          u8"Http.send('PUT', 'https://example.com/me', 'X-Token: abc\\nAccept: text/plain\\n', '', function(status, body, err)\n"
          u8"  Calls = Calls + 1; Why = status .. ' ' .. err end)\n"
          u8"Http.cancel(Http.get('https://example.com/slow', function() Calls = Calls + 100 end))\n");
}
#endif

TEST_CASE("http-facade: a context with no Http service gets 0 back, and nothing is sent")
{
    RegisterCoreTypes();
    http::RegisterHttpScriptFacade();
#ifdef OPTION_HAS_LUAU
    RefPtr<IScriptManager> manager = CreateLuauScriptManager(DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> scriptContext = manager->CreateContext();
    REQUIRE(scriptContext->Load(u8"Id = Http.get('https://example.com/', function() end)\n", u8"none").IsOk());
    CHECK(scriptContext->GetGlobal(u8"Id").Get<f64>() == doctest::Approx(0.0));
#endif
}
