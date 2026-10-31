// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :script_create partition
//
// script_create: seed a fresh script asset from the chosen backend's own starter (the language
// cook's NewAssetTemplate, never hardcoded text), through the creation the editor's New Asset menu
// and asset_create run (pipeline::CreateScriptInstance). The starter source is written to
// Sources/<name>.<ext> with a behaviour's class named after the asset, and a ScriptClassAsset
// envelope records {fileName, language} in the source database. The agent then edits the file
// (the file is the truth), checks it with script_validate, and runs asset_cook to make the class
// attachable.

module;
#include "Core/Prelude.h"

export module editor.mcp:script_create;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.vfs;
import foundation.script;
import foundation.mcp;
import pipeline.core;
import script.pipeline;
import editor.project;
import :session;
import :script_validate; // ScriptLanguageChoices (the shared language enum)

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

namespace editor::mcp::detail
{
    inline Array<String> ScriptTierChoices()
    {
        Array<String> choices;
        choices.PushBack(String(u8"behavior"));
        choices.PushBack(String(u8"level"));
        choices.PushBack(String(u8"game"));
        return choices;
    }

    inline pipeline::ScriptTier ParseScriptTier(StringView name)
    {
        if (name == StringView(u8"level"))
        {
            return pipeline::ScriptTier::Level;
        }
        if (name == StringView(u8"game"))
        {
            return pipeline::ScriptTier::Game;
        }
        return pipeline::ScriptTier::Behavior;
    }
}

export namespace editor::mcp
{
    // Registers script_create against `server` (it writes into the open project's source DB and
    // Sources/ directory).
    inline void RegisterScriptCreateTool(foundation::mcp::McpServer& server,
                                         ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        ProjectSession* s = &session;

        server.RegisterTool(
            u8"script_create",
            u8"Create a new script asset from the backend's starter template: writes "
            u8"Sources/<name>.<ext> with the tier's starter source and creates the script "
            u8"asset referencing it. Tiers: 'behavior' (per-entity, attach via a "
            u8"ScriptComponent slot), 'level' (per-scene, reserved class Level), 'game' (the "
            u8"project orchestrator, reserved class Game). Returns the asset guid and the "
            u8"source file path - edit the FILE to write your gameplay code, use "
            u8"script_validate as the loop, then asset_cook to make the class attachable.",
            SchemaBuilder()
                .Str(u8"name", u8"the asset name (also the source file stem)", true)
                .Enum(u8"language", detail::ScriptLanguageChoices(), u8"the script backend",
                      true)
                .Enum(u8"tier", detail::ScriptTierChoices(),
                      u8"which starter to seed (default behavior)")
                .Str(u8"group", u8"source-DB group path to place it in (slash-joined; "
                                u8"default root)")
                .Build(),
                foundation::mcp::ToolAnnotations::Creates(),
            [s](const JsonValue& args) -> ToolResult
            {
                if (!s->project)
                {
                    return Err(String(u8"no project is open (call project_open first)"));
                }
                const String requestedName = args.Get(u8"name").AsString();
                if (requestedName.IsEmpty())
                {
                    return Err(String(u8"name must not be empty"));
                }
                const String language = args.Get(u8"language").AsString();
                const foundation::script::ScriptBackendDesc* backend =
                    foundation::script::ScriptBackendRegistry::Get().FindByLanguage(
                        language.AsView());
                pipeline::IScriptLanguageCook* cook =
                    pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(language.AsView());
                if (backend == nullptr || backend->fileExtensions.IsEmpty() || cook == nullptr)
                {
                    return Err(Format(u8"the '{}' backend is not available in this host (it "
                                      u8"may be disabled in this build)",
                                      language.AsView()));
                }
                const pipeline::ScriptTier tier =
                    detail::ParseScriptTier(args.Get(u8"tier").AsString().AsView());

                // The same creation File > New and asset_create run: the starter written to
                // Sources/, a behaviour's class named after the asset, the asset pointing at the
                // file.
                const String sourcesRoot = s->project->SourcesRoot();
                pipeline::AssetCreationContext creation;
                creation.root = s->project->SourceDb().RootGroup();
                creation.picked = detail::ResolveGroupPath(creation.root,
                                                           args.Get(u8"group").AsString().AsView());
                creation.sourcesRoot = sourcesRoot.AsView();
                creation.name = requestedName.AsView();
                content::Instance* instance = pipeline::CreateScriptInstance(
                    creation, language.AsView(), backend->fileExtensions[0].AsView(), tier,
                    pipeline::ScriptTierBaseName(tier));
                if (instance == nullptr)
                {
                    return Err(Format(u8"could not create '{}': the source file under '{}' or the "
                                      u8"asset did not write (log_read says which)",
                                      requestedName.AsView(), sourcesRoot.AsView()));
                }
                const String name(instance->Name());
                String fileName(name.AsView());
                fileName.PushBack(utf8char('.'));
                fileName.Append(backend->fileExtensions[0].AsView());
                const String path = PathJoin(sourcesRoot.AsView(), fileName.AsView());

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"guid", detail::GuidToJson(instance->Id()));
                out.Set(u8"name", JsonValue::MakeString(name));
                out.Set(u8"language", JsonValue::MakeString(language));
                out.Set(u8"sourceFile", JsonValue::MakeString(path));
                out.Set(u8"fileName", JsonValue::MakeString(fileName));
                return out;
            });
    }
}
