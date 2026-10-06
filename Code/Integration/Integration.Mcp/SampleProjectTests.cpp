// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The tracked sample projects (Data/SampleProjects: PaperKid, Sky Hopper, Snowline) must stay
// readable at the CURRENT data versions. A wire-version bump or a key rename that forgets to
// upgrade their sources leaves them refused by the strict readers - the cook logged 13 refusals
// (font, six meshes, six UI documents) on PaperKid before the 2026-09-18 upgrade. This registers
// the whole pipeline the way Tools.Cook does, opens a scratch copy (so opening never writes into
// the tracked tree), and reads every instance back.
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.json;
import foundation.mcp;
import foundation.scene;
import foundation.scene.resource;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.composition;
import engine.script;                // ScriptComponentManager: the behaviours a scene stores
import foundation.script.resource;   // ScriptClassSource: what a cooked class declares
import editor.project;
import editor.mcp;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace
{
    // One tools/call round trip through the MCP server; the result must not be an error.
    JsonValue Call(foundation::mcp::McpServer& server, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue request = JsonValue::MakeObject();
        request.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        request.Set(u8"id", JsonValue::MakeNumber(1));
        request.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        request.Set(u8"params", Move(params));
        foundation::mcp::LineOutcome line = server.HandleLine(request.ToString().AsView());
        REQUIRE(line.state == foundation::mcp::LineState::Answered);
        JsonValue response = foundation::json::Parse(line.response.AsView()).value;
        REQUIRE(response.Has(u8"result"));
        REQUIRE(response.Get(u8"result").Get(u8"isError").AsBool() == false);
        return foundation::json::Parse(
                   response.Get(u8"result").Get(u8"content").At(0).Get(u8"text").AsString().AsView())
            .value;
    }

    // Every instance under a group (recursively) reads back; fails the case on a refusal. A
    // scene or prefab is its STREAM, not its document: the entities, components and system
    // settings carry their own data versions, so the stream loads into a scratch scene of the
    // full composition the way a page opens it (a stale RigidBody payload sat behind a valid
    // SceneDocument for a month).
    usize ReadAllInstances(foundation::content::Group& group)
    {
        usize read = 0;
        for (foundation::content::Instance* instance : group.Instances())
        {
            const RefPtr<ISerializable> object = instance->ReadObject();
            const String name(instance->Name());
            const std::string shown(reinterpret_cast<const char*>(name.CStr()), name.Size());
            CHECK_MESSAGE(object.Get() != nullptr, "sample project instance refused: ", shown);
            if (instance->TypeName() == StringView(u8"SceneDocument") ||
                instance->TypeName() == StringView(u8"PrefabDocument"))
            {
                foundation::scene::Scene scratch(DefaultAllocator(), name.AsView());
                engine::AddAllSceneManagers(scratch);
                const Status loaded = foundation::scene::LoadScene(*instance, scratch);
                CHECK_MESSAGE(loaded.IsOk(), "sample project scene stream refused: ", shown);
            }
            ++read;
        }
        for (foundation::content::Group* child : group.Groups())
        {
            read += ReadAllInstances(*child);
        }
        return read;
    }
}

namespace
{
    // The script overrides checked under `group`: each must name a property its behaviour's
    // COOKED class declares. The key is the property name's hash, so a hash change that forgets
    // to rehash the sources leaves overrides that silently apply to nothing.
    usize CheckOverrides(editor::EditorProject& project, foundation::content::Group& group)
    {
        usize count = 0;
        for (foundation::content::Instance* instance : group.Instances())
        {
            if (instance->TypeName() != StringView(u8"SceneDocument") &&
                instance->TypeName() != StringView(u8"PrefabDocument"))
            {
                continue;
            }
            const String name(instance->Name());
            const std::string shown(reinterpret_cast<const char*>(name.CStr()), name.Size());
            foundation::scene::Scene scratch(DefaultAllocator(), name.AsView());
            engine::AddAllSceneManagers(scratch);
            if (!foundation::scene::LoadScene(*instance, scratch).IsOk())
            {
                continue; // the read test above reports a refused stream
            }
            // One check for both places an override lives: a behaviour's, and the scene's Level
            // script's (the sceneScript settings block).
            const auto check = [&](const Guid& scriptId,
                                   Span<const engine::script::ScriptPropertyOverride> overrides)
            {
                foundation::content::Instance* cooked = project.CookedDb().GetInstance(scriptId);
                REQUIRE_MESSAGE(cooked != nullptr, "the script is cooked: ", shown);
                const RefPtr<ISerializable> object = cooked->ReadObject();
                const auto* source = Cast<foundation::script::ScriptClassSource>(object.Get());
                REQUIRE_MESSAGE(source != nullptr, "the script is a class: ", shown);
                for (const engine::script::ScriptPropertyOverride& o : overrides)
                {
                    bool declared = false;
                    for (const foundation::script::ScriptPropertyDesc& property : source->properties)
                    {
                        declared = declared || property.hash == o.nameHash;
                    }
                    CHECK_MESSAGE(declared, shown, ": override ", o.nameHash,
                                  " names no property of its class");
                    ++count;
                }
            };
            auto* scripts = scratch.GetSystem<engine::script::ScriptComponentManager>();
            REQUIRE(scripts != nullptr);
            for (const foundation::scene::EntityHandle owner : scripts->Owners())
            {
                for (const engine::script::ScriptBehavior& behavior : scripts->Get(owner)->behaviors)
                {
                    check(behavior.script.id,
                          Span<const engine::script::ScriptPropertyOverride>{
                              behavior.overrides.Data(), behavior.overrides.Size()});
                }
            }
            if (auto* level = scratch.GetSystem<engine::script::SceneScriptSystem>();
                level != nullptr && !level->Settings().script.id.IsNil())
            {
                const engine::script::SceneScriptSettings& settings = level->Settings();
                check(settings.script.id, Span<const engine::script::ScriptPropertyOverride>{
                                              settings.overrides.Data(), settings.overrides.Size()});
            }
        }
        for (foundation::content::Group* child : group.Groups())
        {
            count += CheckOverrides(project, *child);
        }
        return count;
    }
}

namespace
{
    struct SampleExpectations
    {
        usize read = 0;       // instances that read back, at least
        f64 cooked = 0.0;     // assets a forced cook builds, at least
        usize overrides = 0;  // scene-stored script overrides, each naming a declared property
    };

    // A tracked sample reads at the current data versions, cooks with no failure through the
    // tools an agent uses, and every script override its scenes store names a property its
    // cooked class declares. On a scratch copy, so opening never writes into the tracked tree.
    void CheckSampleProject(StringView sample, StringView scratchDir, const SampleExpectations& expect)
    {
        pipeline::RegisterPipelineTypes(); // every asset/product/resource type (idempotent)
        engine::RegisterAllSceneComponentReflection();
        engine::RegisterAllScriptFacades(); // idempotent; a case must not lean on another's
        // The readers log WHY they refuse; put that on the console so a red run names the record.
        ConsoleSink console;
        GlobalLogger().AddSink(&console);

        const String dataRoot = foundation::vfs::FindDataRoot();
        REQUIRE_FALSE(dataRoot.IsEmpty());
        const String source = PathJoin(dataRoot.AsView(), Format(u8"SampleProjects/{}", sample).AsView());
        REQUIRE(DirectoryExists(source.AsView()));

        const std::filesystem::path scratch(std::string(reinterpret_cast<const char*>(scratchDir.Data()),
                                                        scratchDir.Size()));
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        std::filesystem::copy(std::filesystem::path(reinterpret_cast<const char*>(source.CStr())),
                              scratch, std::filesystem::copy_options::recursive, ec);
        REQUIRE_FALSE(ec);
        {
            UniquePtr<editor::EditorProject> project =
                editor::EditorProject::Open(DefaultAllocator(), scratchDir);
            REQUIRE(static_cast<bool>(project));
            const usize read = ReadAllInstances(*project->SourceDb().RootGroup());
            CHECK(read >= expect.read);
        }

        // And it COOKS, through the same tools an agent uses: the driver builds items in parallel
        // on job workers, which is where the per-build registrations (core types, markup, script
        // facades) used to race - a double free that aborted Tools.Cook on PaperKid.
        {
            pipeline::BuilderRegistry builders{DefaultAllocator()};
            pipeline::RegisterAllBuilders(builders);
            pipeline::ImporterRegistry importers{DefaultAllocator()};
            pipeline::RegisterAllImporters(importers);
            foundation::mcp::McpServer server;
            editor::mcp::ProjectSession session;
            editor::mcp::ProjectOwner owner;
            editor::mcp::RegisterProjectOpenTools(server, session, owner);
            editor::mcp::RegisterProjectInfoTool(server, session);
            editor::mcp::RegisterAssetTools(server, session);
            editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
            editor::mcp::RegisterAssetWriteTools(server, session, importers, operations);
            JsonValue open = JsonValue::MakeObject();
            open.Set(u8"directory", JsonValue::MakeString(String(scratchDir)));
            (void)Call(server, u8"project_open", Move(open));
            JsonValue args = JsonValue::MakeObject();
            args.Set(u8"force", JsonValue::MakeBool(true));
            const JsonValue cooked = Call(server, u8"asset_cook", Move(args));
            CHECK(cooked.Get(u8"cooked").AsNumber() >= expect.cooked);
            CHECK(cooked.Get(u8"failed").AsNumber() == doctest::Approx(0.0));
        }

        // Every script override a scene stores names a property its cooked class declares (taken
        // from Sedulous 80272182: it fails on the old FNV basis, naming the follow camera's target).
        {
            UniquePtr<editor::EditorProject> project =
                editor::EditorProject::Open(DefaultAllocator(), scratchDir);
            REQUIRE(static_cast<bool>(project));
            const usize overrides = CheckOverrides(*project, *project->SourceDb().RootGroup());
            CHECK(overrides >= expect.overrides);
        }
        std::filesystem::remove_all(scratch, ec);
        GlobalLogger().RemoveSink(&console);
    }
}

TEST_CASE("sample project: every PaperKid source reads at the CURRENT data versions")
{
    // 2 fonts, the input map, 5 meshes, 6 scenes (five blocks and the title), 15 kit prefabs,
    // 10 scripts, 7 UI documents, 5 navigation zones, the minimap's render texture, 19 audio
    // clips (14 effects, 5 music tracks), 2 animation clips and the throw guides' material; the
    // overrides are the blocks' Level, camera, minimap and kit behaviour settings
    CheckSampleProject(u8"PaperKid", u8"scratch_paperkid_versions", {74u, 53.0, 58u});
}

TEST_CASE("sample project: Sky Hopper (PlatformerGame) reads, cooks and its overrides match")
{
    // 13 models with their parts, 12 clips of audio, 2 fonts, 5 effects + prefabs, 3 levels,
    // 8 scripts, 9 screens and a theme, the input map
    CheckSampleProject(u8"PlatformerGame", u8"scratch_platformer_versions", {150u, 100.0, 20u});
}

TEST_CASE("sample project: Snowline reads, cooks and its overrides match")
{
    // 3 courses, each a terrain (heightfield, splatmap, terrain asset) and a scene; the rider
    // (rigged mesh, skeleton, 8 clips, its animation graph), 9 models, 33 materials, 16 prefabs,
    // 13 scripts, 4 UI documents, 4 particle effects, 3 sounds, the input map and the render
    // profiles; the overrides are the gates', gems', kickers', ghosts', gap's and avalanche's settings
    CheckSampleProject(u8"Snowline", u8"scratch_snowline_versions", {127u, 108.0, 161u});
}
