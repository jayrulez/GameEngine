// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
using foundation::json::JsonValue;
namespace json = foundation::json;

namespace
{
    // Parse an answered line into a JsonValue (must be answered + valid JSON).
    JsonValue Response(const LineOutcome& line)
    {
        REQUIRE(line.state == LineState::Answered);
        json::ParseResult p = json::Parse(line.response.AsView());
        REQUIRE(p.ok);
        return p.value;
    }

    bool Contains(StringView hay, StringView needle)
    {
        if (needle.Size() == 0 || needle.Size() > hay.Size())
        {
            return needle.Size() == 0;
        }
        for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
        {
            bool match = true;
            for (usize j = 0; j < needle.Size(); ++j)
            {
                if (hay.Data()[i + j] != needle.Data()[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return true;
            }
        }
        return false;
    }

    // A server with an echo tool (validated arg), an always-failing tool, and one resource.
    void Setup(McpServer& s)
    {
        s.RegisterTool(u8"echo", u8"Echoes the message back",
                       SchemaBuilder().Str(u8"message", u8"text to echo", true).Build(),
                       foundation::mcp::ToolAnnotations::ReadOnly(),
                       [](const JsonValue& args) -> ToolResult
                       {
                           JsonValue out = JsonValue::MakeObject();
                           out.Set(u8"echoed", JsonValue::MakeString(args.Get(u8"message").AsString()));
                           return out;
                       });
        s.RegisterTool(u8"fail", u8"Always fails", SchemaBuilder().Build(),
        foundation::mcp::ToolAnnotations::ReadOnly(),
                       [](const JsonValue&) -> ToolResult
                       { return Err(String(u8"cook failed: bad input")); });
        s.RegisterResource(u8"mem://greeting", u8"greeting", u8"text/plain", u8"a greeting",
                           []() -> Result<String, String> { return String(u8"hello"); });
    }
}

// --- Lifecycle -------------------------------------------------------------

TEST_CASE("mcp: initialize returns the pinned version + {tools,resources} caps + serverInfo")
{
    McpServer s;
    Setup(s);
    JsonValue r = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}"));
    CHECK(r.Get(u8"jsonrpc").AsString() == StringView(u8"2.0"));
    CHECK(r.Get(u8"id").AsInt() == 1);
    const JsonValue res = r.Get(u8"result");
    CHECK(res.Get(u8"protocolVersion").AsString() == kProtocolVersion);
    const JsonValue caps = res.Get(u8"capabilities");
    CHECK(caps.Has(u8"tools"));
    CHECK(caps.Has(u8"resources"));
    CHECK_FALSE(caps.Has(u8"prompts")); // exactly {tools, resources}
    CHECK_FALSE(caps.Has(u8"sampling"));
    CHECK(res.Get(u8"serverInfo").Get(u8"name").IsString());
}

TEST_CASE("mcp: notifications never get a response (initialized + unknown/cancelled)")
{
    McpServer s;
    CHECK(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")
              .state == LineState::Notification);
    CHECK(s.HandleLine(
               u8"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{}}")
              .state == LineState::Notification);
    CHECK(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"method\":\"totally/unknown\"}").state ==
          LineState::Notification);
}

TEST_CASE("mcp: ping")
{
    McpServer s;
    JsonValue r = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"ping\"}"));
    CHECK(r.Has(u8"result"));
}

TEST_CASE("mcp: request id type (string vs number) is preserved verbatim")
{
    McpServer s;
    JsonValue rn = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"ping\"}"));
    CHECK(rn.Get(u8"id").IsNumber());
    CHECK(rn.Get(u8"id").AsInt() == 42);
    JsonValue rs = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"ping\"}"));
    CHECK(rs.Get(u8"id").IsString());
    CHECK(rs.Get(u8"id").AsString() == StringView(u8"abc"));
}

// --- Tools -----------------------------------------------------------------

TEST_CASE("mcp: tools/list emits names, descriptions, and inputSchemas")
{
    McpServer s;
    Setup(s);
    JsonValue r = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
    const JsonValue tools = r.Get(u8"result").Get(u8"tools");
    CHECK(tools.Count() == 2);
    const JsonValue echo = tools.At(0);
    CHECK(echo.Get(u8"name").AsString() == StringView(u8"echo"));
    const JsonValue schema = echo.Get(u8"inputSchema");
    CHECK(schema.Get(u8"type").AsString() == StringView(u8"object"));
    CHECK(schema.Get(u8"properties").Has(u8"message"));
    CHECK(schema.Get(u8"required").At(0).AsString() == StringView(u8"message"));
}

TEST_CASE("mcp: tools/list emits every tool's annotations - a read-only tool and an overwriting "
          "one say so in the MCP hints")
{
    McpServer s;
    Setup(s); // echo + fail are read-only
    s.RegisterTool(u8"clobber", u8"Replaces a thing", SchemaBuilder().Build(),
                   ToolAnnotations::Overwrites(),
                   [](const JsonValue&) -> ToolResult { return JsonValue::MakeObject(); });
    JsonValue r = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
    const JsonValue tools = r.Get(u8"result").Get(u8"tools");
    REQUIRE(tools.Count() == 3);
    const auto find = [&tools](StringView name) -> JsonValue
    {
        for (usize i = 0; i < static_cast<usize>(tools.Count()); ++i)
        {
            if (tools.At(i).Get(u8"name").AsString().AsView() == name)
            {
                return tools.At(i).Get(u8"annotations");
            }
        }
        return JsonValue::MakeNull();
    };
    JsonValue echo = find(u8"echo");
    REQUIRE(echo.IsObject());
    CHECK(echo.Get(u8"readOnlyHint").AsBool());
    CHECK_FALSE(echo.Get(u8"destructiveHint").AsBool());
    CHECK(echo.Get(u8"idempotentHint").AsBool());
    CHECK_FALSE(echo.Get(u8"openWorldHint").AsBool());
    JsonValue clobber = find(u8"clobber");
    REQUIRE(clobber.IsObject());
    CHECK_FALSE(clobber.Get(u8"readOnlyHint").AsBool());
    CHECK(clobber.Get(u8"destructiveHint").AsBool());
    CHECK(clobber.Get(u8"idempotentHint").AsBool());

    // A delete is destructive, and again finds nothing more to remove (Sedulous ac63071d).
    const ToolAnnotations deletes = ToolAnnotations::Deletes();
    CHECK_FALSE(deletes.readOnly);
    CHECK(deletes.destructive);
    CHECK(deletes.idempotent);
}

TEST_CASE("mcp: tools/call round-trips through the registry (result is JSON-stringified text)")
{
    McpServer s;
    Setup(s);
    JsonValue r = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"message\":\"hi\"}}}"));
    const JsonValue res = r.Get(u8"result");
    CHECK(res.Get(u8"isError").AsBool() == false);
    const JsonValue content = res.Get(u8"content");
    CHECK(content.Count() == 1);
    CHECK(content.At(0).Get(u8"type").AsString() == StringView(u8"text"));
    const JsonValue payload = JsonValue::Parse(content.At(0).Get(u8"text").AsString());
    CHECK(payload.Get(u8"echoed").AsString() == StringView(u8"hi"));
}

TEST_CASE("mcp: a TOOL failure is a successful response with isError:true + the real text")
{
    McpServer s;
    Setup(s);
    JsonValue r = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"fail\"}}"));
    CHECK(r.Has(u8"result"));      // NOT a protocol error
    CHECK_FALSE(r.Has(u8"error"));
    CHECK(r.Get(u8"result").Get(u8"isError").AsBool() == true);
    CHECK(r.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString() ==
          StringView(u8"cook failed: bad input"));
}

TEST_CASE("mcp: schema-validation failure is a PROTOCOL -32602 naming the field")
{
    McpServer s;
    Setup(s);
    // Missing required 'message'.
    JsonValue r = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{}}}"));
    CHECK(r.Has(u8"error"));
    CHECK(r.Get(u8"error").Get(u8"code").AsInt() == -32602);
    CHECK(Contains(r.Get(u8"error").Get(u8"message").AsString().AsView(), u8"message"));
    // Wrong type for 'message'.
    JsonValue r2 = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"message\":5}}}"));
    CHECK(r2.Get(u8"error").Get(u8"code").AsInt() == -32602);
    CHECK(Contains(r2.Get(u8"error").Get(u8"message").AsString().AsView(), u8"message"));
    // Unknown tool -> -32602.
    JsonValue r3 = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"nope\"}}"));
    CHECK(r3.Get(u8"error").Get(u8"code").AsInt() == -32602);
}

TEST_CASE("mcp: an argument the schema does not declare is refused, naming the tool and what it takes")
{
    McpServer s;
    Setup(s);
    s.RegisterTool(u8"open", u8"Takes any field", []()
                   {
                       JsonValue schema = SchemaBuilder().Str(u8"name").Build();
                       schema.Set(u8"additionalProperties", JsonValue::MakeBool(true));
                       return schema;
                   }(),
                   foundation::mcp::ToolAnnotations::ReadOnly(),
                   [](const JsonValue& args) -> ToolResult
                   {
                       JsonValue out = JsonValue::MakeObject();
                       out.Set(u8"fields", JsonValue::MakeNumber(static_cast<f64>(args.Count())));
                       return out;
                   });
    // A misspelt argument: refused, not dropped, so the call never runs without it.
    JsonValue r = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
        u8"\"arguments\":{\"message\":\"hi\",\"mesage\":\"hi\"}}}"));
    REQUIRE(r.Has(u8"error"));
    CHECK(r.Get(u8"error").Get(u8"code").AsInt() == -32602);
    CHECK(r.Get(u8"error").Get(u8"message").AsString() ==
          StringView(u8"echo: no argument 'mesage' (it takes: message)"));
    // A tool that takes nothing says so.
    JsonValue r2 = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"fail\","
        u8"\"arguments\":{\"force\":true}}}"));
    CHECK(r2.Get(u8"error").Get(u8"message").AsString() == StringView(u8"fail: no argument 'force' (it takes none)"));
    // A schema that declares additionalProperties:true takes any field, and still types the declared ones.
    JsonValue r3 = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"open\","
        u8"\"arguments\":{\"name\":\"a\",\"extra\":1}}}"));
    REQUIRE(r3.Has(u8"result"));
    CHECK(Contains(r3.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString().AsView(), u8"2"));
    JsonValue r4 = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"open\","
        u8"\"arguments\":{\"name\":5}}}"));
    CHECK(r4.Get(u8"error").Get(u8"code").AsInt() == -32602);
}

// --- Subset rejections -----------------------------------------------------

TEST_CASE("mcp: unknown method is -32601")
{
    McpServer s;
    JsonValue r = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"prompts/list\"}"));
    CHECK(r.Get(u8"error").Get(u8"code").AsInt() == -32601);
}

TEST_CASE("mcp: a top-level batch array is rejected -32600 (id null)")
{
    McpServer s;
    JsonValue r = Response(s.HandleLine(u8"[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]"));
    CHECK(r.Get(u8"error").Get(u8"code").AsInt() == -32600);
    CHECK(r.Get(u8"id").IsNull());
}

// --- Resources -------------------------------------------------------------

TEST_CASE("mcp: resources/list + resources/read; unknown uri -> -32602")
{
    McpServer s;
    Setup(s);
    JsonValue l = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/list\"}"));
    const JsonValue resources = l.Get(u8"result").Get(u8"resources");
    CHECK(resources.Count() == 1);
    CHECK(resources.At(0).Get(u8"uri").AsString() == StringView(u8"mem://greeting"));

    JsonValue rd = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/read\",\"params\":{\"uri\":\"mem://greeting\"}}"));
    CHECK(rd.Get(u8"result").Get(u8"contents").At(0).Get(u8"text").AsString() == StringView(u8"hello"));

    JsonValue miss = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\",\"params\":{\"uri\":\"mem://nope\"}}"));
    CHECK(miss.Get(u8"error").Get(u8"code").AsInt() == -32602);
}

TEST_CASE("mcp: a resource PROVIDER contributes a dynamic set (list + read + mime + miss)")
{
    McpServer s;
    Setup(s); // the static mem://greeting
    // A provider whose set changes between calls - exactly what static registration can't do.
    Array<String> names;
    names.PushBack(String(u8"alpha"));
    ResourceProvider provider;
    provider.list = [&names](Array<Resource>& out)
    {
        for (const String& n : names)
        {
            Resource r;
            r.uri = Format(u8"dyn://{}", n.AsView());
            r.name = n;
            r.mimeType = String(u8"application/xml");
            r.description = String(u8"dynamic entry");
            out.PushBack(Move(r));
        }
    };
    provider.read = [&names](StringView uri) -> Optional<Result<String, String>>
    {
        for (const String& n : names)
        {
            if (uri == Format(u8"dyn://{}", n.AsView()).AsView())
            {
                return Result<String, String>(Format(u8"content-of-{}", n.AsView()));
            }
        }
        return {}; // not ours
    };
    s.RegisterResourceProvider(Move(provider));

    // list = static + the provider's CURRENT entries.
    JsonValue l1 = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/list\"}"));
    CHECK(l1.Get(u8"result").Get(u8"resources").Count() == 2);

    // The set grows without re-registration.
    names.PushBack(String(u8"beta"));
    JsonValue l2 = Response(s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/list\"}"));
    CHECK(l2.Get(u8"result").Get(u8"resources").Count() == 3);

    // Reads route through the provider and carry ITS declared mime type.
    JsonValue rd = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\",\"params\":{\"uri\":\"dyn://beta\"}}"));
    const JsonValue entry = rd.Get(u8"result").Get(u8"contents").At(0);
    CHECK(entry.Get(u8"text").AsString() == StringView(u8"content-of-beta"));
    CHECK(entry.Get(u8"mimeType").AsString() == StringView(u8"application/xml"));

    // Static reads still work, and a uri no one owns is still -32602.
    JsonValue st = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/read\",\"params\":{\"uri\":\"mem://greeting\"}}"));
    CHECK(st.Get(u8"result").Get(u8"contents").At(0).Get(u8"text").AsString() == StringView(u8"hello"));
    JsonValue miss = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/read\",\"params\":{\"uri\":\"dyn://gamma\"}}"));
    CHECK(miss.Get(u8"error").Get(u8"code").AsInt() == -32602);
}

// --- Framing / transport ---------------------------------------------------

TEST_CASE("mcp: framing survives garbage (-32700) and drives multiple messages; loop stays alive")
{
    McpServer s;
    Setup(s);
    InMemoryTransport t;
    t.Push(String(u8"this is not json"));                                        // -> -32700
    t.Push(String(u8"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")); // -> nothing
    t.Push(String(u8"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}"));       // -> result
    Serve(s, t);

    REQUIRE(t.OutputCount() == 2); // the notification produced no line
    JsonValue e = json::Parse(t.Output(0).AsView()).value;
    CHECK(e.Get(u8"error").Get(u8"code").AsInt() == -32700);
    CHECK(e.Get(u8"id").IsNull());
    JsonValue p = json::Parse(t.Output(1).AsView()).value;
    CHECK(p.Get(u8"id").AsInt() == 7);
    CHECK(p.Has(u8"result"));
}

// --- Observer -------------------------------------------------------------

TEST_CASE("mcp: the tool observer hears every finished tools/call by name, with its outcome, "
          "and nothing else")
{
    McpServer s;
    Setup(s);
    Array<String> heard;
    s.SetToolObserver([&heard](StringView tool, bool isError)
                      { heard.PushBack(Format(u8"{}:{}", tool, isError ? u8"err" : u8"ok")); });

    (void)s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":"
                       u8"{\"name\":\"echo\",\"arguments\":{\"message\":\"hi\"}}}");
    (void)s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                       u8"{\"name\":\"fail\"}}");
    // Not a finished tool run: a schema refusal (-32602), an unknown tool, a ping.
    (void)s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
                       u8"{\"name\":\"echo\",\"arguments\":{}}}");
    (void)s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
                       u8"{\"name\":\"nope\"}}");
    (void)s.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}");

    REQUIRE(heard.Size() == 2);
    CHECK(heard[0] == u8"echo:ok");
    CHECK(heard[1] == u8"fail:err");
}

// --- Not finished ---------------------------------------------------------

namespace
{
    // A tool that answers on its `answerOnCall`-th entry and says "not finished" before that:
    // the shape of a tool waiting on a background job, minus the job.
    struct SlowTool
    {
        u32 calls = 0;
        u32 answerOnCall = 3;
    };

    void RegisterSlow(McpServer& s, SlowTool& slow)
    {
        s.RegisterTool(u8"slow", u8"Answers after a few re-entries", SchemaBuilder().Build(),
        foundation::mcp::ToolAnnotations::ReadOnly(),
                       [&slow](const JsonValue&) -> ToolOutcome
                       {
                           ++slow.calls;
                           if (slow.calls < slow.answerOnCall)
                           {
                               return ToolOutcome::NotFinished();
                           }
                           JsonValue out = JsonValue::MakeObject();
                           out.Set(u8"calls", JsonValue::MakeNumber(static_cast<f64>(slow.calls)));
                           return out;
                       });
    }

    constexpr StringView kSlowCall =
        u8"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"slow\"}}";
}

TEST_CASE("mcp: a tool that is not finished makes HandleLine say so, until the same line lands "
          "its answer")
{
    McpServer s;
    SlowTool slow;
    RegisterSlow(s, slow);

    LineOutcome first = s.HandleLine(kSlowCall);
    CHECK(first.state == LineState::NotFinished);
    CHECK(first.response.IsEmpty());
    LineOutcome second = s.HandleLine(kSlowCall);
    CHECK(second.state == LineState::NotFinished);
    JsonValue r = Response(s.HandleLine(kSlowCall));
    CHECK(r.Get(u8"id").AsInt() == 9);
    CHECK(r.Get(u8"result").Get(u8"isError").AsBool() == false);
    CHECK(Contains(r.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString().AsView(),
                   u8"\"calls\":3"));
    CHECK(slow.calls == 3);

    // A finished tool is unaffected: its ToolResult converts to a finished outcome.
    Setup(s);
    JsonValue e = Response(s.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\","
        u8"\"arguments\":{\"message\":\"hi\"}}}"));
    CHECK(e.Get(u8"result").Get(u8"isError").AsBool() == false);
}

TEST_CASE("mcp: Serve re-enters a not-finished line until it answers - one output line, the "
          "handler entered once per attempt")
{
    McpServer s;
    Setup(s);
    SlowTool slow;
    slow.answerOnCall = 4;
    RegisterSlow(s, slow);
    InMemoryTransport t;
    t.Push(String(kSlowCall));
    t.Push(String(u8"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}"));
    Serve(s, t);

    REQUIRE(t.OutputCount() == 2); // the waits produced no line; the ping still got through
    CHECK(slow.calls == 4);
    JsonValue r = json::Parse(t.Output(0).AsView()).value;
    CHECK(r.Get(u8"id").AsInt() == 9);
    CHECK(Contains(r.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString().AsView(),
                   u8"\"calls\":4"));
    CHECK(json::Parse(t.Output(1).AsView()).value.Get(u8"id").AsInt() == 7);
}

// Sedulous d509904a: an unfinished call has an identity across its re-entries (the transport's
// id, or its line without one), its own state, destroyed when it answers or its caller leaves.
namespace
{
    struct CallCounter final : ToolCallState
    {
        explicit CallCounter(u32* alive) : alive(alive) { ++*alive; }
        ~CallCounter() override { --*alive; }
        u32* alive;
        u32 entries = 0;
    };
}

TEST_CASE("mcp: an unfinished call keeps its own state across its re-entries, until it answers "
          "or is abandoned")
{
    McpServer server;
    u32 alive = 0;
    server.RegisterTool(
        u8"slow", u8"answers on its third entry", SchemaBuilder().Build(), ToolAnnotations::ReadOnly(),
        [&alive](ToolCall& call, const JsonValue&) -> ToolOutcome
        {
            if (!call.isReentry)
            {
                CHECK(call.state.Get() == nullptr);
                call.state = MakeUnique<CallCounter>(DefaultAllocator(), &alive);
            }
            CallCounter* counter = call.State<CallCounter>();
            REQUIRE(counter != nullptr);
            if (++counter->entries < 3)
            {
                return ToolOutcome::NotFinished();
            }
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"entries", JsonValue::MakeNumber(counter->entries));
            out.Set(u8"id", JsonValue::MakeNumber(static_cast<f64>(call.id)));
            return ToolResult(Move(out));
        });
    const StringView line =
        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"slow\",\"arguments\":{}}}";

    // Two identical calls in flight at once, told apart by their ids.
    CHECK(server.HandleLine(line, 7).state == LineState::NotFinished);
    CHECK(server.HandleLine(line, 8).state == LineState::NotFinished);
    CHECK(server.CallsInFlight() == 2u);
    CHECK(alive == 2u);
    CHECK(server.HandleLine(line, 7).state == LineState::NotFinished);
    const LineOutcome seven = server.HandleLine(line, 7); // its third entry: it answers
    REQUIRE(seven.state == LineState::Answered);
    const JsonValue result = Response(seven).Get(u8"result");
    const JsonValue payload = json::Parse(result.Get(u8"content").At(0).Get(u8"text").AsString().AsView()).value;
    CHECK(payload.Get(u8"entries").AsNumber() == doctest::Approx(3.0));
    CHECK(payload.Get(u8"id").AsNumber() == doctest::Approx(7.0));
    CHECK(server.CallsInFlight() == 1u); // answered: its state went
    CHECK(alive == 1u);

    // The other's caller leaves: its call ends and its state goes with it.
    server.AbandonCall(8);
    CHECK(server.CallsInFlight() == 0u);
    CHECK(alive == 0u);
    server.AbandonCall(99); // no such call: nothing

    // Without an id a call is known by its line, as before.
    CHECK(server.HandleLine(line).state == LineState::NotFinished);
    CHECK(server.HandleLine(line).state == LineState::NotFinished);
    CHECK(server.HandleLine(line).state == LineState::Answered);
    CHECK(server.CallsInFlight() == 0u);
    CHECK(alive == 0u);
}
