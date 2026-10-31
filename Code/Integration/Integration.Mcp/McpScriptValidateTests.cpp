// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: script_validate, the compile check. Every enabled backend's own New Asset
// starter must validate through the tool with its metadata harvested (the class name and handlers
// recognized), and broken source must come back with real compile errors carrying line numbers.
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Core/Prelude.h"
import foundation.core;
import foundation.json;
import foundation.mcp;
import pipeline.core;
import pipeline.registration;
import script.pipeline;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;

namespace
{
    JsonValue SvCall(McpServer& s, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(u8"script_validate"));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = s.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        return json::Parse(line.response.AsView()).value;
    }

    JsonValue SvOk(McpServer& s, JsonValue arguments)
    {
        JsonValue resp = SvCall(s, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(
            resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    JsonValue SvArgs(StringView language, StringView source)
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"language", JsonValue::MakeString(String(language)));
        a.Set(u8"source", JsonValue::MakeString(String(source)));
        return a;
    }

    bool HasHandler(const JsonValue& handlers, StringView name)
    {
        for (i64 i = 0; i < handlers.Count(); ++i)
        {
            if (handlers.At(i).AsString() == name)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("integration.mcp: script_validate - every enabled backend's starter compiles; "
          "broken source reports lines")
{
    pipeline::RegisterPipelineTypes(); // registers the enabled language cooks (idempotent)

    McpServer server;
    editor::mcp::RegisterScriptValidateTool(server);

    const StringView languages[] = {u8"angelscript", u8"luau"};
    usize enabled = 0;
    for (StringView language : languages)
    {
        pipeline::IScriptLanguageCook* cook =
            pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(language);
        if (cook == nullptr)
        {
            continue; // backend disabled in this build config
        }
        ++enabled;

        // The backend's own Behavior starter validates, and the harvest recognized it.
        const String starter = cook->NewAssetTemplate(pipeline::ScriptTier::Behavior, {});
        JsonValue ok = SvOk(server, SvArgs(language, starter));
        CHECK(ok.Get(u8"valid").AsBool() == true);
        CHECK(ok.Get(u8"errors").Count() == 0);
        CHECK(ok.Get(u8"className").AsString() == StringView(u8"NewBehavior"));
        CHECK(HasHandler(ok.Get(u8"handlers"), u8"onUpdate"));
        CHECK(ok.Get(u8"checkLevel").AsString() == StringView(u8"compile"));

        // Broken source fails with at least one line-numbered error.
        JsonValue bad =
            SvOk(server, SvArgs(language, u8"this is ( not : a valid { script"));
        CHECK(bad.Get(u8"valid").AsBool() == false);
        REQUIRE(bad.Get(u8"errors").Count() >= 1);
        CHECK(bad.Get(u8"errors").At(0).Get(u8"line").AsNumber() >= 1.0);
        CHECK(String(bad.Get(u8"errors").At(0).Get(u8"message").AsString()).Size() > 0u);
    }
    CHECK(enabled >= 1); // the suite must actually exercise at least one backend

    // A missing required argument is a protocol error from schema validation, not a tool error.
    JsonValue noSource = JsonValue::MakeObject();
    noSource.Set(u8"language", JsonValue::MakeString(u8"luau"));
    JsonValue params = JsonValue::MakeObject();
    params.Set(u8"name", JsonValue::MakeString(u8"script_validate"));
    params.Set(u8"arguments", Move(noSource));
    JsonValue req = JsonValue::MakeObject();
    req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
    req.Set(u8"id", JsonValue::MakeNumber(2));
    req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
    req.Set(u8"params", Move(params));
    LineOutcome line = server.HandleLine(req.ToString().AsView());
    REQUIRE(line.state == LineState::Answered);
    JsonValue resp = json::Parse(line.response.AsView()).value;
    CHECK(resp.Get(u8"error").Get(u8"code").AsInt() == -32602);
}

namespace
{
    JsonValue ScCall(McpServer& s, StringView tool, JsonValue arguments, bool expectOk)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = s.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        JsonValue resp = json::Parse(line.response.AsView()).value;
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == !expectOk);
        if (!expectOk)
        {
            return resp;
        }
        return JsonValue::Parse(
            resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }
}

TEST_CASE("integration.mcp: script_create - starter-seeded asset, unique names, and the "
          "create->validate loop")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_script_project", ec);
    pipeline::RegisterPipelineTypes();

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::RegisterScriptValidateTool(server);
    editor::mcp::RegisterScriptCreateTool(server, session);

    // With no project open, the tool refuses with guidance.
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"name", JsonValue::MakeString(u8"Mover"));
        a.Set(u8"language", JsonValue::MakeString(u8"luau"));
        (void)ScCall(server, u8"script_create", Move(a), /*expectOk=*/false);
    }

    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"directory", JsonValue::MakeString(u8"mcp_script_project"));
        a.Set(u8"name", JsonValue::MakeString(u8"Scripts"));
        (void)ScCall(server, u8"project_create", Move(a), true);
    }
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"directory", JsonValue::MakeString(u8"mcp_script_project"));
        (void)ScCall(server, u8"project_open", Move(a), true);
    }

    // Create a Luau behavior; the starter lands on disk and the asset records it.
    JsonValue created;
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"name", JsonValue::MakeString(u8"Mover"));
        a.Set(u8"language", JsonValue::MakeString(u8"luau"));
        created = ScCall(server, u8"script_create", Move(a), true);
    }
    CHECK(created.Get(u8"name").AsString() == StringView(u8"Mover"));
    CHECK(created.Get(u8"fileName").AsString() == StringView(u8"Mover.luau"));
    const String sourceFile(created.Get(u8"sourceFile").AsString());
    REQUIRE(std::filesystem::is_regular_file(
        reinterpret_cast<const char*>(sourceFile.CStr()), ec));

    // The created file's content validates through script_validate, as in the authoring loop.
    std::ifstream in(reinterpret_cast<const char*>(sourceFile.CStr()));
    std::stringstream text;
    text << in.rdbuf();
    const std::string content = text.str();
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"language", JsonValue::MakeString(u8"luau"));
        a.Set(u8"source", JsonValue::MakeString(String(StringView(
                              reinterpret_cast<const utf8char*>(content.c_str())))));
        JsonValue validated = ScCall(server, u8"script_validate", Move(a), true);
        CHECK(validated.Get(u8"valid").AsBool() == true);
        // The class is named after the asset, not NewBehavior.
        CHECK(validated.Get(u8"className").AsString() == StringView(u8"Mover"));
    }

    // A second create with the same name gets a unique name and never overwrites.
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"name", JsonValue::MakeString(u8"Mover"));
        a.Set(u8"language", JsonValue::MakeString(u8"luau"));
        JsonValue second = ScCall(server, u8"script_create", Move(a), true);
        CHECK(second.Get(u8"name").AsString() != StringView(u8"Mover"));
        CHECK(second.Get(u8"guid").AsString() != created.Get(u8"guid").AsString());
    }
}
