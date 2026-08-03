// Draconic::ScriptEditor - the `draconic.script.editor` module (tooling).
//
// Source-side script authoring + cook (docs/design/scripting.md §5 + §7.5 B3), fully
// BACKEND-NEUTRAL - no language syntax lives here:
//   * ScriptClassAsset (editor::Asset): the copied script file + its LANGUAGE id
//     (defaulted from the imported file's extension - backend neutrality B3). The asset
//     is just source bytes + a language id + cooked metadata; nothing language-specific.
//   * IScriptLanguageCook: the per-language cook SERVICE. Each language library provides
//     one (Wren: draconic.script.wren.editor; AngelScript: draconic.script.angelscript.editor)
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
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"
#include "Draconic.Core/Log/Log.h"

export module draconic.script.editor;

import draconic.core;
import draconic.editor;
import draconic.editor.core;
import draconic.content;
import draconic.script;
import draconic.script.resource;

using namespace draconic::core;

export namespace draconic::script
{
    namespace content = draconic::content;

    // Source asset: the script file + the backend that compiles it.
    class ScriptClassAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(ScriptClassAsset, draconic::editor::Asset)
    public:
        String language; // backend id ("wren"), defaulted from the file extension

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName
            draconic::core::Serialize(ar, "language", language);
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
        // `entity.send("heal", ...)` -> `onHeal(...)` (P2). The runtime dispatch gate is
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
            String message;
        };
        Array<Entry> errors;

        void OnError(const ScriptError& error) override
        {
            Entry entry{error.kind, String(error.module), error.line, String(error.message)};
            errors.PushBack(Move(entry));
        }
    };

    /// Logs a cook's captured compile/runtime errors as cook errors with the asset's
    /// file/line. `preludeLines` is the line count any language framing injected AHEAD of
    /// the user source - subtracted back so reported lines match the authored file. Shared
    /// by every cook (the reporting format is not language-specific).
    inline void ReportScriptCookErrors(StringView fileName, const CookScriptErrorSink& sink,
                                       i32 preludeLines = 0)
    {
        if (sink.errors.IsEmpty())
        {
            DRACONIC_LOG_ERROR(u8"Script", u8"'{}': compile failed - cook failed", fileName);
            return;
        }
        for (const CookScriptErrorSink::Entry& e : sink.errors)
        {
            const i32 line = e.line > preludeLines ? e.line - preludeLines : e.line;
            DRACONIC_LOG_ERROR(u8"Script", u8"{}:{}: {} - cook failed",
                               e.module.IsEmpty() ? fileName : e.module.AsView(), line, e.message);
        }
    }

    // ---- the per-language cook service (backend neutrality §7.5) ----

    /// A language's cook: compile-check + metadata harvest + the New-Asset starter. The
    /// only place a language's specifics live on the cook side; the neutral builder
    /// resolves one by languageId and delegates. Implemented per language in its own
    /// library (draconic.script.<lang>.editor), registered via ScriptLanguageCookRegistry.
    class IScriptLanguageCook
    {
    public:
        virtual ~IScriptLanguageCook() = default;

        /// The New-Asset starter source (the behavior convention pre-filled).
        [[nodiscard]] virtual StringView NewAssetTemplate() const = 0;

        /// Compile-check `source` (named `assetName` for error reporting) and harvest its
        /// metadata into `out` (language, className, handlers, usesCoroutines, and any
        /// property metadata the language supports); report cook errors through `sink`;
        /// return success. On failure the builder does not write, so the LAST good cooked
        /// record survives (live instances keep running the old class).
        [[nodiscard]] virtual bool Cook(StringView source, StringView assetName,
                                        CookScriptErrorSink& sink, ScriptClassSource& out) = 0;
    };

    /// The cook registry (mirrors ScriptBackendRegistry): a language library registers its
    /// cook here, keyed by languageId; the builder resolves through it - never by type.
    class ScriptLanguageCookRegistry
    {
    public:
        [[nodiscard]] static ScriptLanguageCookRegistry& Get()
        {
            static ScriptLanguageCookRegistry instance;
            return instance;
        }

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

    private:
        struct Entry
        {
            String languageId;
            UniquePtr<IScriptLanguageCook> cook;
        };
        Array<Entry> m_cooks;
    };

    // Cooks a ScriptClassAsset -> ScriptClassSource by delegating to the language cook the
    // asset's LANGUAGE resolves to (B3). A THIN shell: it reads the source, resolves the
    // cook, and delegates - no language syntax, no VM handling.
    class ScriptClassAssetBuilder final : public draconic::editor::DefaultAssetBuilder
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
        [[nodiscard]] u32 Version() const override { return 1; }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
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
                DRACONIC_LOG_ERROR(u8"Script", u8"'{}': source file missing - cook failed",
                                   scriptAsset.fileName.View());
                return read;
            }

            const StringView language = scriptAsset.language.IsEmpty()
                                            ? StringView(u8"wren")
                                            : scriptAsset.language.AsView();

            // B3: the cook comes from the registry, by LANGUAGE - never a named cook type.
            // No cook = a configuration error, surfaced as a cook error.
            IScriptLanguageCook* cook = ScriptLanguageCookRegistry::Get().FindByLanguage(language);
            if (cook == nullptr)
            {
                DRACONIC_LOG_ERROR(
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
    // backend claims (B3 - language-clean), copies the file into Sources/, and creates
    // a ScriptClassAsset whose language records the owning backend. No options dialog.
    class ScriptFileImporter final : public draconic::editor::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Script"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return ScriptBackendRegistry::Get().FindByExtension(extension) != nullptr;
        }

        [[nodiscard]] RefPtr<draconic::editor::ImportOptions> CreateOptions() const override
        {
            return {}; // no options dialog - the drop imports immediately
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, draconic::editor::EditorProject& project,
               content::Group& group, const draconic::editor::ImportOptions*, Object*,
               Array<draconic::editor::DeferredImportWrite>*) override
        {
            const String extension = draconic::editor::FileExtensionLower(sourcePath);
            const ScriptBackendDesc* backend =
                ScriptBackendRegistry::Get().FindByExtension(extension.AsView());
            if (backend == nullptr)
            {
                return Err(ErrorCode::NotSupported);
            }

            Result<String> fileName = draconic::editor::CopyIntoSources(project, sourcePath);
            if (!fileName.HasValue())
            {
                return Err(fileName.Error());
            }

            const StringView stem = draconic::editor::FileStemOf(fileName.Value().AsView());
            content::Instance* instance =
                group.CreateInstance(stem, ScriptClassAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }

            ScriptClassAsset asset;
            asset.fileName = draconic::vfs::SourcePath(fileName.Value().AsView());
            asset.language = String(backend->languageId.AsView());
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // ---- ScriptPage editing model (scripting.md §5) ----

    /// The headless half of the in-editor ScriptPage: the edit buffer for one script asset's
    /// source file plus the save + compile-check loop, factored OUT of the UI so it is
    /// unit-testable without a window. Save writes the source file (the recook + hot reload is
    /// driven by the page through EditorContext::RequestCook - the SAME path an external edit
    /// takes). Validate() compile-checks the CURRENT buffer through the language cook the
    /// product build already delegates to: it captures the cook's ScriptError file/line +
    /// message for inline surfacing but NEVER writes a product, so a failing edit leaves the
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
            String message;
        };

        /// Bind to an asset's source file: the project's Sources/ root, the asset's file name,
        /// and its language id (empty defaults to Wren, matching the builder).
        void Bind(StringView sourcesRoot, StringView fileName, StringView language)
        {
            m_sourcesRoot = String(sourcesRoot);
            m_fileName = String(fileName);
            m_language = language.IsEmpty() ? String(u8"wren") : String(language);
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

        /// Compile-check the CURRENT buffer through the language cook (no product write).
        /// Fills Errors() with the captured ScriptErrors and, on success, the harvested class
        /// name. Returns whether it compiled. An unknown language surfaces one config error.
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
            ScriptClassSource out;
            const bool ok = cook->Cook(m_source.AsView(), m_fileName.AsView(), sink, out);
            for (const CookScriptErrorSink::Entry& entry : sink.errors)
            {
                CompileError e;
                e.kind = entry.kind;
                e.module = entry.module;
                e.line = entry.line;
                e.message = entry.message;
                m_errors.PushBack(Move(e));
            }
            if (ok)
            {
                m_className = out.className;
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
        GlobalTypeRegistry().Register(ScriptClassAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ScriptClassAsset>();
    }

    // ScriptClassAsset::StaticType() is defined WITH reflected properties in ScriptAssetImpl.cpp
    // (reflection track P1).
}
