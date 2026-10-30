// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The model viewer's MCP surface (Documentation/Specs/model-viewer.md, P7), through the MCP
// server in process as a client sees it: the tool list is the command list (names, schemas,
// annotations), the guide names every tool, and a call changes the library as the command does,
// a refused one coming back as a tool error with the command's message.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import modelviewer;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace
{
    String Folder(StringView name)
    {
        const String path = PathJoin(GetCurrentDirectory().AsView(), name);
        (void)RemoveDirectoryRecursive(path.AsView());
        REQUIRE(CreateDirectories(path.AsView()));
        return path;
    }

    JsonValue Response(const foundation::mcp::LineOutcome& line)
    {
        REQUIRE(line.state == foundation::mcp::LineState::Answered);
        return JsonValue::Parse(line.response);
    }

    // A tool call's result: the command's JSON (in the text content), and whether it was an error.
    struct Call
    {
        bool error = false;
        JsonValue value;
        String text;
    };
    Call CallTool(foundation::mcp::McpServer& server, StringView name, const JsonValue& args)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(String(u8"name"), JsonValue::MakeString(String(name)));
        params.Set(String(u8"arguments"), args);
        JsonValue request = JsonValue::MakeObject();
        request.Set(String(u8"jsonrpc"), JsonValue::MakeString(String(u8"2.0")));
        request.Set(String(u8"id"), JsonValue::MakeNumber(1));
        request.Set(String(u8"method"), JsonValue::MakeString(String(u8"tools/call")));
        request.Set(String(u8"params"), params);
        const JsonValue result = Response(server.HandleLine(request.ToString().AsView())).Get(String(u8"result"));
        Call call;
        call.error = result.Get(String(u8"isError")).AsBool();
        call.text = result.Get(String(u8"content")).At(0).Get(String(u8"text")).AsString();
        if (!call.error)
            call.value = JsonValue::Parse(call.text);
        return call;
    }

    struct Host
    {
        modelviewer::LibrarySession session{DefaultAllocator()};
        modelviewer::SettingsStore settings;
        foundation::mcp::McpServer server;
        explicit Host(StringView folder)
        {
            String error;
            REQUIRE(session.Open(folder, error));
            modelviewer::RegisterLibraryCommands(session);
            modelviewer::RegisterInfoCommand(session, folder,
                                             []()
                                             {
                                                 JsonValue v = JsonValue::MakeObject();
                                                 v.Set(String(u8"mode"), JsonValue::MakeString(String(u8"test")));
                                                 return v;
                                             });
            modelviewer::RegisterWindowlessCommands(session.Commands()); // as --headless serves them
            modelviewer::RegisterSettingsCommands(session.Commands(), settings, Span<const String>{});
            modelviewer::RegisterCommandTools(server, session.Commands());
        }
    };
}

TEST_CASE("model viewer mcp: the tool list is the command list, with each command's schema and kind")
{
    Host host(Folder(u8"scratch_viewer_mcp_list").AsView());
    const JsonValue tools = Response(host.server.HandleLine(u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"))
                                .Get(String(u8"result"))
                                .Get(String(u8"tools"));
    const Span<const modelviewer::Command> commands = host.session.Commands().All();
    REQUIRE(static_cast<usize>(tools.Count()) == commands.Size());
    for (usize i = 0; i < commands.Size(); ++i)
    {
        const modelviewer::Command& command = commands[i];
        const JsonValue tool = tools.At(static_cast<i64>(i));
        CHECK(tool.Get(String(u8"name")).AsString() == modelviewer::ToolNameOf(command.name.AsView()));
        CHECK(tool.Get(String(u8"description")).AsString() == command.description);
        CHECK(tool.Get(String(u8"inputSchema")).ToString() == command.schema.ToString());
        const JsonValue hints = tool.Get(String(u8"annotations"));
        CHECK(hints.Get(String(u8"readOnlyHint")).AsBool() == (command.kind == modelviewer::CommandKind::Reads));
        if (command.kind == modelviewer::CommandKind::Deletes)
            CHECK(hints.Get(String(u8"destructiveHint")).AsBool());
    }
    CHECK(modelviewer::ToolNameOf(u8"entry.note.set") == u8"entry_note_set");
}

TEST_CASE("model viewer mcp: the guide names every tool")
{
    Host host(Folder(u8"scratch_viewer_mcp_guide").AsView());
    const String starts[] = {GetCurrentDirectory(), String(PathParent(ExecutablePath().AsView()))};
    const String guide = modelviewer::LocateGuide(Span<const String>(starts, 2));
    REQUIRE_FALSE(guide.IsEmpty());
    Result<Array<byte>> bytes = ReadFile(guide.AsView());
    REQUIRE(bytes.HasValue());
    const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
    for (const modelviewer::Command& command : host.session.Commands().All())
    {
        const String named = Format(u8"`{}`", modelviewer::ToolNameOf(command.name.AsView()));
        INFO(reinterpret_cast<const char*>(named.CStr()));
        CHECK(text.ContainsIgnoreCase(named.AsView()));
    }
    // And it is served as the guide's resource.
    modelviewer::RegisterGuideResource(host.server, guide.AsView());
    const JsonValue read = Response(host.server.HandleLine(
        u8"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/read\",\"params\":{\"uri\":\"docs://ModelViewerGuide.md\"}}"));
    CHECK(read.Get(String(u8"result")).Get(String(u8"contents")).At(0).Get(String(u8"text")).AsString().AsView().StartsWith(u8"# Model viewer guide"));
}

TEST_CASE("model viewer mcp: a call changes the library as the command does; a refused one is a tool error")
{
    const String folder = Folder(u8"scratch_viewer_mcp_call");
    Host host(folder.AsView());
    JsonValue create = JsonValue::MakeObject();
    create.Set(String(u8"name"), JsonValue::MakeString(String(u8"Moonfall")));
    const Call created = CallTool(host.server, u8"collection_create", create);
    REQUIRE_FALSE(created.error);
    const String id = created.value.Get(String(u8"id")).AsString();
    // The command sees what the tool did.
    const modelviewer::CommandResult tree = host.session.Commands().Execute(u8"collection.tree");
    REQUIRE(tree.value.Count() == 1);
    CHECK(tree.value.At(0).Get(String(u8"id")).AsString() == id);

    // A refused change: the command's message, as a tool error.
    JsonValue blank = JsonValue::MakeObject();
    blank.Set(String(u8"name"), JsonValue::MakeString(String(u8"")));
    const Call refused = CallTool(host.server, u8"collection_create", blank);
    CHECK(refused.error);
    CHECK(refused.text.AsView().ContainsIgnoreCase(u8"needs a name"));

    // viewer_info: the host's say, with the library's folder and writability.
    const Call info = CallTool(host.server, u8"viewer_info", JsonValue::MakeObject());
    REQUIRE_FALSE(info.error);
    CHECK(info.value.Get(String(u8"mode")).AsString() == u8"test");
    CHECK(info.value.Get(String(u8"writable")).AsBool());
    CHECK(info.value.Get(String(u8"library")).AsString() == folder);

    // An undo through MCP steps back over the tool's change.
    CHECK_FALSE(CallTool(host.server, u8"history_undo", JsonValue::MakeObject()).error);
    CHECK(host.session.Commands().Execute(u8"collection.tree").value.Count() == 0);
}

TEST_CASE("model viewer mcp: without a window, each window command says there is none")
{
    Host host(Folder(u8"scratch_viewer_mcp_windowless").AsView());
    for (const modelviewer::WindowCommandSpec& spec : modelviewer::WindowCommandSpecs())
    {
        const modelviewer::Command* command = host.session.Commands().Find(spec.name);
        REQUIRE(command != nullptr);
        CHECK(command->kind == spec.kind);
        const modelviewer::CommandResult result = host.session.Commands().Execute(spec.name);
        CHECK_FALSE(result.ok);
        CHECK(result.error.AsView().ContainsIgnoreCase(u8"there is no window"));
    }
    // Through MCP, as an agent on --headless meets it.
    const Call call = CallTool(host.server, u8"tabs_list", JsonValue::MakeObject());
    CHECK(call.error);
    CHECK(call.text.AsView().ContainsIgnoreCase(u8"--headless"));
}
