// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp.Script - `foundation.mcp.script`
//
// The scripting MCP tool: script_api. It dumps the bound API per backend, the exact script-visible
// names and signatures a given language (AngelScript, Luau, ...) bound, so an agent can write
// correct scripts instead of guessing. This is the reflection registry's answer projected through
// a real backend: DescribeBoundApi says what a backend bound and how it spells it (they differ per
// language), which is what a script author needs, not the C++ names.
//
// The tool is backend-neutral: it resolves whatever backends the host registered
// (ScriptBackendRegistry) at call time, spins up a throwaway manager per language, replays the
// standard facade and reflected-type registration (the same sequence the editor's API browser
// uses), and reads the result. The host must have registered its backends (for example through
// Pipeline::Registration's RegisterPipelineTypes) before the tool is called.

module;
#include "Core/Prelude.h"

export module foundation.mcp.script;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.script;
import foundation.script.facades;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace foundation::mcp::script_detail
{
    // A domain's readable name from the small known set (it hashes the literals, so there is no
    // Pipeline or Editor module dependency). Empty for a custom domain; the in-player bit works for
    // any domain.
    inline StringView DomainName(TypeDomain domain)
    {
        if (domain == kRuntimeTypeDomain)
        {
            return u8"Runtime";
        }
        if (domain == TypeDomain(u8"Pipeline"))
        {
            return u8"Pipeline";
        }
        if (domain == TypeDomain(u8"Editor"))
        {
            return u8"Editor";
        }
        return u8"";
    }

    inline StringView KindName(foundation::script::ScriptApiMemberKind kind)
    {
        switch (kind)
        {
        case foundation::script::ScriptApiMemberKind::Property:
            return u8"property";
        case foundation::script::ScriptApiMemberKind::Constant:
            return u8"constant";
        case foundation::script::ScriptApiMemberKind::Operator:
            return u8"operator";
        case foundation::script::ScriptApiMemberKind::Method:
        default:
            return u8"method";
        }
    }

    // Build the {language, displayName, typeCount, types:[{scriptName,isNamespace,members:[...]}]}
    // object for one backend, spinning up a throwaway manager and reading its bound API.
    inline JsonValue DescribeBackend(const foundation::script::ScriptBackendDesc& backend)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"language", JsonValue::MakeString(backend.languageId));
        out.Set(u8"displayName", JsonValue::MakeString(backend.displayName));

        RefPtr<foundation::script::IScriptManager> manager =
            foundation::script::CreateScriptManagerForLanguage(backend.languageId.AsView(),
                                                               core::DefaultAllocator());
        JsonValue types = JsonValue::MakeArray();
        if (manager.Get() != nullptr)
        {
            foundation::script::RegisterReflectedTypes(*manager);
            for (const foundation::script::ScriptApiType& t : manager->DescribeBoundApi())
            {
                JsonValue typeObj = JsonValue::MakeObject();
                typeObj.Set(u8"scriptName", JsonValue::MakeString(t.scriptName));
                typeObj.Set(u8"isNamespace", JsonValue::MakeBool(t.isNamespace));
                // Availability domain, looked up in the registry by the reflected identity: agents
                // need to know a type exists for authoring (an asset, say) but not in a runtime
                // player. inPlayer is set for the Runtime domain and works for any domain; domain is
                // the readable name when known.
                if (t.typeId != 0)
                {
                    const TypeDomain domain = GlobalTypeRegistry().DomainOf(t.typeId);
                    const StringView domainName = DomainName(domain);
                    if (!domainName.IsEmpty())
                    {
                        typeObj.Set(u8"domain", JsonValue::MakeString(String(domainName)));
                    }
                    typeObj.Set(u8"inPlayer", JsonValue::MakeBool(domain == kRuntimeTypeDomain));
                }
                JsonValue members = JsonValue::MakeArray();
                for (const foundation::script::ScriptApiMember& m : t.members)
                {
                    JsonValue memberObj = JsonValue::MakeObject();
                    memberObj.Set(u8"name", JsonValue::MakeString(m.name));
                    memberObj.Set(u8"signature", JsonValue::MakeString(m.signature));
                    memberObj.Set(u8"isStatic", JsonValue::MakeBool(m.isStatic));
                    memberObj.Set(u8"readOnly", JsonValue::MakeBool(m.readOnly));
                    memberObj.Set(u8"kind", JsonValue::MakeString(String(KindName(m.kind))));
                    members.Add(Move(memberObj));
                }
                typeObj.Set(u8"members", Move(members));
                types.Add(Move(typeObj));
            }
        }
        out.Set(u8"typeCount", JsonValue::MakeNumber(static_cast<f64>(types.Count())));
        out.Set(u8"types", Move(types));
        return out;
    }
}

export namespace foundation::mcp
{
    // Registers script_api against `server`. The backends it reports are whatever the host has
    // registered in the global ScriptBackendRegistry (resolved live at call time).
    inline void RegisterScriptTools(McpServer& server)
    {
        server.RegisterTool(
            u8"script_api",
            u8"The per-backend bound scripting API: every script-visible type and member a language "
            u8"(AngelScript/Luau/...) actually binds, spelled the way scripts use it. Use it to write "
            u8"correct scripts. Optional 'language' narrows to one backend.",
            SchemaBuilder()
                .Str(u8"language", u8"backend language id (e.g. \"angelscript\", \"luau\"); default: all")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [](const JsonValue& args) -> Result<JsonValue, String>
            {
                const String language = args.Get(u8"language").AsString();

                // The standard headless build sequence, as the editor's API browser runs it: core
                // types and the script facades must be reflected before a manager can bind them.
                RegisterCoreTypes();
                foundation::script::RegisterScriptFacadeReflection();

                JsonValue languages = JsonValue::MakeArray();
                bool matched = false;
                for (const foundation::script::ScriptBackendDesc& backend :
                     foundation::script::ScriptBackendRegistry::Get().All())
                {
                    if (!language.IsEmpty() && backend.languageId.AsView() != language.AsView())
                    {
                        continue;
                    }
                    matched = true;
                    languages.Add(script_detail::DescribeBackend(backend));
                }

                if (!language.IsEmpty() && !matched)
                {
                    return Err(Format(u8"no scripting backend registered for language '{}'",
                                      language.AsView()));
                }

                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"languages", Move(languages));
                return out;
            });
    }
}
