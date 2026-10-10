// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp - the golden agent-shaped tool-call flow.
//
// SUBJECT is the flow crossing collections: an agent drives the MCP protocol (foundation.mcp) to
// scaffold, open, and inspect a project on disk (editor.mcp -> the headless EditorProject over
// VFS + the content databases), all through JSON-RPC lines. This is the collection's first MCP
// resident; asset_import/asset_cook/scene_write extend it as those tools land.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.mcp;
import foundation.mcp.reflection;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
import foundation.mcp.script;
#if defined(OPTION_HAS_ANGELSCRIPT)
import foundation.script.angelscript;
#elif defined(OPTION_HAS_LUAU)
import foundation.script.luau;
#endif
import editor.mcp;
import editor.project;  // the seeded project, opened to check it
import fonts.pipeline;  // FontAsset (the seeded default UI font)

using namespace foundation::core;
using namespace foundation::mcp;
using foundation::json::JsonValue;
namespace json = foundation::json;

namespace
{
    // Register the first available backend + its language id (pipeline.registration also registers
    // the enabled backends; this makes the language the script_api assertion expects explicit).
    [[nodiscard]] StringView RegisterSomeBackend()
    {
#if defined(OPTION_HAS_ANGELSCRIPT)
        foundation::script::angelscript::RegisterAngelScriptBackend();
        return StringView(u8"angelscript");
#else
        foundation::script::RegisterLuauScriptBackend();
        return StringView(u8"luau");
#endif
    }

    JsonValue CallResponse(McpServer& s, StringView tool, JsonValue arguments)
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
        return json::Parse(line.response.AsView()).value;
    }

    JsonValue CallOk(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = CallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == false);
        return JsonValue::Parse(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    JsonValue Obj() { return JsonValue::MakeObject(); }
    JsonValue With(JsonValue o, StringView k, StringView v)
    {
        o.Set(String(k), JsonValue::MakeString(String(v)));
        return o;
    }
}

TEST_CASE("integration.mcp: an agent creates, opens, and inspects a project via MCP tools")
{
    // Deterministic fixture: wipe any prior scratch project (.test-scratch is the CWD here).
    std::error_code ec;
    std::filesystem::remove_all("mcp_fixture_project", ec);

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    RegisterReflectionTools(server); // the same host serves reflection + project tools

    // list advertises both tool families
    JsonValue tools = json::Parse(server.HandleLine(
                                        u8"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}").response
                                       .AsView())
                          .value.Get(u8"result")
                          .Get(u8"tools");
    CHECK(tools.Count() >= 5); // project_create/open/info + type_list/type_info

    // create -> open -> info
    JsonValue created = CallOk(server, u8"project_create",
                               With(With(Obj(), u8"directory", u8"mcp_fixture_project"), u8"name",
                                    u8"Fixture"));
    CHECK(created.Get(u8"created").AsBool() == true);

    JsonValue opened =
        CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_fixture_project"));
    CHECK(opened.Get(u8"name").AsString() == StringView(u8"Fixture"));

    JsonValue info = CallOk(server, u8"project_info", Obj());
    CHECK(info.Get(u8"name").AsString() == StringView(u8"Fixture"));
    CHECK(info.Get(u8"directory").AsString().Size() > 0u);
    CHECK(info.Get(u8"sourcesRoot").AsString().Size() > 0u);

    // The settings group as the Project Settings dialog's tabs: every setting in one category,
    // General first.
    const JsonValue categories = info.Get(u8"settingCategories");
    REQUIRE(categories.Count() > 1);
    CHECK(categories.KeyAt(0) == StringView(u8"General"));
    i64 grouped = 0;
    bool msaaOnDisplay = false;
    for (i64 i = 0; i < categories.Count(); ++i)
    {
        const JsonValue names = categories.Get(categories.KeyAt(i));
        grouped += names.Count();
        for (i64 n = 0; n < names.Count(); ++n)
        {
            msaaOnDisplay = msaaOnDisplay ||
                            (categories.KeyAt(i) == StringView(u8"Display") &&
                             names.At(n).AsString() == StringView(u8"renderMsaaSamples"));
        }
    }
    CHECK(grouped == info.Get(u8"settings").Count());
    CHECK(msaaOnDisplay);

    // The manifest is written to disk.
    CHECK(std::filesystem::exists("mcp_fixture_project/Project.xml"));

    // Opening a nonexistent project is a TOOL error (real text), not a protocol fault.
    JsonValue bad =
        CallResponse(server, u8"project_open", With(Obj(), u8"directory", u8"nope_not_a_project"));
    CHECK(bad.Get(u8"result").Get(u8"isError").AsBool() == true);
}

// project_create seeds the editor's starter content: Snowline, made over MCP, had no default UI
// font, so its exported game showed no text (the engine's built-in font is not in a dist).
TEST_CASE("integration.mcp: project_create seeds the default UI font, the sky and the primitives")
{
    pipeline::RegisterPipelineTypes();
    std::error_code ec;
    std::filesystem::remove_all("mcp_seeded_project", ec);
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    (void)pipeline::RegisterAllCreators(creators);
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner, &creators, dataRoot.AsView());
    JsonValue created = CallOk(server, u8"project_create",
                               With(With(Obj(), u8"directory", u8"mcp_seeded_project"), u8"name", u8"Seeded"));
    CHECK(created.Get(u8"seeded").AsBool());

    UniquePtr<editor::EditorProject> project =
        editor::EditorProject::Open(DefaultAllocator(), u8"mcp_seeded_project");
    REQUIRE(static_cast<bool>(project));
    const Guid font = project->Settings().defaultUiFontId;
    REQUIRE_FALSE(font.IsNil());
    foundation::content::Instance* fontInstance = project->SourceDb().GetInstance(font);
    REQUIRE(fontInstance != nullptr);
    RefPtr<ISerializable> read = fontInstance->ReadObject();
    const pipeline::FontAsset* asset = Cast<pipeline::FontAsset>(read.Get());
    REQUIRE(asset != nullptr);
    CHECK(asset->family == StringView(u8"Roboto"));
    CHECK(asset->mode == pipeline::FontBakeMode::DistanceField); // one bake draws every size clean
    foundation::content::Group* root = project->SourceDb().RootGroup();
    REQUIRE(root->GetGroup(u8"Environment") != nullptr);
    CHECK(root->GetGroup(u8"Environment")->GetInstance(u8"BlueSky") != nullptr);
    CHECK(std::filesystem::exists("mcp_seeded_project/Sources/Roboto-Regular.ttf"));
}

TEST_CASE("integration.mcp: asset_list / asset_info read the open project's content DB")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_asset_project", ec);

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::RegisterAssetTools(server, session);

    CallOk(server, u8"project_create",
           With(With(Obj(), u8"directory", u8"mcp_asset_project"), u8"name", u8"Assets"));
    CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_asset_project"));

    // A fresh project's source DB has no assets yet.
    CHECK(CallOk(server, u8"asset_list", Obj()).Get(u8"count").AsInt() == 0);

    // Seed one instance directly in the source DB, then read it back through the tools.
    Guid id;
    REQUIRE(Guid::TryParse(u8"12345678-1234-1234-1234-1234567890ab", id));
    session.project->SourceDb().RootGroup()->AddInstance(id, u8"hero", u8"rtti::test",
                                                         u8"TextureAsset");

    JsonValue list = CallOk(server, u8"asset_list", Obj());
    CHECK(list.Get(u8"count").AsInt() == 1);
    JsonValue first = list.Get(u8"assets").At(0);
    CHECK(first.Get(u8"name").AsString() == StringView(u8"hero"));
    CHECK(first.Get(u8"type").AsString() == StringView(u8"TextureAsset"));

    JsonValue seen = CallOk(server, u8"asset_info",
                            With(Obj(), u8"guid", u8"12345678-1234-1234-1234-1234567890ab"));
    CHECK(seen.Get(u8"name").AsString() == StringView(u8"hero"));
    CHECK(seen.Get(u8"typeNamespace").AsString() == StringView(u8"rtti::test"));

    // A well-formed but absent guid is a tool error (real text), not a protocol fault.
    JsonValue missing = CallResponse(server, u8"asset_info",
                                     With(Obj(), u8"guid", u8"00000000-0000-0000-0000-000000000000"));
    CHECK(missing.Get(u8"result").Get(u8"isError").AsBool() == true);

    // A malformed guid is likewise a tool error.
    JsonValue malformed =
        CallResponse(server, u8"asset_info", With(Obj(), u8"guid", u8"not-a-guid"));
    CHECK(malformed.Get(u8"result").Get(u8"isError").AsBool() == true);
}

#if defined(OPTION_HAS_ANGELSCRIPT) || defined(OPTION_HAS_LUAU)
namespace
{
    std::string Utf8(StringView s)
    {
        return std::string(reinterpret_cast<const char*>(String(s).CStr()));
    }

    // The import->cook flow is backend-GENERIC: RegisterPipelineTypes registers every enabled cook,
    // the ScriptFileImporter accepts each backend's extension, and the cook resolves by language.
    // Only the source language + file extension differ, so drive the identical flow per backend.
    void DriveImportCookFlow(StringView projectDir, StringView sourceStem, StringView ext,
                             StringView sourceBody)
    {
        std::error_code ec;
        const std::string projStd = Utf8(projectDir);
        const std::string stemStd = Utf8(sourceStem);
        const std::string srcFileStd = stemStd + "." + Utf8(ext);
        std::filesystem::remove_all(projStd, ec);
        std::filesystem::remove(srcFileStd, ec);

        // The host assembles the pipeline registries once (as Tools.Mcp does), from the composition
        // root - asset_cook/asset_import route through these.
        pipeline::RegisterPipelineTypes();
        pipeline::BuilderRegistry builders{DefaultAllocator()};
        pipeline::RegisterAllBuilders(builders);
        pipeline::ImporterRegistry importers{DefaultAllocator()};
        pipeline::RegisterAllImporters(importers);

        McpServer server;
        editor::mcp::ProjectSession session;
        editor::mcp::ProjectOwner owner;
        editor::mcp::RegisterProjectOpenTools(server, session, owner);
        editor::mcp::RegisterProjectInfoTool(server, session);
        editor::mcp::RegisterAssetTools(server, session);
        editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
        editor::mcp::RegisterAssetWriteTools(server, session, importers, operations);

        CallOk(server, u8"project_create",
               With(With(Obj(), u8"directory", projectDir), u8"name", u8"Write"));
        CallOk(server, u8"project_open", With(Obj(), u8"directory", projectDir));

        // A real source file on disk (CWD is .test-scratch). A trivial class cooks clean.
        {
            std::ofstream out(srcFileStd);
            out << Utf8(sourceBody);
        }
        const std::filesystem::path abs = std::filesystem::absolute(srcFileStd);
        const String absPath(reinterpret_cast<const utf8char*>(abs.string().c_str()));

        // Import routes by extension through the shared importer set into the source DB.
        JsonValue imported = CallOk(server, u8"asset_import",
                                    With(With(Obj(), u8"source", absPath.AsView()), u8"group",
                                         u8"scripts"));
        CHECK(imported.Get(u8"type").AsString() == StringView(u8"ScriptClassAsset"));
        CHECK(imported.Get(u8"name").AsString() == sourceStem);
        CHECK(imported.Get(u8"importer").AsString() == StringView(u8"Script"));
        const String guid = imported.Get(u8"guid").AsString();

        // It is written to the source DB, in the requested group.
        JsonValue list = CallOk(server, u8"asset_list", Obj());
        CHECK(list.Get(u8"count").AsInt() == 1);
        CHECK(list.Get(u8"assets").At(0).Get(u8"group").AsString() == StringView(u8"scripts"));

        // The raw source was copied under Sources/ and the envelope written under Content/.
        CHECK(std::filesystem::exists(projStd + "/Sources/" + srcFileStd));
        CHECK(std::filesystem::exists(projStd + "/Content/scripts/" + stemStd + ".xasset"));

        // Cook builds it into the cooked DB (product guid == source guid).
        JsonValue cooked = CallOk(server, u8"asset_cook", Obj());
        CHECK(cooked.Get(u8"planned").AsInt() == 1);
        CHECK(cooked.Get(u8"cooked").AsInt() == 1);
        CHECK(cooked.Get(u8"failed").AsInt() == 0);
        CHECK(std::filesystem::exists(projStd + "/Cooked/scripts/" + stemStd + ".rasset"));

        // The cooked product is now visible via asset_info on the cooked DB, under the source guid.
        JsonValue product =
            CallOk(server, u8"asset_info",
                   With(With(Obj(), u8"guid", guid.AsView()), u8"database", u8"cooked"));
        CHECK(product.Get(u8"guid").AsString() == guid);

        // A second cook is a no-op (nothing dirtied).
        JsonValue again = CallOk(server, u8"asset_cook", Obj());
        CHECK(again.Get(u8"planned").AsInt() == 0);
        CHECK(again.Get(u8"upToDate").AsInt() == 1);
    }
}

#ifdef OPTION_HAS_ANGELSCRIPT
TEST_CASE("integration.mcp: an agent imports + cooks an AngelScript source via MCP tools")
{
    DriveImportCookFlow(u8"mcp_write_as", u8"mcp_write_as_src", u8"as",
                        u8"class Hello {\n  Hello() {}\n  void greet() {}\n}\n");
}
#endif // OPTION_HAS_ANGELSCRIPT

#ifdef OPTION_HAS_LUAU
TEST_CASE("integration.mcp: an agent imports + cooks a Luau source via MCP tools")
{
    DriveImportCookFlow(u8"mcp_write_luau", u8"mcp_write_luau_src", u8"luau",
                        u8"Hello = {}\nHello.__index = Hello\n"
                        u8"function Hello.new(entity)\n  return setmetatable({}, Hello)\nend\n"
                        u8"function Hello:greet() end\n");
}
#endif // OPTION_HAS_LUAU
#endif // OPTION_HAS_ANGELSCRIPT || OPTION_HAS_LUAU

TEST_CASE("integration.mcp: script_api reports the COMPLETE engine surface, headless, no device")
{
    // The surface-describing composition root: every subsystem facade, metadata only - no device,
    // no subsystem instantiated. This is the whole point of Fable's ruling A: the headless MCP host
    // reports the true engine script surface (RigidBody/Audio/Ui/run/...), not just core.
    engine::RegisterAllScriptFacades();
    const StringView backend = RegisterSomeBackend();

    McpServer server;
    RegisterScriptTools(server);

    JsonValue api = CallOk(server, u8"script_api", With(Obj(), u8"language", String(backend)));
    JsonValue reported = api.Get(u8"languages").At(0);
    CHECK(reported.Get(u8"language").AsString() == backend);

    // Collect the bound script names.
    JsonValue types = reported.Get(u8"types");
    Array<String> names;
    for (i64 i = 0; i < types.Count(); ++i)
    {
        names.PushBack(types.At(i).Get(u8"scriptName").AsString());
    }
    const auto has = [&names](StringView n)
    {
        for (const String& s : names)
        {
            if (s.AsView() == n)
            {
                return true;
            }
        }
        return false;
    };

    // Core facades AND the gameplay-subsystem facades must all be present.
    CHECK(has(u8"Entity"));
    CHECK(has(u8"RigidBodyComponent")); // physics
    CHECK(has(u8"Audio"));              // audio
    CHECK(has(u8"ui"));                 // game UI (scriptName alias, bound lowercase)
    CHECK(has(u8"run"));                // run/scene-load facade (GameInstance)
    CHECK(has(u8"Net"));                // networking
    // The full surface is much larger than the core-only 17 (physics/audio/input/ui/... added).
    CHECK(reported.Get(u8"typeCount").AsInt() > 30);
}

TEST_CASE("integration.mcp: project_info before any project is open is a tool error")
{
    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);

    JsonValue resp = CallResponse(server, u8"project_info", JsonValue::MakeObject());
    REQUIRE(resp.Has(u8"result"));
    CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == true);
}


TEST_CASE("integration.mcp: asset_import names the other claimants of an extension, and a hint "
          "picks one")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_import_claimants", ec);
    pipeline::RegisterPipelineTypes();
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders);
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::RegisterAllImporters(importers);

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterAssetWriteTools(server, session, importers, operations);
    CallOk(server, u8"project_create",
           With(With(Obj(), u8"directory", u8"mcp_import_claimants"), u8"name", u8"Claims"));
    CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_import_claimants"));

    // A real PNG from the engine's data; several importers claim the extension (texture, image,
    // heightfield, the terrain masks). Two copies, since a re-import of the same source lands
    // on the asset it already made.
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const String png = PathJoin(dataRoot.AsView(), u8"Assets/images/logo.png");
    REQUIRE(FileExists(png.AsView()));
    std::filesystem::copy_file(std::filesystem::path(reinterpret_cast<const char*>(png.CStr())),
                               "mcp_import_claimants_second.png",
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    const std::filesystem::path secondAbs = std::filesystem::absolute("mcp_import_claimants_second.png");
    const String second(reinterpret_cast<const utf8char*>(secondAbs.string().c_str()));

    // No hint: the first claimant imports, and the result names every other one.
    JsonValue unhinted = CallOk(server, u8"asset_import", With(Obj(), u8"source", png.AsView()));
    CHECK(unhinted.Get(u8"importer").AsString() == StringView(u8"Texture"));
    CHECK(unhinted.Get(u8"type").AsString() == StringView(u8"TextureAsset"));
    const JsonValue others = unhinted.Get(u8"alsoClaimableBy");
    const usize claimants = static_cast<usize>(importers.FindAllFor(u8"png").Size());
    REQUIRE(claimants >= 3u);
    REQUIRE(static_cast<usize>(others.Count()) == claimants - 1);
    bool image = false;
    bool heightfield = false;
    bool texture = false;
    for (usize i = 0; i < static_cast<usize>(others.Count()); ++i)
    {
        const String label = others.At(i).AsString();
        image = image || label == u8"Image";
        heightfield = heightfield || label == u8"Heightfield";
        texture = texture || label == u8"Texture";
    }
    CHECK(image);
    CHECK(heightfield);
    CHECK_FALSE(texture); // the one that imported is not an alternative to itself

    // A hint that claims the extension picks that importer; the result names the rest.
    JsonValue hinted = CallOk(server, u8"asset_import",
                              With(With(Obj(), u8"source", second.AsView()), u8"importer", u8"Image"));
    CHECK(hinted.Get(u8"importer").AsString() == StringView(u8"Image"));
    CHECK(hinted.Get(u8"type").AsString() == StringView(u8"ImageAsset"));
    CHECK(static_cast<usize>(hinted.Get(u8"alsoClaimableBy").Count()) == claimants - 1);

    // A hint nothing answers to is refused before any import, naming the claimants in
    // registration order (the unhinted default first).
    JsonValue refused = CallResponse(server, u8"asset_import",
                                     With(With(Obj(), u8"source", png.AsView()), u8"importer",
                                          u8"Sculpture"));
    REQUIRE(refused.Get(u8"result").Get(u8"isError").AsBool());
    const String reason = refused.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString();
    CHECK(reason.AsView().StartsWith(
        u8"no importer 'Sculpture' claims '.png'; the claimants are: Texture Image Heightfield"));

    std::filesystem::remove("mcp_import_claimants_second.png", ec);
    owner.project = nullptr;
    std::filesystem::remove_all("mcp_import_claimants", ec);
}
