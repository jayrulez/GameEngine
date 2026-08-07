// Draconic::ScriptWrenEditor - implementation unit for the Wren cook service.
//
// The Wren-specific cook: the `static properties` Fiber probe, the harvest-record parse,
// the compile check (framed with the SAME behavior-module prelude the runtime uses, so
// `is Behavior` and facade imports resolve at cook exactly as at runtime), and the cook
// registration. No Wren C header here either - the cook VM is reached through the neutral
// IScriptContext surface.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.script.wren.editor;

import draconic.foundation;
import draconic.editor.core; // FileStemOf
import draconic.script;
import draconic.script.resource;
import draconic.script.facades;
import draconic.script.editor;
import draconic.script.wren;

using namespace draconic::foundation;

namespace draconic::script
{
    namespace
    {
        // Parses "a,b,c[,d]" into up to 4 floats; returns the count parsed.
        [[nodiscard]] u32 ParseFloatList(StringView text, f32 (&out)[4])
        {
            u32 count = 0;
            usize begin = 0;
            for (usize i = 0; i <= text.Size() && count < 4; ++i)
            {
                if (i == text.Size() || text[i] == u8',')
                {
                    const StringView piece = text.SubStr(begin, i - begin);
                    begin = i + 1;
                    if (piece.IsEmpty())
                    {
                        continue;
                    }
                    // Minimal float parse (sign, digits, dot, exponent-free harvest output).
                    f64 value = 0.0;
                    f64 scale = 1.0;
                    bool negative = false;
                    bool afterDot = false;
                    for (usize j = 0; j < piece.Size(); ++j)
                    {
                        const utf8char c = piece[j];
                        if (j == 0 && c == u8'-')
                        {
                            negative = true;
                            continue;
                        }
                        if (c == u8'.')
                        {
                            afterDot = true;
                            continue;
                        }
                        if (c < u8'0' || c > u8'9')
                        {
                            continue;
                        }
                        if (afterDot)
                        {
                            scale *= 0.1;
                            value += (c - u8'0') * scale;
                        }
                        else
                        {
                            value = value * 10.0 + (c - u8'0');
                        }
                    }
                    out[count++] = static_cast<f32>(negative ? -value : value);
                }
            }
            return count;
        }

        // The Wren property probe, appended INTO the class's own module so the class name
        // resolves without imports. Wren maps have no guaranteed key order - the cook SORTS
        // the parsed records by name for deterministic bytes.
        [[nodiscard]] String BuildWrenPropertyProbe(StringView className)
        {
            String cls(className);
            String probe;
            probe += u8"var drHarvestResult = \"\"\n";
            probe += u8"var drHarvestError = \"\"\n";
            // NOTE Wren parses a single-line `{ ... }` as an EXPRESSION body - every block
            // holding a statement must span multiple lines.
            probe += u8"var drHarvestSer\n";
            probe += u8"drHarvestSer = Fn.new {|v|\n";
            probe += u8"  var out = \"?\"\n";
            probe += u8"  if (v == null) {\n";
            probe += u8"    out = \"~\"\n";
            probe += u8"  } else if (v is Num) {\n";
            probe += u8"    out = \"n:\" + v.toString\n";
            probe += u8"  } else if (v is Bool) {\n";
            probe += u8"    out = \"b:\" + v.toString\n";
            probe += u8"  } else if (v is String) {\n";
            probe += u8"    out = \"s:\" + v\n";
            probe += u8"  } else if (v is List) {\n";
            probe += u8"    var parts = \"\"\n";
            probe += u8"    for (e in v) {\n";
            probe += u8"      if (parts != \"\") parts = parts + \",\"\n";
            probe += u8"      parts = parts + e.toString\n";
            probe += u8"    }\n";
            probe += u8"    out = \"l:\" + parts\n";
            probe += u8"  }\n";
            probe += u8"  return out\n";
            probe += u8"}\n";
            probe += u8"var drHarvestFiber = Fiber.new {\n";
            probe += u8"  var m = ";
            probe += cls.AsView();
            probe += u8".properties\n";
            probe += u8"  var out = \"\"\n";
            probe += u8"  for (k in m.keys) {\n";
            probe += u8"    var entry = m[k]\n";
            probe += u8"    var type = \"\"\n";
            probe += u8"    var dflt = \"~\"\n";
            probe += u8"    var desc = \"\"\n";
            probe += u8"    if (entry is List) {\n";
            probe += u8"      if (entry.count > 0) { type = entry[0].toString }\n";
            probe += u8"      if (entry.count > 1) { dflt = drHarvestSer.call(entry[1]) }\n";
            probe += u8"      if (entry.count > 2) { desc = entry[2].toString }\n";
            probe += u8"    } else {\n";
            probe += u8"      type = entry.toString\n";
            probe += u8"    }\n";
            probe += u8"    out = out + k + \"\\x1f\" + type + \"\\x1f\" + dflt + \"\\x1f\" + desc "
                     u8"+ \"\\x1e\"\n";
            probe += u8"  }\n";
            probe += u8"  drHarvestResult = out\n";
            probe += u8"}\n";
            probe += u8"var drHarvestCaught = drHarvestFiber.try()\n";
            probe +=
                u8"if (drHarvestCaught != null) { drHarvestError = drHarvestCaught.toString }\n";
            return probe;
        }

        // One record of the probe's harvest string -> a property desc. Record layout:
        // name \x1F typeString \x1F default \x1F description, where default is "~" (none),
        // "n:<num>", "b:true|false", "s:<text>" or "l:<a,b,c[,d]>".
        [[nodiscard]] bool ParseHarvestRecord(StringView record, ScriptPropertyDesc& out,
                                              String& outError)
        {
            StringView fields[4];
            u32 fieldCount = 0;
            usize begin = 0;
            for (usize i = 0; i <= record.Size() && fieldCount < 4; ++i)
            {
                if (i == record.Size() || record[i] == utf8char(0x1F))
                {
                    fields[fieldCount++] = record.SubStr(begin, i - begin);
                    begin = i + 1;
                }
            }
            if (fieldCount < 2 || fields[0].IsEmpty())
            {
                outError = String(u8"malformed property record");
                return false;
            }
            out.name = String(fields[0]);
            out.hash = ScriptPropertyNameHash(fields[0]);
            out.description = fieldCount > 3 ? String(fields[3]) : String{};
            if (!ParseScriptPropertyType(fields[1], out.type, out.assetType))
            {
                outError = String(u8"property '");
                outError += fields[0];
                outError += u8"' has unknown type '";
                outError += fields[1];
                outError +=
                    u8"' (valid: float, int, bool, string, color, vec3, entity, asset:<TypeName>)";
                return false;
            }

            // Default value: typed from the serialized payload; "~" = the type's default.
            ScriptPropertyValue& value = out.defaultValue;
            value.kind = out.type;
            const StringView payload = fieldCount > 2 ? fields[2] : StringView(u8"~");
            if (payload == u8"~" || payload.Size() < 2)
            {
                return true;
            }
            const utf8char tag = payload[0];
            const StringView body = payload.SubStr(2, payload.Size() - 2);
            switch (out.type)
            {
            case ScriptPropertyType::Float:
            case ScriptPropertyType::Int:
                if (tag == u8'n')
                {
                    f32 numbers[4] = {};
                    if (ParseFloatList(body, numbers) > 0)
                    {
                        value.number = static_cast<f64>(numbers[0]);
                        if (out.type == ScriptPropertyType::Int)
                        {
                            value.number = static_cast<f64>(static_cast<i64>(value.number));
                        }
                    }
                }
                break;
            case ScriptPropertyType::Bool:
                value.boolean = (tag == u8'b' && body == u8"true");
                break;
            case ScriptPropertyType::String:
                if (tag == u8's')
                {
                    value.text = String(body);
                }
                break;
            case ScriptPropertyType::Color:
                if (tag == u8'l')
                {
                    f32 numbers[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    const u32 parsed = ParseFloatList(body, numbers);
                    if (parsed >= 3)
                    {
                        value.color = Color{numbers[0], numbers[1], numbers[2],
                                            parsed >= 4 ? numbers[3] : 1.0f};
                    }
                }
                break;
            case ScriptPropertyType::Vec3:
                if (tag == u8'l')
                {
                    f32 numbers[4] = {};
                    if (ParseFloatList(body, numbers) >= 3)
                    {
                        value.vector = Float3{numbers[0], numbers[1], numbers[2]};
                    }
                }
                break;
            case ScriptPropertyType::Entity:
            case ScriptPropertyType::Asset:
                // Only null defaults are expressible in script; guids come from overrides.
                break;
            case ScriptPropertyType::None:
            default:
                break;
            }
            return true;
        }

        // Whether the (comment-stripped) source declares the `static properties` getter.
        [[nodiscard]] bool DeclaresStaticProperties(StringView source)
        {
            const String stripped = StripScriptComments(source);
            const StringView text = stripped.AsView();
            const StringView keyword = u8"static properties";
            if (text.Size() < keyword.Size())
            {
                return false;
            }
            for (usize i = 0; i + keyword.Size() <= text.Size(); ++i)
            {
                if (text.SubStr(i, keyword.Size()) == keyword)
                {
                    return true;
                }
            }
            return false;
        }

        // Appends the probe into the class's own module (same chunk name = same Wren
        // module, so the class resolves), then reads the harvest globals back.
        [[nodiscard]] Status HarvestWrenProperties(IScriptContext& context, StringView fileName,
                                                   StringView className, CookScriptErrorSink& sink,
                                                   Array<ScriptPropertyDesc>& outProperties)
        {
            const String probe = BuildWrenPropertyProbe(className);
            if (!context.Load(probe.AsView(), fileName).IsOk())
            {
                ReportScriptCookErrors(fileName, sink);
                return Status{ErrorCode::InvalidArgument};
            }
            const Variant errorVariant = context.GetGlobal(u8"drHarvestError");
            if (const String* probeError = errorVariant.TryGet<String>();
                probeError != nullptr && !probeError->IsEmpty())
            {
                DRACONIC_LOG_ERROR(
                    u8"Script",
                    u8"'{}': `static properties` of class '{}' faulted: {} - cook failed", fileName,
                    className, *probeError);
                return Status{ErrorCode::InvalidArgument};
            }
            const Variant resultVariant = context.GetGlobal(u8"drHarvestResult");
            const String* harvest = resultVariant.TryGet<String>();
            if (harvest == nullptr)
            {
                return Status{};
            } // no properties getter at all

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
                    DRACONIC_LOG_ERROR(u8"Script", u8"'{}': {} - cook failed", fileName, error);
                    return Status{ErrorCode::InvalidArgument};
                }
                outProperties.PushBack(Move(desc));
            }
            // Wren map order is unspecified: sort by name for deterministic cooked bytes.
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
            for (usize i = 1; i < outProperties.Size(); ++i)
            {
                for (usize j = i; j > 0 && lessThan(outProperties[j].name.AsView(),
                                                    outProperties[j - 1].name.AsView());
                     --j)
                {
                    ScriptPropertyDesc tmp = Move(outProperties[j]);
                    outProperties[j] = Move(outProperties[j - 1]);
                    outProperties[j - 1] = Move(tmp);
                }
            }
            return Status{};
        }

        class WrenScriptCook final : public IScriptLanguageCook
        {
        public:
            [[nodiscard]] StringView NewAssetTemplate(ScriptTier tier) const override
            {
                switch (tier)
                {
                case ScriptTier::Level:
                    return kScriptLevelStarter;
                case ScriptTier::Game:
                    return kScriptGameStarter;
                case ScriptTier::Behavior:
                default:
                    return kScriptBehaviorStarter;
                }
            }

            [[nodiscard]] bool Cook(StringView source, StringView assetName,
                                    CookScriptErrorSink& sink, ScriptClassSource& out) override
            {
                out.language = String(u8"wren");
                out.sourceName = String(assetName); // the source file identity (breakpoint key)
                out.source = String(source);

                // B3: the harvest VM comes from the registry, by LANGUAGE.
                RefPtr<IScriptManager> manager = CreateScriptManagerForLanguage(u8"wren");
                if (manager.Get() == nullptr)
                {
                    DRACONIC_LOG_ERROR(
                        u8"Script", u8"'{}': no Wren backend registered - cook failed", assetName);
                    return false;
                }
                // The cook VM registers the SAME "main"-module surface the runtime does
                // (foundation math + the behavior facades), so the framing and explicit imports
                // compile identically here. All idempotent.
                RegisterFoundationTypes();
                RegisterScriptFacadeReflection();
                RegisterReflectedTypes(*manager);
                RefPtr<IScriptContext> context = manager->CreateContext();
                if (context.Get() == nullptr)
                {
                    return false;
                }
                context->SetErrorHandler(&sink);

                // Compile check: frame the source with the backend's OWN behavior-module
                // framing (facade prelude + coroutine base) so `is Behavior` resolves at
                // harvest exactly as at runtime. Error lines shift by the injected line
                // count, which the reporter subtracts back.
                const String framedEmpty = manager->AssembleBehaviorModuleSource({});
                const i32 preludeLines =
                    static_cast<i32>(detail::CountNewlines(framedEmpty.AsView()));
                const StringView single[] = {source};
                const String compileSource =
                    manager->AssembleBehaviorModuleSource(Span<const StringView>{single, 1});
                if (!context->Load(compileSource.AsView(), assetName).IsOk())
                {
                    ReportScriptCookErrors(assetName, sink, preludeLines);
                    return false;
                }

                out.className =
                    FindScriptClassName(source, draconic::editor::FileStemOf(assetName));
                out.handlers = ScanScriptHandlers(source);
                // Wren coroutine opt-in: the shared startCoroutine( surface OR extending
                // the Wren `Behavior` base (`is Behavior`).
                out.usesCoroutines =
                    ScriptReferencesCoroutineStart(source) ||
                    detail::Contains(StripScriptComments(source).AsView(), u8"is Behavior");

                // Probe only when the convention is DECLARED (a class without a
                // `static properties` getter legitimately has no inspector rows).
                if (!out.className.IsEmpty() && DeclaresStaticProperties(source))
                {
                    const Status harvested = HarvestWrenProperties(
                        *context, assetName, out.className.AsView(), sink, out.properties);
                    if (!harvested.IsOk())
                    {
                        return false;
                    }
                }
                return true;
            }
        };
    }

    void RegisterWrenScriptCook()
    {
        // The cook needs the Wren backend in the registry; register it idempotently so a
        // cook is never resolvable without its VM.
        draconic::script::wren::RegisterWrenScriptBackend();
        ScriptLanguageCookRegistry::Get().Register(
            String(u8"wren"), UniquePtr<IScriptLanguageCook>(
                                  DefaultAllocator().New<WrenScriptCook>(), DefaultAllocator()));
    }
}
