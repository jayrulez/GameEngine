// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export partition.
//
// The export pipeline as a library (the Tools.Export CLI and the editor's Export menu are thin
// callers; tests drive it headlessly): cook, stage scenes as binary envelopes, pack Content.pak,
// then write the dist manifest. The caller stages the player executable, since where it lives is
// the caller's concern rather than the pipeline's.
//
//   dist layout:  <out>/Content.pak   cooked products and scenes (one binary DB)
//                 <out>/player.xml    dist manifest (ProjectSettings shape)
//
// Scenes have no builder because their cooked form is the authored form, so export re-encodes
// each XML envelope to binary and copies the "scene" stream under the same guids; resource refs
// and the manifest's defaultScene path keep working in the pak.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.project:export_pipeline;

import foundation.core;
import foundation.vfs;
import foundation.vfs.pak;
import foundation.content;
import engine.project;
import foundation.scene.resource;
import foundation.shaders.resource; // ShaderSource: a custom shader's product, stripped for a dist
import pipeline.core;
import pipeline.cook;
import :project;
import :export_preset;
import :export_roots;
import :export_template;

using namespace foundation::core;
using namespace pipeline;

export namespace editor
{
    struct ExportStats
    {
        usize cooked = 0;
        usize cookFailed = 0;
        usize scenesStaged = 0;
        usize filesPacked = 0;
        // The project's custom shaders the dist ships (their cooked ShaderSource products): the
        // shader pack step cooks each into the dist's pack, as bytecode.
        Array<Guid> shaders;
    };

    // === Reachability pruning ===
    //
    // Opt-in per preset (ExportPreset::pruneToReachable): a dist ships only the closure of its
    // entry points instead of the whole cooked directory. The closure reuses the cook's dependency
    // graph (CookDriver::PlanFor) for asset-to-asset edges. A caller-supplied SceneReferenceScanner
    // supplies the scene-graph edges the cook does not model (a scene's component resource Refs and
    // its prefab instances), because scenes and prefabs have no builder and so fall outside
    // PlanFor's read-dependency walk.

    // Why a root is in the dist. Keep the enum stable for the report.
    enum class ExportRootReason
    {
        DefaultScene,  // ProjectSettings::defaultSceneId
        StartupScript, // the startup script's own imported asset (the script file ships regardless)
        Flag,          // an instance flagged "Always Export" (ExportRoots::instances)
        Group, // an instance under a group flagged "Always export contents" (ExportRoots::groups)
        ManifestDefault, // a manifest default reference (input map, bus layout, UI theme, UI font,
                         // the loading document)
    };

    [[nodiscard]] StringView ExportRootReasonName(ExportRootReason r);

    // A seed entry point: the dist is the closure of these.
    struct ExportRoot
    {
        Guid id;
        String name; // the instance's source path (report display); "" if unresolved
        ExportRootReason reason = ExportRootReason::DefaultScene;
    };

    // What a scan of one scene or prefab instance yields: the guids it references directly.
    // Resources feed PlanFor, which then closes the asset-to-asset edges; prefabs are staged and
    // also rescanned for their own references (a scene references a prefab, which references assets).
    struct SceneReferences
    {
        Array<Guid> resources; // component resource Ref ids (mesh, material, texture, ... instances)
        Array<Guid> prefabs;   // prefab-instance ids nested in this scene or prefab
    };

    // Caller hook: collects one scene or prefab instance's direct references (see
    // SceneReferences). The export library stays independent of subsystems, so the driving tool,
    // which owns the full component-manager set (render, physics, animation, ...), supplies this.
    // The usual implementation loads the instance (LoadScene over all managers), resolves its Refs
    // through a ResourceManager with no factories and reads back
    // ResourceManager::CollectUnresolved (every bound id, since nothing was built), plus each
    // parked prefab instance's prefabId. The scene-stream transcode (sceneStreams) is a caller
    // hook for the same reason.
    using SceneReferenceScanner = Function<void(
        foundation::content::Instance&, foundation::content::ContentDatabase&, SceneReferences&)>;

    // The record of a pruned export, kept loud and auditable: which roots were kept and why, and
    // what was dropped. It lives on ExportResult (the CLI prints it, the editor Console shows it)
    // and is written beside the dist as export-report.txt. Empty, with pruned=false, for an export
    // that packs everything.
    struct PruningReport
    {
        bool pruned = false;
        Array<ExportRoot> roots; // the seed entry points and their reasons
        usize keptCount = 0;     // scenes and cooked products included (the closure's size on disk)
        Array<String> dropped;   // instance paths excluded from the dist (unreferenced or unfinished)
    };

    namespace detail
    {
        // The game's native target name from the manifest's dev-module path:
        // "Native/libScratchGame.so" gives "ScratchGame" (.dll and names without the lib prefix
        // too). Empty when the shape is not recognized; callers treat that as no native module.
        [[nodiscard]] inline foundation::core::String
        NativeTargetFromModulePath(foundation::core::StringView nativeModule)
        {
            namespace core = foundation::core;
            core::StringView name = nativeModule;
            for (core::usize i = name.Size(); i > 0; --i)
            {
                if (name.Data()[i - 1] == '/' || name.Data()[i - 1] == '\\')
                {
                    name = core::StringView(name.Data() + i, name.Size() - i);
                    break;
                }
            }
            if (name.EndsWith(u8".so"))
            {
                name = core::StringView(name.Data(), name.Size() - 3);
            }
            else if (name.EndsWith(u8".dll"))
            {
                name = core::StringView(name.Data(), name.Size() - 4);
            }
            if (name.StartsWith(u8"lib"))
            {
                name = core::StringView(name.Data() + 3, name.Size() - 3);
            }
            return core::String(name);
        }

        // The ship build's Bin suffix, "-Ship-<sanitized project name>". It is per project so two
        // games' ship outputs never collide in the checkout's Bin, as the -Shared and -ASAN
        // suffixes keep their builds apart.
        [[nodiscard]] inline foundation::core::String
        ShipOutputSuffix(foundation::core::StringView projectName)
        {
            foundation::core::String s(u8"-Ship-");
            for (foundation::core::usize i = 0; i < projectName.Size(); ++i)
            {
                const char c = static_cast<char>(projectName.Data()[i]);
                const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9');
                s.PushBack(ok ? projectName.Data()[i] : u8'-');
            }
            return s;
        }

        // The staged player filename: the chosen name (preset.playerName, else the template's player
        // binary) with the target platform's executable extension ensured. Windows executables must end
        // in .exe or the OS will not launch them ("MyGame" becomes "MyGame.exe"); other platforms have
        // no suffix. Idempotent: a name that already ends in .exe is left alone.
        [[nodiscard]] inline foundation::core::String PlayerOutputName(
            foundation::core::StringView platform, foundation::core::StringView playerName,
            foundation::core::StringView templateBinary)
        {
            foundation::core::String name = playerName.IsEmpty()
                                                ? foundation::core::String(templateBinary)
                                                : foundation::core::String(playerName);
            if (platform.StartsWith(u8"Win") && !name.AsView().EndsWith(u8".exe"))
            {
                name += u8".exe";
            }
            // The web player is its page: a new name must still end in .html, or the browser
            // downloads it.
            if (templateBinary.EndsWith(u8".html") && !name.AsView().EndsWith(u8".html"))
            {
                name += u8".html";
            }
            return name;
        }

        // Whether a cooked file's owning instance is reachable. The owning instance path is `file`
        // up to a '.' in its name region (envelope "<path>.<ext>" and stream "<path>.<stream>.bin"
        // or ".data" both begin with "<path>."). Each '.' boundary is tested against the set of
        // reachable instance paths; the '.' delimiter makes prefix matching collision-safe (a peer
        // "CubeBig" never matches "Cube.").
        [[nodiscard]] inline bool FileOwnerReachable(StringView file,
                                                     const HashMap<String, u8>& reachablePaths)
        {
            usize nameStart = 0;
            for (usize i = 0; i < file.Size(); ++i)
            {
                if (file[i] == utf8char('/'))
                {
                    nameStart = i + 1;
                }
            }
            for (usize i = nameStart; i < file.Size(); ++i)
            {
                if (file[i] == utf8char('.'))
                {
                    if (reachablePaths.Find(String(file.SubStr(0, i))) != nullptr)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Recursively adds every file under `folder` to the pak (locator = mount-relative path).
        // When `reachablePaths` is non-null, packs only files whose owning instance path is in the
        // set (closure pruning); null packs the whole tree (the default, export everything).
        // `excludedPaths` names instances whose files are left out (a stripped copy is staged
        // instead).
        bool PackTree(foundation::vfs::IFileSystem& mount,
                      foundation::vfs::IEnumerableFileSystem& enumerable, StringView folder,
                      foundation::vfs::PakBuilder& pak, usize& fileCount,
                      const HashMap<String, u8>* reachablePaths = nullptr,
                      const HashMap<String, u8>* excludedPaths = nullptr)
        {
            Array<foundation::vfs::DirEntry> entries;
            if (!enumerable.Enumerate(folder, entries).IsOk())
            {
                return folder.IsEmpty();
            }
            for (const foundation::vfs::DirEntry& entry : entries)
            {
                const String path = PathJoin(folder, entry.name.AsView());
                if (entry.isDirectory)
                {
                    if (!PackTree(mount, enumerable, path.AsView(), pak, fileCount, reachablePaths,
                                  excludedPaths))
                    {
                        return false;
                    }
                    continue;
                }
                if (reachablePaths != nullptr &&
                    !FileOwnerReachable(path.AsView(), *reachablePaths))
                {
                    continue; // pruned: not part of the reachable closure
                }
                if (excludedPaths != nullptr && FileOwnerReachable(path.AsView(), *excludedPaths))
                {
                    continue; // replaced by a staged copy
                }
                UniquePtr<IStream> stream = mount.Open(path.AsView(), FileMode::Read);
                if (!stream)
                {
                    return false;
                }
                Array<byte> bytes;
                bytes.Resize(static_cast<usize>(stream->Size()));
                if (stream->Read(bytes.Data(), bytes.Size()) != bytes.Size())
                {
                    return false;
                }
                pak.Add(path.AsView(), Span<const byte>{bytes.Data(), bytes.Size()});
                ++fileCount;
            }
            return true;
        }

        // The staging DB's group for `path` ("A/B"), created as needed.
        [[nodiscard]] inline foundation::content::Group* StagingGroup(
            foundation::content::ContentDatabase& staging, StringView path)
        {
            foundation::content::Group* group = staging.RootGroup();
            usize start = 0;
            for (usize i = 0; i <= path.Size(); ++i)
            {
                if (i == path.Size() || path[i] == utf8char('/'))
                {
                    if (i > start)
                    {
                        group = group->CreateGroup(path.SubStr(start, i - start));
                        if (group == nullptr)
                        {
                            return nullptr;
                        }
                    }
                    start = i + 1;
                }
            }
            return group;
        }

        // A custom shader's cooked product, staged without its HLSL under the same guid and path.
        // The dist's pack carries the shader as bytecode; the entries, lattices and property table
        // stay, so materials and the runtime still resolve it.
        bool StageStrippedShader(foundation::content::Instance& product,
                                 foundation::content::ContentDatabase& staging)
        {
            RefPtr<ISerializable> object = product.ReadObject();
            auto* shader = Cast<foundation::shaders::ShaderSource>(object.Get());
            foundation::content::Group* group =
                StagingGroup(staging, product.OwningGroup().Path().AsView());
            if (shader == nullptr || group == nullptr)
            {
                return false;
            }
            shader->StripSources(); // every program's HLSL
            foundation::content::Instance* staged = group->CreateInstanceWithId(
                product.Id(), product.Name(), foundation::shaders::ShaderSource::StaticType());
            return staged != nullptr && staged->WriteObject(*shader).IsOk();
        }

        // Re-encodes a scene instance into a binary envelope in the staging DB, with the same guid
        // and path.
        bool StageScene(foundation::content::Instance& scene,
                        foundation::content::ContentDatabase& staging,
                        const HashMap<Guid, Array<byte>>* sceneStreams)
        {
            foundation::content::Group* group = staging.RootGroup();
            const String path = scene.OwningGroup().Path();
            usize start = 0;
            const StringView folder = path.AsView();
            for (usize i = 0; i <= folder.Size(); ++i)
            {
                if (i == folder.Size() || folder[i] == utf8char('/'))
                {
                    if (i > start)
                    {
                        group = group->CreateGroup(folder.SubStr(start, i - start));
                        if (group == nullptr)
                        {
                            return false;
                        }
                    }
                    start = i + 1;
                }
            }

            // Scenes and prefabs stage the same way (runtime scenes keep prefab instances as a ref
            // plus deltas, so the prefab payloads must ship in the pak for the respawn at load time).
            RefPtr<ISerializable> object = scene.ReadObject();
            foundation::content::Instance* staged = nullptr;
            if (auto* doc = Cast<foundation::scene::SceneDocument>(object.Get()))
            {
                staged = group->CreateInstanceWithId(scene.Id(), scene.Name(),
                                                     foundation::scene::SceneDocument::StaticType());
                if (staged == nullptr || !staged->WriteObject(*doc).IsOk())
                {
                    return false;
                }
            }
            else if (auto* prefab = Cast<foundation::scene::PrefabDocument>(object.Get()))
            {
                staged = group->CreateInstanceWithId(scene.Id(), scene.Name(),
                                                     foundation::scene::PrefabDocument::StaticType());
                if (staged == nullptr || !staged->WriteObject(*prefab).IsOk())
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            // Use the pre-transcoded binary stream (the editor's main-thread pass) when available,
            // else the source stream as it is. The runtime detects the encoding, so an XML source
            // staged as is (the headless CLI, with no engine subsystems to transcode with) still
            // loads, only with the cost of parsing text.
            if (sceneStreams != nullptr)
            {
                if (const Array<byte>* pre = sceneStreams->Find(scene.Id()))
                {
                    return staged->WriteData(u8"scene", Span<const byte>{pre->Data(), pre->Size()})
                        .IsOk();
                }
            }
            if (UniquePtr<IStream> stream = scene.ReadData(u8"scene"))
            {
                Array<byte> bytes;
                bytes.Resize(static_cast<usize>(stream->Size()));
                if (stream->Read(bytes.Data(), bytes.Size()) != bytes.Size())
                {
                    return false;
                }
                if (!staged->WriteData(u8"scene", Span<const byte>{bytes.Data(), bytes.Size()})
                         .IsOk())
                {
                    return false;
                }
            }
            return true;
        }

        void CollectScenes(foundation::content::Group& group,
                           Array<foundation::content::Instance*>& out)
        {
            for (foundation::content::Instance* instance : group.Instances())
            {
                if (instance->TypeName() == u8"SceneDocument" ||
                    instance->TypeName() == u8"PrefabDocument")
                {
                    out.PushBack(instance);
                }
            }
            for (foundation::content::Group* child : group.Groups())
            {
                CollectScenes(*child, out);
            }
        }

        // Every instance in a database (used to enumerate cooked products for the dropped report).
        void CollectAllInstances(foundation::content::Group& group,
                                 Array<foundation::content::Instance*>& out)
        {
            for (foundation::content::Instance* instance : group.Instances())
            {
                out.PushBack(instance);
            }
            for (foundation::content::Group* child : group.Groups())
            {
                CollectAllInstances(*child, out);
            }
        }

        // The source instance whose asset imports `fileName` (the startup script's own asset, if the
        // project imported the script). Nil when none; the script file still ships either way.
        [[nodiscard]] inline Guid FindAssetByFileName(foundation::content::ContentDatabase& db,
                                                      StringView fileName)
        {
            Array<foundation::content::Instance*> instances;
            CollectAllInstances(*db.RootGroup(), instances);
            for (foundation::content::Instance* instance : instances)
            {
                RefPtr<ISerializable> object = instance->ReadObject();
                if (const Asset* asset = Cast<Asset>(object.Get()))
                {
                    if (!asset->fileName.IsEmpty() && asset->fileName.View() == fileName)
                    {
                        return instance->Id();
                    }
                }
            }
            return Guid{};
        }

        void RemoveTreeRecursive(StringView root)
        {
            foundation::vfs::NativeFileSystem fs(root, foundation::core::DefaultAllocator());
            Array<foundation::vfs::DirEntry> entries;
            if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
            {
                for (const foundation::vfs::DirEntry& entry : entries)
                {
                    if (entry.isDirectory)
                    {
                        RemoveTreeRecursive(PathJoin(root, entry.name.AsView()).AsView());
                    }
                    else
                    {
                        (void)fs.AsWritable()->Delete(entry.name.AsView());
                    }
                }
            }
            (void)RemoveDirectory(root);
        }

        // Copies srcDir/srcName to dstDir/dstName, preserving permissions (staged executables need
        // the +x bit, which a VFS read and write would drop). True on success.
        bool CopyFilePreserving(StringView srcDir, StringView srcName, StringView dstDir,
                                StringView dstName)
        {
            // Core/System backend (POSIX re-applies the source mode; Windows CopyFile preserves it
            // natively). std::filesystem is not used in this module: referencing it from exported
            // inline code breaks GCC's module serialization for importers ("failed to load
            // pendings for __gnu_cxx::__concurrence_unlock_error").
            return FileCopyPreserving(PathJoin(srcDir, srcName).AsView(),
                                      PathJoin(dstDir, dstName).AsView());
        }

        // Last path component of `path` (after the final '/' or '\\').
        [[nodiscard]] inline StringView BaseName(StringView path)
        {
            usize start = 0;
            for (usize i = 0; i < path.Size(); ++i)
            {
                if (path[i] == utf8char('/') || path[i] == utf8char('\\'))
                {
                    start = i + 1;
                }
            }
            return path.SubStr(start, path.Size() - start);
        }

        // A filesystem-safe output subdirectory from a preset name (letters, digits, '-', '_' and
        // '.' are kept; anything else becomes '-').
        [[nodiscard]] inline String SanitizeName(StringView name)
        {
            String out;
            for (usize i = 0; i < name.Size(); ++i)
            {
                const utf8char c = name[i];
                const bool ok = (c >= utf8char('a') && c <= utf8char('z')) ||
                                (c >= utf8char('A') && c <= utf8char('Z')) ||
                                (c >= utf8char('0') && c <= utf8char('9')) || c == utf8char('-') ||
                                c == utf8char('_') || c == utf8char('.');
                out += ok ? StringView(&c, 1) : StringView(u8"-");
            }
            return out.IsEmpty() ? String(u8"export") : out;
        }
    }

    // A step and progress sink: `onProgress(stepLabel, fraction[0..1])`. Optional: the CLI passes
    // none, and the editor's background job wires it to a JobContext for the status-bar progress bar.
    using ExportProgress = Function<void(StringView, f32)>;

    // === Reachability pruning helpers ===

    /// Seeds the export roots, the entry points whose closure the dist ships: the default scene,
    /// the startup script's own imported asset (if any), every asset the manifest's settings name,
    /// and the project's explicit "Always Export" set (ExportRoots), which is flagged instances
    /// (Flag) and every instance under a flagged group subtree (Group). Deduplicated by guid,
    /// keeping the first (highest-priority) reason, so the pruning report lists each root once
    /// with a stable reason.
    [[nodiscard]] Array<ExportRoot> CollectExportRoots(EditorProject& project);

    /// Expands seed roots across the scene-graph edges the cook does not model: scans each scene or
    /// prefab root for its component resource Refs (fed to PlanFor) and its prefab instances
    /// (staged and rescanned), transitively. Returns the deduplicated guid set to seed
    /// CookDriver::PlanFor with; PlanFor then closes the asset-to-asset edges. `scanner` supplies
    /// the scene-to-asset edges. Without it the scene contents cannot be discovered, so the caller
    /// must supply it when pruning.
    [[nodiscard]] Array<Guid> ExpandReachableRoots(EditorProject& project,
                                                   const Array<ExportRoot>& seeds,
                                                   const SceneReferenceScanner& scanner);

    /// Cooks and computes the reachable closure with one CookDriver: PlanFor(planRoots) yields the
    /// closure (asset to asset), and when `cook` is set, Execute cooks only that set, so only what
    /// ships is cooked. Fills stats.cooked, stats.cookFailed and `outReachable` (the full closure's
    /// guids).
    [[nodiscard]] Status CookReachable(EditorProject& project, BuilderRegistry& builders,
                                       Span<const Guid> planRoots, bool cook, bool rebuild,
                                       ExportStats& stats, Array<Guid>& outReachable,
                                       const ExportProgress& onProgress = {});

    /// Renders a pruning report as readable text (the export-report.txt on disk and the Console dump).
    [[nodiscard]] String FormatPruningReport(const PruningReport& report);

    /// One packed content variant in the dist. Desktop dists have exactly one, with an empty key
    /// and "Content.pak" packed from the host `Cooked/`. The web dist has "bc" and "astc", each a
    /// complete pak from its per-target cooked DB (Cooked-web-bc/, Cooked-web-astc/). `key` is the
    /// capability family the wasm boot matches an adapter feature against, never a platform name.
    /// `cookedDir` is the absolute cooked-DB directory this pak is packed from.
    struct ContentVariant
    {
        String key;       // "" (the single desktop pak), "bc" or "astc"
        String pakName;   // dist pak filename ("Content.pak", "Content-bc.pak" or "Content-astc.pak")
        String cookedDir; // absolute cooked-DB directory to pack from
    };

    /// The content variants a preset ships: desktop platforms get the single host pak, and "Web"
    /// and "Wasm" get the BC and ASTC variant paks. The cookedDir paths are siblings of the host
    /// Cooked/, so the desktop pack never sees them.
    [[nodiscard]] Array<ContentVariant> VariantsForPlatform(EditorProject& project,
                                                            StringView platform);

    /// Stages scenes, packs the content pak or paks and writes the dist manifest into `outDir`. It
    /// does not cook; it assumes the cooked directories are up to date (ExportProject or
    /// CookVariantTargets run first). Fills stats.scenesStaged and stats.filesPacked.
    // `reachable` (opt-in closure pruning): when non-null, stage only reachable scenes and pack only
    // cooked products whose source guid is in the set; otherwise the whole cooked directory and
    // every scene. `roots` and `outReport` feed the pruning report (kept roots and reasons, the
    // dropped list), written beside the dist as export-report.txt and returned to the caller.
    // `variants`: the content variants to pack. Empty gives a single Content.pak from the host
    // Cooked/. Reachability is scanned once (host DB) and applied to every variant pak, since the
    // closure's guid set is the same across variants.
    [[nodiscard]] Status ExportContent(EditorProject& project, StringView outDir,
                                       ExportStats& stats, const ExportProgress& onProgress = {},
                                       const HashMap<Guid, Array<byte>>* sceneStreams = nullptr,
                                       const HashMap<Guid, u8>* reachable = nullptr,
                                       const Array<ExportRoot>* roots = nullptr,
                                       PruningReport* outReport = nullptr,
                                       Span<const ContentVariant> variants = {},
                                       bool includeShaderSource = false);

    /// Cooks the per-target variant DBs: for each variant with a non-empty key, runs CookForTarget
    /// into its cookedDir, carrying platform-invariant products forward from the already-cooked
    /// host DB (records loaded from the host .cache) and recooking only the variant products
    /// (textures) for the target. Empty-key (desktop) variants do nothing. Call after the host
    /// cook and before ExportContent. Returns the summed cook failures.
    [[nodiscard]] Status CookVariantTargets(EditorProject& project, BuilderRegistry& builders,
                                            Span<const ContentVariant> variants, bool rebuild,
                                            const ExportProgress& onProgress = {});

    /// The scene and prefab text sources under `group`, pre-transcoded to the binary wire format
    /// by `stager` (the editor's SceneStreamStager, main thread only) and keyed by instance for
    /// the export's packer. An instance the stager declines (not a scene, or a failure) is left
    /// out, and the exporter then stages its source as it is.
    inline void CollectSceneStreams(
        foundation::content::Group& group,
        const Function<bool(foundation::content::Instance&, Array<byte>&)>& stager,
        HashMap<Guid, Array<byte>>& out)
    {
        for (foundation::content::Instance* instance : group.Instances())
        {
            Array<byte> bytes;
            if (stager(*instance, bytes))
            {
                out.InsertOrAssign(instance->Id(), Move(bytes));
            }
        }
        for (foundation::content::Group* child : group.Groups())
        {
            CollectSceneStreams(*child, stager, out);
        }
    }

    /// Cooks, then runs ExportContent, all in one (the CLI's one-shot path). The builder registry
    /// is the executable's full set (kept the same across the cook, editor and export tools).
    /// `rebuild` forces a clean cook. `variants`: when non-empty (a web preset), the host cook is
    /// followed by CookVariantTargets and ExportContent packs one pak per variant. Empty gives a
    /// single pak.
    [[nodiscard]] Status ExportProject(EditorProject& project, StringView outDir,
                                       BuilderRegistry& builders, bool rebuild,
                                       ExportStats* outStats = nullptr,
                                       const ExportProgress& onProgress = {},
                                       const HashMap<Guid, Array<byte>>* sceneStreams = nullptr,
                                       Span<const ContentVariant> variants = {},
                                       bool includeShaderSource = false);

    struct ExportResult
    {
        ExportStats content;         // cook, stage and pack totals
        usize filesStaged = 0;       // the player, template sidecars and preset additionalFiles copied
        String outputDir;            // where the dist was written
        String engineVersionWarning; // set when the resolved template was built against a different
                                     // engine version (soft mismatch); empty otherwise. The export
                                     // still runs; callers may surface this to the user.
        PruningReport pruning; // closure-pruning report (roots and reasons, kept and dropped
                               // counts); pruned=false for an export that packs everything
    };

    /// Builds the project's native module for the development loop: a persistent per-project
    /// shared-engine build (<project>/.cache/native-dev) over the engine checkout, building only
    /// the game target, whose .so lands at the manifest's nativeModule path (the Native/CMakeLists
    /// output-directory rule). Pair it with the editor's Reload Native Module action for the full
    /// in-editor loop. Blocking; run it off the UI thread.
    [[nodiscard]] Status BuildDevNativeModule(EditorProject& project);

    /// Writes a preset's own render resolution, window or orientation over what the project's
    /// dist manifest (player.xml in `outDir`) says, so the platform's values ship. Does nothing
    /// when the preset overrides none of them.
    [[nodiscard]] Status ApplyPresetDisplay(StringView outDir, const ExportPreset& preset);

    /// Produces one preset's dist under `outRoot`: resolves its template, exports the content
    /// (ExportProject), then stages the template's player and sidecars and the preset's
    /// additionalFiles. The CLI and the editor both produce the whole dist through this one entry
    /// point.
    // `cook` = false skips the cook and only packs and stages (ExportContent). The editor uses this
    // after cooking through its CookService, so the cook, which changes the DB the UI reads, never
    // runs on a background job. The CLI leaves it true (cook and content in one go).
    // `dataRoot` is the engine data root (the caller's resolved one): the engine shaders are cooked
    // from <dataRoot>/Shaders into the dist's own Data/Engine/Shaders/shaders.dpak (and
    // Data/.dataroot), so the player finds them with the same discovery walk every executable uses.
    [[nodiscard]] Status ExportOne(EditorProject& project, const ExportPreset& preset,
                                   const TemplateRegistry& templates, BuilderRegistry& builders,
                                   StringView outRoot, StringView dataRoot, bool rebuild,
                                   ExportResult* outResult = nullptr,
                                   const ExportProgress& onProgress = {}, bool cook = true,
                                   const HashMap<Guid, Array<byte>>* sceneStreams = nullptr,
                                   const SceneReferenceScanner* scanner = nullptr,
                                   const Array<Guid>* precomputedReachableRoots = nullptr);

    /// Produces every preset's dist under `outRoot` (a failing preset is logged and skipped; the
    /// others continue). Returns Ok only when all presets succeeded.
    [[nodiscard]] Status ExportAll(EditorProject& project, Span<const ExportPreset> presets,
                                   const TemplateRegistry& templates, BuilderRegistry& builders,
                                   StringView outRoot, StringView dataRoot, bool rebuild,
                                   const ExportProgress& onProgress = {}, bool cook = true,
                                   const HashMap<Guid, Array<byte>>* sceneStreams = nullptr,
                                   const SceneReferenceScanner* scanner = nullptr,
                                   const Array<Guid>* precomputedReachableRoots = nullptr);
}
