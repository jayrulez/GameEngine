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

module;
#include "Core/Prelude.h"

export module editor.mcp:script_validate;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.script.resource;
import script.pipeline;

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
    // Registers script_validate. It is project-independent (it compiles in-memory source); the
    // host's language cooks must be registered first (Pipeline::Registration does it).
    inline void RegisterScriptValidateTool(foundation::mcp::McpServer& server)
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
            u8"language sees the call; check signatures with script_api.",
            SchemaBuilder()
                .Str(u8"source", u8"the full script source text", true)
                .Enum(u8"language", detail::ScriptLanguageChoices(),
                      u8"the script backend to compile against", true)
                .Str(u8"name", u8"a display name for error messages (e.g. the intended file "
                               u8"name; default 'script')")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [](const JsonValue& args) -> ToolResult
            {
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

                const String source = args.Get(u8"source").AsString();
                pipeline::CookScriptErrorSink sink;
                foundation::script::ScriptClassSource harvested;
                const bool ok = cook->Cook(source.AsView(), name.AsView(), sink, harvested);

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
