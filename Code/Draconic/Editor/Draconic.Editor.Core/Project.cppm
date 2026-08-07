// Draconic::EditorCore - :project partition.
//
// The project model (docs/design/editor.md §3.9, decided 2026-07-11): a project is a
// self-contained directory with a fixed layout + an XML manifest:
//
//   <root>/Project.xml   shared manifest (committed) - ProjectSettings via the XML serializer
//   <root>/Content/      SOURCE content DB (XML factory, .xasset) - authored, committed
//   <root>/Sources/      raw import sources (.fbx/.png/...) referenced by Asset::fileName
//                        (mounted as AssetBuildContext.sources at cook time) - committed
//   <root>/Cooked/       cooked content DB (binary factory, .rasset) - generated, gitignored
//   <root>/Editor/       per-user editor state (dock layout, open pages) - gitignored
//   <root>/.cache/       thumbnails + incremental-cook hash db - gitignored
//
// EditorProject::Open mounts Content/ + Cooked/ and opens both ContentDatabases (Traktor's
// source-db / output-db split). The manifest reserves `nativeModule` for the tagged-for-later
// optional per-project native game module (see design doc §5 deferred).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.core:project;

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.xml.serialization;
import draconic.engine.project;
import :export_roots; // the project owns its "Always Export" set (export_roots.xml)

using namespace draconic::foundation;

export namespace draconic::editor
{
    // The manifest payload + directory layout live in draconic.engine.project (runtime-side,
    // editor-free - the player/dist builds read the same manifest without editor code).
    using draconic::project::kCookedAssetExtension;
    using draconic::project::kEngineVersionString;
    using draconic::project::kProjectCacheDir;
    using draconic::project::kProjectContentDir;
    using draconic::project::kProjectCookedDir;
    using draconic::project::kProjectEditorDir;
    using draconic::project::kProjectManifestFile;
    using draconic::project::kProjectSourcesDir;
    using draconic::project::kSourceAssetExtension;
    using draconic::project::ProjectSettings;

    // An opened project: the manifest + the mounted source and cooked content databases.
    class EditorProject
    {
    public:
        EditorProject(const EditorProject&) = delete;
        EditorProject& operator=(const EditorProject&) = delete;

        // Scaffold a new project at `directory` (created if absent; its PARENT must exist):
        // writes the manifest and creates the fixed subdirectories. Fails with AlreadyExists
        // if a manifest is already present.
        [[nodiscard]] static Status Create(StringView directory, StringView name)
        {
            if (!CreateDirectory(directory))
            {
                return Status{ErrorCode::NotFound};
            }
            vfs::NativeFileSystem root(directory);
            if (root.Exists(kProjectManifestFile))
            {
                return Status{ErrorCode::AlreadyExists};
            }

            const StringView dirs[] = {kProjectContentDir, kProjectSourcesDir, kProjectCookedDir,
                                       kProjectEditorDir, kProjectCacheDir};
            for (StringView dir : dirs)
            {
                if (!CreateDirectory(PathJoin(directory, dir).AsView()))
                {
                    return Status{ErrorCode::Internal};
                }
            }

            ProjectSettings settings;
            settings.name = String(name);
            return WriteManifest(root, settings);
        }

        // Open an existing project: read the manifest, ensure the fixed subdirectories exist,
        // mount Content/ + Cooked/, and scan both content databases.
        [[nodiscard]] static UniquePtr<EditorProject> Open(StringView directory)
        {
            vfs::NativeFileSystem root(directory);
            ProjectSettings settings;
            if (!draconic::project::LoadProjectSettings(root, settings).IsOk())
            {
                // A present-but-unreadable manifest is an ERROR the user must see (the app
                // shell only reflects failure in the status bar); absent = the scaffold path.
                if (root.Exists(kProjectManifestFile))
                {
                    DRACONIC_LOG_ERROR(u8"Project",
                                       u8"'{}/{}' exists but failed to parse (old or corrupt "
                                       u8"format?) - project not opened",
                                       directory, kProjectManifestFile);
                }
                return UniquePtr<EditorProject>{};
            }

            // Generated dirs may be missing on a fresh checkout (Cooked/Editor/.cache are
            // gitignored) - recreate them so mounts and state saves always have a target.
            const StringView dirs[] = {kProjectContentDir, kProjectSourcesDir, kProjectCookedDir,
                                       kProjectEditorDir, kProjectCacheDir};
            for (StringView dir : dirs)
            {
                if (!CreateDirectory(PathJoin(directory, dir).AsView()))
                {
                    return UniquePtr<EditorProject>{};
                }
            }

            if (!settings.engineVersion.IsEmpty() &&
                settings.engineVersion != draconic::project::kEngineVersionString)
            {
                // Today: informational. The launcher/project-manager (planned) routes projects
                // to their engine version and drives migration on upgrade.
                DRACONIC_LOG_WARNING(
                    u8"Project", u8"project was last saved by engine {} (this editor is {})",
                    settings.engineVersion, draconic::project::kEngineVersionString);
            }
            EditorProject* project = DefaultAllocator().New<EditorProject>(directory, settings);
            return UniquePtr<EditorProject>(project, DefaultAllocator());
        }

        [[nodiscard]] StringView Name() const noexcept { return m_settings.name.AsView(); }
        [[nodiscard]] StringView Directory() const noexcept { return m_directory.AsView(); }
        [[nodiscard]] ProjectSettings& Settings() noexcept { return m_settings; }
        [[nodiscard]] const ProjectSettings& Settings() const noexcept { return m_settings; }

        /// The authored source database (XML envelopes) - what the editor edits.
        [[nodiscard]] draconic::content::ContentDatabase& SourceDb() noexcept
        {
            return *m_sourceDb;
        }
        /// The cooked output database (binary envelopes) - what the runtime loads.
        [[nodiscard]] draconic::content::ContentDatabase& CookedDb() noexcept
        {
            return *m_cookedDb;
        }

        /// Root for raw import sources (mounted as AssetBuildContext.sources at cook time).
        [[nodiscard]] String SourcesRoot() const
        {
            return PathJoin(m_directory.AsView(), kProjectSourcesDir);
        }
        /// Per-user editor state directory (dock layout, open pages).
        [[nodiscard]] String EditorStateRoot() const
        {
            return PathJoin(m_directory.AsView(), kProjectEditorDir);
        }
        /// Cache directory (thumbnails, cook hashes).
        [[nodiscard]] String CacheRoot() const
        {
            return PathJoin(m_directory.AsView(), kProjectCacheDir);
        }

        // Persist the manifest (settings changed in the editor).
        [[nodiscard]] Status SaveSettings()
        {
            vfs::NativeFileSystem root(m_directory.AsView());
            return WriteManifest(root, m_settings);
        }

        /// The project's explicit "Always Export" roots (export_roots.xml). Loaded on Open; the
        /// export driver seeds these as Flag/Group roots (docs/design/export-reachability.md §2).
        [[nodiscard]] ExportRootsSet& ExportRoots() noexcept { return m_exportRoots; }
        [[nodiscard]] const ExportRootsSet& ExportRoots() const noexcept { return m_exportRoots; }

        // Persist the export roots to <project>/export_roots.xml (the editor calls this after a
        // right-click "Always Export" toggle mutates the set).
        [[nodiscard]] Status SaveExportRoots()
        {
            vfs::NativeFileSystem root(m_directory.AsView());
            vfs::IWritableFileSystem* writable = root.AsWritable();
            if (writable == nullptr)
            {
                return Status{ErrorCode::NotSupported};
            }
            return draconic::editor::SaveExportRoots(*writable, m_exportRoots);
        }

        // Internal (public for allocator New); use Create/Open. Fields are moved out of
        // `settings` one by one (ISerializable's deleted copy suppresses the implicit move).
        EditorProject(StringView directory, ProjectSettings& settings)
            : m_directory(directory),
              m_contentMount(MakeUnique<vfs::NativeFileSystem>(
                  DefaultAllocator(), PathJoin(directory, kProjectContentDir).AsView())),
              m_cookedMount(MakeUnique<vfs::NativeFileSystem>(
                  DefaultAllocator(), PathJoin(directory, kProjectCookedDir).AsView())),
              m_sourceDb(MakeUnique<draconic::content::ContentDatabase>(
                  DefaultAllocator(), *m_contentMount, draconic::xml::XmlSerializerFactory(),
                  kSourceAssetExtension)),
              m_cookedDb(MakeUnique<draconic::content::ContentDatabase>(
                  DefaultAllocator(), *m_cookedMount, BinarySerializerFactory(),
                  kCookedAssetExtension))
        {
            // Per-field move (ISerializable deletes copy/move) - EVERY ProjectSettings field
            // must appear here; a missed one silently drops manifest data on Open.
            m_settings.name = Move(settings.name);
            m_settings.engineVersion = Move(settings.engineVersion);
            m_settings.defaultSceneId = settings.defaultSceneId;
            m_settings.defaultScene = Move(settings.defaultScene);
            m_settings.startupScript = Move(settings.startupScript);
            m_settings.startupScriptId =
                settings.startupScriptId; // authoritative game-script asset (v6)
            m_settings.nativeModule = Move(settings.nativeModule);
            m_settings.defaultInputMapId =
                settings.defaultInputMapId; // was MISSING: Open dropped it
            m_settings.defaultBusLayoutId =
                settings.defaultBusLayoutId; // was ALSO missing: Open dropped it (v5)
            m_settings.defaultUiThemeId = settings.defaultUiThemeId;
            m_settings.defaultUiFontId = settings.defaultUiFontId; // fonts triad (v7)

            // The "Always Export" set is a separate committed sidecar (export_roots.xml). Absent =
            // no explicit roots (the empty set), which is the common case; only a project that has
            // flagged assets carries the file. A present-but-unreadable file leaves the set empty
            // (never blocks Open) - the export then over-includes (safe), never mis-prunes.
            {
                vfs::NativeFileSystem root(directory);
                (void)LoadExportRoots(root, m_exportRoots);
            }
        }

    private:
        [[nodiscard]] static Status WriteManifest(vfs::NativeFileSystem& root,
                                                  ProjectSettings& settings)
        {
            vfs::IWritableFileSystem* writable = root.AsWritable();
            if (writable == nullptr)
            {
                return Status{ErrorCode::NotSupported};
            }
            // One manifest format: the lean lib's helpers (versioned payload included) - the
            // player reads the same file with zero editor code.
            return draconic::project::SaveProjectSettings(*writable, settings);
        }

        String m_directory;
        ProjectSettings m_settings;
        ExportRootsSet m_exportRoots; // "Always Export" set (export_roots.xml); empty when absent
        UniquePtr<vfs::NativeFileSystem> m_contentMount;
        UniquePtr<vfs::NativeFileSystem> m_cookedMount;
        UniquePtr<draconic::content::ContentDatabase> m_sourceDb;
        UniquePtr<draconic::content::ContentDatabase> m_cookedDb;
    };

}
