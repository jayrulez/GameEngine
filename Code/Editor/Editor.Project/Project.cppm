// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :project partition.
//
// The project model: a project is a
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
// source-db / output-db split). The manifest reserves `nativeModule` for an optional
// per-project native game module.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module editor.project:project;

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization;
import engine.project;
import pipeline.core; // AssetCreatorRegistry (the starter content's primitives)
import :export_roots; // the project owns its "Always Export" set (export_roots.xml)

using namespace foundation::core;
using namespace engine::project;
namespace vfs = foundation::vfs;

export namespace editor
{
    // The manifest payload + directory layout live in engine.project (runtime-side,
    // editor-free - the player/dist builds read the same manifest without editor code).
    using engine::project::kCookedAssetExtension;
    using engine::project::kEngineVersionString;
    using engine::project::kProjectCacheDir;
    using engine::project::kProjectContentDir;
    using engine::project::kProjectCookedDir;
    using engine::project::kProjectEditorDir;
    using engine::project::kProjectManifestFile;
    using engine::project::kProjectSourcesDir;
    using engine::project::kSourceAssetExtension;
    using engine::project::ProjectSettings;

    // An opened project: the manifest + the mounted source and cooked content databases.
    class EditorProject
    {
    public:
        EditorProject(const EditorProject&) = delete;
        EditorProject& operator=(const EditorProject&) = delete;

        // Scaffold a new project at `directory` (created if absent; its PARENT must exist):
        // writes the manifest and creates the fixed subdirectories. Fails with AlreadyExists
        // if a manifest is already present.
        [[nodiscard]] static Status Create(IAllocator& allocator, StringView directory,
                                           StringView name)
        {
            if (!CreateDirectory(directory))
            {
                return Status{ErrorCode::NotFound};
            }
            vfs::NativeFileSystem root(directory, allocator);
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
        [[nodiscard]] static UniquePtr<EditorProject> Open(IAllocator& allocator,
                                                           StringView directory)
        {
            vfs::NativeFileSystem root(directory, allocator);
            ProjectSettings settings;
            if (!engine::project::LoadProjectSettings(root, settings).IsOk())
            {
                // A present-but-unreadable manifest is an ERROR the user must see (the app
                // shell only reflects failure in the status bar); absent = the scaffold path.
                if (root.Exists(kProjectManifestFile))
                {
                    LOG_ERROR(u8"Project",
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
                settings.engineVersion != engine::project::kEngineVersionString)
            {
                // Today: informational. The launcher/project-manager (planned) routes projects
                // to their engine version and drives migration on upgrade.
                LOG_WARNING(
                    u8"Project", u8"project was last saved by engine {} (this editor is {})",
                    settings.engineVersion, engine::project::kEngineVersionString);
            }
            EditorProject* project = allocator.New<EditorProject>(allocator, directory, settings);
            return UniquePtr<EditorProject>(project, allocator);
        }

        [[nodiscard]] StringView Name() const noexcept { return m_settings.name.AsView(); }
        [[nodiscard]] StringView Directory() const noexcept { return m_directory.AsView(); }
        [[nodiscard]] ProjectSettings& Settings() noexcept { return m_settings; }
        [[nodiscard]] const ProjectSettings& Settings() const noexcept { return m_settings; }

        /// The authored source database (XML envelopes) - what the editor edits.
        [[nodiscard]] foundation::content::ContentDatabase& SourceDb() noexcept
        {
            return *m_sourceDb;
        }
        /// The cooked output database (binary envelopes) - what the runtime loads.
        [[nodiscard]] foundation::content::ContentDatabase& CookedDb() noexcept
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
            vfs::NativeFileSystem root(m_directory.AsView(), *m_allocator);
            return WriteManifest(root, m_settings);
        }

        /// The project's explicit "Always Export" roots (export_roots.xml). Loaded on Open; the
        /// export driver seeds these as Flag/Group roots.
        [[nodiscard]] ExportRootsSet& ExportRoots() noexcept { return m_exportRoots; }
        [[nodiscard]] const ExportRootsSet& ExportRoots() const noexcept { return m_exportRoots; }

        // Persist the export roots to <project>/export_roots.xml (the editor calls this after a
        // right-click "Always Export" toggle mutates the set).
        [[nodiscard]] Status SaveExportRoots()
        {
            vfs::NativeFileSystem root(m_directory.AsView(), *m_allocator);
            vfs::IWritableFileSystem* writable = root.AsWritable();
            if (writable == nullptr)
            {
                return Status{ErrorCode::NotSupported};
            }
            return editor::SaveExportRoots(*writable, m_exportRoots);
        }

        // Internal (public for allocator New); use Create/Open. Fields are moved out of
        // `settings` one by one (ISerializable's deleted copy suppresses the implicit move).
        EditorProject(IAllocator& allocator, StringView directory, ProjectSettings& settings)
            : m_allocator(&allocator), m_directory(directory),
              m_contentMount(MakeUnique<vfs::NativeFileSystem>(
                  (*m_allocator), PathJoin(directory, kProjectContentDir).AsView(),
                  (*m_allocator))),
              m_cookedMount(MakeUnique<vfs::NativeFileSystem>(
                  (*m_allocator), PathJoin(directory, kProjectCookedDir).AsView(),
                  (*m_allocator))),
              m_sourceDb(MakeUnique<foundation::content::ContentDatabase>(
                  (*m_allocator), (*m_allocator), *m_contentMount,
                  foundation::xml::XmlSerializerFactory(), kSourceAssetExtension)),
              m_cookedDb(MakeUnique<foundation::content::ContentDatabase>(
                  (*m_allocator), (*m_allocator), *m_cookedMount,
                  BinarySerializerFactory(), kCookedAssetExtension))
        {
            // The manifest's one copy (ISerializable deletes copy/move): through its Serialize
            // body, so no field can be left behind. A hand list here dropped the input map, the
            // bus layout, then the loading document and the MSAA, each in turn.
            (void)engine::project::CopyProjectSettings(settings, m_settings);

            // The "Always Export" set is a separate committed sidecar (export_roots.xml). Absent =
            // no explicit roots (the empty set), which is the common case; only a project that has
            // flagged assets carries the file. A present-but-unreadable file leaves the set empty
            // (never blocks Open) - the export then over-includes (safe), never mis-prunes.
            {
                vfs::NativeFileSystem root(directory, (*m_allocator));
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
            return engine::project::SaveProjectSettings(*writable, settings);
        }
        IAllocator* m_allocator;

        String m_directory;
        ProjectSettings m_settings;
        ExportRootsSet m_exportRoots; // "Always Export" set (export_roots.xml); empty when absent
        UniquePtr<vfs::NativeFileSystem> m_contentMount;
        UniquePtr<vfs::NativeFileSystem> m_cookedMount;
        UniquePtr<foundation::content::ContentDatabase> m_sourceDb;
        UniquePtr<foundation::content::ContentDatabase> m_cookedDb;
    };

    // --- Native-code scaffolding (game-native-code.md N4) --------------------------------

    // A CMake-safe target name from the project name ("My Game 2!" -> "MyGame2";
    // "Native" fallback when nothing survives).
    [[nodiscard]] String NativeTargetNameFromProjectName(StringView projectName);

    // "Add Native Code": generate <project>/Native (CMakeLists + <Target>Plugin.cpp -
    // the NativeSample reference shape, parameterized), point the manifest's
    // nativeModule at the platform's dev-module path (Native/<Target>.dll on Windows,
    // Native/lib<Target>.so elsewhere - it is what gets loaded), and save it.
    // Fails with AlreadyExists
    // when the project already declares a module or Native/ exists - never overwrites
    // user code. Defined in ProjectScaffoldImpl.cpp.
    [[nodiscard]] Status ScaffoldNativeModule(EditorProject& project);

    // The starter content a new project is seeded with, whether made by the editor's New Project
    // or the MCP project_create: Roboto as a distance-field font set as the manifest's default UI
    // font (what a shipped game's text draws with), BlueSky.hdr as the default sky, and the cube,
    // sphere and plane primitives (every primitive with `allPrimitives`) through `creators`.
    // Payload files come from `dataRoot`'s Assets/. The caller saves the settings and the source
    // database. Defined in ProjectSeedImpl.cpp.
    void SeedStarterContent(IAllocator& allocator, EditorProject& project,
                            const pipeline::AssetCreatorRegistry& creators, StringView dataRoot,
                            bool allPrimitives = false);

}
