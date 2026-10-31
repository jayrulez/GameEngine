// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the MCP host over real loopback against a scratch project. Covers the token
// file, host_info's editor state, project_info on the live project, the tool surface (the shared
// engine tools and host_info, with none of the stdio host's own), the bearer token refusal, and
// the report of finished calls.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.net;
import foundation.http;
import foundation.json;
import foundation.mcp;
import foundation.mcp.http;
import pipeline.core;
import editor.core;
import pipeline.importer; // the import framework (a consumer imports it itself)
import editor.mcp;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace http = foundation::http;
namespace json = foundation::json;
using json::JsonValue;

namespace
{
    http::HttpRequest Post(StringView token, StringView body)
    {
        http::HttpRequest r;
        r.method = String(u8"POST");
        r.target = String(u8"/mcp");
        if (!token.IsEmpty())
        {
            r.headers.PushBack(
                http::HttpHeader{String(u8"Authorization"), Format(u8"Bearer {}", token)});
        }
        r.body.Resize(body.Size());
        for (usize i = 0; i < body.Size(); ++i)
        {
            r.body[i] = static_cast<byte>(body[i]);
        }
        return r;
    }

    // One JSON-RPC call from the client thread: the parsed response, or null on a transport
    // failure (the assertions then read as "no answer").
    JsonValue Call(u16 port, StringView token, StringView body)
    {
        Result<http::HttpResponse, String> r = http::HttpFetch(u8"127.0.0.1", port, Post(token, body));
        if (!r.HasValue() || r.Value().status != 200)
        {
            return JsonValue::MakeNull();
        }
        return json::Parse(r.Value().BodyText()).value;
    }

    // A finished tool's JSON payload out of the tools/call envelope.
    JsonValue Payload(const JsonValue& response)
    {
        return json::Parse(
                   response.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString().AsView())
            .value;
    }

    String ReadText(StringView path)
    {
        Result<Array<byte>> bytes = ReadFile(path);
        if (!bytes.HasValue())
        {
            return String();
        }
        return String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()),
                                 bytes.Value().Size()));
    }

    // A page that counts how often its asset changed under it.
    class WatchingPage final : public EditorPage
    {
    public:
        explicit WatchingPage(const Guid& asset) : EditorPage(DefaultAllocator())
        {
            SetInstanceId(asset);
        }
        [[nodiscard]] StringView Title() const override { return u8"watching"; }
        [[nodiscard]] Status Save() override { return Status{}; }
        void OnAssetExternallyModified() override { ++told; }
        u32 told = 0;
    };
}

TEST_CASE("editor-mcp-host: serves the engine surface over the live project on loopback, "
          "writes the token file, refuses a wrong token, and reports its finished calls")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_host_project", ec);
    std::filesystem::remove_all("mcp_host_userdata", ec);
    REQUIRE(EditorProject::Create(DefaultAllocator(), u8"mcp_host_project", u8"Hosted").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), u8"mcp_host_project");
    REQUIRE(project);
    EditorLogBuffer logBuffer{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};

    editor::mcp::ProjectSession session;
    session.project = project.Get();
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    EditorContext context{DefaultAllocator()};
    // A domain's contribution (registered at boot) reaches the served surface.
    context.RegisterMcpToolContribution(
        [](foundation::mcp::McpServer& server)
        {
            server.RegisterTool(u8"contributed_tool", u8"from a domain",
                                foundation::mcp::SchemaBuilder().Build(),
                                foundation::mcp::ToolAnnotations::ReadOnly(),
                                [](const JsonValue&) -> foundation::mcp::ToolResult
                                { return JsonValue::MakeObject(); });
        });
    app::EditorMcpHost host(DefaultAllocator(), context, session, logBuffer, builders, importers,
                            context.Creators(), editor::mcp::EngineToolPaths{}, operations,
                            String(u8"test-stamp"));
    // The host wires a tool's write over a source asset to the open pages editing it, the way
    // any change made outside a page reaches them.
    {
        Random rng(5);
        const Guid edited = Guid::Generate(rng);
        auto* page = static_cast<WatchingPage*>(context.AdoptPage(UniquePtr<EditorPage>(
            DefaultAllocator().New<WatchingPage>(edited), DefaultAllocator())));
        REQUIRE(session.onAssetWritten);
        session.onAssetWritten(edited);
        CHECK(page->told == 1u);
        session.onAssetWritten(Guid::Generate(rng));
        CHECK(page->told == 1u);
        context.ClosePage(page);
    }
    Array<String> finished;
    host.OnToolFinished = [&finished](StringView tool, bool isError)
    { finished.PushBack(Format(u8"{}:{}", tool, isError ? u8"err" : u8"ok")); };
    CHECK_FALSE(host.IsRunning());

    app::EditorMcpHostConfig config;
    config.port = 0;
    config.token = String(u8"sekrit");
    config.tokenFileDirectory = String(u8"mcp_host_userdata");
    REQUIRE(host.Start(config));
    REQUIRE(host.IsRunning());
    const u16 port = host.BoundPort();
    REQUIRE(port != 0);
    CHECK(ReadText(u8"mcp_host_userdata/mcp-token") == u8"sekrit");

    struct Outcome
    {
        JsonValue info;
        JsonValue project;
        JsonValue tools;
        bool wrongTokenRefused = false;
    } outcome;
    bool done = false;
    Thread client(
        [&]
        {
            outcome.info = Call(port, u8"sekrit",
                                u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                u8"\"params\":{\"name\":\"host_info\",\"arguments\":{}}}");
            outcome.project = Call(port, u8"sekrit",
                                   u8"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                                   u8"\"params\":{\"name\":\"project_info\",\"arguments\":{}}}");
            outcome.tools =
                Call(port, u8"sekrit", u8"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
            Result<http::HttpResponse, String> refused =
                http::HttpFetch(u8"127.0.0.1", port, Post(u8"wrong", u8"{}"));
            outcome.wrongTokenRefused = refused.HasValue() && refused.Value().status == 401;
            done = true;
        });
    for (u32 i = 0; i < 5000 && !done; ++i)
    {
        host.Pump();
        SleepMilliseconds(1);
    }
    client.Join();

    // host_info: this host's identity and the live project.
    REQUIRE(outcome.info.IsObject());
    JsonValue info = Payload(outcome.info);
    CHECK(info.Get(u8"serverName").AsString() == StringView(u8"engine-editor-mcp"));
    CHECK(info.Get(u8"buildStamp").AsString() == StringView(u8"test-stamp"));
    CHECK(info.Get(u8"host").Get(u8"kind").AsString() == StringView(u8"editor"));
    CHECK(info.Get(u8"host").Get(u8"projectOpen").AsBool());
    CHECK(info.Get(u8"host").Get(u8"projectName").AsString() == StringView(u8"Hosted"));
    // project_info answers for the same project object the editor holds.
    REQUIRE(outcome.project.IsObject());
    CHECK(Payload(outcome.project).Get(u8"name").AsString() == StringView(u8"Hosted"));
    // The surface: the shared engine tools, host_info, and the domain's contribution; never
    // the stdio host's project_open.
    REQUIRE(outcome.tools.IsObject());
    const JsonValue tools = outcome.tools.Get(u8"result").Get(u8"tools");
    CHECK(static_cast<usize>(tools.Count()) == editor::mcp::kEngineToolCount + 2);
    bool hasHostInfo = false;
    bool hasProjectOpen = false;
    bool hasContributed = false;
    for (usize i = 0; i < static_cast<usize>(tools.Count()); ++i)
    {
        const String name = tools.At(i).Get(u8"name").AsString(); // AsString returns by value
        hasHostInfo = hasHostInfo || name == u8"host_info";
        hasProjectOpen = hasProjectOpen || name == u8"project_open";
        hasContributed = hasContributed || name == u8"contributed_tool";
    }
    CHECK(hasHostInfo);
    CHECK(hasContributed);
    CHECK_FALSE(hasProjectOpen);
    CHECK(outcome.wrongTokenRefused);
    // Every finished call was reported, in order.
    REQUIRE(finished.Size() == 2u);
    CHECK(finished[0] == u8"host_info:ok");
    CHECK(finished[1] == u8"project_info:ok");

    host.Stop();
    CHECK_FALSE(host.IsRunning());
    std::filesystem::remove_all("mcp_host_project", ec);
    std::filesystem::remove_all("mcp_host_userdata", ec);
}

TEST_CASE("editor-mcp-host: an empty token never serves")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_host_project2", ec);
    REQUIRE(EditorProject::Create(DefaultAllocator(), u8"mcp_host_project2", u8"Hosted").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), u8"mcp_host_project2");
    REQUIRE(project);
    EditorLogBuffer logBuffer{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    session.project = project.Get();
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    EditorContext context{DefaultAllocator()};
    {
        app::EditorMcpHost host(DefaultAllocator(), context, session, logBuffer, builders,
                                importers, context.Creators(), editor::mcp::EngineToolPaths{},
                                operations, String(u8"test-stamp"));
        app::EditorMcpHostConfig config; // no token
        CHECK_FALSE(host.Start(config));
        CHECK_FALSE(host.IsRunning());
        CHECK(static_cast<bool>(session.onAssetWritten)); // wired while the host lives
    }
    // The session outlives the host; what the host wired on it went with the host.
    CHECK_FALSE(static_cast<bool>(session.onAssetWritten));
    std::filesystem::remove_all("mcp_host_project2", ec);
}
