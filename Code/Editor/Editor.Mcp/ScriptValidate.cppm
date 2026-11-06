// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :script_validate partition
//
// script_validate: compile a script source against a chosen backend without saving anything,
// through the same per-language cook service the asset pipeline uses (ScriptLanguageCookRegistry),
// so what validates here is what would cook. It returns the compile errors with file and line,
// and on success the harvested metadata (class name, declared handlers, editor properties,
// coroutine use), so the agent sees what the engine recognized, not just "ok".
//
// This is only a compile check. A misspelled engine method is a compile error only where the
// language sees the call (AngelScript compiles against the bound API; Luau resolves a member at
// run time); a typed check would need the Luau analyzer toolchain. The tool's description says
// so.
//
// With a `platform`, the script is also checked against what the open project's plugins leave on
// that platform: the types of the plugins this host loaded that the project does not enable there
// are left out, as an export for that platform checks it, so an agent finds a use of a plugin's
// facade before the export refuses it.

module;
#include "Core/Prelude.h"

export module editor.mcp:script_validate;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.script.resource;
import script.pipeline;
import pipeline.importer; // FileStemOf: the class named like the file
import editor.project; // detail::AbsentPluginTypes (what a platform lacks of the project's plugins)
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace editor::mcp::detail
{
    inline Array<String> ScriptLanguageChoices()
    {
        Array<String> choices;
        choices.PushBack(String(u8"angelscript"));
        choices.PushBack(String(u8"luau"));
        return choices;
    }
}

export namespace editor::mcp
{
    // Registers script_validate. It compiles in-memory source, so it needs no project but for a
    // `platform` (the open project's plugins on it); the host's language cooks must be registered
    // first (Pipeline::Registration does it).
    inline void RegisterScriptValidateTool(foundation::mcp::McpServer& server, ProjectSession& session)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;

        server.RegisterTool(
            u8"script_validate",
            u8"COMPILE-CHECK a script source against a backend without saving: the exact "
            u8"compile the asset cook would run. Returns compile errors with line numbers, "
            u8"and on success the harvested metadata (className, handlers, properties, "
            u8"usesCoroutines) - confirm the engine recognized what you wrote. Use as the "
            u8"validation loop while authoring scripts, BEFORE creating the asset. Limits: "
            u8"compile only - a misspelled engine method is a compile error only where the "
            u8"language sees the call; check signatures with script_api. With `platform`, also "
            u8"checks it against what the open project's plugins leave there (a plugin the project "
            u8"does not enable on that platform takes its facades with it), as an export for it does.",
            SchemaBuilder()
                .Str(u8"source", u8"the full script source text", true)
                .Enum(u8"language", detail::ScriptLanguageChoices(),
                      u8"the script backend to compile against", true)
                .Str(u8"name", u8"a display name for error messages (e.g. the intended file "
                               u8"name; default 'script'); its stem is the class it plays when "
                               u8"the source declares several")
                .Str(u8"className", u8"the class it plays when the source declares several (a "
                                    u8"script asset's Class); default: the one named like `name`")
                .Str(u8"platform", u8"also check it for this platform's plugins (Linux64, Win64, Web); "
                                   u8"needs an open project")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [&session](const JsonValue& args) -> ToolResult
            {
                const String platform = args.Get(u8"platform").AsString();
                if (!platform.IsEmpty() && session.project == nullptr)
                {
                    return Err(String(u8"a `platform` check needs an open project (its plugins)"));
                }
                const String language = args.Get(u8"language").AsString();
                String name = args.Get(u8"name").AsString();
                if (name.IsEmpty())
                {
                    name = String(u8"script");
                }
                pipeline::IScriptLanguageCook* cook =
                    pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(language.AsView());
                if (cook == nullptr)
                {
                    return Err(Format(u8"the '{}' backend is not available in this host (its "
                                      u8"cook did not register - the backend may be disabled "
                                      u8"in this build)",
                                      language.AsView()));
                }

                String preferred = args.Get(u8"className").AsString();
                if (preferred.IsEmpty())
                {
                    preferred = String(pipeline::FileStemOf(pipeline::FileNameOf(name.AsView())));
                }
                const String source = args.Get(u8"source").AsString();
                pipeline::CookScriptErrorSink sink;
                foundation::script::ScriptClassSource harvested;
                bool ok = cook->Cook(source.AsView(), name.AsView(), preferred.AsView(), sink, harvested);
                // Then against the platform's surface, when one is asked for and lacks plugins.
                String absent;
                if (ok && !platform.IsEmpty() && session.plugins != nullptr)
                {
                    Array<TypeId> hidden;
                    editor::detail::AbsentPluginTypes(*session.project, *session.plugins, platform.AsView(), hidden,
                                                      absent);
                    if (!hidden.IsEmpty())
                    {
                        String className;
                        ok = cook->Check(source.AsView(), name.AsView(), preferred.AsView(), sink, className,
                                         Span<const TypeId>(hidden.Data(), hidden.Size()));
                    }
                }

                JsonValue errors = JsonValue::MakeArray();
                for (const pipeline::CookScriptErrorSink::Entry& e : sink.errors)
                {
                    JsonValue entry = JsonValue::MakeObject();
                    entry.Set(u8"module", JsonValue::MakeString(
                                              e.module.IsEmpty() ? name : e.module));
                    entry.Set(u8"line", JsonValue::MakeNumber(static_cast<f64>(e.line)));
                    entry.Set(u8"message", JsonValue::MakeString(e.message));
                    errors.Add(Move(entry));
                }

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"valid", JsonValue::MakeBool(ok));
                out.Set(u8"errors", Move(errors));
                if (!platform.IsEmpty())
                {
                    // The plugins the platform lacks, whose types the check left out.
                    out.Set(u8"platform", JsonValue::MakeString(platform));
                    out.Set(u8"absentPlugins", JsonValue::MakeString(absent));
                }
                if (ok)
                {
                    out.Set(u8"className", JsonValue::MakeString(harvested.className));
                    JsonValue handlers = JsonValue::MakeArray();
                    for (const String& h : harvested.handlers)
                    {
                        handlers.Add(JsonValue::MakeString(h));
                    }
                    out.Set(u8"handlers", Move(handlers));
                    JsonValue properties = JsonValue::MakeArray();
                    for (const foundation::script::ScriptPropertyDesc& p : harvested.properties)
                    {
                        properties.Add(JsonValue::MakeString(p.name));
                    }
                    out.Set(u8"properties", Move(properties));
                    out.Set(u8"usesCoroutines",
                            JsonValue::MakeBool(harvested.usesCoroutines));
                }
                // Tells the caller what this validation covers, as scene_validate does.
                out.Set(u8"checkLevel", JsonValue::MakeString(u8"compile"));
                return out;
            });
    }
}
