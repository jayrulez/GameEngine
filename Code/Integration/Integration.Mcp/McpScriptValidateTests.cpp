// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: script_validate, the compile check. Every enabled backend's own New Asset
// starter must validate through the tool with its metadata harvested (the class name and handlers
// recognized), and broken source must come back with real compile errors carrying line numbers.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.json;
import foundation.mcp;
import pipeline.core;
import pipeline.registration;
import script.pipeline;
import foundation.runtime; // a plugin host that loaded a plugin with a script type
import engine.project;
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
    editor::mcp::ProjectSession session; // no project: the tool compiles in-memory source
    editor::mcp::RegisterScriptValidateTool(server, session);

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
    RemoveDirectoryRecursive(u8"mcp_script_project");
    pipeline::RegisterPipelineTypes();

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    editor::mcp::RegisterProjectInfoTool(server, session);
    editor::mcp::RegisterScriptValidateTool(server, session);
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
    REQUIRE(FileExists(sourceFile.AsView()));

    // The created file's content validates through script_validate, as in the authoring loop.
    Result<Array<byte>> text = ReadFile(sourceFile.AsView());
    REQUIRE(text.HasValue());
    const String content(StringView(reinterpret_cast<const utf8char*>(text.Value().Data()), text.Value().Size()));
    {
        JsonValue a = JsonValue::MakeObject();
        a.Set(u8"language", JsonValue::MakeString(u8"luau"));
        a.Set(u8"source", JsonValue::MakeString(content));
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

#ifdef OPTION_HAS_ANGELSCRIPT
// A plugin whose Engine part gives scripts a type of its own (`Lantern`), enabled on Win64 only.
namespace validateprobe
{
    struct Lantern
    {
        f32 glow = 0.0f;
        Lantern() = default;
        explicit Lantern(f32 value) : glow(value) {}
    };
    REFLECT_VALUE(Lantern, "rtti::validateprobe")
    {
        builder.Property<&Lantern::glow>("glow").Constructor<f32>();
    }
}

namespace
{
    constexpr char8_t kLanternPlugin[] = u8"lanternplugin";

    void LoadLantern(foundation::runtime::PluginContext& context)
    {
        validateprobe::RttiRegisterValue_Lantern();
        context.Types().Register(TypeOf<validateprobe::Lantern>());
    }
    const foundation::runtime::PluginPart* LanternEngine()
    {
        static constexpr foundation::runtime::PluginPart part{kLanternPlugin, foundation::runtime::PluginPartKind::Engine,
                                                              &LoadLantern, nullptr};
        return &part;
    }
}

// With a `platform`, script_validate also checks the source against what the open project's
// plugins leave there: a script using a type of a plugin the project enables on Win64 only is valid
// for Win64 and refused for Linux64, which names the absent plugin; without a project a platform is
// refused.
TEST_CASE("integration.mcp: script_validate - a platform check leaves out the plugins the project does not enable there")
{
    pipeline::RegisterPipelineTypes();
    const StringView dir = u8"mcp_script_platform_project";
    RemoveDirectoryRecursive(dir);
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"Lanterns").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(project);
    foundation::runtime::EnabledPlugin entry;
    entry.id = String(kLanternPlugin);
    entry.platforms.PushBack(String(u8"Win64"));
    project->Settings().plugins.PushBack(Move(entry));

    foundation::runtime::PluginManifest manifest;
    manifest.id = String(kLanternPlugin);
    manifest.version = String(u8"1.0.0");
    manifest.engineVersion = String(engine::project::kEngineVersionString);
    manifest.parts.PushBack(foundation::runtime::PluginManifestPart{String(u8"Engine")});
    manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(u8"Linux64")});
    manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(u8"Win64")});
    MemoryStream xml;
    REQUIRE(foundation::runtime::WritePluginManifest(manifest, xml).IsOk());
    Array<byte> manifestText;
    for (byte b : xml.Bytes())
    {
        manifestText.PushBack(b);
    }
    manifestText.PushBack(byte{0});
    const foundation::runtime::PluginPartEntryFn parts[] = {&LanternEngine};
    const foundation::runtime::LinkedPlugin linked{reinterpret_cast<const char8_t*>(manifestText.Data()), parts, 1};
    foundation::runtime::SubsystemManager subsystems(DefaultAllocator());
    foundation::runtime::PluginHost host(DefaultAllocator(), subsystems, foundation::runtime::HostRole::Mcp,
                                         engine::project::kEngineVersionString, u8"Win64");
    REQUIRE(host.AddLinked(Span<const foundation::runtime::LinkedPlugin>(&linked, 1)).IsOk());
    REQUIRE(host.Load(Span<const foundation::runtime::EnabledPlugin>(project->Settings().plugins.Data(),
                                                                     project->Settings().plugins.Size()))
                .loaded.Size() == 1u);

    McpServer server;
    editor::mcp::ProjectSession session;
    editor::mcp::RegisterScriptValidateTool(server, session);
    constexpr StringView kSource = u8"double shine()\n{\n    Lantern l(2.0f);\n    return l.glow;\n}\n";
    const auto onPlatform = [&](StringView platform)
    {
        JsonValue args = SvArgs(u8"angelscript", kSource);
        args.Set(u8"platform", JsonValue::MakeString(String(platform)));
        return args;
    };
    {
        const JsonValue refused = SvCall(server, onPlatform(u8"Linux64"));
        CHECK(refused.Get(u8"result").Get(u8"isError").AsBool()); // no project open
    }

    session.project = project.Get();
    session.plugins = &host;
    const JsonValue win = SvOk(server, onPlatform(u8"Win64"));
    CHECK(win.Get(u8"valid").AsBool());
    CHECK(win.Get(u8"absentPlugins").AsString().IsEmpty());
    const JsonValue linux = SvOk(server, onPlatform(u8"Linux64"));
    CHECK_FALSE(linux.Get(u8"valid").AsBool());
    CHECK(linux.Get(u8"absentPlugins").AsString() == String(u8"'lanternplugin'"));
    CHECK(linux.Get(u8"errors").Count() > 0u);

    session.project = nullptr;
    session.plugins = nullptr;
    host.Unload();
    project.Reset();
    RemoveDirectoryRecursive(dir);
}
#endif // OPTION_HAS_ANGELSCRIPT
