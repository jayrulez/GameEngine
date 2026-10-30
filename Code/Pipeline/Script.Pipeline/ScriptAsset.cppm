// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script - the `foundation.script.editor` module (tooling).
//
// Source-side script authoring + cook, fully
// BACKEND-NEUTRAL - no language syntax lives here:
//   * ScriptClassAsset (pipeline::Asset): the copied script file + its LANGUAGE id
//     (defaulted from the imported file's extension). The asset
//     is just source bytes + a language id + cooked metadata; nothing language-specific.
//   * IScriptLanguageCook: the per-language cook SERVICE. Each language library provides
//     one (AngelScript: foundation.script.angelscript.editor)
//     and registers it into ScriptLanguageCookRegistry, keyed by languageId (mirroring the
//     ScriptBackendRegistry). A cook compile-checks + harvests metadata; the New-Asset
//     starter template is its NewAssetTemplate().
//   * ScriptClassAssetBuilder: a THIN shell - resolves the cook by the asset's language
//     and delegates NewAssetTemplate / Cook. It never names a language or a backend type.
//   * ScriptFileImporter: drop-import for any extension a registered backend claims; no
//     options dialog.
//   * Shared cook conventions (NOT language syntax): the on<Upper>(...) handler scan
//     (ScanScriptHandlers), the C-family comment stripper (StripScriptComments), the first
//     top-level `class Name` scan (FindScriptClassName), and the shared `startCoroutine(`
//     surface detection (ScriptReferencesCoroutineStart). Every cook reuses these.
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

export module script.pipeline;

import foundation.core;
import pipeline.core;
import pipeline.importer;
import foundation.content;
import foundation.script;
import foundation.script.resource;

using namespace foundation::core;
using namespace foundation::script;

export namespace pipeline{
    namespace content = foundation::content;

    // Source asset: the script file + the backend that compiles it.
    class ScriptClassAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ScriptClassAsset, pipeline::Asset)
    public:
        String language; // backend id ("angelscript"), defaulted from the file extension

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "language", language);
        }
    };

    // ---- shared cook helpers (unit-testable without a project; NOT language syntax) ----

    /// First top-level `class Name` whose name matches `preferredName` (the file stem),
    /// else the FIRST top-level class; empty when the source declares none (a utility
    /// module). Line comments / block comments are ignored. `class Name` is a C-family
    /// convention shared by every language backend, so this stays neutral + shared.
    [[nodiscard]] inline String FindScriptClassName(StringView source, StringView preferredName);

    /// The declared lifecycle/event/message handlers out of the `on<Upper>(...)` naming
    /// convention, by source scan (comments stripped). Both languages use it, so it is a
    /// shared cook helper, not language syntax.
    [[nodiscard]] inline Array<String> ScanScriptHandlers(StringView source);

    /// Strips `//` line comments and `/* */` block comments (string literals respected).
    /// C-family; both languages use it.
    [[nodiscard]] inline String StripScriptComments(StringView source);

    /// True when the (comment-stripped) source references the shared `startCoroutine(`
    /// coroutine surface. A convention shared by every backend's coroutine facade - not
    /// language syntax. A language whose coroutine opt-in has ADDITIONAL markers layers
    /// them on top inside its own cook.
    [[nodiscard]] inline bool ScriptReferencesCoroutineStart(StringView source);

    namespace detail
    {
        [[nodiscard]] inline bool IsIdentChar(utf8char c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') ||
                   (c >= u8'0' && c <= u8'9') || c == u8'_';
        }

        // First occurrence of `needle` in `haystack` (byte scan).
        [[nodiscard]] inline bool Contains(StringView haystack, StringView needle) noexcept
        {
            if (needle.IsEmpty() || needle.Size() > haystack.Size())
            {
                return false;
            }
            for (usize i = 0; i + needle.Size() <= haystack.Size(); ++i)
            {
                if (haystack.SubStr(i, needle.Size()) == needle)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] inline usize CountNewlines(StringView text) noexcept
        {
            usize n = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] == u8'\n')
                {
                    ++n;
                }
            }
            return n;
        }
    }

    inline String StripScriptComments(StringView source)
    {
        String out;
        out.Reserve(source.Size());
        bool inString = false;
        bool inLineComment = false;
        bool inBlockComment = false;
        for (usize i = 0; i < source.Size(); ++i)
        {
            const utf8char c = source[i];
            const utf8char next = (i + 1 < source.Size()) ? source[i + 1] : utf8char(0);
            if (inLineComment)
            {
                if (c == u8'\n')
                {
                    inLineComment = false;
                    out.PushBack(c);
                }
                continue;
            }
            if (inBlockComment)
            {
                if (c == u8'*' && next == u8'/')
                {
                    inBlockComment = false;
                    ++i;
                }
                else if (c == u8'\n')
                {
                    out.PushBack(c);
                } // keep line numbers stable-ish
                continue;
            }
            if (inString)
            {
                if (c == u8'\\')
                {
                    out.PushBack(c);
                    if (next != 0)
                    {
                        out.PushBack(next);
                        ++i;
                    }
                    continue;
                }
                if (c == u8'"')
                {
                    inString = false;
                }
                out.PushBack(c);
                continue;
            }
            if (c == u8'"')
            {
                inString = true;
                out.PushBack(c);
                continue;
            }
            if (c == u8'/' && next == u8'/')
            {
                inLineComment = true;
                ++i;
                continue;
            }
            if (c == u8'/' && next == u8'*')
            {
                inBlockComment = true;
                ++i;
                continue;
            }
            out.PushBack(c);
        }
        return out;
    }

    inline String FindScriptClassName(StringView source, StringView preferredName)
    {
        const String stripped = StripScriptComments(source);
        const StringView text = stripped.AsView();
        String first;
        bool lineStart = true;
        for (usize i = 0; i < text.Size(); ++i)
        {
            const utf8char c = text[i];
            if (c == u8'\n')
            {
                lineStart = true;
                continue;
            }
            if (lineStart && (c == u8' ' || c == u8'\t'))
            {
                continue;
            }
            if (lineStart)
            {
                lineStart = false;
                const StringView keyword = u8"class ";
                if (i + keyword.Size() < text.Size() && text.SubStr(i, keyword.Size()) == keyword)
                {
                    usize begin = i + keyword.Size();
                    while (begin < text.Size() && text[begin] == u8' ')
                    {
                        ++begin;
                    }
                    usize end = begin;
                    while (end < text.Size() && detail::IsIdentChar(text[end]))
                    {
                        ++end;
                    }
                    if (end > begin)
                    {
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
                }
            }
        }
        return first;
    }

    inline Array<String> ScanScriptHandlers(StringView source)
    {
        // Any method declared as `on<Upper>...(` is a dispatchable handler: the fixed
        // lifecycle set (onStart/onUpdate/...), the reserved event handlers
        // (onContactBegin, onTriggerEnter, ...), AND user message handlers reached by
        // `entity.send("heal", ...)` -> `onHeal(...)`. The runtime dispatch gate is
        // ScriptClass::HasHandler, so harvesting the whole convention here is what makes
        // custom messages and events cost nothing per frame. A leading lowercase after
        // `on` (e.g. `onlyOnce`) is NOT a handler; a getter (`onFoo {`, no parens) is not
        // either - handlers take an argument list.
        const String stripped = StripScriptComments(source);
        const StringView text = stripped.AsView();
        Array<String> found;
        for (usize i = 0; i + 2 < text.Size(); ++i)
        {
            if (text[i] != u8'o' || text[i + 1] != u8'n')
            {
                continue;
            }
            if (i > 0 && detail::IsIdentChar(text[i - 1]))
            {
                continue;
            }
            const utf8char third = text[i + 2];
            if (third < u8'A' || third > u8'Z')
            {
                continue;
            } // on + UpperCase only
            usize end = i + 2;
            while (end < text.Size() && detail::IsIdentChar(text[end]))
            {
                ++end;
            }
            usize after = end;
            while (after < text.Size() && (text[after] == u8' ' || text[after] == u8'\t'))
            {
                ++after;
            }
            if (after >= text.Size() || text[after] != u8'(')
            {
                continue;
            } // method, not getter
            const StringView name = text.SubStr(i, end - i);
            bool duplicate = false;
            for (const String& existing : found)
            {
                if (existing.AsView() == name)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                found.PushBack(String(name));
            }
        }
        return found;
    }

    inline bool ScriptReferencesCoroutineStart(StringView source)
    {
        const String stripped = StripScriptComments(source);
        return detail::Contains(stripped.AsView(), u8"startCoroutine(");
    }

    // ---- shared property-harvest record parsing (NOT language syntax) ----
    //
    // Every cook that harvests inspector properties (AngelScript's declared-property probe,
    // Luau's construct-and-walk probe) emits the SAME record string so the parse is shared:
    //   name \x1F typeString \x1F default \x1F description   (records joined by \x1E)
    // where default is "~" (none), "n:<num>", "b:true|false", "s:<text>" or "l:<a,b,c[,d]>".
    // The language-specific part is only the PROBE that produces this string; the parse below
    // is a shared cook convention, like ScanScriptHandlers.

    /// Parses "a,b,c[,d]" into up to 4 floats; returns the count parsed. Harvest payloads are
    /// exponent-free, so this is a minimal sign/digits/dot parse.
    [[nodiscard]] inline u32 ParseFloatList(StringView text, f32 (&out)[4])
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

    /// One harvest record -> a property desc. Record layout: name \x1F typeString \x1F default
    /// \x1F description. Returns false (with outError set) on a malformed record or unknown type.
    [[nodiscard]] inline bool ParseHarvestRecord(StringView record, ScriptPropertyDesc& out,
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
                    value.color =
                        Color{numbers[0], numbers[1], numbers[2], parsed >= 4 ? numbers[3] : 1.0f};
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

    // ---- cook error plumbing (shared by every cook service) ----

    // Captures compile/runtime errors during a cook's compile/harvest (file/line for the
    // cook error report - ScriptError already carries them).
    class CookScriptErrorSink final : public IScriptErrorHandler
    {
    public:
        struct Entry
        {
            ScriptErrorKind kind;
            String module;
            i32 line;
            i32 column;
            String message;
        };
        Array<Entry> errors;

        void OnError(const ScriptError& error) override
        {
            Entry entry{error.kind, String(error.module), error.line, error.column,
                        String(error.message)};
            errors.PushBack(Move(entry));
        }
    };

    /// A cook problem past the compile (a property the runtime could not fill, a replicated list
    /// naming no property), reported with the compile errors so whoever asked for the cook
    /// (script_validate, the asset cook) reads why, and logged. Shared by every cook.
    inline void ReportCookProblem(CookScriptErrorSink& sink, StringView assetName, const String& message)
    {
        LOG_ERROR(u8"Script", u8"'{}': {} - cook failed", assetName, message);
        sink.OnError(ScriptError{ScriptErrorKind::Compile, assetName, -1, -1, message.AsView()});
    }

    /// Logs a cook's captured compile/runtime errors as cook errors with the asset's
    /// file/line. `preludeLines` is the line count any language framing injected AHEAD of
    /// the user source - subtracted back so reported lines match the authored file. Shared
    /// by every cook (the reporting format is not language-specific).
    inline void ReportScriptCookErrors(StringView fileName, const CookScriptErrorSink& sink,
                                       i32 preludeLines = 0)
    {
        if (sink.errors.IsEmpty())
        {
            LOG_ERROR(u8"Script", u8"'{}': compile failed - cook failed", fileName);
            return;
        }
        for (const CookScriptErrorSink::Entry& e : sink.errors)
        {
            const i32 line = e.line > preludeLines ? e.line - preludeLines : e.line;
            LOG_ERROR(u8"Script", u8"{}:{}: {} - cook failed",
                               e.module.IsEmpty() ? fileName : e.module.AsView(), line, e.message);
        }
    }

    // ---- the per-language cook service ----

    /// The scripting tier a New-Asset starter targets. The three contracts a script can
    /// implement: a per-entity Behavior (class named freely, `construct new(entity)`), a
    /// per-scene Level (`construct new(scene)`, one per scene), and the game orchestrator
    /// (mandatory class `Game`, `construct new()`). Each cook seeds a starter per tier.
    enum class ScriptTier
    {
        Behavior,
        Level,
        Game
    };

    /// A language's cook: compile-check + metadata harvest + the New-Asset starter. The
    /// only place a language's specifics live on the cook side; the neutral builder
    /// resolves one by languageId and delegates. Implemented per language in its own
    /// library (foundation.script.<lang>.editor), registered via ScriptLanguageCookRegistry.
    class IScriptLanguageCook
    {
    public:
        virtual ~IScriptLanguageCook() = default;

        /// The New-Asset starter source for `tier` (the tier's convention pre-filled). A
        /// behaviour's class is `className` (a valid identifier: ClassNameFor makes one; empty is
        /// "NewBehavior"); the level and game tiers keep their reserved class names and ignore it.
        [[nodiscard]] virtual String NewAssetTemplate(ScriptTier tier, StringView className) const = 0;

        /// A cook-fingerprint contribution beyond the shared builder Version: a bytecode-emitting
        /// cook returns its COMPILER version here so a vendor bump recooks (bytecode is
        /// version-locked). Default 0 - a source-only cook contributes nothing.
        [[nodiscard]] virtual u32 CookVersion() const { return 0; }

        /// Compile-check `source` (named `assetName` for error reporting) and harvest its
        /// metadata into `out` (language, className, handlers, usesCoroutines, and any
        /// property metadata the language supports); report cook errors through `sink`;
        /// return success. On failure the builder does not write, so the LAST good cooked
        /// record survives (live instances keep running the old class).
        [[nodiscard]] virtual bool Cook(StringView source, StringView assetName,
                                        CookScriptErrorSink& sink, ScriptClassSource& out) = 0;

        /// Compile-check `source` as Cook would, WITHOUT running any of it and without the
        /// harvest: errors to `sink`, the class name (empty when none is found) to `className`.
        /// For checking a script as it is typed. Returns whether it compiled.
        [[nodiscard]] virtual bool Check(StringView source, StringView assetName,
                                         CookScriptErrorSink& sink, String& className) = 0;
    };

    /// The cook registry (mirrors ScriptBackendRegistry): a language library registers its
    /// cook here, keyed by languageId; the builder resolves through it - never by type.
    class ScriptLanguageCookRegistry
    {
    public:
        // NON-inline (ScriptAssetImpl.cpp): language libraries register their cooks
        // and the builder resolves from another - per-library copies would mean "no
        // cook for language" (shared-libraries.md rendezvous rule).
        [[nodiscard]] static ScriptLanguageCookRegistry& Get();

        /// Idempotent by languageId (a re-register replaces - hot-reload friendly).
        void Register(String languageId, UniquePtr<IScriptLanguageCook> cook)
        {
            for (Entry& existing : m_cooks)
            {
                if (existing.languageId == languageId)
                {
                    existing.cook = Move(cook);
                    return;
                }
            }
            Entry entry;
            entry.languageId = Move(languageId);
            entry.cook = Move(cook);
            m_cooks.PushBack(Move(entry));
        }

        [[nodiscard]] IScriptLanguageCook* FindByLanguage(StringView languageId)
        {
            for (Entry& entry : m_cooks)
            {
                if (entry.languageId == languageId)
                {
                    return entry.cook.Get();
                }
            }
            return nullptr;
        }

        /// Sum of every registered cook's CookVersion() - folded into the ScriptClassAssetBuilder
        /// fingerprint so a compiler/vendor bump in ANY bytecode language recooks the script
        /// packs. Order-independent (a sum), and 0 when every cook is source-only.
        [[nodiscard]] u32 CombinedCookVersion() const
        {
            u32 total = 0;
            for (const Entry& entry : m_cooks)
            {
                if (entry.cook.Get() != nullptr)
                {
                    total += entry.cook->CookVersion();
                }
            }
            return total;
        }

    private:
        struct Entry
        {
            String languageId;
            UniquePtr<IScriptLanguageCook> cook;
        };
        Array<Entry> m_cooks;
    };

    // Cooks a ScriptClassAsset -> ScriptClassSource by delegating to the language cook the
    // asset's LANGUAGE resolves to. A THIN shell: it reads the source, resolves the
    // cook, and delegates - no language syntax, no VM handling.
    class ScriptClassAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ScriptClassAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ScriptClassSource::StaticType();
        }
        [[nodiscard]] u32 Version() const override
        {
            // Base cook-logic version (2: the cooked record gained a bytecode field; 3: each
            // property a replicated flag; 4: the record carries data version 1, so the reader
            // refuses one from before the flag) + every language cook's compiler version, so a
            // Luau (or other bytecode) vendor bump recooks every script pack automatically.
            return 4 + ScriptLanguageCookRegistry::Get().CombinedCookVersion();
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const ScriptClassAsset& scriptAsset = static_cast<const ScriptClassAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            String source;
            const Status read = ReadSourceText(ctx, scriptAsset.fileName.View(), source);
            if (!read.IsOk())
            {
                LOG_ERROR(u8"Script", u8"'{}': source file missing - cook failed",
                                   scriptAsset.fileName.View());
                return read;
            }

            const StringView language = scriptAsset.language.IsEmpty()
                                            ? StringView(u8"angelscript")
                                            : scriptAsset.language.AsView();

            // The cook comes from the registry, by LANGUAGE - never a named cook type.
            // No cook = a configuration error, surfaced as a cook error.
            IScriptLanguageCook* cook = ScriptLanguageCookRegistry::Get().FindByLanguage(language);
            if (cook == nullptr)
            {
                LOG_ERROR(
                    u8"Script", u8"'{}': no script cook registered for language '{}' - cook failed",
                    scriptAsset.fileName.View(), language);
                return Status{ErrorCode::NotSupported};
            }

            ScriptClassSource cooked;
            CookScriptErrorSink sink;
            if (!cook->Cook(source.AsView(), scriptAsset.fileName.View(), sink, cooked))
            {
                return Status{ErrorCode::InvalidArgument};
            }
            return ctx.output->WriteObject(cooked);
        }
    };

    // OS-file importer (editor drag-drop): accepts any extension a REGISTERED script
    // backend claims (language-clean), copies the file into Sources/, and creates
    // a ScriptClassAsset whose language records the owning backend. No options dialog.
    class ScriptFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Script"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return ScriptBackendRegistry::Get().FindByExtension(extension) != nullptr;
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"ScriptClassAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath,
                                                        u8"ScriptClassAsset");
        }

        [[nodiscard]] RefPtr<pipeline::ImportOptions> CreateOptions(IAllocator&) const override
        {
            return {}; // no options dialog - the drop imports immediately
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               content::Group& group, const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            const String extension = pipeline::FileExtensionLower(sourcePath);
            const ScriptBackendDesc* backend =
                ScriptBackendRegistry::Get().FindByExtension(extension.AsView());
            if (backend == nullptr)
            {
                return Err(ErrorCode::NotSupported);
            }

            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, ScriptClassAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();

            ScriptClassAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            asset.language = String(backend->languageId.AsView());
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // ---- ScriptPage editing model ----

    /// The headless half of the in-editor ScriptPage: the edit buffer for one script asset's
    /// source file plus the save + compile-check loop, factored OUT of the UI so it is
    /// unit-testable without a window. Save writes the source file (the recook + hot reload is
    /// driven by the page through EditorContext::RequestCook - the SAME path an external edit
    /// takes). Validate() compile-checks the CURRENT buffer through the language cook's Check,
    /// which compiles as the product build does but runs none of the source: it captures the
    /// ScriptError file/line/column + message for inline surfacing but NEVER writes a product, so a failing edit leaves the
    /// last-good cooked ScriptClass untouched (live instances keep running the old class).
    class ScriptSourceDocument
    {
    public:
        struct CompileError
        {
            ScriptErrorKind kind = ScriptErrorKind::Compile;
            String module; // reporting module/file (empty = the asset's own file)
            i32 line = 0;  // NOTE: as the cook's error handler captured it (may include the
                           // backend's framing prelude offset; the message is authoritative)
            i32 column = -1; // 1-based, or -1 when the backend does not say
            String message;
        };

        /// Bind to an asset's source file: the project's Sources/ root, the asset's file name,
        /// and its language id (empty defaults to AngelScript, matching the builder).
        void Bind(StringView sourcesRoot, StringView fileName, StringView language)
        {
            m_sourcesRoot = String(sourcesRoot);
            m_fileName = String(fileName);
            m_language = language.IsEmpty() ? String(u8"angelscript") : String(language);
        }

        [[nodiscard]] StringView FileName() const noexcept { return m_fileName.AsView(); }
        [[nodiscard]] StringView Language() const noexcept { return m_language.AsView(); }

        /// Read the bound source file into the edit buffer (and mark it as the saved baseline).
        [[nodiscard]] Status Load()
        {
            const String path = PathJoin(m_sourcesRoot.AsView(), m_fileName.AsView());
            Result<Array<byte>> bytes = ReadFile(path.AsView());
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            const Array<byte>& data = bytes.Value();
            m_source =
                String(StringView(reinterpret_cast<const utf8char*>(data.Data()), data.Size()));
            m_saved = m_source;
            return Status{};
        }

        [[nodiscard]] StringView Source() const noexcept { return m_source.AsView(); }
        void SetSource(StringView source) { m_source = String(source); }
        [[nodiscard]] bool IsModified() const { return m_source.AsView() != m_saved.AsView(); }

        /// Persist the edit buffer to the bound source file (clears IsModified on success).
        /// The caller drives the recook + hot reload afterwards (EditorContext::RequestCook).
        [[nodiscard]] Status Save()
        {
            const String path = PathJoin(m_sourcesRoot.AsView(), m_fileName.AsView());
            const Status written = WriteFile(
                path.AsView(),
                Span<const byte>(reinterpret_cast<const byte*>(m_source.Data()), m_source.Size()));
            if (written.IsOk())
            {
                m_saved = m_source;
            }
            return written;
        }

        /// Compile-check the CURRENT buffer through the language cook's Check (nothing runs, no
        /// product write). Fills Errors() with the captured ScriptErrors and, on success, the
        /// class name. Returns whether it compiled. An unknown language surfaces one config error.
        [[nodiscard]] bool Validate()
        {
            m_errors.Clear();
            m_className = String{};
            IScriptLanguageCook* cook =
                ScriptLanguageCookRegistry::Get().FindByLanguage(m_language.AsView());
            if (cook == nullptr)
            {
                CompileError e;
                e.message = String(u8"no script cook registered for language '");
                e.message.Append(m_language.AsView());
                e.message.Append(u8"'");
                m_errors.PushBack(Move(e));
                m_lastCompileOk = false;
                return false;
            }
            CookScriptErrorSink sink;
            String className;
            const bool ok = cook->Check(m_source.AsView(), m_fileName.AsView(), sink, className);
            for (const CookScriptErrorSink::Entry& entry : sink.errors)
            {
                CompileError e;
                e.kind = entry.kind;
                e.module = entry.module;
                e.line = entry.line;
                e.column = entry.column;
                e.message = entry.message;
                m_errors.PushBack(Move(e));
            }
            if (ok)
            {
                m_className = Move(className);
            }
            m_lastCompileOk = ok;
            return ok;
        }

        [[nodiscard]] Span<const CompileError> Errors() const noexcept
        {
            return Span<const CompileError>{m_errors.Data(), m_errors.Size()};
        }
        [[nodiscard]] StringView ClassName() const noexcept { return m_className.AsView(); }
        [[nodiscard]] bool LastCompileOk() const noexcept { return m_lastCompileOk; }

    private:
        String m_sourcesRoot;
        String m_fileName;
        String m_language;
        String m_source;
        String m_saved;
        String m_className;
        Array<CompileError> m_errors;
        bool m_lastCompileOk = true;
    };

    // Registers the asset type for content-DB construction + deserialization.
    inline void RegisterScriptAssets()
    {
        GlobalTypeRegistry().Register(ScriptClassAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<ScriptClassAsset>();
    }

    // ScriptClassAsset::StaticType() is defined WITH reflected properties in ScriptAssetImpl.cpp.

    /// The placeholder a behaviour starter spells its class name with.
    inline constexpr StringView kStarterClassName = u8"{ClassName}";

    /// `starter` with every kStarterClassName replaced by `className` ("NewBehavior" when empty).
    [[nodiscard]] inline String FillStarterClassName(StringView starter, StringView className)
    {
        const StringView name = className.IsEmpty() ? StringView(u8"NewBehavior") : className;
        String out;
        usize i = 0;
        while (i < starter.Size())
        {
            if (i + kStarterClassName.Size() <= starter.Size() &&
                starter.SubStr(i, kStarterClassName.Size()) == kStarterClassName)
            {
                out.Append(name);
                i += kStarterClassName.Size();
                continue;
            }
            out.PushBack(starter[i]);
            ++i;
        }
        return out;
    }

    /// A behaviour's class name from its asset's name: the ASCII letters, digits and underscores,
    /// a leading digit prefixed with an underscore; `fallback` when nothing is left. So a script
    /// asset and the class it holds share a name ("Player Controller" holds PlayerController).
    [[nodiscard]] inline String ClassNameFor(StringView assetName, StringView fallback)
    {
        String out;
        for (const utf8char c : assetName)
        {
            const bool letter = (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z');
            const bool digit = c >= u8'0' && c <= u8'9';
            if (letter || digit || c == u8'_')
            {
                out.PushBack(c);
            }
        }
        if (out.IsEmpty())
        {
            return String(fallback);
        }
        if (out[0] >= u8'0' && out[0] <= u8'9')
        {
            String prefixed(u8"_");
            prefixed.Append(out.AsView());
            return prefixed;
        }
        return out;
    }

    /// The stem a new script of `tier` is named by when the caller names none ("NewBehavior").
    [[nodiscard]] inline StringView ScriptTierBaseName(ScriptTier tier) noexcept
    {
        switch (tier)
        {
        case ScriptTier::Level:
            return u8"NewLevel";
        case ScriptTier::Game:
            return u8"NewGame";
        case ScriptTier::Behavior:
        default:
            return u8"NewBehavior";
        }
    }

    /// A new script asset: the language cook's starter for `tier` written to the sources folder
    /// as "<name>.<extension>" (a behaviour's class named after the asset), and a
    /// ScriptClassAsset linking it with its language. Null when the language has no cook or a
    /// write is refused.
    [[nodiscard]] foundation::content::Instance* CreateScriptInstance(
        const AssetCreationContext& context, StringView languageId, StringView extension,
        ScriptTier tier, StringView baseName);
    /// File > New's script creators: three per registered backend with a cook (Behavior,
    /// Level, Game). Answers how many it registered. Register the cooks first.
    usize RegisterScriptCreators(AssetCreatorRegistry& registry);
}
