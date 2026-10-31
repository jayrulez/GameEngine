// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp - project_export: a REAL dist through the
// tool - cook + scene staging + pack + player staging - against a scratch project with one
// authored scene. The host template resolves from the directory of THIS test executable
// (which sits next to Engine.Player in Bin, exactly the layout the MCP host and export CLI
// run from).
#include <doctest/doctest.h>
#include <cstdlib> // setenv (the scratch templates root)
#include <filesystem>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs; // FindDataRoot: the engine shaders the export cooks
import foundation.json;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import foundation.mcp;
import pipeline.core;
import pipeline.registration;
import engine.composition;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
using foundation::json::JsonValue;
namespace scene = foundation::scene;

namespace
{
    JsonValue ExCall(McpServer& s, StringView tool, JsonValue arguments, bool expectOk)
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
        return expectOk ? JsonValue::Parse(
                              resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString())
                        : resp;
    }

    JsonValue ExStr(JsonValue o, StringView k, StringView v)
    {
        o.Set(String(k), JsonValue::MakeString(String(v)));
        return o;
    }

    // Directory of THIS executable (next to Engine.Player in Bin) - the host-template source.
    String TestExeDir() { return String(PathParent(ExecutablePath().AsView())); }
}

TEST_CASE("integration.mcp: project_export - a real dist from an authored project")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_export_project", ec);
    // An empty templates root, so the export takes the host template (this build's player and its
    // sidecars) and never a template installed on the machine running the test.
    std::filesystem::remove_all("mcp_export_templates", ec);
    std::filesystem::create_directories("mcp_export_templates", ec);
#ifdef _WIN32
    _putenv_s("ENV_TEMPLATES_DIR", "mcp_export_templates");
#else
    setenv("ENV_TEMPLATES_DIR", "mcp_export_templates", 1);
#endif

    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterPipelineTypes();
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    engine::RegisterAllSceneComponentReflection();

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::InlineProjectOperations operations(session, builders, TestExeDir(),
                                                    foundation::vfs::FindDataRoot());
    editor::mcp::RegisterProjectExportTool(server, session, operations);

    // No project -> guided refusal.
    (void)ExCall(server, u8"project_export", JsonValue::MakeObject(), /*expectOk=*/false);

    (void)ExCall(server, u8"project_create",
                 ExStr(ExStr(JsonValue::MakeObject(), u8"directory", u8"mcp_export_project"),
                       u8"name", u8"Exportable"),
                 true);
    (void)ExCall(server, u8"project_open",
                 ExStr(JsonValue::MakeObject(), u8"directory", u8"mcp_export_project"), true);

    // One authored scene so the dist has content to stage.
    {
        scene::Scene authored(DefaultAllocator(), u8"main");
        engine::AddAllSceneManagers(authored);
        (void)authored.CreateEntity(u8"anchor");
        auto* inst = session.project->SourceDb().RootGroup()->CreateInstance(
            u8"main", scene::SceneDocument::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
    }

    // An unknown preset is refused with the available names.
    (void)ExCall(server, u8"project_export",
                 ExStr(JsonValue::MakeObject(), u8"preset", u8"NoSuchPreset"), false);

    // The default (synthesized host) preset exports a full dist.
    JsonValue exported = ExCall(server, u8"project_export", JsonValue::MakeObject(), true);
    CHECK(exported.Get(u8"exported").AsBool() == true);
    CHECK(exported.Get(u8"scenesStaged").AsNumber() >= 1.0);
    CHECK(exported.Get(u8"cookFailed").AsNumber() == doctest::Approx(0.0));
    const String outputDir(exported.Get(u8"outputDir").AsString());
    REQUIRE(outputDir.Size() > 0u);
    const std::filesystem::path out(reinterpret_cast<const char*>(outputDir.CStr()));
    CHECK(std::filesystem::is_regular_file(out / "Content.pak", ec));
    CHECK(std::filesystem::is_regular_file(out / "player.xml", ec));
    CHECK(exported.Get(u8"filesStaged").AsNumber() >= 1.0); // the player + sidecars are staged
    // The licences the player's binary carries go with it: a sidecar the build lists.
    CHECK(std::filesystem::is_regular_file(out / "THIRD-PARTY-NOTICES.txt", ec));

#ifdef _WIN32
    _putenv_s("ENV_TEMPLATES_DIR", "");
#else
    unsetenv("ENV_TEMPLATES_DIR");
#endif
    std::filesystem::remove_all("mcp_export_templates", ec);
}
