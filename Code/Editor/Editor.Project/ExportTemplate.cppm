// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_template partition.
//
// Export templates: portable, per-platform prebuilt bundles (a player binary + its runtime sidecars +
// a template.xml manifest) that presets reference by id/platform. They live
// in a machine-local templates root (not committed), are importable/downloadable, and are decoupled
// from any one machine's paths. The HOST implicit template is synthesized from the running tool's own
// directory (Bin/...), so a dev export for the current platform needs zero setup.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
// NO <filesystem> here - see ImportTemplate below. It lives in ExportTemplateImpl.cpp.

// Export-template id prefix; CMake bakes TEMPLATE_ID_PREFIX_VALUE on the policy target. Required -
// no in-source default (keeps the prefix value out of the source). Tests build expected ids from the
// same macro so they always agree.
#ifndef TEMPLATE_ID_PREFIX
#error "TEMPLATE_ID_PREFIX is not defined - set TEMPLATE_ID_PREFIX_VALUE in the root CMakeLists (policy target)"
#endif

export module editor.project:export_template;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;
import engine.project;
import :export_preset; // ExportPreset, ExportPresetSet
import :template_icons; // the icon a created template carries

using namespace foundation::core;

export namespace editor
{
    namespace vfs = foundation::vfs;

    inline constexpr StringView kTemplateManifestFile = u8"template.xml";

    // A prebuilt per-platform export bundle. The serialized fields come from template.xml; `directory`
    // and `isHost` are runtime-resolved (the registry sets them) and NOT serialized. ISerializable so
    // template.xml round-trips as a versioned payload; the registry owns instances via UniquePtr.
    class ExportTemplate final : public ISerializable
    {
        RTTI_OBJECT(ExportTemplate, ISerializable)
    public:
        String id;       // "<prefix>-win64-release-0.1.0" (unique within the templates root)
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
        // The template's icon, an SVG file in its bundle (kTemplateIconFile when made by
        // CreateTemplate), which the editor shows it by. Empty in a manifest from before it: the
        // editor shows its platform's built-in icon.
        String icon;

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
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
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
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
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
    // sidecar list beside it (written by util_copy_runtime_deps).
    inline constexpr StringView kPlayerBaseName = BUILTIN_PLAYER_BASENAME;

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
    // template carries the config/compiler that built the running tool: a Debug
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
        out.engineVersion = String(engine::project::kEngineVersionString);
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
        return PathJoin(GetUserDataDirectory().AsView(), u8"templates");
    }

    // Resolve the templates root, most-specific first: an explicit `overrideRoot` (the editor's
    // EditorExportSettings::templatesRoot; empty when unset) wins, then $ENV_TEMPLATES_DIR, then
    // DefaultTemplatesRoot(). Shared by the CLI (which passes no override) and the editor (which passes
    // its setting), so both resolve identically.
    [[nodiscard]] inline String ResolveTemplatesRoot(StringView overrideRoot = {})
    {
        if (!overrideRoot.IsEmpty())
        {
            return String(overrideRoot);
        }
        if (Optional<String> env = GetEnvironmentVariable(u8"ENV_TEMPLATES_DIR");
            env.HasValue() && !env->IsEmpty())
        {
            return static_cast<String&&>(*env);
        }
        return DefaultTemplatesRoot();
    }

    // Install a template bundle (a dir holding template.xml + the player + sidecars) into
    // `templatesRoot` under its manifest id, so the registry picks it up. Recursive copy (overwrites
    // an existing install of the same id). `outId` receives the imported id. NotFound if the source has
    // no valid template.xml. Shared by the Tools.Export CLI and the editor's Import Template action.
    // Defined out-of-line in ExportTemplateImpl.cpp: the body needs <filesystem> for the
    // recursive copy, and that header must not reach this INTERFACE. On MSVC an STL header in a
    // module interface's global module fragment gets attached to the module, and consumers then
    // fail to re-materialize it from the .ifc - here as
    //   type_traits(2410): error C2678: binary '&': no operator found which takes a left-hand
    //   operand of type '_Bitmask'
    // from std::_Bitmask_includes_all<__std_fs_stats_flags>, which broke Editor.App.
    // (Same shape as <stop_token> via <thread> in Core's JobSystem.) The impl unit already
    // included <filesystem> for exactly this - only the bodies were in the wrong place.
    [[nodiscard]] Status ImportTemplate(StringView srcDir, StringView templatesRoot,
                                        String* outId = nullptr);

    // Remove an installed template bundle: delete `<templatesRoot>/<templateId>` and everything
    // under it (the editor's templates-manager Remove action; the registry drops it next Refresh).
    // The HOST template is synthesized, not on disk, so it is never removable this way - callers
    // must not offer Remove for it. Empty id / a missing dir is a soft error, not a crash.
    // Out-of-line for the same reason as ImportTemplate above.
    [[nodiscard]] Status RemoveTemplate(StringView templatesRoot, StringView templateId);

    // Does a template's engineVersion match this running build's? Empty (an unstamped/hand-written
    // manifest) is treated as a match - the export driver only soft-warns on a real mismatch, so the
    // editor's templates-manager surfaces the same "!" note only when a stamped version differs.
    [[nodiscard]] inline bool TemplateEngineMatches(const ExportTemplate& tmpl)
    {
        return tmpl.engineVersion.IsEmpty() ||
               tmpl.engineVersion.AsView() == engine::project::kEngineVersionString;
    }

    // Where CreateTemplate writes the bundle it builds.
    enum class TemplateOutput
    {
        Install,      // into <destRoot>/<id> under the templates root (usable immediately)
        ExportFolder, // directly into <destRoot> (a self-contained bundle to zip/distribute)
    };

    // What a created template is called, when not the canonical id and name: a second bundle for
    // one platform (the Steam Deck build beside the desktop Linux one) sits beside it rather than
    // replacing it. Empty fields keep the canonical ones.
    struct TemplateIdentity
    {
        StringView id;
        StringView name;
        StringView notes;
        // A built-in icon's name (kBuiltInTemplateIcons: `handheld` for the Steam Deck build) or the
        // path of an .svg to carry; empty gives the platform's built-in icon.
        StringView icon;
    };

    // Synthesize + materialize a template from a "Bin/<Config>/<Platform>-<Compiler>" build dir.
    // Reuses SynthesizeHostTemplate to read the platform + the
    // build-emitted "<player>.runtime-libs", then stamps config + compiler (parsed from the dir path)
    // and engineVersion, and gives it a canonical id "<prefix>-<platform>-<config>-<engineVersion>".
    // Copies the player binary + each sidecar (FileCopyPreserving, keeping +x) and writes template.xml.
    //
    // Two output modes (TemplateOutput): Install writes to <destRoot>/<id> (the templates root, so the
    // registry picks it up next Refresh); ExportFolder writes straight into <destRoot> (zip that folder
    // to distribute, then Import it elsewhere). `outId` / `outDir` receive the id and the bundle dir.
    // NotFound if the player binary is missing from `configDir`.
    [[nodiscard]] inline Status CreateTemplate(StringView configDir, StringView destRoot,
                                               TemplateOutput mode, String* outId = nullptr,
                                               String* outDir = nullptr,
                                               const TemplateIdentity& identity = {})
    {
        vfs::NativeFileSystem configFs(configDir, foundation::core::DefaultAllocator());

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
        tmpl.id = String(TEMPLATE_ID_PREFIX);
        tmpl.id += u8"-";
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
        if (!identity.id.IsEmpty())
        {
            tmpl.id = String(identity.id);
        }
        if (!identity.name.IsEmpty())
        {
            tmpl.name = String(identity.name);
        }
        if (!identity.notes.IsEmpty())
        {
            tmpl.notes = String(identity.notes);
        }

        // The player must exist in the source dir, or there's nothing to package.
        if (!configFs.Exists(tmpl.playerBinary.AsView()))
        {
            return Status{ErrorCode::NotFound};
        }

        const String bundleDir = (mode == TemplateOutput::Install)
                                     ? PathJoin(destRoot, tmpl.id.AsView())
                                     : String(destRoot);
        // Installing replaces a bundle of the same id whole: a file the new build no longer lists
        // (a sidecar dropped since) must not linger and ship.
        if (mode == TemplateOutput::Install && DirectoryExists(bundleDir.AsView()) &&
            !RemoveDirectoryRecursive(bundleDir.AsView()))
        {
            return Status{ErrorCode::Internal};
        }
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
        vfs::NativeFileSystem bundleFs(bundleDir.AsView(), foundation::core::DefaultAllocator());

        // The icon the template carries: an .svg given by path is copied in; otherwise the built-in
        // one named, or the platform's. An unknown name or a missing file refuses the template
        // rather than shipping it without the icon its maker asked for.
        if (identity.icon.EndsWith(u8".svg"))
        {
            if (!FileCopyPreserving(identity.icon,
                                    PathJoin(bundleDir.AsView(), kTemplateIconFile).AsView()))
            {
                return Status{ErrorCode::NotFound};
            }
        }
        else
        {
            const StringView svg = BuiltInTemplateIconSvg(
                identity.icon.IsEmpty() ? DefaultTemplateIconName(tmpl.platform.AsView()) : identity.icon);
            if (svg.IsEmpty())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (Status s = bundleFs.AsWritable()->Save(
                    kTemplateIconFile, Span<const byte>(reinterpret_cast<const byte*>(svg.Data()), svg.Size()));
                !s.IsOk())
            {
                return s;
            }
        }
        tmpl.icon = String(kTemplateIconFile);

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

    // The SVG a template is shown by: the icon file in its bundle, or (the host template, a bundle
    // from before icons, an unreadable file) the built-in icon of its platform.
    [[nodiscard]] inline String TemplateIconSvg(const ExportTemplate& tmpl, IAllocator& allocator)
    {
        if (!tmpl.icon.IsEmpty())
        {
            String svg = ReadTemplateFile(tmpl.directory.AsView(), tmpl.icon.AsView(), allocator);
            if (!svg.IsEmpty())
            {
                return svg;
            }
        }
        return String(BuiltInTemplateIconSvg(DefaultTemplateIconName(tmpl.platform.AsView())), allocator);
    }

    // The installed export templates (imported bundles under a templates root) plus the synthesized
    // host template. Resolves a preset to the template that will produce its dist.
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
                                MakeUnique<ExportTemplate>(foundation::core::DefaultAllocator());
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

            UniquePtr<ExportTemplate> host = MakeUnique<ExportTemplate>(foundation::core::DefaultAllocator());
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

        // The template for `(platform, config)`, preferring a real imported
        // bundle over the synthesized host template. Resolution order:
        //   1. exact (platform, config) - imported bundle, else the host template for that config.
        //   2. platform-only fallback (config mismatch/absent): the nearest config, preferring Release,
        //      again imported over host - so an old preset with no config still resolves.
        // An empty `config` means Release (the product default).
        [[nodiscard]] const ExportTemplate* FindBy(StringView platform, StringView config) const;

        // An explicit templateId wins; otherwise the installed template for the preset's
        // (platform, config) - the preset's config defaults to Release when blank. Null when nothing
        // matches (caller: "import a template").
        [[nodiscard]] const ExportTemplate* Resolve(const ExportPreset& preset) const;

    private:
        Array<UniquePtr<ExportTemplate>> m_templates;
    };

    RTTI_DEFINE_OBJECT_VERSIONED(ExportTemplate, "rtti::editor::editor", 2)
}
