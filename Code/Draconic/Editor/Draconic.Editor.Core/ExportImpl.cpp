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

module draconic.editor.core;

import draconic.foundation;
import draconic.vfs;
import draconic.vfs.pak;
import draconic.content;
import draconic.engine.project;
import draconic.scene.resource;
import draconic.editor;
import draconic.editor.cook;
import draconic.shaders;
import :project;
import :export_preset;
import :export_roots;
import :export_template;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace
    {
        // The cooked-blob formats a target platform's runtime needs: web = WGSL, Windows = SPIR-V
        // (Vulkan) + DXIL (DX12), other desktop = SPIR-V (Vulkan). Kept generous rather than guessing
        // the backend a desktop dist will pick at runtime.
        Array<shaders::CookedShaderFormat> FormatsForPlatform(StringView platform)
        {
            Array<shaders::CookedShaderFormat> formats;
            if (platform.StartsWith(u8"Web") || platform.StartsWith(u8"Wasm"))
            {
                formats.PushBack(shaders::CookedShaderFormat::Wgsl);
            }
            else if (platform.StartsWith(u8"Win"))
            {
                formats.PushBack(shaders::CookedShaderFormat::SpirV);
                formats.PushBack(shaders::CookedShaderFormat::Dxil);
            }
            else
            {
                formats.PushBack(shaders::CookedShaderFormat::SpirV);
            }
            return formats;
        }

        // The DXC runtime libs (dxcompiler / dxil), which a cooked dist no longer needs - the shader
        // pack retires the runtime compiler. Matched by substring on the sidecar's file name.
        bool IsDxcRuntimeLib(StringView name)
        {
            const auto contains = [&](StringView needle)
            {
                if (needle.Size() > name.Size())
                {
                    return false;
                }
                for (usize i = 0; i + needle.Size() <= name.Size(); ++i)
                {
                    if (name.SubStr(i, needle.Size()) == needle)
                    {
                        return true;
                    }
                }
                return false;
            };
            return contains(u8"dxcompiler") || contains(u8"dxil");
        }

        // Resolve the engine shader source root for the cook (baked source path, else a "Shaders"
        // dir beside a relocated editor).
        StringView EngineShaderDir()
        {
#ifdef DRACONIC_ENGINE_SHADER_DIR
            constexpr StringView baked = u8"" DRACONIC_ENGINE_SHADER_DIR;
#else
            constexpr StringView baked = u8"Shaders";
#endif
            if (DirectoryExists(baked))
            {
                return baked;
            }
            return u8"Shaders";
        }

        // Cook the built-in shaders for `platform` and write <outputDir>/shaders.dpak. The dist
        // renders from this pack with no runtime compiler (see the ShaderSystem cooked path). Returns
        // the variant count via `outVariants`; Status carries any cook/compile failure.
        Status StageShaderPack(StringView outputDir, StringView platform, u32& outVariants)
        {
            outVariants = 0;
            shaders::Compiler* compiler = nullptr;
            if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() ||
                compiler == nullptr)
            {
                DRACONIC_LOG_ERROR(u8"Export",
                                   u8"cannot cook shaders: the DXC compiler is unavailable");
                return Status{ErrorCode::Internal};
            }

            const Array<shaders::CookedShaderFormat> formats = FormatsForPlatform(platform);
            shaders::ShaderCookOptions opts;
            opts.shaderDir = EngineShaderDir();
            opts.scratchDir = outputDir; // WGSL intermediates (deleted); unused for SPIR-V/DXIL
            opts.formats = Span<const shaders::CookedShaderFormat>(formats.Data(), formats.Size());

            shaders::CookedShaderPack pack;
            const shaders::ShaderCookReport report =
                shaders::CookEngineShaders(*compiler, opts, pack);
            compiler->Destroy();

            for (usize i = 0; i < report.errors.Size(); ++i)
            {
                DRACONIC_LOG_ERROR(u8"Export", u8"shader cook: {}", report.errors[i]);
            }
            if (!report.success)
            {
                return Status{ErrorCode::Internal};
            }

            const String packPath = PathJoin(outputDir, u8"shaders.dpak");
            FileStream out(packPath.AsView(), FileMode::Write);
            if (!out.IsValid() || !pack.Write(out).IsOk())
            {
                DRACONIC_LOG_ERROR(u8"Export", u8"could not write shaders.dpak to '{}'", outputDir);
                return Status{ErrorCode::Internal};
            }
            outVariants = static_cast<u32>(pack.Count());
            return Status{};
        }
    }

    StringView ExportRootReasonName(ExportRootReason r)
    {
        switch (r)
        {
        case ExportRootReason::DefaultScene:
            return u8"default-scene";
        case ExportRootReason::StartupScript:
            return u8"startup-script";
        case ExportRootReason::Flag:
            return u8"always-export";
        case ExportRootReason::Group:
            return u8"always-export-group";
        case ExportRootReason::ManifestDefault:
            return u8"manifest-default";
        }
        return u8"?";
    }

    Array<ExportRoot> CollectExportRoots(EditorProject& project)
    {
        Array<ExportRoot> roots;
        HashMap<Guid, u8> seen;
        const auto add = [&](const Guid& id, ExportRootReason reason)
        {
            if (id.IsNil() || seen.Find(id) != nullptr)
            {
                return;
            }
            seen.InsertOrAssign(id, u8(1));
            ExportRoot root;
            root.id = id;
            root.reason = reason;
            if (draconic::content::Instance* inst = project.SourceDb().GetInstance(id))
            {
                root.name = inst->Path();
            }
            roots.PushBack(Move(root));
        };

        const draconic::project::ProjectSettings& settings = project.Settings();

        add(settings.defaultSceneId, ExportRootReason::DefaultScene);

        // The startup game script is a cooked ScriptClass asset (guid-authoritative) - seed it as a
        // reachability root so it (and the assets IT loads, via the normal AssetRef contract) ship.
        add(settings.startupScriptId, ExportRootReason::StartupScript);

        // Every manifest default the PLAYER binds at startup must ship, or the binding silently
        // fails in a pruned dist (the theme/input-map/bus-layout gap existed before the font).
        add(settings.defaultInputMapId, ExportRootReason::ManifestDefault);
        add(settings.defaultBusLayoutId, ExportRootReason::ManifestDefault);
        add(settings.defaultUiThemeId, ExportRootReason::ManifestDefault);
        add(settings.defaultUiFontId, ExportRootReason::ManifestDefault);

        // Phase 2 "Always Export": explicit instance flags, then group subtrees (dynamic membership -
        // whatever is under the flagged folder now). A group that also contains the default scene /
        // a directly-flagged instance is deduped above, keeping the earlier reason.
        const ExportRootsSet& always = project.ExportRoots();
        for (const Guid& id : always.instances)
        {
            add(id, ExportRootReason::Flag);
        }
        for (const String& groupPath : always.groups)
        {
            Array<Guid> members;
            CollectGroupInstances(project.SourceDb(), groupPath.AsView(), members);
            for (const Guid& id : members)
            {
                add(id, ExportRootReason::Group);
            }
        }
        return roots;
    }

    Array<Guid> ExpandReachableRoots(EditorProject& project, const Array<ExportRoot>& seeds,
                                     const SceneReferenceScanner& scanner)
    {
        Array<Guid> out;
        HashMap<Guid, u8> seen;
        Array<Guid> queue;
        const auto push = [&](const Guid& id)
        {
            if (id.IsNil() || seen.Find(id) != nullptr)
            {
                return;
            }
            seen.InsertOrAssign(id, u8(1));
            out.PushBack(id);
            queue.PushBack(id);
        };
        for (const ExportRoot& r : seeds)
        {
            push(r.id);
        }

        usize head = 0;
        while (head < queue.Size())
        {
            const Guid id = queue[head++];
            draconic::content::Instance* inst = project.SourceDb().GetInstance(id);
            if (inst == nullptr)
            {
                continue;
            }
            const bool isSceneLike =
                inst->TypeName() == u8"SceneDocument" || inst->TypeName() == u8"PrefabDocument";
            if (!isSceneLike || !scanner)
            {
                continue;
            }

            SceneReferences refs;
            scanner(*inst, project.SourceDb(), refs);
            for (const Guid& g : refs.resources)
            {
                push(g);
            }
            for (const Guid& g : refs.prefabs)
            {
                push(g);
            }
        }
        return out;
    }

    Status CookReachable(EditorProject& project, BuilderRegistry& builders,
                         Span<const Guid> planRoots, bool cook, bool rebuild, ExportStats& stats,
                         Array<Guid>& outReachable, const ExportProgress& onProgress)
    {
        draconic::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView());
        draconic::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView());
        JobSystem jobs;
        CookDriver driver(project.SourceDb(), project.CookedDb(), builders, &sourcesMount,
                          &cacheMount, &jobs);
        CookPlan plan = driver.PlanFor(planRoots, rebuild);
        outReachable = plan.reachable;

        if (cook)
        {
            CookProgress cookProgress;
            cookProgress.onItem = [&onProgress](usize done, usize total, StringView path, bool)
            {
                if (!onProgress)
                {
                    return;
                }
                const f32 frac =
                    (total > 0) ? 0.05f + (static_cast<f32>(done) / static_cast<f32>(total)) * 0.55f
                                : 0.6f;
                String step(u8"Cooking ");
                step += path;
                onProgress(step.AsView(), frac);
            };
            const CookStats cookStats = driver.Execute(plan, &cookProgress);
            stats.cooked = cookStats.cooked;
            stats.cookFailed = cookStats.failed;
            if (cookStats.failed > 0)
            {
                DRACONIC_LOG_ERROR(u8"Export", u8"aborting - the cook has {} failure(s)",
                                   cookStats.failed);
                return Status{ErrorCode::Internal};
            }
        }
        return Status{};
    }

    String FormatPruningReport(const PruningReport& report)
    {
        String out(u8"Export reachability pruning report\n");
        out += u8"==================================\n";
        out += Format(u8"kept: {} instance(s) in the closure\n", report.keptCount);
        out += Format(u8"roots: {}\n", report.roots.Size());
        for (const ExportRoot& r : report.roots)
        {
            out += u8"  - ";
            out += r.name.IsEmpty() ? StringView(u8"(unresolved)") : r.name.AsView();
            out += u8"  [";
            out += ExportRootReasonName(r.reason);
            out += u8"]\n";
        }
        out += Format(u8"dropped: {} instance(s)\n", report.dropped.Size());
        for (const String& d : report.dropped)
        {
            out += u8"  - ";
            out += d.AsView();
            out += u8"\n";
        }
        return out;
    }

    Status ExportContent(EditorProject& project, StringView outDir, ExportStats& stats,
                         const ExportProgress& onProgress,
                         const HashMap<Guid, Array<byte>>* sceneStreams,
                         const HashMap<Guid, u8>* reachable, const Array<ExportRoot>* roots,
                         PruningReport* outReport)
    {

        // --- stage scenes ---
        if (onProgress)
        {
            onProgress(u8"Staging scenes...", 0.65f);
        }
        if (!CreateDirectory(outDir))
        {
            return Status{ErrorCode::NotSupported};
        }
        const String stagingDir = PathJoin(outDir, u8".stage-scenes");
        (void)CreateDirectory(stagingDir.AsView());
        draconic::vfs::NativeFileSystem stagingMount(stagingDir.AsView());
        Array<String> droppedScenes; // for the pruning report
        {
            draconic::content::ContentDatabase staging(stagingMount, BinarySerializerFactory(),
                                                       draconic::project::kCookedAssetExtension);
            Array<draconic::content::Instance*> scenes;
            detail::CollectScenes(*project.SourceDb().RootGroup(), scenes);
            usize staged = 0;
            for (draconic::content::Instance* scene : scenes)
            {
                if (reachable != nullptr && reachable->Find(scene->Id()) == nullptr)
                {
                    droppedScenes.PushBack(scene->Path()); // pruned: not reachable from any root
                    continue;
                }
                if (!detail::StageScene(*scene, staging, sceneStreams))
                {
                    DRACONIC_LOG_ERROR(u8"Export", u8"failed to stage scene '{}'", scene->Path());
                    return Status{ErrorCode::Internal};
                }
                ++staged;
            }
            stats.scenesStaged = staged;
        }

        // --- 3. pack ---
        // When pruning, resolve the reachable guids to their COOKED instance paths so the pack walk
        // can filter cooked files (a file belongs to instance P iff it begins "P.").
        UniquePtr<HashMap<String, u8>> reachablePaths;
        usize keptProducts = 0;
        Array<String> droppedProducts;
        if (reachable != nullptr)
        {
            reachablePaths = MakeUnique<HashMap<String, u8>>(DefaultAllocator());
            Array<draconic::content::Instance*> cooked;
            detail::CollectAllInstances(*project.CookedDb().RootGroup(), cooked);
            for (draconic::content::Instance* product : cooked)
            {
                if (reachable->Find(product->Id()) != nullptr)
                {
                    reachablePaths->InsertOrAssign(product->Path(), u8(1));
                    ++keptProducts;
                }
            }
            // Dropped = the authored (non-scene) source assets excluded from the dist. Computed from
            // the SOURCE db, not the cooked db: a scoped cook never PRODUCES the unreachable assets,
            // so they wouldn't appear cooked - but they're exactly what pruning left out, so the
            // report must name them (scenes are reported via droppedScenes above).
            Array<draconic::content::Instance*> sources;
            detail::CollectAllInstances(*project.SourceDb().RootGroup(), sources);
            for (draconic::content::Instance* src : sources)
            {
                const bool isSceneLike =
                    src->TypeName() == u8"SceneDocument" || src->TypeName() == u8"PrefabDocument";
                if (!isSceneLike && reachable->Find(src->Id()) == nullptr)
                {
                    droppedProducts.PushBack(src->Path());
                }
            }
        }

        if (onProgress)
        {
            onProgress(u8"Packing Content.pak...", 0.78f);
        }
        draconic::vfs::PakBuilder pak;
        draconic::vfs::NativeFileSystem cookedMount(
            PathJoin(project.Directory(), draconic::project::kProjectCookedDir).AsView());
        if (!detail::PackTree(cookedMount, *cookedMount.AsEnumerable(), u8"", pak,
                              stats.filesPacked, reachablePaths.Get()) ||
            !detail::PackTree(stagingMount, *stagingMount.AsEnumerable(), u8"", pak,
                              stats.filesPacked))
        {
            DRACONIC_LOG_ERROR(u8"Export", u8"packing failed");
            return Status{ErrorCode::Internal};
        }
        // The startup game script needs no special staging - it is a cooked ScriptClass asset in the
        // reachability closure, so it already rides in the content DB pak like every other asset. The
        // dist manifest carries its guid (below); the player binds it from the content DB.
        const String pakPath = PathJoin(outDir, draconic::project::kDistContentPak);
        if (!pak.Write(pakPath.AsView()).IsOk())
        {
            DRACONIC_LOG_ERROR(u8"Export", u8"failed to write Content.pak");
            return Status{ErrorCode::Internal};
        }

        // --- 4. dist manifest ---
        if (onProgress)
        {
            onProgress(u8"Writing manifest...", 0.9f);
        }
        {
            draconic::vfs::NativeFileSystem outMount(outDir);
            draconic::project::ProjectSettings dist;
            dist.name = String(project.Settings().name.AsView());
            dist.defaultSceneId = project.Settings().defaultSceneId;
            dist.defaultScene = String(project.Settings().defaultScene.AsView());
            dist.startupScriptId = project.Settings().startupScriptId;
            dist.startupScript =
                String(project.Settings().startupScript.AsView()); // display mirror
            // The manifest defaults the player binds at startup (previously dropped from the
            // dist manifest entirely - the theme/input-map/bus-layout bindings could never fire
            // in a shipped build).
            dist.defaultInputMapId = project.Settings().defaultInputMapId;
            dist.defaultBusLayoutId = project.Settings().defaultBusLayoutId;
            dist.defaultUiThemeId = project.Settings().defaultUiThemeId;
            dist.defaultUiFontId = project.Settings().defaultUiFontId;
            if (!draconic::project::SaveProjectSettings(*outMount.AsWritable(), dist,
                                                        draconic::project::kDistManifestFile)
                     .IsOk())
            {
                DRACONIC_LOG_ERROR(u8"Export", u8"failed to write the dist manifest");
                return Status{ErrorCode::Internal};
            }
        }

        // --- 5. pruning report (loud + auditable: pruning can silently break a shipped game) ---
        if (reachable != nullptr)
        {
            PruningReport report;
            report.pruned = true;
            if (roots != nullptr)
            {
                report.roots = *roots;
            }
            report.keptCount = stats.scenesStaged + keptProducts;
            for (const String& d : droppedScenes)
            {
                report.dropped.PushBack(String(d.AsView()));
            }
            for (const String& d : droppedProducts)
            {
                report.dropped.PushBack(String(d.AsView()));
            }

            const String text = FormatPruningReport(report);
            DRACONIC_LOG_INFO(u8"Export", u8"pruned dist: {} kept, {} dropped ({} root(s))",
                              report.keptCount, report.dropped.Size(), report.roots.Size());
            {
                draconic::vfs::NativeFileSystem outMount(outDir);
                (void)outMount.AsWritable()->Save(
                    u8"export-report.txt",
                    Span<const byte>(reinterpret_cast<const byte*>(text.CStr()), text.Size()));
            }
            if (outReport != nullptr)
            {
                *outReport = Move(report);
            }
        }

        detail::RemoveTreeRecursive(stagingDir.AsView());
        return Status{};
    }

    Status ExportProject(EditorProject& project, StringView outDir, BuilderRegistry& builders,
                         bool rebuild, ExportStats* outStats, const ExportProgress& onProgress,
                         const HashMap<Guid, Array<byte>>* sceneStreams)
    {
        ExportStats stats;

        // --- cook ---
        if (onProgress)
        {
            onProgress(u8"Cooking content...", 0.05f);
        }
        draconic::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView());
        draconic::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView());
        JobSystem jobs;
        CookDriver driver(project.SourceDb(), project.CookedDb(), builders, &sourcesMount,
                          &cacheMount, &jobs);
        CookPlan plan = driver.Plan(rebuild);
        CookProgress cookProgress;
        cookProgress.onItem = [&onProgress](usize done, usize total, StringView path, bool)
        {
            if (!onProgress)
            {
                return;
            }
            const f32 frac =
                (total > 0) ? 0.05f + (static_cast<f32>(done) / static_cast<f32>(total)) * 0.55f
                            : 0.6f; // cook occupies 0.05..0.60 of the export
            String step(u8"Cooking ");
            step += path;
            onProgress(step.AsView(), frac);
        };
        const CookStats cookStats = driver.Execute(plan, &cookProgress);
        stats.cooked = cookStats.cooked;
        stats.cookFailed = cookStats.failed;
        if (cookStats.failed > 0)
        {
            DRACONIC_LOG_ERROR(u8"Export", u8"aborting - the cook has {} failure(s)",
                               cookStats.failed);
            if (outStats != nullptr)
            {
                *outStats = stats;
            }
            return Status{ErrorCode::Internal};
        }

        // --- content ---
        const Status s = ExportContent(project, outDir, stats, onProgress, sceneStreams);
        if (outStats != nullptr)
        {
            *outStats = stats;
        }
        return s;
    }

    Status ExportOne(EditorProject& project, const ExportPreset& preset,
                     const TemplateRegistry& templates, BuilderRegistry& builders,
                     StringView outRoot, bool rebuild, ExportResult* outResult,
                     const ExportProgress& onProgress, bool cook,
                     const HashMap<Guid, Array<byte>>* sceneStreams,
                     const SceneReferenceScanner* scanner,
                     const Array<Guid>* precomputedReachableRoots)
    {
        const ExportTemplate* tmpl = templates.Resolve(preset);
        if (tmpl == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"Export",
                               u8"no export template for preset '{}' (platform '{}') - import one",
                               preset.name, preset.platform);
            return Status{ErrorCode::NotFound};
        }

        ExportResult result;

        // Soft engine-version match (mirrors EditorProject::Open's project-manifest check): a template
        // built against a different engine version may be binary-incompatible with the cooked content,
        // but we don't know that it is - so warn and keep going rather than block. Empty version = an
        // older/hand-written template with no stamp; skip.
        if (!tmpl->engineVersion.IsEmpty() &&
            tmpl->engineVersion != draconic::project::kEngineVersionString)
        {
            DRACONIC_LOG_WARNING(u8"Export",
                                 u8"template '{}' was built against engine {} but this build is {} "
                                 u8"- exporting anyway",
                                 tmpl->id, tmpl->engineVersion,
                                 draconic::project::kEngineVersionString);
            result.engineVersionWarning = String(u8"Template '");
            result.engineVersionWarning += tmpl->id;
            result.engineVersionWarning += u8"' targets engine ";
            result.engineVersionWarning += tmpl->engineVersion;
            result.engineVersionWarning += u8" (this build is ";
            result.engineVersionWarning += draconic::project::kEngineVersionString;
            result.engineVersionWarning += u8").";
        }

        const String subdir = preset.outputSubdir.IsEmpty()
                                  ? detail::SanitizeName(preset.name.AsView())
                                  : String(preset.outputSubdir.AsView());
        result.outputDir = PathJoin(outRoot, subdir.AsView());

        // Ensure the output dir (and outRoot) exist before the content pipeline writes into it.
        (void)CreateDirectories(result.outputDir.AsView());

        // Closure pruning is opt-in per preset. It needs the scene->asset edges discovered - EITHER
        // by a live scene-reference scanner (the CLI, which loads scenes on its own single thread),
        // OR by a precomputed reachable-root set (the editor, which pre-scans on the MAIN thread and
        // passes the guids into this background job - scene loading is main-thread-only). Without
        // either we must NOT silently drop content, so fall back to pack-everything with a warning.
        const bool haveScanner = (scanner != nullptr && *scanner);
        const bool havePrecomputed = (precomputedReachableRoots != nullptr);
        bool prune = preset.pruneToReachable;
        if (prune && !haveScanner && !havePrecomputed)
        {
            DRACONIC_LOG_WARNING(
                u8"Export",
                u8"preset '{}' requests pruning but no scene-reference scanner or precomputed root "
                u8"set was supplied - exporting everything",
                preset.name);
            prune = false;
        }

        Status contentStatus;
        if (prune)
        {
            // Seed roots -> expand scene-graph edges -> PlanFor closure -> cook + stage/pack only it.
            // The scene-edge expansion is either precomputed (editor main-thread pre-scan) or run
            // inline via the scanner (CLI). `seeds` is still recomputed here for the report (metadata
            // only, background-safe); it drives display, while planRoots drives cook/pack.
            const Array<ExportRoot> seeds = CollectExportRoots(project);
            const Array<Guid> planRoots = havePrecomputed
                                              ? *precomputedReachableRoots
                                              : ExpandReachableRoots(project, seeds, *scanner);
            Array<Guid> reachableList;
            contentStatus = CookReachable(project, builders,
                                          Span<const Guid>(planRoots.Data(), planRoots.Size()),
                                          cook, rebuild, result.content, reachableList, onProgress);
            if (contentStatus.IsOk())
            {
                HashMap<Guid, u8> reachable;
                for (const Guid& g : reachableList)
                {
                    reachable.InsertOrAssign(g, u8(1));
                }
                contentStatus =
                    ExportContent(project, result.outputDir.AsView(), result.content, onProgress,
                                  sceneStreams, &reachable, &seeds, &result.pruning);
            }
        }
        else
        {
            contentStatus = cook ? ExportProject(project, result.outputDir.AsView(), builders,
                                                 rebuild, &result.content, onProgress)
                                 : ExportContent(project, result.outputDir.AsView(), result.content,
                                                 onProgress, sceneStreams);
        }
        if (!contentStatus.IsOk())
        {
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::Internal};
        }

        if (onProgress)
        {
            onProgress(u8"Staging player...", 0.93f);
        }
        // Player: <template dir>/<playerBinary> -> <outDir>/<preset.playerName | template.playerBinary>.
        const String outName = preset.playerName.IsEmpty() ? String(tmpl->playerBinary.AsView())
                                                           : String(preset.playerName.AsView());
        if (!detail::CopyFilePreserving(tmpl->directory.AsView(), tmpl->playerBinary.AsView(),
                                        result.outputDir.AsView(), outName.AsView()))
        {
            DRACONIC_LOG_ERROR(u8"Export", u8"failed to stage player '{}' from template '{}'",
                               tmpl->playerBinary, tmpl->id);
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::Internal};
        }
        ++result.filesStaged;

        // Cooked engine shaders: produce shaders.dpak beside the player so the dist renders with no
        // runtime compiler. A cook failure is fatal - a dist without shaders cannot render.
        if (onProgress)
        {
            onProgress(u8"Cooking shaders...", 0.95f);
        }
        {
            u32 shaderVariants = 0;
            const Status packStatus =
                StageShaderPack(result.outputDir.AsView(), preset.platform.AsView(), shaderVariants);
            if (!packStatus.IsOk())
            {
                if (outResult != nullptr)
                {
                    *outResult = result;
                }
                return Status{ErrorCode::Internal};
            }
            ++result.filesStaged;
            DRACONIC_LOG_INFO(u8"Export", u8"staged shaders.dpak ({} variants)", shaderVariants);
        }

        if (onProgress && !tmpl->sidecars.IsEmpty())
        {
            onProgress(u8"Staging runtime libs...", 0.96f);
        }
        // Template sidecars (runtime libs) from the template dir. The cooked shader pack retires the
        // runtime DXC compiler, so its libs are dropped here - the dist ships no dxcompiler/dxil
        // (retires the DXC-runtime-sidecar fragility class for dists).
        for (const String& sidecar : tmpl->sidecars)
        {
            if (IsDxcRuntimeLib(sidecar.AsView()))
            {
                DRACONIC_LOG_INFO(u8"Export", u8"omitting DXC sidecar '{}' (dist renders from the "
                                              u8"cooked shader pack)",
                                  sidecar);
                continue;
            }
            if (detail::CopyFilePreserving(tmpl->directory.AsView(), sidecar.AsView(),
                                           result.outputDir.AsView(), sidecar.AsView()))
            {
                ++result.filesStaged;
            }
            else
            {
                DRACONIC_LOG_WARNING(u8"Export", u8"sidecar '{}' not found in template '{}'",
                                     sidecar, tmpl->id);
            }
        }

        // Symbols (PDB/DWARF) are stripped from the dist by default; stage them only when the preset
        // opts in (export-templates.md symbols policy). sidecars[] always stage; symbols[] gate here.
        if (preset.stageSymbols)
        {
            for (const String& symbol : tmpl->symbols)
            {
                if (detail::CopyFilePreserving(tmpl->directory.AsView(), symbol.AsView(),
                                               result.outputDir.AsView(), symbol.AsView()))
                {
                    ++result.filesStaged;
                }
                else
                {
                    DRACONIC_LOG_WARNING(u8"Export",
                                         u8"symbol file '{}' not found in template '{}'", symbol,
                                         tmpl->id);
                }
            }
        }

        // Preset additionalFiles (game extras, project-relative) -> <outDir>/<basename>.
        for (const String& extra : preset.additionalFiles)
        {
            const StringView base = detail::BaseName(extra.AsView());
            if (detail::CopyFilePreserving(project.Directory(), extra.AsView(),
                                           result.outputDir.AsView(), base))
            {
                ++result.filesStaged;
            }
            else
            {
                DRACONIC_LOG_WARNING(u8"Export", u8"additional file '{}' not found", extra);
            }
        }

        if (onProgress)
        {
            onProgress(u8"Done", 1.0f);
        }
        if (outResult != nullptr)
        {
            *outResult = result;
        }
        return Status{};
    }

    Status ExportAll(EditorProject& project, Span<const ExportPreset> presets,
                     const TemplateRegistry& templates, BuilderRegistry& builders,
                     StringView outRoot, bool rebuild, const ExportProgress& onProgress, bool cook,
                     const HashMap<Guid, Array<byte>>* sceneStreams,
                     const SceneReferenceScanner* scanner,
                     const Array<Guid>* precomputedReachableRoots)
    {
        // The reachable-root closure is project-level (not per-preset), so one precomputed set
        // seeds every pruning preset in the run.
        usize ok = 0;
        const usize n = presets.Size();
        for (usize i = 0; i < n; ++i)
        {
            const ExportPreset& preset = presets[i];
            // Scale each preset's 0..1 into its slice (i..i+1)/n and prefix its name.
            const ExportProgress scoped = [&onProgress, i, n, &preset](StringView step, f32 frac)
            {
                if (!onProgress)
                {
                    return;
                }
                String s(preset.name.AsView());
                s += u8": ";
                s += step;
                onProgress(s.AsView(), (static_cast<f32>(i) + frac) / static_cast<f32>(n));
            };
            ExportResult result;
            if (ExportOne(project, preset, templates, builders, outRoot, rebuild, &result, scoped,
                          cook, sceneStreams, scanner, precomputedReachableRoots)
                    .IsOk())
            {
                ++ok;
                DRACONIC_LOG_INFO(u8"Export", u8"exported '{}' -> {} ({} files staged)",
                                  preset.name, result.outputDir, result.filesStaged);
            }
            else
            {
                DRACONIC_LOG_ERROR(u8"Export", u8"preset '{}' failed", preset.name);
            }
        }
        return (ok == n) ? Status{} : Status{ErrorCode::Internal};
    }
}
