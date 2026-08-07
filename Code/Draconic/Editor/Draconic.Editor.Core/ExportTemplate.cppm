// Draconic::EditorCore - :export_template partition.
//
// Export templates: portable, per-platform prebuilt bundles (a player binary + its runtime sidecars +
// a template.xml manifest) that presets reference by id/platform (docs/design/export.md §2). They live
// in a machine-local templates root (not committed), are importable/downloadable, and are decoupled
// from any one machine's paths. The HOST implicit template is synthesized from the running tool's own
// directory (Bin/...), so a dev export for the current platform needs zero setup.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <filesystem> // recursive dir copy when importing a template bundle

export module draconic.editor.core:export_template;

import draconic.foundation;
import draconic.vfs;
import draconic.xml.serialization;
import draconic.engine.project;
import :export_preset; // ExportPreset, ExportPresetSet

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace vfs = draconic::vfs;

    inline constexpr StringView kTemplateManifestFile = u8"template.xml";

    // A prebuilt per-platform export bundle. The serialized fields come from template.xml; `directory`
    // and `isHost` are runtime-resolved (the registry sets them) and NOT serialized. ISerializable so
    // template.xml round-trips as a versioned payload; the registry owns instances via UniquePtr.
    class ExportTemplate final : public ISerializable
    {
        DRACONIC_OBJECT(ExportTemplate, ISerializable)
    public:
        String id;       // "draconic-win64-release-0.1.0" (unique within the templates root)
        String name;     // "Windows Desktop Release 0.1.0"
        String platform; // "Win64" / "Linux64"
        String config; // "Debug" / "Release" / "RelWithDebInfo" (identity); empty read => "Release"
        String compiler;      // "MSVC" / "Clang" / "GCC" - metadata only, NOT a selector
        String engineVersion; // engine this was built against (soft-matched; warn on mismatch)
        String playerBinary;  // player exe filename within the template dir
        Array<String>
            sidecars; // required runtime files (relative to the template dir), always staged
        Array<String>
            symbols; // optional symbol files (PDB/DWARF); staged only when the preset opts in
        String notes;

        String directory;    // NOT serialized: absolute dir the bundle lives in (host: the Bin dir)
        bool isHost = false; // NOT serialized: synthesized host template vs imported from disk

        // The config for identity/resolution, treating an unstamped (v1) template as Release.
        [[nodiscard]] StringView EffectiveConfig() const noexcept;

        void Serialize(ISerializer& ar) override;
    };

    // Read a template.xml (relative path `fileName`) from `root`. NotFound when absent.
    [[nodiscard]] inline Status LoadTemplateManifest(vfs::IFileSystem& root, ExportTemplate& out,
                                                     StringView fileName = kTemplateManifestFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(*stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ExportTemplate::StaticType());
        out.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        return ctx->serializer->IsOk() ? Status{} : ctx->serializer->GetStatus();
    }

    // Write a template.xml to `root`.
    [[nodiscard]] inline Status SaveTemplateManifest(vfs::IWritableFileSystem& writable,
                                                     ExportTemplate& tmpl,
                                                     StringView fileName = kTemplateManifestFile)
    {
        MemoryStream buffer;
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ExportTemplate::StaticType());
        tmpl.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        if (!ctx->serializer->IsOk())
        {
            return ctx->serializer->GetStatus();
        }
        ctx->Flush(buffer);
        return writable.Save(fileName, buffer.Bytes());
    }

    // Synthesize the host implicit template from the running tool's own directory (Bin/...), so a dev
    // export for the current platform works with no import.
    // The player target's base name (no exe extension); "<base>.runtime-libs" is the build-emitted
    // sidecar list beside it (written by draconic_copy_runtime_deps).
    inline constexpr StringView kPlayerBaseName = DRACONIC_PLAYER_BASENAME;

    // Read a "<name>.runtime-libs" list (one library basename per line) from `fs` into `out`, skipping
    // blank lines and trimming trailing CR/whitespace. Absent/empty file => no entries added.
    inline void ReadRuntimeLibs(vfs::IFileSystem& fs, StringView fileName, Array<String>& out)
    {
        UniquePtr<IStream> stream = fs.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return;
        }
        const usize size = static_cast<usize>(stream->Size());
        if (size == 0)
        {
            return;
        }
        Array<byte> bytes;
        bytes.Resize(size);
        if (stream->Read(bytes.Data(), size) != size)
        {
            return;
        }

        const auto isSpace = [](byte b)
        {
            return b == static_cast<byte>(' ') || b == static_cast<byte>('\t') ||
                   b == static_cast<byte>('\r');
        };
        usize start = 0;
        for (usize i = 0; i <= size; ++i)
        {
            if (i != size && bytes[i] != static_cast<byte>('\n'))
            {
                continue;
            }
            usize s = start, e = i;
            while (s < e && isSpace(bytes[s]))
            {
                ++s;
            }
            while (e > s && isSpace(bytes[e - 1]))
            {
                --e;
            }
            if (e > s)
            {
                out.PushBack(
                    String(StringView(reinterpret_cast<const utf8char*>(bytes.Data() + s), e - s)));
            }
            start = i + 1;
        }
    }

    // Lowercase an ASCII string (used to build stable, case-insensitive template ids). Non-ASCII
    // bytes pass through unchanged (platform/config tags are ASCII).
    [[nodiscard]] inline String AsciiLower(StringView in)
    {
        String out(in);
        for (usize i = 0; i < out.Size(); ++i)
        {
            utf8char* d = const_cast<utf8char*>(out.Data());
            if (d[i] >= utf8char('A') && d[i] <= utf8char('Z'))
            {
                d[i] = static_cast<utf8char>(d[i] - 'A' + 'a');
            }
        }
        return out;
    }

    // Parse a "…/Bin/<Config>/<Platform>-<Compiler>" build dir into its config + compiler tags:
    // the last path component is "<Platform>-<Compiler>" (compiler = the suffix after the final
    // '-'); the component one level up is "<Config>". Fields left untouched when a segment is
    // absent (a non-Bin dir), so the caller's defaults survive.
    inline void DeriveConfigAndCompiler(StringView dir, String& outConfig, String& outCompiler)
    {
        StringView p = dir;
        const auto isSep = [](utf8char c) { return c == utf8char('/') || c == utf8char('\\'); };
        while (!p.IsEmpty() && isSep(p[p.Size() - 1]))
        {
            p = p.SubStr(0, p.Size() - 1);
        }
        if (p.IsEmpty())
        {
            return;
        }

        // Split off the last component (the "<Platform>-<Compiler>" leaf).
        usize leafStart = 0;
        for (usize i = 0; i < p.Size(); ++i)
        {
            if (isSep(p[i]))
            {
                leafStart = i + 1;
            }
        }
        const StringView leaf = p.SubStr(leafStart, p.Size() - leafStart);
        const StringView parent = (leafStart > 0) ? p.SubStr(0, leafStart - 1) : StringView{};

        // compiler = leaf suffix after the last '-' (strip a trailing "-ASAN"-style suffix's owner:
        // we only take the final '-' segment, matching "<Platform>-<Compiler>").
        usize dash = leaf.Size();
        for (usize i = 0; i < leaf.Size(); ++i)
        {
            if (leaf[i] == utf8char('-'))
            {
                dash = i;
            }
        }
        if (dash < leaf.Size())
        {
            outCompiler = String(leaf.SubStr(dash + 1, leaf.Size() - dash - 1));
        }

        // config = the parent dir's last component.
        if (!parent.IsEmpty())
        {
            usize cfgStart = 0;
            for (usize i = 0; i < parent.Size(); ++i)
            {
                if (isSep(parent[i]))
                {
                    cfgStart = i + 1;
                }
            }
            outConfig = String(parent.SubStr(cfgStart, parent.Size() - cfgStart));
        }
    }

    // Synthesize the host implicit template from the running tool's own directory (Bin/...), so a dev
    // export for the current platform works with no import. Sidecars come from the build-emitted
    // "<player>.runtime-libs" in that directory (config-driven; empty on rpath platforms). The host
    // template carries the config/compiler that built the running tool (export-templates.md): a Debug
    // editor synthesizes a Debug host template - so its id is "host-<platform>-<config>".
    inline void SynthesizeHostTemplate(StringView hostToolDir, vfs::IFileSystem* hostToolFs,
                                       ExportTemplate& out)
    {
        out.platform = String(GetHostPlatformName());
        out.config = String(GetBuildConfigName());
        if (out.config.IsEmpty())
        {
            out.config = String(u8"Release");
        }
        out.compiler = String(GetBuildCompilerName());
        out.id = String(u8"host-");
        out.id += out.platform;
        out.id += u8"-";
        out.id += out.config;
        out.name = out.platform;
        out.name += u8" ";
        out.name += out.config;
        out.name += u8" (host build)";
        out.engineVersion = String(draconic::project::kEngineVersionString);
        out.playerBinary =
            GetExecutableName(kPlayerBaseName); // host-based (this template is the host)
        out.directory = String(hostToolDir);
        out.isHost = true;
        if (hostToolFs != nullptr)
        {
            String manifest(kPlayerBaseName);
            manifest += u8".runtime-libs";
            ReadRuntimeLibs(*hostToolFs, manifest.AsView(), out.sidecars);
        }
    }

    // The built-in default templates root: <user-data-dir>/templates.
    [[nodiscard]] inline String DefaultTemplatesRoot()
    {
        return PathJoin(GetUserDataDirectory(u8"draconic").AsView(), u8"templates");
    }

    // Resolve the templates root, most-specific first: an explicit `overrideRoot` (the editor's
    // EditorExportSettings::templatesRoot; empty when unset) wins, then $DRACONIC_TEMPLATES_DIR, then
    // DefaultTemplatesRoot(). Shared by the CLI (which passes no override) and the editor (which passes
    // its setting), so both resolve identically.
    [[nodiscard]] inline String ResolveTemplatesRoot(StringView overrideRoot = {})
    {
        if (!overrideRoot.IsEmpty())
        {
            return String(overrideRoot);
        }
        if (Optional<String> env = GetEnvironmentVariable(u8"DRACONIC_TEMPLATES_DIR");
            env.HasValue() && !env->IsEmpty())
        {
            return static_cast<String&&>(*env);
        }
        return DefaultTemplatesRoot();
    }

    // Install a template bundle (a dir holding template.xml + the player + sidecars) into
    // `templatesRoot` under its manifest id, so the registry picks it up. Recursive copy (overwrites
    // an existing install of the same id). `outId` receives the imported id. NotFound if the source has
    // no valid template.xml. Shared by the Draconic.Tools.Export CLI and the editor's Import Template action.
    [[nodiscard]] inline Status ImportTemplate(StringView srcDir, StringView templatesRoot,
                                               String* outId = nullptr)
    {
        vfs::NativeFileSystem srcFs(srcDir);
        ExportTemplate manifest;
        if (!LoadTemplateManifest(srcFs, manifest).IsOk() || manifest.id.IsEmpty())
        {
            return Status{ErrorCode::NotFound};
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        const String rootCopy(templatesRoot);
        fs::create_directories(reinterpret_cast<const char*>(rootCopy.CStr()), ec);
        const String dst = PathJoin(templatesRoot, manifest.id.AsView());
        const String srcCopy(srcDir);
        fs::copy(fs::path(reinterpret_cast<const char*>(srcCopy.CStr())),
                 fs::path(reinterpret_cast<const char*>(dst.CStr())),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            return Status{ErrorCode::Internal};
        }
        if (outId != nullptr)
        {
            *outId = manifest.id;
        }
        return Status{};
    }

    // Remove an installed template bundle: delete `<templatesRoot>/<templateId>` and everything
    // under it (the editor's templates-manager Remove action; the registry drops it next Refresh).
    // The HOST template is synthesized, not on disk, so it is never removable this way - callers
    // must not offer Remove for it. Empty id / a missing dir is a soft error, not a crash.
    [[nodiscard]] inline Status RemoveTemplate(StringView templatesRoot, StringView templateId)
    {
        if (templateId.IsEmpty())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        const String dir = PathJoin(templatesRoot, templateId);
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto removed =
            fs::remove_all(fs::path(reinterpret_cast<const char*>(dir.CStr())), ec);
        if (ec)
        {
            return Status{ErrorCode::Internal};
        }
        return (removed > 0) ? Status{}
                             : Status{ErrorCode::NotFound}; // nothing deleted => not there
    }

    // Does a template's engineVersion match this running build's? Empty (an unstamped/hand-written
    // manifest) is treated as a match - the export driver only soft-warns on a real mismatch, so the
    // editor's templates-manager surfaces the same "!" note only when a stamped version differs.
    [[nodiscard]] inline bool TemplateEngineMatches(const ExportTemplate& tmpl)
    {
        return tmpl.engineVersion.IsEmpty() ||
               tmpl.engineVersion.AsView() == draconic::project::kEngineVersionString;
    }

    // Where CreateTemplate writes the bundle it builds.
    enum class TemplateOutput
    {
        Install,      // into <destRoot>/<id> under the templates root (usable immediately)
        ExportFolder, // directly into <destRoot> (a self-contained bundle to zip/distribute)
    };

    // Synthesize + materialize a template from a "Bin/<Config>/<Platform>-<Compiler>" build dir
    // (export-templates.md "Create"). Reuses SynthesizeHostTemplate to read the platform + the
    // build-emitted "<player>.runtime-libs", then stamps config + compiler (parsed from the dir path)
    // and engineVersion, and gives it a canonical id "draconic-<platform>-<config>-<engineVersion>".
    // Copies the player binary + each sidecar (FileCopyPreserving, keeping +x) and writes template.xml.
    //
    // Two output modes (TemplateOutput): Install writes to <destRoot>/<id> (the templates root, so the
    // registry picks it up next Refresh); ExportFolder writes straight into <destRoot> (zip that folder
    // to distribute, then Import it elsewhere). `outId` / `outDir` receive the id and the bundle dir.
    // NotFound if the player binary is missing from `configDir`.
    [[nodiscard]] inline Status CreateTemplate(StringView configDir, StringView destRoot,
                                               TemplateOutput mode, String* outId = nullptr,
                                               String* outDir = nullptr)
    {
        vfs::NativeFileSystem configFs(configDir);

        ExportTemplate tmpl;
        SynthesizeHostTemplate(configDir, &configFs,
                               tmpl); // platform + player + sidecars + engineVersion
        tmpl.isHost = false;

        // A WEB build dir (Bin/<Config>/Emscripten-*) holds the browser player page, not a host
        // executable: platform "Web", the player is the .html, and the sidecars manifest lists
        // .js/.wasm/serve.py. The resulting template exports exactly like a desktop one - the
        // export stages page + sidecars + Content.pak + player.xml + the WGSL shaders.dpak, and
        // the served folder runs in a browser (the player FETCHES the three dist files).
        {
            String webPage(kPlayerBaseName);
            webPage += u8".html";
            if (configFs.Exists(webPage.AsView()))
            {
                tmpl.platform = String(u8"Web");
                tmpl.playerBinary = webPage;
                tmpl.compiler = String(u8"Emscripten");
                tmpl.sidecars.Clear();
                String manifest(kPlayerBaseName);
                manifest += u8".runtime-libs";
                ReadRuntimeLibs(configFs, manifest.AsView(), tmpl.sidecars);
            }
        }

        // config/compiler come from WHICH Bin/<Config>/<Platform>-<Compiler> dir is being packaged
        // (not the running tool's), so a Debug editor can still create a Release template.
        String parsedConfig, parsedCompiler;
        DeriveConfigAndCompiler(configDir, parsedConfig, parsedCompiler);
        if (!parsedConfig.IsEmpty())
        {
            tmpl.config = parsedConfig;
        }
        if (tmpl.config.IsEmpty())
        {
            tmpl.config = String(u8"Release");
        }
        if (!parsedCompiler.IsEmpty() && !(tmpl.platform.AsView() == u8"Web"))
        {
            tmpl.compiler = parsedCompiler;
        }

        // Canonical id + name (lowercased platform/config for a stable, case-insensitive id).
        tmpl.id = String(u8"draconic-");
        tmpl.id += AsciiLower(tmpl.platform.AsView());
        tmpl.id += u8"-";
        tmpl.id += AsciiLower(tmpl.config.AsView());
        tmpl.id += u8"-";
        tmpl.id += tmpl.engineVersion;
        tmpl.name = tmpl.platform;
        tmpl.name += u8" ";
        tmpl.name += tmpl.config;
        tmpl.name += u8" ";
        tmpl.name += tmpl.engineVersion;

        // The player must exist in the source dir, or there's nothing to package.
        if (!configFs.Exists(tmpl.playerBinary.AsView()))
        {
            return Status{ErrorCode::NotFound};
        }

        const String bundleDir = (mode == TemplateOutput::Install)
                                     ? PathJoin(destRoot, tmpl.id.AsView())
                                     : String(destRoot);
        if (!CreateDirectories(bundleDir.AsView()))
        {
            return Status{ErrorCode::NotSupported};
        }

        // Copy the player, then each required sidecar (a missing sidecar is fatal - the bundle would be
        // incomplete; unlike export-time staging where a stale list only warns).
        if (!FileCopyPreserving(PathJoin(configDir, tmpl.playerBinary.AsView()).AsView(),
                                PathJoin(bundleDir.AsView(), tmpl.playerBinary.AsView()).AsView()))
        {
            return Status{ErrorCode::Internal};
        }
        for (const String& sidecar : tmpl.sidecars)
        {
            if (!FileCopyPreserving(PathJoin(configDir, sidecar.AsView()).AsView(),
                                    PathJoin(bundleDir.AsView(), sidecar.AsView()).AsView()))
            {
                return Status{ErrorCode::Internal};
            }
        }

        tmpl.directory = bundleDir;
        vfs::NativeFileSystem bundleFs(bundleDir.AsView());
        if (Status s = SaveTemplateManifest(*bundleFs.AsWritable(), tmpl); !s.IsOk())
        {
            return s;
        }

        if (outId != nullptr)
        {
            *outId = tmpl.id;
        }
        if (outDir != nullptr)
        {
            *outDir = bundleDir;
        }
        return Status{};
    }

    // The installed export templates (imported bundles under a templates root) plus the synthesized
    // host template. Resolves a preset to the template that will produce its dist (export.md §6).
    class TemplateRegistry
    {
    public:
        // Rebuild the set. Imported templates come from `templatesRootFs` (each immediate subdir's
        // template.xml); `templatesRootPath` is that root's absolute path (used to record each
        // template's on-disk directory). Either may be empty/null (=> host template only). The host
        // template is synthesized from `hostToolDir`, reading its sidecars from `hostToolFs` (a VFS
        // rooted at that dir; null => host template with no sidecars).
        void Refresh(StringView templatesRootPath, vfs::IFileSystem* templatesRootFs,
                     StringView hostToolDir, vfs::IFileSystem* hostToolFs)
        {
            m_templates.Clear();

            if (templatesRootFs != nullptr)
            {
                if (vfs::IEnumerableFileSystem* en = templatesRootFs->AsEnumerable())
                {
                    Array<vfs::DirEntry> entries;
                    if (en->Enumerate(u8"", entries).IsOk())
                    {
                        for (const vfs::DirEntry& entry : entries)
                        {
                            if (!entry.isDirectory)
                            {
                                continue;
                            }
                            const String manifestPath =
                                PathJoin(entry.name.AsView(), kTemplateManifestFile);
                            UniquePtr<ExportTemplate> tmpl =
                                MakeUnique<ExportTemplate>(DefaultAllocator());
                            if (LoadTemplateManifest(*templatesRootFs, *tmpl, manifestPath.AsView())
                                    .IsOk())
                            {
                                tmpl->directory = PathJoin(templatesRootPath, entry.name.AsView());
                                tmpl->isHost = false;
                                m_templates.PushBack(
                                    static_cast<UniquePtr<ExportTemplate>&&>(tmpl));
                            }
                        }
                    }
                }
            }

            UniquePtr<ExportTemplate> host = MakeUnique<ExportTemplate>(DefaultAllocator());
            SynthesizeHostTemplate(hostToolDir, hostToolFs, *host);
            m_templates.PushBack(static_cast<UniquePtr<ExportTemplate>&&>(host));
        }

        [[nodiscard]] usize Count() const noexcept { return m_templates.Size(); }
        [[nodiscard]] const ExportTemplate* At(usize i) const { return m_templates[i].Get(); }

        [[nodiscard]] const ExportTemplate* FindById(StringView id) const
        {
            for (const UniquePtr<ExportTemplate>& t : m_templates)
            {
                if (t->id.AsView() == id)
                {
                    return t.Get();
                }
            }
            return nullptr;
        }

        // The template for `(platform, config)` (export-templates.md), preferring a real imported
        // bundle over the synthesized host template. Resolution order:
        //   1. exact (platform, config) - imported bundle, else the host template for that config.
        //   2. platform-only fallback (config mismatch/absent): the nearest config, preferring Release,
        //      again imported over host - so an old preset with no config still resolves.
        // An empty `config` means Release (the product default).
        [[nodiscard]] const ExportTemplate* FindBy(StringView platform, StringView config) const;

        // export.md §6: an explicit templateId wins; otherwise the installed template for the preset's
        // (platform, config) - the preset's config defaults to Release when blank. Null when nothing
        // matches (caller: "import a template").
        [[nodiscard]] const ExportTemplate* Resolve(const ExportPreset& preset) const;

    private:
        Array<UniquePtr<ExportTemplate>> m_templates;
    };

    DRACONIC_DEFINE_OBJECT_VERSIONED(ExportTemplate, "draconic::editor", 2)
}
