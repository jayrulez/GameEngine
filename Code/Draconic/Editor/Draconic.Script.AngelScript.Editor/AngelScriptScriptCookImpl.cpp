// Draconic::ScriptAngelScriptEditor - implementation unit for the AngelScript cook.
//
// Compile-check + the shared handler scan + PROPERTY HARVEST. The behavior class is
// built through the vendored CScriptBuilder add-on, which pre-processes `[metadata]`
// declarations; the cook then walks the behavior class's member fields and, for every
// field WITH metadata, resolves the field's declared type + the metadata default +
// description into a ScriptPropertyDesc (identical metadata shape to the Wren cook).
//
// The AngelScript editor-property convention (typed member field + [metadata]):
//     class Mover {
//         [4.0, "units per second"]  float   speed;    // Float,  default 4.0
//         [null]                     Entity@ target;    // Entity, default null
//         ["asset:AudioClip"]        Guid@   clip;      // Asset(AudioClip)
//     }
// The field's declared type picks the ScriptPropertyType (float->Float, int->Int,
// bool->Bool, string->String, Color->Color, Float3->Vec3, Entity->Entity); a Guid field
// with an `asset:<TypeName>` metadata tag -> Asset. A field with NO metadata is NOT a
// property (explicit declaration only - matching the Wren `static properties` principle).
//
// This is a module IMPLEMENTATION unit, so the AngelScript SDK header AND the
// scriptbuilder add-on header live here only (GCC module hygiene by construction).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>

#include <cstdlib>
#include <string>
#include <vector>

module draconic.script.angelscript.editor;

import draconic.foundation;
import draconic.editor.core; // FileStemOf
import draconic.script;
import draconic.script.resource;
import draconic.script.facades;
import draconic.script.editor;
import draconic.script.angelscript;

using namespace draconic::foundation;

namespace draconic::script
{
    namespace
    {
        // ---- small string helpers (metadata parsing) ------------------------------

        inline StringView ViewOfCStr(const char* text) noexcept
        {
            return (text != nullptr) ? StringView(reinterpret_cast<const utf8char*>(text))
                                     : StringView{};
        }

        inline std::string StdFromView(StringView text)
        {
            return std::string(reinterpret_cast<const char*>(text.Data()), text.Size());
        }

        // Strips one layer of surrounding double quotes and unescapes \" and \\.
        [[nodiscard]] String Unquote(StringView text)
        {
            const StringView trimmed = Trim(text);
            if (trimmed.Size() < 2 || trimmed[0] != u8'"' || trimmed[trimmed.Size() - 1] != u8'"')
            {
                return String(trimmed);
            }
            const StringView body = trimmed.SubStr(1, trimmed.Size() - 2);
            String out;
            out.Reserve(body.Size());
            for (usize i = 0; i < body.Size(); ++i)
            {
                if (body[i] == u8'\\' && i + 1 < body.Size())
                {
                    const utf8char next = body[i + 1];
                    if (next == u8'"' || next == u8'\\')
                    {
                        out.PushBack(next);
                        ++i;
                        continue;
                    }
                }
                out.PushBack(body[i]);
            }
            return out;
        }

        [[nodiscard]] bool IsQuoted(StringView text) noexcept
        {
            const StringView t = Trim(text);
            return t.Size() >= 2 && t[0] == u8'"' && t[t.Size() - 1] == u8'"';
        }

        // Splits metadata content into top-level comma-separated tokens, honouring string
        // literals and (), [], {} nesting (so a `(1, 0, 0)` vector default is one token).
        // Each token is trimmed. `[default, "description"]` -> { "default", "\"description\"" }.
        [[nodiscard]] Array<String> SplitTopLevel(StringView content)
        {
            Array<String> tokens;
            usize begin = 0;
            i32 depth = 0;
            bool inString = false;
            for (usize i = 0; i < content.Size(); ++i)
            {
                const utf8char c = content[i];
                if (inString)
                {
                    if (c == u8'\\')
                    {
                        ++i;
                        continue;
                    }
                    if (c == u8'"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (c == u8'"')
                {
                    inString = true;
                    continue;
                }
                if (c == u8'(' || c == u8'[' || c == u8'{')
                {
                    ++depth;
                    continue;
                }
                if (c == u8')' || c == u8']' || c == u8'}')
                {
                    if (depth > 0)
                    {
                        --depth;
                    }
                    continue;
                }
                if (c == u8',' && depth == 0)
                {
                    tokens.PushBack(String(Trim(content.SubStr(begin, i - begin))));
                    begin = i + 1;
                }
            }
            tokens.PushBack(String(Trim(content.SubStr(begin, content.Size() - begin))));
            return tokens;
        }

        [[nodiscard]] f64 ParseNumber(StringView text)
        {
            const std::string s = StdFromView(Trim(text));
            return std::strtod(s.c_str(), nullptr);
        }

        // Parses up to 4 numbers from a `(a, b, c[, d])` or bare `a, b, c` list.
        [[nodiscard]] u32 ParseNumberList(StringView text, f32 (&out)[4])
        {
            StringView body = Trim(text);
            if (body.Size() >= 2 && (body[0] == u8'(' || body[0] == u8'[' || body[0] == u8'{'))
            {
                body = body.SubStr(1, body.Size() - 2);
            }
            u32 count = 0;
            usize begin = 0;
            for (usize i = 0; i <= body.Size() && count < 4; ++i)
            {
                if (i == body.Size() || body[i] == u8',')
                {
                    const StringView piece = Trim(body.SubStr(begin, i - begin));
                    begin = i + 1;
                    if (!piece.IsEmpty())
                    {
                        out[count++] = static_cast<f32>(ParseNumber(piece));
                    }
                }
            }
            return count;
        }

        // ---- AngelScript field type -> ScriptPropertyType -------------------------

        // The declared field type, normalised: trailing `@`/spaces and a leading `const `
        // stripped (so `Entity@`, `const Guid@` both reduce to `Entity`/`Guid`).
        [[nodiscard]] StringView NormalizeTypeName(StringView decl) noexcept
        {
            StringView t = Trim(decl);
            const StringView constPrefix = u8"const ";
            if (t.Size() > constPrefix.Size() && t.SubStr(0, constPrefix.Size()) == constPrefix)
            {
                t = Trim(t.SubStr(constPrefix.Size(), t.Size() - constPrefix.Size()));
            }
            usize end = t.Size();
            while (end > 0 && (t[end - 1] == u8'@' || IsWhiteSpace(t[end - 1])))
            {
                --end;
            }
            return t.SubStr(0, end);
        }

        [[nodiscard]] bool IsIntTypeName(StringView t) noexcept
        {
            return t == u8"int" || t == u8"int8" || t == u8"int16" || t == u8"int32" ||
                   t == u8"int64" || t == u8"uint" || t == u8"uint8" || t == u8"uint16" ||
                   t == u8"uint32" || t == u8"uint64";
        }

        // Resolves the ScriptPropertyType for a field of declared type `typeName` whose
        // metadata's first token is `firstToken`. Returns false when the field type is not
        // a supported editor-property type (a metadata'd field of an unsupported type is a
        // cook error, surfaced by the caller). `outAssetType` is filled for Asset only.
        [[nodiscard]] bool ResolvePropertyType(StringView typeName, StringView firstToken,
                                               ScriptPropertyType& outKind, String& outAssetType)
        {
            outAssetType = String{};
            if (typeName == u8"float" || typeName == u8"double")
            {
                outKind = ScriptPropertyType::Float;
                return true;
            }
            if (IsIntTypeName(typeName))
            {
                outKind = ScriptPropertyType::Int;
                return true;
            }
            if (typeName == u8"bool")
            {
                outKind = ScriptPropertyType::Bool;
                return true;
            }
            if (typeName == u8"string")
            {
                outKind = ScriptPropertyType::String;
                return true;
            }
            if (typeName == u8"Color")
            {
                outKind = ScriptPropertyType::Color;
                return true;
            }
            if (typeName == u8"Float3")
            {
                outKind = ScriptPropertyType::Vec3;
                return true;
            }
            if (typeName == u8"Entity")
            {
                outKind = ScriptPropertyType::Entity;
                return true;
            }
            if (typeName == u8"Guid")
            {
                // A Guid property is a typed asset reference declared by an `asset:<TypeName>`
                // metadata tag (the first token, a quoted string).
                if (IsQuoted(firstToken))
                {
                    const String tag = Unquote(firstToken);
                    const StringView prefix = u8"asset:";
                    if (tag.Size() > prefix.Size() &&
                        tag.AsView().SubStr(0, prefix.Size()) == prefix)
                    {
                        outKind = ScriptPropertyType::Asset;
                        outAssetType =
                            String(tag.AsView().SubStr(prefix.Size(), tag.Size() - prefix.Size()));
                        return true;
                    }
                }
                return false;
            }
            return false;
        }

        // Fills the default value for a resolved property from the metadata's first token.
        // Only what the language can express literally: scalars, bool, string, Color/Vec3
        // numeric lists, and `null` for entity/asset refs (which leaves the nil default).
        void ParseDefault(ScriptPropertyType type, StringView firstToken, ScriptPropertyValue& out)
        {
            out.kind = type;
            const StringView token = Trim(firstToken);
            if (token.IsEmpty() || token == u8"null")
            {
                return;
            }
            switch (type)
            {
            case ScriptPropertyType::Float:
                out.number = ParseNumber(token);
                break;
            case ScriptPropertyType::Int:
                out.number = static_cast<f64>(static_cast<i64>(ParseNumber(token)));
                break;
            case ScriptPropertyType::Bool:
                out.boolean = (token == u8"true");
                break;
            case ScriptPropertyType::String:
                out.text = Unquote(token);
                break;
            case ScriptPropertyType::Color:
            {
                f32 n[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                if (ParseNumberList(token, n) >= 3)
                {
                    out.color = Color{n[0], n[1], n[2], n[3]};
                }
                break;
            }
            case ScriptPropertyType::Vec3:
            {
                f32 n[4] = {};
                if (ParseNumberList(token, n) >= 3)
                {
                    out.vector = Float3{n[0], n[1], n[2]};
                }
                break;
            }
            case ScriptPropertyType::Entity:
            case ScriptPropertyType::Asset:
            case ScriptPropertyType::None:
            default:
                break; // only null defaults are expressible; overrides carry guids
            }
        }

        // ---- CScriptBuilder message routing into the cook error sink ---------------

        struct CookMessageForward
        {
            CookScriptErrorSink* sink;
        };

        void ForwardBuildMessage(const asSMessageInfo* message, void* param)
        {
            if (message == nullptr || message->type != asMSGTYPE_ERROR)
            {
                return;
            }
            CookMessageForward* forward = static_cast<CookMessageForward*>(param);
            if (forward == nullptr || forward->sink == nullptr)
            {
                return;
            }
            const ScriptError error{ScriptErrorKind::Compile, ViewOfCStr(message->section),
                                    static_cast<i32>(message->row), ViewOfCStr(message->message)};
            forward->sink->OnError(error);
        }

        // Walks the behavior class's member fields and appends one ScriptPropertyDesc per
        // field WITH metadata. Returns false (a cook error) when a metadata'd field has an
        // unsupported type. Fields without metadata are simply not properties.
        [[nodiscard]] bool HarvestProperties(CScriptBuilder& builder, asIScriptEngine* engine,
                                             StringView className, StringView assetName,
                                             Array<ScriptPropertyDesc>& out)
        {
            asIScriptModule* module = builder.GetModule();
            if (module == nullptr)
            {
                return true;
            }
            const String classNameStr(className);
            asITypeInfo* type =
                module->GetTypeInfoByDecl(reinterpret_cast<const char*>(classNameStr.CStr()));
            if (type == nullptr)
            {
                return true;
            } // utility module or no such class

            const int classTypeId = type->GetTypeId();
            const asUINT count = type->GetPropertyCount();
            for (asUINT i = 0; i < count; ++i)
            {
                const char* fieldName = nullptr;
                int fieldTypeId = 0;
                if (type->GetProperty(i, &fieldName, &fieldTypeId) < 0 || fieldName == nullptr)
                {
                    continue;
                }
                std::vector<std::string> metadata =
                    builder.GetMetadataForTypeProperty(classTypeId, static_cast<int>(i));
                if (metadata.empty() || metadata[0].empty())
                {
                    continue;
                } // not a property

                const Array<String> tokens = SplitTopLevel(ViewOfCStr(metadata[0].c_str()));
                const StringView firstToken = tokens.IsEmpty() ? StringView{} : tokens[0].AsView();
                const StringView typeName =
                    NormalizeTypeName(ViewOfCStr(engine->GetTypeDeclaration(fieldTypeId, false)));

                ScriptPropertyDesc desc;
                desc.name = String(ViewOfCStr(fieldName));
                desc.hash = ScriptPropertyNameHash(desc.name.AsView());
                if (!ResolvePropertyType(typeName, firstToken, desc.type, desc.assetType))
                {
                    DRACONIC_LOG_ERROR(u8"Script",
                                       u8"'{}': property '{}' has unsupported type '{}' "
                                       u8"(valid: float, int, bool, string, Color, Float3, Entity, "
                                       u8"or Guid tagged \"asset:<TypeName>\") - cook failed",
                                       assetName, desc.name, typeName);
                    return false;
                }
                // A reflected/resource property (Color, Float3, Entity, or an asset:Guid) is a
                // REFERENCE type in AngelScript (every reflected type is asOBJ_REF). Declared as a
                // VALUE member it cannot receive its value at runtime - AngelScript owns the member's
                // lifecycle and lazily reconstructs it, discarding the applied value (see the
                // AngelScript backend's WriteTypedAddress). So require a handle here, at cook, with the
                // exact fix - the runtime warning is defense-in-depth for non-cooked paths.
                // (game-ready-scripting.md Section 16: Fable's option-1+3 ruling.)
                const bool reflectedRef = desc.type == ScriptPropertyType::Color ||
                                          desc.type == ScriptPropertyType::Vec3 ||
                                          desc.type == ScriptPropertyType::Entity ||
                                          desc.type == ScriptPropertyType::Asset;
                if (reflectedRef && (fieldTypeId & asTYPEID_OBJHANDLE) == 0)
                {
                    DRACONIC_LOG_ERROR(
                        u8"Script",
                        u8"'{}': property '{}' must be a handle - declare it '{}@ {}' (a reflected or "
                        u8"resource property is a reference type; a value member silently drops its "
                        u8"value at runtime) - cook failed",
                        assetName, desc.name, typeName, desc.name);
                    return false;
                }
                ParseDefault(desc.type, firstToken, desc.defaultValue);
                // Description = the second top-level token, when a quoted string.
                if (tokens.Size() > 1 && IsQuoted(tokens[1].AsView()))
                {
                    desc.description = Unquote(tokens[1].AsView());
                }
                out.PushBack(Move(desc));
            }
            return true;
        }

        class AngelScriptScriptCook final : public IScriptLanguageCook
        {
        public:
            [[nodiscard]] StringView NewAssetTemplate(ScriptTier tier) const override
            {
                switch (tier)
                {
                case ScriptTier::Level:
                    return kAngelScriptLevelStarter;
                case ScriptTier::Game:
                    return kAngelScriptGameStarter;
                case ScriptTier::Behavior:
                default:
                    return kAngelScriptBehaviorStarter;
                }
            }

            [[nodiscard]] bool Cook(StringView source, StringView assetName,
                                    CookScriptErrorSink& sink, ScriptClassSource& out) override
            {
                out.language = String(u8"angelscript");
                out.sourceName = String(assetName); // the source file identity (breakpoint key)
                out.source = String(source);

                RefPtr<IScriptManager> manager = CreateScriptManagerForLanguage(u8"angelscript");
                if (manager.Get() == nullptr)
                {
                    DRACONIC_LOG_ERROR(u8"Script",
                                       u8"'{}': no AngelScript backend registered - cook failed",
                                       assetName);
                    return false;
                }
                // Same "main"-module surface the runtime registers (core + facades), so a
                // facade-using behavior compiles at cook exactly as at runtime; FinalizeTypes
                // emits every reflected type into the engine CScriptBuilder builds against.
                RegisterFoundationTypes();
                RegisterScriptFacadeReflection();
                RegisterReflectedTypes(*manager);

                asIScriptEngine* engine = static_cast<asIScriptEngine*>(
                    draconic::script::angelscript::AngelScriptEngineHandle(*manager));
                if (engine == nullptr)
                {
                    return false;
                }

                // Build the behavior through CScriptBuilder: it strips `[metadata]` (which is
                // NOT valid AngelScript syntax to the raw compiler) and records it for the
                // per-field lookup below. Errors route into the cook sink for the report.
                CookMessageForward forward{&sink};
                engine->SetMessageCallback(asFUNCTION(ForwardBuildMessage), &forward, asCALL_CDECL);

                CScriptBuilder builder;
                if (builder.StartNewModule(engine, "DraconicAsCookHarvest") < 0)
                {
                    engine->ClearMessageCallback();
                    return false;
                }
                const String section(assetName);
                (void)builder.AddSectionFromMemory(reinterpret_cast<const char*>(section.CStr()),
                                                   reinterpret_cast<const char*>(source.Data()),
                                                   static_cast<unsigned>(source.Size()));
                // The same coroutine support section the runtime adds per module, so a
                // coroutine-using behavior compiles here exactly as it runs.
                const StringView coroutinePrelude =
                    draconic::script::angelscript::AngelScriptCoroutineModulePrelude();
                (void)builder.AddSectionFromMemory(
                    "__coroutine_support", reinterpret_cast<const char*>(coroutinePrelude.Data()),
                    static_cast<unsigned>(coroutinePrelude.Size()));

                const int built = builder.BuildModule();
                if (built < 0)
                {
                    ReportScriptCookErrors(assetName, sink);
                    engine->ClearMessageCallback();
                    return false;
                }

                out.className =
                    FindScriptClassName(source, draconic::editor::FileStemOf(assetName));
                out.handlers = ScanScriptHandlers(source);
                out.usesCoroutines = ScriptReferencesCoroutineStart(source);

                bool ok = true;
                if (!out.className.IsEmpty())
                {
                    ok = HarvestProperties(builder, engine, out.className.AsView(), assetName,
                                           out.properties);
                }
                engine->ClearMessageCallback();
                return ok;
            }
        };
    }

    void RegisterAngelScriptScriptCook()
    {
        draconic::script::angelscript::RegisterAngelScriptBackend();
        ScriptLanguageCookRegistry::Get().Register(
            String(u8"angelscript"),
            UniquePtr<IScriptLanguageCook>(DefaultAllocator().New<AngelScriptScriptCook>(),
                                           DefaultAllocator()));
    }
}
