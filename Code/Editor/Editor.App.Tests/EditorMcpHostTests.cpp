// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the MCP host over real loopback against a scratch project. Covers the token
// file, host_info's editor state, project_info on the live project, the tool surface (the shared
// engine tools and host_info, with none of the stdio host's own), the bearer token refusal, and
// the report of finished calls.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.net;
import foundation.http;
import foundation.json;
import foundation.mcp;
import foundation.mcp.http;
import foundation.settings;
import pipeline.core;
import editor.core;
import pipeline.importer; // the import framework (a consumer imports it itself)
import editor.mcp;
import editor.app;
import editor.project;     // ExportPluginServices (the hooks the session carries)
import foundation.runtime; // LinkedPlugin (the plugins the session offers)

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
    (void)RemoveDirectoryRecursive(u8"mcp_host_project");
    (void)RemoveDirectoryRecursive(u8"mcp_host_userdata");
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
    host.OnCall = [&finished](const foundation::mcp::ToolCallReport& report)
    { finished.PushBack(Format(u8"{}:{}", report.tool, report.isError ? u8"err" : u8"ok")); };
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
    CHECK(info.Get(u8"serverName").AsString() == StringView(ENGINE_ID u8"-editor-mcp"));
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
    (void)RemoveDirectoryRecursive(u8"mcp_host_project");
    (void)RemoveDirectoryRecursive(u8"mcp_host_userdata");
}

TEST_CASE("editor-mcp-host: an empty token never serves")
{
    (void)RemoveDirectoryRecursive(u8"mcp_host_project2");
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
    (void)RemoveDirectoryRecursive(u8"mcp_host_project2");
}

TEST_CASE("editor-mcp-host: with no project the host serves the manager; the engine's tools refuse, host_info says so")
{
    const String userData = PathJoin(GetCurrentDirectory().AsView(), u8"mcp_manager_userdata");
    (void)RemoveDirectoryRecursive(userData.AsView());
    RegisterEditorSettingsTypes();
    EditorLogBuffer logBuffer{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    EditorContext context{DefaultAllocator()};
    foundation::settings::Settings settings{DefaultAllocator()};
    settings.Section<EditorMcpSettings>().token = String(u8"sekrit"); // none minted, so nothing is saved

    const auto make = [&](bool forced)
    {
        app::EditorMcpService::Services services;
        services.context = &context;
        services.builders = &builders;
        services.userSettings = &settings;
        services.log = &logBuffer;
        services.forceEnabled = forced; // --mcp
        services.screen = []() { return StringView(u8"projectManager"); };
        services.userDataDirectory = userData; // the token file goes here, never to the real one
        return services;
    };
    {
        // Disabled (no preference, no --mcp): no host, with or without a project.
        app::EditorMcpService disabled(DefaultAllocator(), make(false));
        CHECK_FALSE(disabled.EnsureRunning());
        CHECK_FALSE(disabled.IsRunning());
        // The user's Start (the MCP panel's) runs it whatever the preference says; their Stop holds,
        // over the manager and every project, until they start it again or the preferences change.
        REQUIRE(disabled.StartByUser(nullptr));
        CHECK(disabled.Port() != 0);
        CHECK(disabled.ConnectCommand().AsView().Contains(Format(u8"http://127.0.0.1:{}/mcp", disabled.Port()).AsView()));
        disabled.StopByUser();
        CHECK_FALSE(disabled.IsRunning());
        CHECK(disabled.IsStopHeld());
        CHECK(disabled.Port() == 0);
        CHECK(disabled.ConnectCommand().IsEmpty());
        CHECK_FALSE(disabled.EnsureRunning());
        disabled.Restart(nullptr); // the preferences changed, and they say off
        CHECK_FALSE(disabled.IsStopHeld());
        CHECK_FALSE(disabled.IsRunning());
        // The activity kept the host's own events.
        REQUIRE(disabled.Activity().Size() == 2u);
        CHECK(disabled.Activity().At(0).tool.AsView().StartsWith(u8"Listening on 127.0.0.1:"));
        CHECK(disabled.Activity().At(1).tool == u8"Stopped");
    }

    app::EditorMcpService service(DefaultAllocator(), make(true));
    REQUIRE(service.EnsureRunning());
    app::EditorMcpHost* host = service.Host();
    REQUIRE(host != nullptr);
    CHECK(service.EnsureRunning()); // already running: the same host, not a second
    CHECK(service.Host() == host);
    const u16 port = host->BoundPort();

    struct Outcome
    {
        JsonValue info, assets, page, validate, pages, log;
    } got;
    bool done = false;
    const auto call = [port](StringView tool, StringView args)
    {
        return Call(port, u8"sekrit",
                    Format(u8"{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{{\"name\":\"{}\",\"arguments\":{}}}}}", tool, args)
                        .AsView());
    };
    Thread client(
        [&]
        {
            (void)Call(port, u8"sekrit",
                       u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\","
                       u8"\"clientInfo\":{\"name\":\"test-agent\",\"version\":\"1.0\"}}}");
            got.info = call(u8"host_info", u8"{}");
            got.assets = call(u8"asset_list", u8"{}");
            got.page = call(u8"page_open", u8"{\"guid\":\"00000000-0000-0000-0000-000000000001\"}");
            got.validate = call(u8"scene_validate", u8"{}");
            got.pages = call(u8"page_list", u8"{}");
            got.log = call(u8"log_read", u8"{}");
            done = true;
        });
    for (u32 i = 0; i < 5000 && !done; ++i)
    {
        service.Pump();
        SleepMilliseconds(1);
    }
    client.Join();

    const auto refused = [](const JsonValue& response)
    { return response.Get(u8"result").Get(u8"isError").AsBool() || response.Has(u8"error"); };
    REQUIRE(got.info.IsObject());
    const JsonValue info = Payload(got.info).Get(u8"host");
    CHECK_FALSE(info.Get(u8"projectOpen").AsBool());
    CHECK_FALSE(info.Has(u8"projectName"));
    CHECK(info.Get(u8"screen").AsString() == StringView(u8"projectManager"));
    CHECK(refused(got.assets));
    CHECK(refused(got.page));
    CHECK(got.page.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString().AsView().ContainsIgnoreCase(u8"no project is open"));
    CHECK(refused(got.validate));
    CHECK_FALSE(refused(got.pages)); // nothing open: an empty list
    CHECK_FALSE(refused(got.log));

    CHECK(FileExists(PathJoin(userData.AsView(), u8"mcp-token").AsView()));

    // The activity: the host listening, the client, then each call in order with its outcome.
    const app::McpActivity& activity = service.Activity();
    CHECK(service.Client().name == u8"test-agent");
    Array<String> seen;
    for (usize i = 0; i < activity.Size(); ++i)
        seen.PushBack(activity.At(i).IsCall() ? Format(u8"{}:{}", activity.At(i).tool,
                                                       activity.At(i).outcome == app::McpActivity::Outcome::Failed ? u8"failed" : u8"done")
                                              : activity.At(i).tool);
    REQUIRE(seen.Size() == 8u);
    CHECK(seen[0].AsView().StartsWith(u8"Listening on 127.0.0.1:"));
    CHECK(seen[1] == u8"test-agent 1.0 connected");
    CHECK(seen[2] == u8"host_info:done");
    CHECK(seen[3] == u8"asset_list:failed");
    CHECK(seen[7] == u8"log_read:done");
    CHECK(activity.At(4).arguments == u8"{\"guid\":\"00000000-0000-0000-0000-000000000001\"}");

    // The user's Stop holds even over --mcp.
    service.StopByUser();
    CHECK_FALSE(service.EnsureRunning());
    (void)RemoveDirectoryRecursive(userData.AsView());
}

// Started over a project, the host's session carries what the editor loaded with it: the project's
// plugin host, the export hooks its plugins added (so project_export runs them, as the editor's own
// export does) and the plugins the editor links (what plugin_list offers); stopped, it drops them.
TEST_CASE("editor-mcp-host: the session over a project carries the editor's plugins, export hooks and linked plugins")
{
    const String userData = PathJoin(GetCurrentDirectory().AsView(), u8"mcp_session_userdata");
    (void)RemoveDirectoryRecursive(userData.AsView());
    (void)RemoveDirectoryRecursive(u8"mcp_session_project");
    REQUIRE(EditorProject::Create(DefaultAllocator(), u8"mcp_session_project", u8"Session").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), u8"mcp_session_project");
    REQUIRE(project);
    RegisterEditorSettingsTypes();
    EditorLogBuffer logBuffer{DefaultAllocator()};
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    EditorContext context{DefaultAllocator()};
    foundation::settings::Settings settings{DefaultAllocator()};
    settings.Section<EditorMcpSettings>().token = String(u8"sekrit");
    editor::ExportPluginServices hooks(DefaultAllocator());
    context.SetExportHooks(&hooks);
    const foundation::runtime::LinkedPlugin linked[1] = {{u8"<root/>", nullptr, 0}};

    app::EditorMcpService::Services services;
    services.context = &context;
    services.builders = &builders;
    services.userSettings = &settings;
    services.log = &logBuffer;
    services.forceEnabled = true;
    services.userDataDirectory = userData;
    services.linkedPlugins = Span<const foundation::runtime::LinkedPlugin>(linked, 1);
    app::EditorMcpService service(DefaultAllocator(), Move(services));
    REQUIRE(service.Start(*project));
    CHECK(service.Session().project == project.Get());
    CHECK(service.Session().exportHooks == &hooks);
    CHECK(service.Session().linkedPlugins.Size() == 1u);
    service.Stop();
    CHECK(service.Session().exportHooks == nullptr);

    project.Reset();
    (void)RemoveDirectoryRecursive(u8"mcp_session_project");
    (void)RemoveDirectoryRecursive(userData.AsView());
}
