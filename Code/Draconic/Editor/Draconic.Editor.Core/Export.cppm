// Draconic::EditorCore - :export partition.
//
// The export pipeline as a LIBRARY (the Draconic.Tools.Export CLI and the editor's Export menu are
// thin callers; tests drive it headlessly): cook -> stage scenes as binary envelopes ->
// pack Content.pak -> write the dist manifest. The caller stages the player executable
// (an exe-location concern, not a pipeline one).
//
//   dist layout:  <out>/Content.pak   products + scenes (one binary DB) + game script (raw)
//                 <out>/player.xml    dist manifest (ProjectSettings shape)
//
// Scenes are builder-less by design (their cooked form IS the authored form), so export
// re-encodes each XML envelope to binary and copies the "scene" stream - SAME guids, so
// resource refs and the manifest's defaultScene path keep working in the pak.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.core:export_pipeline;

import draconic.foundation;
import draconic.vfs;
import draconic.vfs.pak;
import draconic.content;
import draconic.engine.project;
import draconic.scene.resource;
import draconic.editor;
import draconic.editor.cook;
import :project;
import :export_preset;
import :export_roots;
import :export_template;

using namespace draconic::foundation;

export namespace draconic::editor
{
    struct ExportStats
    {
        usize cooked = 0;
        usize cookFailed = 0;
        usize scenesStaged = 0;
        usize filesPacked = 0;
    };

    // === Reachability pruning (docs/design/export-reachability.md, Phase 1) ===
    //
    // Opt-in per preset (ExportPreset::pruneToReachable): a dist ships only the CLOSURE of its
    // entry points instead of the whole cooked dir. The closure reuses the cook's dependency graph
    // (CookDriver::PlanFor) for asset->asset edges; a caller-supplied SceneReferenceScanner bridges
    // the scene-graph edges the cook doesn't model (a scene's component resource Refs + its prefab
    // instances), because scenes/prefabs are builder-less and thus outside PlanFor's read-dep walk.

    // Why a root is in the dist. Phase 1 seeds DefaultScene + StartupScript; Phase 2 ("Always
    // Export") adds Flag / Group; Phase 4 adds ScriptLiteral. Keep the enum stable for the report.
    enum class ExportRootReason
    {
        DefaultScene,  // ProjectSettings::defaultSceneId
        StartupScript, // the startup script's own imported asset (the script FILE ships regardless)
        Flag,          // an instance flagged "Always Export" (ExportRoots::instances)
        Group, // an instance under a group flagged "Always export contents" (ExportRoots::groups)
        ManifestDefault, // a manifest default reference (input map / bus layout / UI theme / UI font)
    };

    [[nodiscard]] StringView ExportRootReasonName(ExportRootReason r);

    // A seed entry point: the dist is the closure of these. The Array<ExportRoot> the seeding
    // function returns is the SEAM Phase 2 extends (it just appends Flag/Group roots).
    struct ExportRoot
    {
        Guid id;
        String name; // the instance's source path (report display); "" if unresolved
        ExportRootReason reason = ExportRootReason::DefaultScene;
    };

    // What a scan of one scene/prefab instance yields: the guids it references directly. Resources
    // feed PlanFor (which then closes asset->asset); prefabs are staged AND rescanned for their own
    // references (the scene->prefab->asset chain).
    struct SceneReferences
    {
        Array<Guid> resources; // component resource Ref ids (mesh/material/texture/... instances)
        Array<Guid> prefabs;   // prefab-instance ids nested in this scene/prefab
    };

    // Caller hook: collect one scene/prefab instance's direct references (see SceneReferences).
    // The export LIBRARY stays subsystem-agnostic, so the driving tool - which owns the full
    // component-manager set (render/physics/animation/...) - supplies this. The canonical
    // implementation loads the instance (LoadScene over all managers), resolves its Refs through a
    // factory-less ResourceManager and reads back ResourceManager::CollectUnresolved (every bound
    // id, since nothing built), plus each parked prefab instance's prefabId. Same reason the
    // scene-stream transcode (sceneStreams) is a caller hook.
    using SceneReferenceScanner = Function<void(
        draconic::content::Instance&, draconic::content::ContentDatabase&, SceneReferences&)>;

    // The loud, auditable record of a pruned export: which roots were kept and WHY, plus what was
    // dropped. Lives on ExportResult (CLI prints it, editor Console shows it) and is written beside
    // the dist as export-report.txt. Empty/pruned=false for a pack-everything export.
    struct PruningReport
    {
        bool pruned = false;
        Array<ExportRoot> roots; // the seed entry points + their reasons
        usize keptCount = 0;     // scenes + cooked products shipped (closure size on disk)
        Array<String> dropped;   // instance paths excluded from the dist (WIP/unreferenced)
    };

    namespace detail
    {
        // A cooked file's owning instance is reachable: the owning instance path is `file` up to a
        // '.' in its NAME region (envelope "<path>.<ext>" and stream "<path>.<stream>.bin" both begin
        // with "<path>."). Tests each '.' boundary against the reachable-instance-path set; the '.'
        // delimiter makes prefix matching collision-safe (a peer "CubeBig" never matches "Cube.").
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

        // Recursively add every file under `folder` to the pak (locator = mount-relative path).
        // When `reachablePaths` is non-null, packs ONLY files whose owning instance path is in the
        // set (closure pruning); null packs the whole tree (the default "export everything").
        bool PackTree(draconic::vfs::IFileSystem& mount,
                      draconic::vfs::IEnumerableFileSystem& enumerable, StringView folder,
                      draconic::vfs::PakBuilder& pak, usize& fileCount,
                      const HashMap<String, u8>* reachablePaths = nullptr)
        {
            Array<draconic::vfs::DirEntry> entries;
            if (!enumerable.Enumerate(folder, entries).IsOk())
            {
                return folder.IsEmpty();
            }
            for (const draconic::vfs::DirEntry& entry : entries)
            {
                const String path = PathJoin(folder, entry.name.AsView());
                if (entry.isDirectory)
                {
                    if (!PackTree(mount, enumerable, path.AsView(), pak, fileCount, reachablePaths))
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

        // Re-encode a scene instance into a binary envelope in the staging DB - SAME guid/path.
        bool StageScene(draconic::content::Instance& scene,
                        draconic::content::ContentDatabase& staging,
                        const HashMap<Guid, Array<byte>>* sceneStreams)
        {
            draconic::content::Group* group = staging.RootGroup();
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

            // Scenes AND prefabs stage the same way (runtime scenes keep prefab instances as
            // ref+deltas, so the prefab payloads must ship in the pak for the load-time respawn).
            RefPtr<ISerializable> object = scene.ReadObject();
            draconic::content::Instance* staged = nullptr;
            if (auto* doc = Cast<draconic::scene::SceneDocument>(object.Get()))
            {
                staged = group->CreateInstanceWithId(scene.Id(), scene.Name(),
                                                     draconic::scene::SceneDocument::StaticType());
                if (staged == nullptr || !staged->WriteObject(*doc).IsOk())
                {
                    return false;
                }
            }
            else if (auto* prefab = Cast<draconic::scene::PrefabDocument>(object.Get()))
            {
                staged = group->CreateInstanceWithId(scene.Id(), scene.Name(),
                                                     draconic::scene::PrefabDocument::StaticType());
                if (staged == nullptr || !staged->WriteObject(*prefab).IsOk())
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            // Pre-transcoded BINARY stream (editor's main-thread pass) when available; else
            // the source stream verbatim - the runtime SNIFFS the encoding, so an XML source
            // staged as-is (headless CLI, no engine subsystems to transcode with) still
            // loads, just with text-parse cost.
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

        void CollectScenes(draconic::content::Group& group,
                           Array<draconic::content::Instance*>& out)
        {
            for (draconic::content::Instance* instance : group.Instances())
            {
                if (instance->TypeName() == u8"SceneDocument" ||
                    instance->TypeName() == u8"PrefabDocument")
                {
                    out.PushBack(instance);
                }
            }
            for (draconic::content::Group* child : group.Groups())
            {
                CollectScenes(*child, out);
            }
        }

        // Every instance in a database (used to enumerate cooked products for the dropped report).
        void CollectAllInstances(draconic::content::Group& group,
                                 Array<draconic::content::Instance*>& out)
        {
            for (draconic::content::Instance* instance : group.Instances())
            {
                out.PushBack(instance);
            }
            for (draconic::content::Group* child : group.Groups())
            {
                CollectAllInstances(*child, out);
            }
        }

        // The source instance whose asset imports `fileName` (the startup script's own asset, if the
        // project imported the script). Nil when none - the script FILE still ships either way.
        [[nodiscard]] inline Guid FindAssetByFileName(draconic::content::ContentDatabase& db,
                                                      StringView fileName)
        {
            Array<draconic::content::Instance*> instances;
            CollectAllInstances(*db.RootGroup(), instances);
            for (draconic::content::Instance* instance : instances)
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
            draconic::vfs::NativeFileSystem fs(root);
            Array<draconic::vfs::DirEntry> entries;
            if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
            {
                for (const draconic::vfs::DirEntry& entry : entries)
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

        // Copy srcDir/srcName -> dstDir/dstName, PRESERVING permissions (staged executables need the
        // +x bit, which a VFS read+write would drop). True on success.
        bool CopyFilePreserving(StringView srcDir, StringView srcName, StringView dstDir,
                                StringView dstName)
        {
            // Core/System backend (POSIX re-applies the source mode; Windows CopyFile preserves
            // natively). std::filesystem is deliberately NOT used in this module: referencing it
            // from exported inline code broke GCC's module serialization for importers
            // ("failed to load pendings for __gnu_cxx::__concurrence_unlock_error").
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

        // A filesystem-safe output subdir from a preset name (alnum / - _ . kept, else '-').
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

    // A step/progress sink: `onProgress(stepLabel, fraction[0..1])`. Optional (the CLI passes none;
    // the editor's background job wires it to a JobContext for the status-bar progress bar).
    using ExportProgress = Function<void(StringView, f32)>;

    // === Reachability pruning helpers ===

    /// Seed export roots: the entry points whose closure the dist ships. Phase 1 = defaultSceneId
    /// (+ the startup script's own imported asset, if any); Phase 2 = the project's explicit
    /// "Always Export" set (ExportRoots) - flagged instances (Flag) and every instance under a
    /// flagged group subtree (Group). Deduped by guid, keeping the FIRST (highest-priority) reason,
    /// so the pruning report lists each root once with a stable why.
    [[nodiscard]] Array<ExportRoot> CollectExportRoots(EditorProject& project);

    /// Expand seed roots across the scene-graph edges the cook does not model: scan each scene/prefab
    /// root for its component resource Refs (feed PlanFor) and its prefab instances (staged AND
    /// rescanned), transitively. Returns the deduped guid set to seed CookDriver::PlanFor with -
    /// PlanFor then closes the asset->asset edges. `scanner` bridges scene->asset; without it the
    /// scene contents can't be discovered (caller must supply it when pruning).
    [[nodiscard]] Array<Guid> ExpandReachableRoots(EditorProject& project,
                                                   const Array<ExportRoot>& seeds,
                                                   const SceneReferenceScanner& scanner);

    /// Cook + compute the reachable closure with ONE CookDriver: PlanFor(planRoots) yields the
    /// closure (asset->asset), and - when `cook` - Execute cooks ONLY that set (cook only what
    /// ships). Fills stats.cooked / stats.cookFailed and `outReachable` (the full closure guids).
    [[nodiscard]] Status CookReachable(EditorProject& project, BuilderRegistry& builders,
                                       Span<const Guid> planRoots, bool cook, bool rebuild,
                                       ExportStats& stats, Array<Guid>& outReachable,
                                       const ExportProgress& onProgress = {});

    /// Render a pruning report as human-readable text (the on-disk export-report.txt + Console dump).
    [[nodiscard]] String FormatPruningReport(const PruningReport& report);

    /// Stage scenes + pack Content.pak + write the dist manifest into `outDir`. Does NOT cook - it
    /// assumes the project's cooked dir is already up to date (the CLI's ExportProject cooks then calls
    /// this; the editor cooks via CookService first, then runs this on a background job). Fills
    /// stats.scenesStaged / stats.filesPacked.
    // `reachable` (opt-in closure pruning): when non-null, stage ONLY reachable scenes and pack ONLY
    // cooked products whose source guid is in the set - the whole cooked dir + every scene otherwise.
    // `roots` + `outReport` feed the pruning report (kept roots + reasons, dropped list), written
    // beside the dist as export-report.txt and returned to the caller. All null => today's behavior,
    // byte-for-byte.
    [[nodiscard]] Status ExportContent(EditorProject& project, StringView outDir,
                                       ExportStats& stats, const ExportProgress& onProgress = {},
                                       const HashMap<Guid, Array<byte>>* sceneStreams = nullptr,
                                       const HashMap<Guid, u8>* reachable = nullptr,
                                       const Array<ExportRoot>* roots = nullptr,
                                       PruningReport* outReport = nullptr);

    /// Cook + ExportContent (the all-in-one; the CLI / one-shot path). The builder registry is the
    /// exe's full set (kept in lockstep across cook/editor/export). `rebuild` forces a clean cook.
    [[nodiscard]] Status ExportProject(EditorProject& project, StringView outDir,
                                       BuilderRegistry& builders, bool rebuild,
                                       ExportStats* outStats = nullptr,
                                       const ExportProgress& onProgress = {},
                                       const HashMap<Guid, Array<byte>>* sceneStreams = nullptr);

    struct ExportResult
    {
        ExportStats content;         // cook/stage/pack totals
        usize filesStaged = 0;       // player + template sidecars + preset additionalFiles copied
        String outputDir;            // where the dist landed
        String engineVersionWarning; // set when the resolved template was built against a different
                                     // engine version (soft mismatch); empty otherwise. The export
                                     // still runs; callers may surface this to the user.
        PruningReport pruning; // closure-pruning report (roots + reasons, kept/dropped counts).
                               // pruned=false for a pack-everything (non-pruned) export.
    };

    /// Produce ONE preset's dist under `outRoot`: resolve its template, export the content
    /// (ExportProject), then stage the template's player + sidecars and the preset's additionalFiles.
    /// The whole dist from one entry point - the CLI and the editor call this identically (the cook
    /// uniformity extended to the player, replacing the old exe-location walk in the CLI).
    // `cook` = false skips the cook and only packs/stages (ExportContent) - the editor uses this AFTER
    // cooking through its CookService (so the cook, which mutates the DB the UI reads, never runs on a
    // background job). The CLI leaves it true (cook + content in one shot).
    [[nodiscard]] Status ExportOne(EditorProject& project, const ExportPreset& preset,
                                   const TemplateRegistry& templates, BuilderRegistry& builders,
                                   StringView outRoot, bool rebuild,
                                   ExportResult* outResult = nullptr,
                                   const ExportProgress& onProgress = {}, bool cook = true,
                                   const HashMap<Guid, Array<byte>>* sceneStreams = nullptr,
                                   const SceneReferenceScanner* scanner = nullptr,
                                   const Array<Guid>* precomputedReachableRoots = nullptr);

    /// Produce EVERY preset's dist under `outRoot` (a failing preset is logged and skipped; the others
    /// continue). Returns Ok only when all presets succeeded.
    [[nodiscard]] Status ExportAll(EditorProject& project, Span<const ExportPreset> presets,
                                   const TemplateRegistry& templates, BuilderRegistry& builders,
                                   StringView outRoot, bool rebuild,
                                   const ExportProgress& onProgress = {}, bool cook = true,
                                   const HashMap<Guid, Array<byte>>* sceneStreams = nullptr,
                                   const SceneReferenceScanner* scanner = nullptr,
                                   const Array<Guid>* precomputedReachableRoots = nullptr);
}
