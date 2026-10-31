// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script.Luau - implementation unit for the Luau cook service.
//
// The Luau-specific cook: the compile check (Load the raw source; Luau classes are plain
// global tables and facades are globals, so no behavior-module framing is needed), the
// construct-and-walk property harvest (construct the class with a nil owner and read the
// instance's scalar fields), and the cook registration. No Luau C header here: the cook VM is
// reached through the neutral IScriptContext surface.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module script.luau.pipeline;

import foundation.core;
import pipeline.importer; // FileStemOf
import foundation.script;
import foundation.script.resource;
import foundation.script.facades;
import script.pipeline;
import foundation.script.luau;

using namespace foundation::core;
using namespace foundation::script;

namespace pipeline{
    namespace
    {
        // The Luau property probe, loaded into the same context as the class (globals persist
        // across chunks in one lua_State, so `<Class>` resolves). It constructs the class with
        // a nil owner and serializes each scalar instance field into the shared harvest-record
        // format (name \031 type \031 payload, records joined by \030) that ParseHarvestRecord
        // reads. Handles/tables/functions (the entity, the scene, methods) are not scalars, so
        // they are skipped. The delimiters use the decimal escapes \031 and \030, which every Lua reads.
        [[nodiscard]] String BuildLuauPropertyProbe(StringView className)
        {
            String cls(className);
            String p;
            p += u8"drHarvestResult = \"\"\n";
            p += u8"drHarvestError = \"\"\n";
            p += u8"do\n";
            p += u8"    local ok, inst = pcall(function() return ";
            p += cls.AsView();
            p += u8".new(nil) end)\n";
            p += u8"    if not ok then\n";
            p += u8"        drHarvestError = tostring(inst)\n";
            p += u8"    elseif type(inst) ~= \"table\" then\n";
            p += u8"        drHarvestError = \"new() did not return a table\"\n";
            p += u8"    else\n";
            p += u8"        local out = \"\"\n";
            p += u8"        for k, v in pairs(inst) do\n";
            p += u8"            if type(k) == \"string\" then\n";
            p += u8"                local vt = type(v)\n";
            p += u8"                local rec = nil\n";
            p += u8"                if vt == \"number\" then\n";
            p += u8"                    rec = \"float\\031n:\" .. tostring(v)\n";
            p += u8"                elseif vt == \"boolean\" then\n";
            p += u8"                    rec = \"bool\\031b:\" .. tostring(v)\n";
            p += u8"                elseif vt == \"string\" then\n";
            p += u8"                    rec = \"string\\031s:\" .. v\n";
            p += u8"                elseif vt == \"vector\" then\n";
            p += u8"                    rec = \"vec3\\031l:\" .. tostring(v.x) .. \",\" .. tostring(v.y) "
                 u8".. \",\" .. tostring(v.z)\n";
            p += u8"                end\n";
            p += u8"                if rec ~= nil then\n";
            p += u8"                    out = out .. k .. \"\\031\" .. rec .. \"\\030\"\n";
            p += u8"                end\n";
            p += u8"            end\n";
            p += u8"        end\n";
            p += u8"        drHarvestResult = out\n";
            p += u8"    end\n";
            p += u8"end\n";
            // The class's `replicated` list (a field of the class table, not an instance field, so
            // never a property itself): the names joined by \030.
            p += u8"drReplicated = \"\"\n";
            p += u8"if type(";
            p += cls.AsView();
            p += u8") == \"table\" and type(";
            p += cls.AsView();
            p += u8".replicated) == \"table\" then\n";
            p += u8"    for _, name in ipairs(";
            p += cls.AsView();
            p += u8".replicated) do drReplicated = drReplicated .. tostring(name) .. \"\\030\" end\n";
            p += u8"end\n";
            return p;
        }

        // Luau has no `class` keyword (so the shared FindScriptClassName does not apply): the
        // class is a global table with a `function <Name>.new(...)` constructor, the same table
        // the runtime instantiates. Find that name; prefer the one matching the file stem, else
        // the first constructor declared. Comments are stripped first.
        [[nodiscard]] String FindLuauClassName(StringView source, StringView preferredName)
        {
            const String stripped = StripScriptComments(source);
            const StringView text = stripped.AsView();
            const StringView keyword = u8"function ";
            const StringView dotNew = u8".new";
            String first;
            for (usize i = 0; i + keyword.Size() < text.Size(); ++i)
            {
                if (text.SubStr(i, keyword.Size()) != keyword)
                {
                    continue;
                }
                usize begin = i + keyword.Size();
                while (begin < text.Size() && (text[begin] == u8' ' || text[begin] == u8'\t'))
                {
                    ++begin;
                }
                usize end = begin;
                while (end < text.Size() && detail::IsIdentChar(text[end]))
                {
                    ++end;
                }
                if (end == begin)
                {
                    continue;
                }
                // The constructor is `<Name>.new` (static, dot); instance methods use a colon.
                if (end + dotNew.Size() > text.Size() || text.SubStr(end, dotNew.Size()) != dotNew)
                {
                    continue;
                }
                const StringView name = text.SubStr(begin, end - begin);
                if (name == preferredName)
                {
                    return String(name);
                }
                if (first.IsEmpty())
                {
                    first = String(name);
                }
            }
            return first;
        }

        // Sort harvested props by name (pairs() order is unspecified) for deterministic bytes.
        void SortPropertiesByName(Array<ScriptPropertyDesc>& properties)
        {
            auto lessThan = [](StringView a, StringView b)
            {
                const usize n = a.Size() < b.Size() ? a.Size() : b.Size();
                for (usize k = 0; k < n; ++k)
                {
                    if (a[k] != b[k])
                    {
                        return a[k] < b[k];
                    }
                }
                return a.Size() < b.Size();
            };
            for (usize i = 1; i < properties.Size(); ++i)
            {
                for (usize j = i; j > 0 && lessThan(properties[j].name.AsView(),
                                                    properties[j - 1].name.AsView());
                     --j)
                {
                    ScriptPropertyDesc tmp = Move(properties[j]);
                    properties[j] = Move(properties[j - 1]);
                    properties[j - 1] = Move(tmp);
                }
            }
        }

        // Marks the properties the class's `replicated` list names. False, with the problem in
        // `error`, for a name that is not one of its properties (a typo must not do nothing).
        [[nodiscard]] bool MarkLuauReplicated(IScriptContext& scriptContext, Array<ScriptPropertyDesc>& properties,
                                              String& error)
        {
            const Variant listVariant = scriptContext.GetGlobal(u8"drReplicated");
            const String* list = listVariant.TryGet<String>();
            if (list == nullptr)
            {
                return true;
            }
            const StringView text = list->AsView();
            usize begin = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] != utf8char(0x1E))
                {
                    continue;
                }
                const StringView name = text.SubStr(begin, i - begin);
                begin = i + 1;
                bool found = false;
                for (ScriptPropertyDesc& property : properties)
                {
                    if (property.name.AsView() == name)
                    {
                        property.replicated = true;
                        found = true;
                    }
                }
                if (!found)
                {
                    error = Format(u8"'{}' in the class's replicated list is not one of its properties", name);
                    return false;
                }
            }
            return true;
        }

        // Constructs the class in the cook VM and harvests its scalar fields. A probe fault or a
        // constructor that faults on the nil owner is not a cook error (the class is valid at
        // runtime with a real owner); it just yields no editor properties, with a note.
        void HarvestLuauProperties(IScriptContext& scriptContext, StringView fileName,
                                   StringView className, Array<ScriptPropertyDesc>& outProperties)
        {
            const String probe = BuildLuauPropertyProbe(className);
            if (!scriptContext.Load(probe.AsView(), fileName).IsOk())
            {
                LOG_WARNING(u8"Script",
                                    u8"'{}': Luau property-harvest probe failed to run - no editor "
                                    u8"properties",
                                    fileName);
                return;
            }
            const Variant errorVariant = scriptContext.GetGlobal(u8"drHarvestError");
            if (const String* probeError = errorVariant.TryGet<String>();
                probeError != nullptr && !probeError->IsEmpty())
            {
                LOG_WARNING(u8"Script",
                                    u8"'{}': constructing '{}' for property harvest faulted ({}) - "
                                    u8"no editor properties (keep the constructor pure field init)",
                                    fileName, className, *probeError);
                return;
            }
            const Variant resultVariant = scriptContext.GetGlobal(u8"drHarvestResult");
            const String* harvest = resultVariant.TryGet<String>();
            if (harvest == nullptr)
            {
                return;
            }
            const StringView text = harvest->AsView();
            usize begin = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] != utf8char(0x1E))
                {
                    continue;
                }
                const StringView record = text.SubStr(begin, i - begin);
                begin = i + 1;
                if (record.IsEmpty())
                {
                    continue;
                }
                ScriptPropertyDesc desc;
                String error;
                if (!ParseHarvestRecord(record, desc, error))
                {
                    LOG_WARNING(u8"Script", u8"'{}': {} - property skipped", fileName, error);
                    continue;
                }
                outProperties.PushBack(Move(desc));
            }
            SortPropertiesByName(outProperties);
        }

        class LuauScriptCook final : public IScriptLanguageCook
        {
        private:
            // The cook VM, from the registry by language, carrying the same global surface the
            // runtime registers (core math and the behavior facades) so top-level facade references
            // compile identically. Idempotent. Null (logged) when the backend is absent.
            [[nodiscard]] static RefPtr<IScriptManager> CreateCookManager(StringView assetName)
            {
                RefPtr<IScriptManager> manager =
                    CreateScriptManagerForLanguage(u8"luau", DefaultAllocator());
                if (manager.Get() == nullptr)
                {
                    LOG_ERROR(u8"Script", u8"'{}': no Luau backend registered - cook failed",
                                       assetName);
                    return manager;
                }
                RegisterCoreTypes();
                RegisterScriptFacadeReflection();
                RegisterReflectedTypes(*manager);
                return manager;
            }

        public:
            [[nodiscard]] String NewAssetTemplate(ScriptTier tier,
                                                  StringView className) const override
            {
                switch (tier)
                {
                case ScriptTier::Level:
                    return String(kLuauLevelStarter);
                case ScriptTier::Game:
                    return String(kLuauGameStarter);
                case ScriptTier::Behavior:
                default:
                    return FillStarterClassName(kLuauBehaviorStarter, className);
                }
            }

            // Luau bytecode is version-locked: fold the vendored bytecode version into the cook
            // fingerprint so a vendor bump recooks every Luau script pack.
            [[nodiscard]] u32 CookVersion() const override { return LuauBytecodeVersion(); }

            // Compiles through the manager's Check, so unlike Cook nothing in the source runs.
            [[nodiscard]] bool Check(StringView source, StringView assetName,
                                     CookScriptErrorSink& sink, String& className) override
            {
                RefPtr<IScriptManager> manager = CreateCookManager(assetName);
                if (manager.Get() == nullptr || !manager->Check(source, assetName, sink))
                {
                    return false;
                }
                className = FindLuauClassName(source, pipeline::FileStemOf(assetName));
                return true;
            }

            [[nodiscard]] bool Cook(StringView source, StringView assetName,
                                    CookScriptErrorSink& sink, ScriptClassSource& out) override
            {
                out.language = String(u8"luau");
                out.sourceName = String(assetName); // the source file identity (breakpoint key)
                out.source = String(source);

                RefPtr<IScriptManager> manager = CreateCookManager(assetName);
                if (manager.Get() == nullptr)
                {
                    return false;
                }
                RefPtr<IScriptContext> scriptContext = manager->CreateContext();
                if (scriptContext.Get() == nullptr)
                {
                    return false;
                }
                scriptContext->SetErrorHandler(&sink);

                // Compile check: Load the raw source. Luau classes are plain global tables and
                // facades are globals, so no behavior-module framing is needed; unknown
                // globals compile fine. Running it defines `<Class>`.
                if (!scriptContext->Load(source, assetName).IsOk())
                {
                    ReportScriptCookErrors(assetName, sink);
                    return false;
                }

                out.className = FindLuauClassName(source, pipeline::FileStemOf(assetName));
                out.handlers = ScanScriptHandlers(source);
                out.usesCoroutines = ScriptReferencesCoroutineStart(source);

                // Bytecode into the pack: the player loads bytecode only; it has no compiler.
                // The source already compiled (Load above), so this succeeds; store the serialized
                // blob so the runtime reconstructs it via IScriptManager::CreateBlob and LoadBlob.
                // The source stays on the record for dev-mode hot reload.
                Result<RefPtr<IScriptBlob>> blob = manager->CompileToBlob(source, assetName);
                if (blob.HasValue() && blob.Value().Get() != nullptr)
                {
                    MemoryStream stream;
                    {
                        BinarySerializer writer(stream, SerializeMode::Write);
                        blob.Value()->Serialize(writer);
                    }
                    const Span<const byte> bytes = stream.Bytes();
                    out.bytecode.Reserve(bytes.Size());
                    for (usize i = 0; i < bytes.Size(); ++i)
                    {
                        out.bytecode.PushBack(bytes[i]);
                    }
                }

                // Editor properties: construct the class and walk its scalar fields. Only when the
                // source declares a class.
                if (!out.className.IsEmpty())
                {
                    HarvestLuauProperties(*scriptContext, assetName, out.className.AsView(),
                                          out.properties);
                    String replicatedError;
                    if (!MarkLuauReplicated(*scriptContext, out.properties, replicatedError))
                    {
                        ReportCookProblem(sink, assetName, replicatedError);
                        return false;
                    }
                }
                return true;
            }
        };
    }

    void RegisterLuauScriptCook()
    {
        // The cook needs the Luau backend in the registry; register it idempotently so a cook is
        // never resolvable without its VM.
        foundation::script::RegisterLuauScriptBackend();
        ScriptLanguageCookRegistry::Get().Register(
            String(u8"luau"), UniquePtr<IScriptLanguageCook>(
                                  DefaultAllocator().New<LuauScriptCook>(), DefaultAllocator()));
    }
}
