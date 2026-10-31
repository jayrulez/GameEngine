// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: the scene and prefab tool flow. An agent opens a project, authors a scene
// through the tools (a write validates first), validates it, reads it back byte for byte, and
// gets refusals with reasons: garbage XML, a guid of the wrong type redirected, a prefab with
// several roots. The seed XML comes from a real SaveScene, so the tools are tested against the
// exact text the editor writes.
#include <doctest/doctest.h>
#include <filesystem>
#include "Core/Prelude.h"
import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.scene;
import foundation.scene.resource;
import foundation.net.replication;
import foundation.mcp;
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
        return JsonValue::Parse(
            resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    String CallErr(McpServer& s, StringView tool, JsonValue arguments)
    {
        JsonValue resp = CallResponse(s, tool, Move(arguments));
        REQUIRE(resp.Has(u8"result"));
        CHECK(resp.Get(u8"result").Get(u8"isError").AsBool() == true);
        return String(resp.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString());
    }

    // Sends a raw JSON-RPC line and returns the response line, for the resources/* methods,
    // which are not tools.
    String Response(McpServer& s, StringView line)
    {
        LineOutcome out = s.HandleLine(line);
        REQUIRE(out.state == LineState::Answered);
        return Move(out.response);
    }

    JsonValue Obj() { return JsonValue::MakeObject(); }
    JsonValue With(JsonValue o, StringView k, StringView v)
    {
        o.Set(String(k), JsonValue::MakeString(String(v)));
        return o;
    }

    // Every occurrence of `from` in `text` replaced with `to`, byte by byte.
    String ReplaceAll(StringView text, StringView from, StringView to)
    {
        StringBuilder out;
        usize i = 0;
        while (i < text.Size())
        {
            if (i + from.Size() <= text.Size() && text.SubStr(i, from.Size()) == from)
            {
                out.Append(to);
                i += from.Size();
                continue;
            }
            out.Append(text[i]);
            ++i;
        }
        return out.Take();
    }
}

TEST_CASE("integration.mcp: scene tools - author, validate, read back, and real refusals")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_scene_project", ec);

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::RegisterSceneTools(server, session);
    editor::mcp::RegisterProjectResources(server, session); // scenes as project:// resources

    (void)CallOk(server, u8"project_create",
                 With(With(Obj(), u8"directory", u8"mcp_scene_project"), u8"name", u8"SceneFix"));
    (void)CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_scene_project"));

    // Seed XML from a real SaveScene, the exact text the editor writes: two entities, one a child,
    // and a real component record (a NetworkComponent on hero). The authored scene has the full
    // manager set, as the validate scratch scene does, so validation exercises a genuine
    // component payload end to end.
    engine::RegisterAllSceneComponentReflection(); // as the MCP host does at startup
    String seedXml;
    String seedGuid;
    {
        scene::Scene authored(DefaultAllocator(), u8"arena");
        engine::AddAllSceneManagers(authored);
        scene::EntityHandle hero = authored.CreateEntity(u8"hero");
        scene::EntityHandle torch = authored.CreateEntity(u8"torch");
        authored.SetParent(torch, hero);
        authored.GetSystem<foundation::net::NetworkComponentManager>()->Add(hero);

        auto* inst = session.project->SourceDb().RootGroup()->CreateInstance(
            u8"seed", scene::SceneDocument::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
        utf8char guidChars[37];
        inst->Id().ToChars(guidChars);
        seedGuid = String(StringView(guidChars, 36));
    }

    // scene_read returns the stored text.
    JsonValue read = CallOk(server, u8"scene_read", With(Obj(), u8"guid", seedGuid.AsView()));
    CHECK(read.Get(u8"name").AsString() == StringView(u8"seed"));
    seedXml = read.Get(u8"xml").AsString();
    CHECK(seedXml.Size() > 0u);

    // scene_validate on raw XML confirms the structure and the component payload. Validation is
    // complete because the scratch scene carries the full engine manager set, so the
    // NetworkComponent record parses through its real manager with zero warnings.
    JsonValue valid = CallOk(server, u8"scene_validate", With(Obj(), u8"xml", seedXml.AsView()));
    CHECK(valid.Get(u8"valid").AsBool() == true);
    CHECK(valid.Get(u8"sceneName").AsString() == StringView(u8"arena"));
    CHECK(valid.Get(u8"entityCount").AsNumber() == doctest::Approx(2.0));
    CHECK(valid.Get(u8"rootCount").AsNumber() == doctest::Approx(1.0));
    CHECK(valid.Get(u8"componentValidation").AsString() == StringView(u8"full"));
    CHECK(valid.Get(u8"warnings").Count() == 0);

    // A record of an unknown component type is skipped with a captured warning. The scene is
    // still valid: the reader skips and warns, and the agent is told what was skipped.
    {
        const String mangled =
            ReplaceAll(seedXml.AsView(), u8"net.Network", u8"bogus.NoSuchComponent");
        JsonValue report =
            CallOk(server, u8"scene_validate", With(Obj(), u8"xml", mangled.AsView()));
        CHECK(report.Get(u8"valid").AsBool() == true);
        CHECK(report.Get(u8"warnings").Count() >= 1);
    }

    // Every successful write announces the asset to the host, and the editor host tells the open
    // pages editing it. A refused write announces nothing.
    Array<Guid> announced;
    session.onAssetWritten = [&announced](const Guid& id) { announced.PushBack(id); };

    // scene_write creates a new scene from the XML. A scene as the editor saved it, which is what
    // scene_read returns, is stored byte for byte.
    JsonValue written = CallOk(
        server, u8"scene_write",
        With(With(With(Obj(), u8"xml", seedXml.AsView()), u8"name", u8"authored"), u8"group",
             u8"Levels"));
    CHECK(written.Get(u8"written").AsBool() == true);
    const String newGuid(written.Get(u8"guid").AsString());
    {
        Guid parsed;
        REQUIRE(Guid::TryParse(newGuid.AsView(), parsed));
        REQUIRE(announced.Size() == 1u);
        CHECK(announced[0] == parsed);
    }
    JsonValue readBack = CallOk(server, u8"scene_read", With(Obj(), u8"guid", newGuid.AsView()));
    CHECK(readBack.Get(u8"xml").AsString() == seedXml.AsView()); // the editor's save, kept as is

    // The same scene written as a generator script would write it, on one line and with numbers
    // formatted differently (1.0 for 1), is stored in the editor's format. The file then reads
    // like one the editor saved, and a later save rewrites nothing.
    {
        const String flat = ReplaceAll(ReplaceAll(seedXml.AsView(), u8"\n", u8"").AsView(), u8"\t", u8"");
        const String oneLine = ReplaceAll(flat.AsView(), u8">1</f32>", u8">1.0</f32>");
        REQUIRE(oneLine != flat); // the seed has a 1 to write as 1.0
        JsonValue compact = CallOk(server, u8"scene_write",
                                   With(With(Obj(), u8"xml", oneLine.AsView()), u8"name", u8"compact"));
        JsonValue stored =
            CallOk(server, u8"scene_read", With(Obj(), u8"guid", compact.Get(u8"guid").AsString()));
        CHECK(stored.Get(u8"xml").AsString() == seedXml.AsView());
        CHECK(announced.Size() == 2u);
    }

    // scene_validate by guid works on the stored stream too.
    JsonValue storedValid =
        CallOk(server, u8"scene_validate", With(Obj(), u8"guid", newGuid.AsView()));
    CHECK(storedValid.Get(u8"valid").AsBool() == true);

    // Refusals carry reasons.
    // (a) Garbage XML never writes.
    const String garbage = CallErr(
        server, u8"scene_write",
        With(With(Obj(), u8"xml", u8"<not a scene>"), u8"name", u8"broken"));
    CHECK(garbage.Size() > 0u);
    CHECK(announced.Size() == 2u); // nothing written, nothing announced
    // (b) A scene guid given to the prefab tool redirects.
    const String redirect =
        CallErr(server, u8"prefab_read", With(Obj(), u8"guid", newGuid.AsView()));
    CHECK(redirect.Size() > 0u);
    // (c) A scene with a single root is a legal prefab and writes fine.
    JsonValue okPrefab = CallOk(
        server, u8"prefab_write",
        With(With(Obj(), u8"xml", seedXml.AsView()), u8"name", u8"goodprefab"));
    CHECK(okPrefab.Get(u8"written").AsBool() == true);
    CHECK(announced.Size() == 3u); // the prefab write announced too
    // One with several roots is refused, citing the single-root rule.
    {
        scene::Scene twoRoots(DefaultAllocator(), u8"pair");
        (void)twoRoots.CreateEntity(u8"a");
        (void)twoRoots.CreateEntity(u8"b");
        auto* inst = session.project->SourceDb().RootGroup()->CreateInstance(
            u8"pairseed", scene::SceneDocument::StaticType());
        REQUIRE(scene::SaveScene(twoRoots, *inst).IsOk());
        utf8char guidChars[37];
        inst->Id().ToChars(guidChars);
        JsonValue pairRead = CallOk(server, u8"scene_read",
                                    With(Obj(), u8"guid", StringView(guidChars, 36)));
        const String pairXml(pairRead.Get(u8"xml").AsString());
        const String refused = CallErr(
            server, u8"prefab_write",
            With(With(Obj(), u8"xml", pairXml.AsView()), u8"name", u8"badprefab2"));
        CHECK(refused.Size() > 0u);
    }

    // scene_validate demands exactly one input.
    (void)CallErr(server, u8"scene_validate", Obj());
    (void)CallErr(server, u8"scene_validate",
                  With(With(Obj(), u8"xml", u8"x"), u8"guid", newGuid.AsView()));

    // The scenes are also read-only resources (project://scene/<guid>), listed live from the
    // source DB: the written scene appears without any re-registration, and its content matches
    // scene_read byte for byte.
    {
        const String listLine =
            Response(server, u8"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"resources/list\"}");
        JsonValue list = json::Parse(listLine.AsView()).value;
        const JsonValue resources = list.Get(u8"result").Get(u8"resources");
        const String wantedUri = Format(u8"project://scene/{}", newGuid.AsView());
        bool found = false;
        for (i64 i = 0; i < resources.Count(); ++i)
        {
            if (resources.At(i).Get(u8"uri").AsString() == wantedUri.AsView())
            {
                found = true;
                CHECK(resources.At(i).Get(u8"mimeType").AsString() ==
                      StringView(u8"application/xml"));
            }
        }
        CHECK(found);

        const String readLine = Response(
            server,
            Format(u8"{}{}{}",
                   StringView(u8"{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"resources/read\","
                              u8"\"params\":{\"uri\":\""),
                   wantedUri.AsView(), StringView(u8"\"}}"))
                .AsView());
        JsonValue read2 = json::Parse(readLine.AsView()).value;
        CHECK(read2.Get(u8"result").Get(u8"contents").At(0).Get(u8"text").AsString() ==
              seedXml.AsView());
    }
}

// Prefab instances are counted apart from the scene's own entities, and an instance whose prefab
// is not a prefab of the project is a warning from both validate and write.
TEST_CASE("integration.mcp: scene tools - validation counts prefab instances and warns on a missing "
          "prefab")
{
    std::error_code ec;
    std::filesystem::remove_all("mcp_scene_prefabs", ec);
    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterSceneTools(server, session);
    (void)CallOk(server, u8"project_create",
                 With(With(Obj(), u8"directory", u8"mcp_scene_prefabs"), u8"name", u8"Prefabs"));
    (void)CallOk(server, u8"project_open", With(Obj(), u8"directory", u8"mcp_scene_prefabs"));
    engine::RegisterAllSceneComponentReflection();

    const auto storedXml = [&](scene::Scene& authored, StringView name) -> String
    {
        auto* inst = session.project->SourceDb().RootGroup()->CreateInstance(
            name, scene::SceneDocument::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(scene::SaveScene(authored, *inst).IsOk());
        utf8char text[37];
        inst->Id().ToChars(text);
        return CallOk(server, u8"scene_read", With(Obj(), u8"guid", StringView(text, 36)))
            .Get(u8"xml")
            .AsString();
    };

    // A one-entity prefab, written the agent's way.
    String prefabId;
    {
        scene::Scene block(DefaultAllocator(), u8"block");
        engine::AddAllSceneManagers(block);
        (void)block.CreateEntity(u8"block");
        const String xml = storedXml(block, u8"staging");
        JsonValue written = CallOk(server, u8"prefab_write",
                                   With(With(Obj(), u8"xml", xml.AsView()), u8"name", u8"Block"));
        prefabId = written.Get(u8"guid").AsString();
    }

    // A level with one entity of its own and two instances: the prefab, and a guid that names
    // nothing.
    Random rng(17);
    const Guid missing = Guid::Generate(rng);
    String levelXml;
    {
        scene::Scene level(DefaultAllocator(), u8"level");
        engine::AddAllSceneManagers(level);
        (void)level.CreateEntity(u8"sun");
        auto good = MakeUnique<scene::Scene::PendingPrefabInstance>(DefaultAllocator());
        REQUIRE(Guid::TryParse(prefabId.AsView(), good->prefabId));
        level.AddPendingPrefabInstance(Move(good));
        auto bad = MakeUnique<scene::Scene::PendingPrefabInstance>(DefaultAllocator());
        bad->prefabId = missing;
        level.AddPendingPrefabInstance(Move(bad));
        levelXml = storedXml(level, u8"level");
    }
    JsonValue report = CallOk(server, u8"scene_validate", With(Obj(), u8"xml", levelXml.AsView()));
    CHECK(report.Get(u8"valid").AsBool());
    CHECK(report.Get(u8"entityCount").AsNumber() == doctest::Approx(1.0)); // its own entities only
    CHECK(report.Get(u8"prefabInstances").AsNumber() == doctest::Approx(2.0));
    const JsonValue warnings = report.Get(u8"warnings");
    REQUIRE(warnings.Count() == 1);
    utf8char missingText[37];
    missing.ToChars(missingText);
    CHECK(warnings.At(0).AsString().AsView().StartsWith(
        Format(u8"prefab instance 1: {} is not a prefab in this project",
               StringView(missingText, 36))
            .AsView()));

    JsonValue written = CallOk(server, u8"scene_write",
                               With(With(Obj(), u8"xml", levelXml.AsView()), u8"name", u8"authored"));
    CHECK(written.Get(u8"warnings").Count() == 1); // the write reports it too
}

TEST_CASE("integration.mcp: host_info reports pid, stamp, versions, and host state")
{
    McpServer server;
    server.SetServerInfo(u8"test-host", u8"9.9.9");
    bool open = false;
    RegisterHostInfoTool(server, String(u8"stamp-abc123"),
                         Function<JsonValue()>{
                             [&open]()
                             {
                                 JsonValue host = JsonValue::MakeObject();
                                 host.Set(u8"projectOpen", JsonValue::MakeBool(open));
                                 return host;
                             }});

    JsonValue info = CallOk(server, u8"host_info", Obj());
    CHECK(info.Get(u8"pid").AsNumber() > 0.0);
    CHECK(info.Get(u8"buildStamp").AsString() == StringView(u8"stamp-abc123"));
    CHECK(info.Get(u8"serverName").AsString() == StringView(u8"test-host"));
    CHECK(info.Get(u8"serverVersion").AsString() == StringView(u8"9.9.9"));
    CHECK(info.Get(u8"protocolVersion").AsString().Size() > 0u);
    CHECK(info.Get(u8"host").Get(u8"projectOpen").AsBool() == false);

    // The host-state lambda is read live, not captured at registration.
    open = true;
    JsonValue after = CallOk(server, u8"host_info", Obj());
    CHECK(after.Get(u8"host").Get(u8"projectOpen").AsBool() == true);
}
