// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :project partition.
//
// The project model: a project is a self-contained directory with a fixed layout and an XML
// manifest:
//
//   <root>/Project.xml   shared manifest (committed): ProjectSettings through the XML serializer
//   <root>/Content/      source content DB (XML factory, .xasset): authored, committed
//   <root>/Sources/      raw import sources (.fbx, .png, ...) referenced by Asset::fileName
//                        (mounted as AssetBuildContext.sources at cook time): committed
//   <root>/Cooked/       cooked content DB (binary factory, .rasset): generated, gitignored
//   <root>/Editor/       per-user editor state (dock layout, open pages): gitignored
//   <root>/.cache/       thumbnails and the incremental-cook hash DB: gitignored
//
// EditorProject::Open mounts Content/ and Cooked/ and opens both ContentDatabases, keeping source
// and output in separate databases. The project's own native code is a plugin package under
// Plugins/<Name>, one entry of the manifest's plugins list.

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
import foundation.runtime; // PluginPartKind (the game package's parts)
import pipeline.core; // AssetCreatorRegistry (the starter content's primitives)
import :export_roots; // the project owns its "Always Export" set (export_roots.xml)

using namespace foundation::core;
using namespace engine::project;
namespace vfs = foundation::vfs;

export namespace editor
{
    // The manifest payload and directory layout live in engine.project, on the runtime side with
    // no editor code, so the player and dist builds read the same manifest without the editor.
    using engine::project::kCookedAssetExtension;
    using engine::project::kEngineVersionString;
    using engine::project::kProjectCacheDir;
    using engine::project::kProjectContentDir;
    using engine::project::kProjectCookedDir;
    using engine::project::kProjectEditorDir;

    /// The picture the launcher shows a project by: its scene view, captured by the editor (when
    /// the default scene opens without one, when it is saved, on Set as Project Thumbnail) or an
    /// image the user chose, in the project's per-machine editor folder (not under git).
    [[nodiscard]] inline String ProjectThumbnailPath(StringView projectDirectory)
    {
        return PathJoin(PathJoin(projectDirectory, kProjectEditorDir).AsView(), u8"launcher-thumbnail.png");
    }
    using engine::project::kProjectManifestFile;
    using engine::project::kProjectSourcesDir;
    using engine::project::kSourceAssetExtension;
    using engine::project::ProjectSettings;

    // An opened project: the manifest and the mounted source and cooked content databases.
    class EditorProject
    {
    public:
        EditorProject(const EditorProject&) = delete;
        EditorProject& operator=(const EditorProject&) = delete;

        // Scaffolds a new project at `directory` (created if absent; its parent must exist):
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

        // Opens an existing project: reads the manifest, ensures the fixed subdirectories exist,
        // mounts Content/ and Cooked/, and scans both content databases.
        [[nodiscard]] static UniquePtr<EditorProject> Open(IAllocator& allocator,
                                                           StringView directory)
        {
            vfs::NativeFileSystem root(directory, allocator);
            ProjectSettings settings;
            if (!engine::project::LoadProjectSettings(root, settings).IsOk())
            {
                // A manifest that is present but unreadable is an error the user must see (the app
                // shell only reflects failure in the status bar); an absent one is the scaffold path.
                if (root.Exists(kProjectManifestFile))
                {
                    LOG_ERROR(u8"Project",
                                       u8"'{}/{}' exists but failed to parse (old or corrupt "
                                       u8"format?) - project not opened",
                                       directory, kProjectManifestFile);
                }
                return UniquePtr<EditorProject>{};
            }

            // Generated directories may be missing on a fresh checkout (Cooked/, Editor/ and .cache/
            // are gitignored); recreate them so mounts and state saves always have a target.
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
                // Informational only here; the project manager's open gate
                // (ProjectManager::DecideOpen) is where the user is asked before an upgrade.
                LOG_WARNING(
                    u8"Project", u8"project was last saved by engine {} (this editor is {})",
                    settings.engineVersion, engine::project::kEngineVersionString);
            }
            EditorProject* project = allocator.New<EditorProject>(allocator, directory, settings);
            return UniquePtr<EditorProject>(project, allocator);
        }

        [[nodiscard]] StringView Name() const noexcept { return m_settings.name.AsView(); }
        [[nodiscard]] StringView Directory() const noexcept { return m_directory.AsView(); }
        /// The allocator the project was opened with, for what its tools build from it.
        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }
        [[nodiscard]] ProjectSettings& Settings() noexcept { return m_settings; }
        [[nodiscard]] const ProjectSettings& Settings() const noexcept { return m_settings; }

        /// The authored source database (XML envelopes), which the editor edits.
        [[nodiscard]] foundation::content::ContentDatabase& SourceDb() noexcept
        {
            return *m_sourceDb;
        }
        /// The cooked output database (binary envelopes), which the runtime loads.
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

        // Saves the manifest (settings changed in the editor).
        [[nodiscard]] Status SaveSettings()
        {
            vfs::NativeFileSystem root(m_directory.AsView(), *m_allocator);
            return WriteManifest(root, m_settings);
        }

        /// The project's explicit "Always Export" roots (export_roots.xml). Loaded on Open; the
        /// export driver seeds these as Flag and Group roots.
        [[nodiscard]] ExportRootsSet& ExportRoots() noexcept { return m_exportRoots; }
        [[nodiscard]] const ExportRootsSet& ExportRoots() const noexcept { return m_exportRoots; }

        // Saves the export roots to <project>/export_roots.xml (the editor calls this after a
        // right-click "Always Export" toggle changes the set).
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
            // The manifest's one copy (ISerializable deletes copy and move) goes through its
            // Serialize body, so no field can be left behind.
            (void)engine::project::CopyProjectSettings(settings, m_settings);

            // The "Always Export" set is a separate committed sidecar (export_roots.xml). Absent
            // means no explicit roots (the empty set), the common case; only a project that has
            // flagged assets carries the file. A file that is present but unreadable leaves the set
            // empty and never blocks Open; the export then includes too much, which is safe, rather
            // than pruning wrongly.
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
            // One manifest format through engine.project's helpers (versioned payload included),
            // so the player reads the same file with no editor code.
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

    // --- Native-code scaffolding --------------------------------

    // A CMake-safe target name from the project name ("My Game 2!" gives "MyGame2"; "Native" when
    // nothing survives).
    [[nodiscard]] String NativeTargetNameFromProjectName(StringView projectName);

    // The game's package (<project>/Plugins/<Game>, its name from the project's): its id, name,
    // path and the parts its manifest lists, in load order.
    struct GamePackage
    {
        String id;
        String name;
        String path; // relative to the project
        Array<foundation::runtime::PluginPartKind> parts;
    };

    // The game's package when its Plugin.xml is there; none otherwise. Defined in
    // ProjectScaffoldImpl.cpp.
    [[nodiscard]] Optional<GamePackage> FindGamePackage(const EditorProject& project);

    // "Add Native Code": generates the game's plugin package, <project>/Plugins/<Game>, with the
    // `parts` asked for (each <Game>.<Kind>/ with its CMakeLists and <Game><Kind>.cpp, the
    // NativeSample shape with the name filled in), or adds to the package there the parts it lacks;
    // lists them in its Plugin.xml in load order, adds the project's entry for it when there is none
    // and saves the manifest. AlreadyExists when every part asked for is there, or the folder or
    // the entry is not this package's, so it never overwrites user code. Defined in
    // ProjectScaffoldImpl.cpp.
    [[nodiscard]] Status ScaffoldGamePackage(EditorProject& project, Span<const foundation::runtime::PluginPartKind> parts);

    // The folders of the project's own packages: each entry with a path whose folder holds a
    // Plugin.xml, as absolute paths. Defined in ProjectScaffoldImpl.cpp.
    [[nodiscard]] Array<String> ProjectPackageDirs(const EditorProject& project);

    // The starter content a new project is seeded with, whether made by the editor's New Project
    // or the MCP project_create: Roboto as a distance-field font set as the manifest's default UI
    // font (what a shipped game's text draws with), a 1K Poly Haven sky (CC0) as the default sky,
    // and the cube, sphere and plane primitives (every primitive with `allPrimitives`) through
    // `creators`. Payload files come from `dataRoot` (the font under Engine/, the sky under
    // Templates/). The caller saves the settings and the source database. Defined in
    // ProjectSeedImpl.cpp.
    void SeedStarterContent(IAllocator& allocator, EditorProject& project,
                            const pipeline::AssetCreatorRegistry& creators, StringView dataRoot,
                            bool allPrimitives = false);

}
