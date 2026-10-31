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
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

module editor.project;

import foundation.core;
import foundation.vfs;
import foundation.vfs.pak;
import foundation.content;
import engine.project;
import foundation.scene.resource;
import pipeline.core;
import pipeline.cook;
import foundation.shaders;
import foundation.shaders.system; // kShaderFolder / kShaderPackPath (the dist data layout)
import foundation.shaders.resource; // ShaderSource: a project's custom shader, cooked into the pack
import :project;
import :export_preset;
import :export_roots;
import :export_template;

using namespace foundation::core;
using namespace foundation;
using namespace pipeline;
namespace shaders = foundation::shaders;
namespace vfs = foundation::vfs;

namespace editor
{
    REFLECT_VALUE(ExportPreset, "rtti::editor::editor")
    {
        using engine::project::kSettingLabelAttribute;
        const auto label = [&builder](StringView text) { builder.PropAttribute(kSettingLabelAttribute, String(text)); };
        const Float4 renderSize{0.0f, 16384.0f, 1.0f, 0.0f};
        const Float4 windowSize{1.0f, 16384.0f, 1.0f, 0.0f};
        RegisterCoreTypes(); // FitMode
        RegisterArrayType<String>(); // additionalFiles
        builder.Property<&ExportPreset::name>("name");
        label(u8"Name");
        builder.Property<&ExportPreset::platform>("platform");
        label(u8"Platform");
        builder.Property<&ExportPreset::config>("config");
        label(u8"Config");
        builder.Property<&ExportPreset::templateId>("templateId");
        label(u8"Template");
        builder.Property<&ExportPreset::playerName>("playerName");
        label(u8"Player name");
        builder.Property<&ExportPreset::outputSubdir>("outputSubdir");
        label(u8"Output subdir");
        builder.Property<&ExportPreset::additionalFiles>("additionalFiles");
        label(u8"Extra files");
        builder.Property<&ExportPreset::stageSymbols>("stageSymbols");
        label(u8"Stage debug symbols");
        builder.Property<&ExportPreset::pruneToReachable>("pruneToReachable");
        label(u8"Prune to reachable content");
        builder.Property<&ExportPreset::includeShaderSource>("includeShaderSource");
        label(u8"Include shader source");
        builder.Property<&ExportPreset::overridesRender>("overridesRender");
        label(u8"Own render size");
        builder.Property<&ExportPreset::renderWidth>("renderWidth");
        label(u8"Render width");
        builder.PropAttribute("range", renderSize);
        builder.Property<&ExportPreset::renderHeight>("renderHeight");
        label(u8"Render height");
        builder.PropAttribute("range", renderSize);
        builder.Property<&ExportPreset::renderFit>("renderFit");
        label(u8"Render fit");
        builder.Property<&ExportPreset::overridesWindow>("overridesWindow");
        label(u8"Own window");
        builder.Property<&ExportPreset::windowWidth>("windowWidth");
        label(u8"Window width");
        builder.PropAttribute("range", windowSize);
        builder.Property<&ExportPreset::windowHeight>("windowHeight");
        label(u8"Window height");
        builder.PropAttribute("range", windowSize);
        builder.Property<&ExportPreset::windowMode>("windowMode");
        label(u8"Window mode");
        builder.Property<&ExportPreset::windowResizable>("windowResizable");
        label(u8"Window resizable");
        builder.Property<&ExportPreset::overridesOrientation>("overridesOrientation");
        label(u8"Own orientation");
        builder.Property<&ExportPreset::orientation>("orientation");
        label(u8"Orientation");
    }

    void RegisterExportPresetReflection()
    {
        (void)engine::project::ProjectSettings::StaticType(); // WindowMode's enumerators
        RttiRegisterValue_ExportPreset();
    }

    namespace
    {
        // The cooked-blob formats a target platform's runtime needs: WGSL for the web, SPIR-V (Vulkan)
        // and DXIL (DX12) for Windows, SPIR-V (Vulkan) for other desktops. Kept generous rather than
        // guessing the backend a desktop dist will pick at runtime.
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

        // The DXC runtime libraries (dxcompiler, dxil), which a cooked dist does not need, since the
        // shader pack replaces the runtime compiler. Matched by substring on the sidecar's file name.
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

        // Cook the built-in shaders (from <dataRoot>/Shaders) and the project's custom shaders the
        // dist ships (`projectShaders`, their cooked ShaderSource products) for `platform`, and
        // write the dist's Data/Engine/Shaders/shaders.dpak plus its Data/.dataroot marker, the layout the
        // player's data-root discovery finds beside the executable. The dist renders from this pack
        // with no runtime compiler (see the ShaderSystem cooked path). Returns the variant count via
        // `outVariants`; Status carries any cook or compile failure.
        Status StageShaderPack(StringView outputDir, StringView dataRoot, StringView platform,
                               EditorProject& project, Span<const Guid> projectShaders,
                               u32& outVariants)
        {
            outVariants = 0;
            const String shaderDir = vfs::DataPath(dataRoot, shaders::kShaderFolder);
            if (!DirectoryExists(shaderDir.AsView()))
            {
                LOG_ERROR(u8"Export", u8"cannot cook shaders: no '{}' under the data root '{}'",
                          shaders::kShaderFolder, dataRoot);
                return Status{ErrorCode::NotFound};
            }
            shaders::Compiler* compiler = nullptr;
            if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() ||
                compiler == nullptr)
            {
                LOG_ERROR(u8"Export",
                                   u8"cannot cook shaders: the DXC compiler is unavailable");
                return Status{ErrorCode::Internal};
            }

            const Array<shaders::CookedShaderFormat> formats = FormatsForPlatform(platform);
            shaders::ShaderCookOptions opts;
            opts.shaderDir = shaderDir.AsView();
            opts.scratchDir = outputDir; // WGSL intermediates (deleted); unused for SPIR-V/DXIL
            opts.formats = Span<const shaders::CookedShaderFormat>(formats.Data(), formats.Size());

            shaders::CookedShaderPack pack;
            shaders::ShaderCookReport report = shaders::CookEngineShaders(*compiler, opts, pack);

            // The project's shaders: each program's stages under their entry points and lattices
            // (a surface shader's depth and pick beside its main one), every variant,
            // with the engine library on the include path (what their cook compiled against).
            const StringView includePaths[] = {shaderDir.AsView()};
            shaders::WgslTranslator translator(*compiler, outputDir);
            translator.SetValidateWithTint(opts.validateWgsl);
            for (const Guid& id : projectShaders)
            {
                foundation::content::Instance* product = project.CookedDb().GetInstance(id);
                RefPtr<ISerializable> object =
                    product != nullptr ? product->ReadObject() : RefPtr<ISerializable>{};
                const auto* source = Cast<shaders::ShaderSource>(object.Get());
                if (source == nullptr)
                {
                    report.errors.PushBack(String(u8"a custom shader's cooked product is missing"));
                    continue;
                }
                const String sourceName = Format(u8"{}.hlsl", source->name);
                for (const shaders::ShaderProgram& program : source->Programs())
                {
                    (void)shaders::CookShaderStage(
                        *compiler, translator, program.name.AsView(), shaders::ShaderStage::Vertex,
                        program.vertexSource, program.vertexEntryPoint, program.vertexVariantMask,
                        sourceName.AsView(), opts, Span<const StringView>(includePaths, 1), pack,
                        report);
                    (void)shaders::CookShaderStage(
                        *compiler, translator, program.name.AsView(),
                        shaders::ShaderStage::Fragment, program.fragmentSource,
                        program.fragmentEntryPoint, program.fragmentVariantMask,
                        sourceName.AsView(), opts, Span<const StringView>(includePaths, 1), pack,
                        report);
                }
            }
            report.success = report.errors.IsEmpty();
            compiler->Destroy();

            for (usize i = 0; i < report.errors.Size(); ++i)
            {
                LOG_ERROR(u8"Export", u8"shader cook: {}", report.errors[i]);
            }
            if (!report.success)
            {
                return Status{ErrorCode::Internal};
            }

            const String distData = PathJoin(outputDir, u8"Data");
            const String packPath = PathJoin(distData.AsView(), shaders::kShaderPackPath);
            if (!CreateDirectories(PathParent(packPath.AsView())))
            {
                LOG_ERROR(u8"Export", u8"could not create '{}'", PathParent(packPath.AsView()));
                return Status{ErrorCode::Internal};
            }
            FileStream out(packPath.AsView(), FileMode::Write);
            if (!out.IsValid() || !pack.Write(out).IsOk())
            {
                LOG_ERROR(u8"Export", u8"could not write '{}'", packPath);
                return Status{ErrorCode::Internal};
            }
            // The marker that makes <dist>/Data a data root for the player's discovery walk.
            constexpr StringView kMarkerText = u8"# Data root marker (foundation.vfs FindDataRoot). "
                                               u8"Staged by the export.\nversion: 1\n";
            const String marker = PathJoin(distData.AsView(), vfs::kDataRootMarker);
            if (!WriteFile(marker.AsView(),
                           Span<const byte>(reinterpret_cast<const byte*>(kMarkerText.Data()),
                                            kMarkerText.Size()))
                     .IsOk())
            {
                LOG_ERROR(u8"Export", u8"could not write '{}'", marker);
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
            if (foundation::content::Instance* inst = project.SourceDb().GetInstance(id))
            {
                root.name = inst->Path();
            }
            roots.PushBack(Move(root));
        };

        const engine::project::ProjectSettings& settings = project.Settings();

        add(settings.defaultSceneId, ExportRootReason::DefaultScene);

        // The startup game script is a cooked ScriptClass asset, identified by guid. Seed it as a
        // reachability root so it, and the assets it loads through the normal AssetRef contract, ship.
        add(settings.startupScriptId, ExportRootReason::StartupScript);

        // Every other asset the player binds at startup must ship, or the binding fails silently
        // in a pruned dist: each reflected asset setting, lists included (the other UI fonts). The
        // scene and the script above keep their own reasons (add keeps the first).
        engine::project::ForEachSettingAsset(
            settings, [&add](const PropertyInfo&, const Guid& id) { add(id, ExportRootReason::ManifestDefault); });

        // "Always Export": explicit instance flags, then group subtrees (membership is dynamic:
        // whatever is under the flagged folder now). A group that also contains the default scene
        // or a directly flagged instance is deduplicated by add, keeping the earlier reason.
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
            foundation::content::Instance* inst = project.SourceDb().GetInstance(id);
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
        foundation::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        foundation::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView(), editor::EditorRootAllocator());
        JobSystem jobs(editor::EditorRootAllocator()); // export tool root
        CookDriver driver(editor::EditorRootAllocator(), project.SourceDb(), project.CookedDb(),
                          builders, &sourcesMount, &cacheMount, &jobs);
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
                LOG_ERROR(u8"Export", u8"aborting - the cook has {} failure(s)",
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

    // Whether this is a web or wasm target, the only kind that splits into BC and ASTC variants.
    static bool IsWebPlatform(StringView platform)
    {
        return platform.StartsWith(u8"Web") || platform.StartsWith(u8"Wasm");
    }

    Array<ContentVariant> VariantsForPlatform(EditorProject& project, StringView platform)
    {
        Array<ContentVariant> out;
        if (IsWebPlatform(platform))
        {
            // Two complete paks, one per compressed texture family the browser might support. The
            // cooked directories are siblings of the host Cooked/, matching Tools.Cook --target.
            const StringView keys[] = {StringView(u8"bc"), StringView(u8"astc")};
            for (const StringView key : keys)
            {
                ContentVariant v;
                v.key = String(key);
                v.pakName = String(u8"Content-");
                v.pakName.Append(key);
                v.pakName.Append(u8".pak");
                v.cookedDir = String(project.Directory());
                v.cookedDir.Append(u8"/Cooked-web-");
                v.cookedDir.Append(key); // Cooked-web-bc / Cooked-web-astc
                out.PushBack(Move(v));
            }
        }
        else
        {
            // Desktop: the single host pak from Cooked/.
            ContentVariant v;
            v.pakName = String(engine::project::kDistContentPak);
            v.cookedDir = PathJoin(project.Directory(), engine::project::kProjectCookedDir);
            out.PushBack(Move(v));
        }
        return out;
    }

    Status CookVariantTargets(EditorProject& project, BuilderRegistry& builders,
                              Span<const ContentVariant> variants, bool rebuild,
                              const ExportProgress& onProgress)
    {
        // The host cook already ran (ExportProject or CookReachable) and saved its records to
        // .cache/cook.db; load them once to gate the copy-forward for every target.
        foundation::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView(), editor::EditorRootAllocator());
        CookDb hostRecords{editor::EditorRootAllocator()};
        hostRecords.Load(cacheMount);

        foundation::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        JobSystem jobs(editor::EditorRootAllocator()); // export tool root
        usize failed = 0;
        for (const ContentVariant& v : variants)
        {
            if (v.key.IsEmpty())
            {
                continue; // desktop: the host DB already is this variant
            }
            if (onProgress)
            {
                String step(u8"Cooking variant ");
                step += v.key.AsView();
                onProgress(step.AsView(), 0.6f);
            }
            // Target id "web-<key>" resolves to the capability profile (web-astc gives ASTC, else BC).
            String targetId(u8"web-");
            targetId += v.key.AsView();

            // Create Cooked-web-<key>/ and .cache/web-<key>/, carrying invariant products forward
            // from the host.
            const String cacheDir = PathJoin(
                PathJoin(project.Directory(), engine::project::kProjectCacheDir), targetId.AsView());
            (void)CreateDirectory(v.cookedDir.AsView());
            (void)CreateDirectory(PathJoin(project.Directory(),
                                           engine::project::kProjectCacheDir).AsView());
            (void)CreateDirectory(cacheDir.AsView());

            foundation::vfs::NativeFileSystem targetCookedMount(v.cookedDir.AsView(), editor::EditorRootAllocator());
            foundation::vfs::NativeFileSystem targetCacheMount(cacheDir.AsView(), editor::EditorRootAllocator());
            foundation::content::ContentDatabase targetDb(editor::EditorRootAllocator(), targetCookedMount, BinarySerializerFactory(),
                                                        engine::project::kCookedAssetExtension);

            const CookStats s = CookForTarget(editor::EditorRootAllocator(), project.SourceDb(),
                                              targetDb, project.CookedDb(),
                                              hostRecords, builders, &sourcesMount, &targetCacheMount,
                                              CookTargetFor(targetId.AsView()), &jobs, rebuild);
            failed += s.failed;
            LOG_INFO(u8"Export", u8"variant '{}' cooked {}, copied-forward {}, failed {}", v.key,
                     s.cooked, s.copiedForward, s.failed);
        }
        return (failed == 0) ? Status{} : Status{ErrorCode::Internal};
    }

    Status ExportContent(EditorProject& project, StringView outDir, ExportStats& stats,
                         const ExportProgress& onProgress,
                         const HashMap<Guid, Array<byte>>* sceneStreams,
                         const HashMap<Guid, u8>* reachable, const Array<ExportRoot>* roots,
                         PruningReport* outReport, Span<const ContentVariant> variants,
                         bool includeShaderSource)
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
        foundation::vfs::NativeFileSystem stagingMount(stagingDir.AsView(), editor::EditorRootAllocator());
        Array<String> droppedScenes; // for the pruning report
        HashMap<String, u8> strippedShaders; // cooked products replaced by a staged, stripped copy
        {
            foundation::content::ContentDatabase staging(editor::EditorRootAllocator(), stagingMount, BinarySerializerFactory(),
                                                       engine::project::kCookedAssetExtension);
            Array<foundation::content::Instance*> scenes;
            detail::CollectScenes(*project.SourceDb().RootGroup(), scenes);
            usize staged = 0;
            for (foundation::content::Instance* scene : scenes)
            {
                if (reachable != nullptr && reachable->Find(scene->Id()) == nullptr)
                {
                    droppedScenes.PushBack(scene->Path()); // pruned: not reachable from any root
                    continue;
                }
                if (!detail::StageScene(*scene, staging, sceneStreams))
                {
                    LOG_ERROR(u8"Export", u8"failed to stage scene '{}'", scene->Path());
                    return Status{ErrorCode::Internal};
                }
                ++staged;
            }
            stats.scenesStaged = staged;

            // The project's custom shaders the dist ships: recorded for the shader pack step,
            // which cooks them to bytecode; staged without their HLSL unless the preset keeps it
            // (the project's own Cooked/ keeps its source for the editor, so the copy is staged).
            const StringView shaderType(reinterpret_cast<const utf8char*>(
                foundation::shaders::ShaderSource::StaticType().name));
            Array<foundation::content::Instance*> products;
            detail::CollectAllInstances(*project.CookedDb().RootGroup(), products);
            for (foundation::content::Instance* product : products)
            {
                if (product->TypeName() != shaderType ||
                    (reachable != nullptr && reachable->Find(product->Id()) == nullptr))
                {
                    continue;
                }
                stats.shaders.PushBack(product->Id());
                if (includeShaderSource)
                {
                    continue;
                }
                if (!detail::StageStrippedShader(*product, staging))
                {
                    LOG_ERROR(u8"Export", u8"failed to stage shader '{}'", product->Path());
                    return Status{ErrorCode::Internal};
                }
                strippedShaders.InsertOrAssign(product->Path(), u8(1));
            }
        }

        // --- 3. pack ---
        // When pruning, resolve the reachable guids to their cooked instance paths so the pack walk
        // can filter cooked files (a file belongs to instance P exactly when it begins with "P.").
        UniquePtr<HashMap<String, u8>> reachablePaths;
        usize keptProducts = 0;
        Array<String> droppedProducts;
        if (reachable != nullptr)
        {
            reachablePaths = MakeUnique<HashMap<String, u8>>(editor::EditorRootAllocator());
            Array<foundation::content::Instance*> cooked;
            detail::CollectAllInstances(*project.CookedDb().RootGroup(), cooked);
            for (foundation::content::Instance* product : cooked)
            {
                if (reachable->Find(product->Id()) != nullptr)
                {
                    reachablePaths->InsertOrAssign(product->Path(), u8(1));
                    ++keptProducts;
                }
            }
            // Dropped means the authored (non-scene) source assets excluded from the dist. It is
            // computed from the source DB, not the cooked DB: a scoped cook never produces the
            // unreachable assets, so they would not appear cooked, but they are exactly what pruning
            // left out, so the report must name them (scenes are reported through droppedScenes above).
            Array<foundation::content::Instance*> sources;
            detail::CollectAllInstances(*project.SourceDb().RootGroup(), sources);
            for (foundation::content::Instance* src : sources)
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
            onProgress(u8"Packing content...", 0.78f);
        }
        // The variants to pack: the caller's list, or by default the single host Content.pak. Each
        // variant is a complete pak: its own cooked DB with the same staged scenes and the same
        // reachable set (the closure's guid set does not vary by variant).
        Array<ContentVariant> effectiveVariants;
        if (variants.IsEmpty())
        {
            ContentVariant desktop;
            desktop.pakName = String(engine::project::kDistContentPak);
            desktop.cookedDir = PathJoin(project.Directory(), engine::project::kProjectCookedDir);
            effectiveVariants.PushBack(Move(desktop));
        }
        else
        {
            for (const ContentVariant& v : variants)
            {
                ContentVariant copy;
                copy.key = String(v.key.AsView());
                copy.pakName = String(v.pakName.AsView());
                copy.cookedDir = String(v.cookedDir.AsView());
                effectiveVariants.PushBack(Move(copy));
            }
        }
        // The startup game script needs no special staging: it is a cooked ScriptClass asset in the
        // reachability closure, so it is already in the content DB pak like every other asset. The
        // dist manifest carries its guid (below); the player binds it from the content DB.
        for (const ContentVariant& v : effectiveVariants)
        {
            foundation::vfs::PakBuilder pak;
            foundation::vfs::NativeFileSystem cookedMount(v.cookedDir.AsView(), editor::EditorRootAllocator());
            if (!detail::PackTree(cookedMount, *cookedMount.AsEnumerable(), u8"", pak,
                                  stats.filesPacked, reachablePaths.Get(), &strippedShaders) ||
                !detail::PackTree(stagingMount, *stagingMount.AsEnumerable(), u8"", pak,
                                  stats.filesPacked))
            {
                LOG_ERROR(u8"Export", u8"packing failed for variant '{}'", v.pakName);
                return Status{ErrorCode::Internal};
            }
            const String pakPath = PathJoin(outDir, v.pakName.AsView());
            if (!pak.Write(pakPath.AsView()).IsOk())
            {
                LOG_ERROR(u8"Export", u8"failed to write '{}'", v.pakName);
                return Status{ErrorCode::Internal};
            }
        }

        // --- 4. dist manifest ---
        if (onProgress)
        {
            onProgress(u8"Writing manifest...", 0.9f);
        }
        {
            foundation::vfs::NativeFileSystem outMount(outDir, editor::EditorRootAllocator());
            // The player reads the same manifest the project writes, so every default it binds at
            // startup (theme, input map, bus layout, font, loading screen, MSAA) comes along.
            engine::project::ProjectSettings dist;
            if (!engine::project::CopyProjectSettings(project.Settings(), dist).IsOk())
            {
                LOG_ERROR(u8"Export", u8"failed to copy the project manifest for the dist");
                return Status{ErrorCode::Internal};
            }
            if (!engine::project::SaveProjectSettings(*outMount.AsWritable(), dist,
                                                        engine::project::kDistManifestFile)
                     .IsOk())
            {
                LOG_ERROR(u8"Export", u8"failed to write the dist manifest");
                return Status{ErrorCode::Internal};
            }
        }

        // --- 5. pruning report (loud and auditable, since pruning can silently break an exported game) ---
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
            LOG_INFO(u8"Export", u8"pruned dist: {} kept, {} dropped ({} root(s))",
                              report.keptCount, report.dropped.Size(), report.roots.Size());
            {
                foundation::vfs::NativeFileSystem outMount(outDir, editor::EditorRootAllocator());
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
                         const HashMap<Guid, Array<byte>>* sceneStreams,
                         Span<const ContentVariant> variants, bool includeShaderSource)
    {
        ExportStats stats;

        // --- cook ---
        if (onProgress)
        {
            onProgress(u8"Cooking content...", 0.05f);
        }
        foundation::vfs::NativeFileSystem sourcesMount(project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        foundation::vfs::NativeFileSystem cacheMount(project.CacheRoot().AsView(), editor::EditorRootAllocator());
        JobSystem jobs(editor::EditorRootAllocator()); // export tool root
        CookDriver driver(editor::EditorRootAllocator(), project.SourceDb(), project.CookedDb(),
                          builders, &sourcesMount, &cacheMount, &jobs);
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
                            : 0.6f; // the cook takes 0.05..0.60 of the export's progress
            String step(u8"Cooking ");
            step += path;
            onProgress(step.AsView(), frac);
        };
        const CookStats cookStats = driver.Execute(plan, &cookProgress);
        stats.cooked = cookStats.cooked;
        stats.cookFailed = cookStats.failed;
        if (cookStats.failed > 0)
        {
            LOG_ERROR(u8"Export", u8"aborting - the cook has {} failure(s)",
                               cookStats.failed);
            if (outStats != nullptr)
            {
                *outStats = stats;
            }
            return Status{ErrorCode::Internal};
        }

        // --- variant target cooks (web: BC and ASTC), after the host cook and before the pack ---
        if (!variants.IsEmpty())
        {
            const Status vs = CookVariantTargets(project, builders, variants, rebuild, onProgress);
            if (!vs.IsOk())
            {
                LOG_ERROR(u8"Export", u8"aborting - a variant cook failed");
                if (outStats != nullptr)
                {
                    *outStats = stats;
                }
                return Status{ErrorCode::Internal};
            }
        }

        // --- content ---
        const Status s =
            ExportContent(project, outDir, stats, onProgress, sceneStreams, nullptr, nullptr,
                          nullptr, variants, includeShaderSource);
        if (outStats != nullptr)
        {
            *outStats = stats;
        }
        return s;
    }

    namespace
    {
#if PLATFORM_WINDOWS
        // Windows hosts both toolchains, and only one of them needs the developer environment set
        // in this process. cl.exe finds its headers and libraries only through the vcvars
        // variables (INCLUDE, LIB, VCToolsInstallDir); clang and clang-cl locate an MSVC
        // installation themselves (registry and Program Files probing), so demanding vcvars from a
        // clang-built editor would block a setup that works. The pinned compiler is the one that
        // built this editor, so this asks about that exact binary: its basename minus .exe is
        // "cl", which does not match "clang-cl" or "clang++".
        [[nodiscard]] bool ShipCompilerIsMsvcCl()
        {
            StringView path =
                StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CXX_COMPILER));
            for (usize i = path.Size(); i > 0; --i)
            {
                if (path.Data()[i - 1] == '/' || path.Data()[i - 1] == '\\')
                {
                    path = StringView(path.Data() + i, path.Size() - i);
                    break;
                }
            }
            if (path.Size() >= 4)
            {
                const StringView tail(path.Data() + path.Size() - 4, 4);
                if (tail == u8".exe" || tail == u8".EXE")
                {
                    path = StringView(path.Data(), path.Size() - 4);
                }
            }
            return path.Size() == 2 && (path.Data()[0] == 'c' || path.Data()[0] == 'C') &&
                   (path.Data()[1] == 'l' || path.Data()[1] == 'L');
        }
#endif // PLATFORM_WINDOWS (both callers are Windows-only; unused elsewhere it fails -Werror)

        // Builds Engine.GamePlayer for a native project: re-invokes cmake over the engine checkout
        // with the game's native directory wired in (ENGINE_GAME_NATIVE_DIR and _TARGET), a
        // persistent per-project build directory under the project's .cache (for incremental
        // relinks), and a per-project Bin suffix so ship outputs never collide with development
        // builds. Returns the built player's path. `web` builds the wasm ship player by the same
        // mechanism through the Emscripten toolchain ($EMSDK must be in the environment, so launch
        // the editor or CLI from a shell with emsdk_env.sh sourced), with a lower -j (wasm-ld with
        // ASYNCIFY uses a lot of memory); the output is the html, js and wasm trio under the
        // template basename.
        [[nodiscard]] Status BuildShipPlayer(EditorProject& project, StringView config, bool web,
                                             const ExportProgress& onProgress, String& outPlayer)
        {
            const StringView engineRoot =
                StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_ENGINE_ROOT));
            const StringView cmakePath =
                StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CMAKE_PATH));
            String toolchainDef;
            if (web)
            {
                const Optional<String> emsdk = GetEnvironmentVariable(u8"EMSDK");
                if (!emsdk.HasValue() || emsdk.Value().IsEmpty())
                {
                    LOG_ERROR(u8"Export",
                              u8"web native export needs the Emscripten SDK: EMSDK is not set - "
                              u8"launch the editor/CLI from a shell with emsdk_env.sh sourced");
                    return Status{ErrorCode::NotFound};
                }
                toolchainDef = Format(
                    u8"-DCMAKE_TOOLCHAIN_FILE={}/upstream/emscripten/cmake/Modules/Platform/"
                    u8"Emscripten.cmake",
                    emsdk.Value());
            }
#if PLATFORM_WINDOWS
            // When the pinned toolchain is MSVC cl.exe, the desktop ship build requires the Visual
            // Studio developer environment in this process, and it is required explicitly rather
            // than synthesized. RunProcess takes an executable path with no shell, so the
            // alternative would be discovering vcvarsall.bat and running the whole configure and
            // build through `cmd.exe /c "vcvarsall && cmake ..."`, which means guessing the VS
            // install, adding a second quoting layer around every argument, and getting an exit
            // code that can come from either command. Requiring a Developer Command Prompt is the
            // same contract the web build already has with EMSDK, and it fails here with one clear
            // message instead of a compiler error 200 lines into a build. A clang or clang-cl
            // editor skips this, since clang finds MSVC itself.
            if (!web && ShipCompilerIsMsvcCl() &&
                !GetEnvironmentVariable(u8"VCToolsInstallDir").HasValue())
            {
                LOG_ERROR(u8"Export",
                          u8"native ship export with MSVC needs the Visual Studio build "
                          u8"environment: VCToolsInstallDir is not set - launch the editor/CLI "
                          u8"from a Developer Command Prompt (or a shell that has run "
                          u8"vcvars64.bat)");
                return Status{ErrorCode::NotFound};
            }
#endif
            if (!DirectoryExists(PathJoin(engineRoot, u8"Code").AsView()))
            {
                LOG_ERROR(u8"Export",
                          u8"native ship link needs the engine source checkout at '{}' - not "
                          u8"found (relocated editor?)",
                          engineRoot);
                return Status{ErrorCode::NotFound};
            }
            const String nativeDir = PathJoin(project.Directory(), u8"Native");
            if (!DirectoryExists(nativeDir.AsView()))
            {
                LOG_ERROR(u8"Export",
                          u8"project declares a native module but '{}' does not exist "
                          u8"(the native source convention is <project>/Native)",
                          nativeDir);
                return Status{ErrorCode::NotFound};
            }
            const String target =
                detail::NativeTargetFromModulePath(project.Settings().nativeModule.AsView());
            if (target.IsEmpty())
            {
                LOG_ERROR(u8"Export", u8"cannot derive the native target from nativeModule '{}'",
                          project.Settings().nativeModule);
                return Status{ErrorCode::InvalidArgument};
            }
            const String suffix = detail::ShipOutputSuffix(project.Settings().name.AsView());
            const String buildType = config.IsEmpty() ? String(u8"Release") : String(config);
            const String shipDir = PathJoin(
                project.Directory(),
                Format(u8"{}/ship-{}{}", engine::project::kProjectCacheDir,
                       web ? StringView(u8"web-") : StringView(u8""), buildType)
                    .AsView());

            if (onProgress)
            {
                onProgress(u8"Configuring native ship build...", 0.86f);
            }
            // Pin the ship toolchain to the one that built this editor (the system default may be
            // a different compiler; the checkout's build conventions stay authoritative).
            const String defCompiler =
                Format(u8"-DCMAKE_CXX_COMPILER={}",
                       StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CXX_COMPILER)));
            const String defBuildType = Format(u8"-DCMAKE_BUILD_TYPE={}", buildType);
            const String defGameDir = Format(u8"-DENGINE_GAME_NATIVE_DIR={}", nativeDir);
            const String defGameTarget = Format(u8"-DENGINE_GAME_NATIVE_TARGET={}", target);
            const String defSuffix = Format(u8"-DBUILDSYSTEM_OUTPUT_SUFFIX={}", suffix);
            // Web: the toolchain file selects the compiler (never pin clang++ under it).
            const StringView compilerOrToolchain =
                web ? toolchainDef.AsView() : defCompiler.AsView();
            const StringView cfgArgs[] = {u8"-S",       engineRoot,
                                          u8"-B",       shipDir.AsView(),
                                          u8"-G",       u8"Ninja",
                                          compilerOrToolchain,   defBuildType.AsView(),
                                          defGameDir.AsView(),   defGameTarget.AsView(),
                                          defSuffix.AsView()};
            const ProcessResult configured =
                RunProcess(cmakePath, Span<const StringView>(cfgArgs, 11));
            if (!configured.Ok())
            {
                LOG_ERROR(u8"Export", u8"native ship configure failed ({}):\n{}",
                          configured.exitCode, configured.output);
                return Status{ErrorCode::Internal};
            }

            if (onProgress)
            {
                onProgress(u8"Building native ship player (this can take a while)...", 0.88f);
            }
            const StringView buildArgs[] = {u8"--build", shipDir.AsView(), u8"--target",
                                            u8"Engine.GamePlayer", u8"-j",
                                            web ? StringView(u8"2") : StringView(u8"4")};
            const ProcessResult built =
                RunProcess(cmakePath, Span<const StringView>(buildArgs, 6));
            if (!built.Ok())
            {
                LOG_ERROR(u8"Export", u8"native ship build failed ({}) - tail:\n{}",
                          built.exitCode, built.output);
                return Status{ErrorCode::Internal};
            }

            // The player lands under the checkout's suffixed Bin; the platform-compiler tag is the
            // build's own concern, so search for it rather than rebuild the name.
            const String binRoot = Format(u8"{}/Bin/{}", engineRoot, buildType);
            vfs::NativeFileSystem bin(binRoot.AsView(), editor::EditorRootAllocator());
            if (vfs::IEnumerableFileSystem* enumerable = bin.AsEnumerable())
            {
                Array<vfs::DirEntry> entries;
                if (enumerable->Enumerate(u8"", entries).IsOk())
                {
                    for (const vfs::DirEntry& entry : entries)
                    {
                        if (entry.isDirectory && entry.name.AsView().EndsWith(suffix.AsView()))
                        {
                            // Web: the page of the html, js and wasm trio; desktop: the executable.
                            const String candidate =
                                Format(u8"{}/{}/{}", binRoot, entry.name,
                                       web ? StringView(u8"Engine.GamePlayer.html")
                                           : StringView(u8"Engine.GamePlayer"));
                            if (FileExists(candidate.AsView()))
                            {
                                outPlayer = candidate;
                                return Status{};
                            }
                        }
                    }
                }
            }
            LOG_ERROR(u8"Export", u8"native ship build succeeded but Engine.GamePlayer was not "
                                  u8"found under '{}/*{}'",
                      binRoot, suffix);
            return Status{ErrorCode::NotFound};
        }
    }

    Status BuildDevNativeModule(EditorProject& project)
    {
        const StringView engineRoot =
            StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_ENGINE_ROOT));
        const StringView cmakePath =
            StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CMAKE_PATH));
        if (project.Settings().nativeModule.IsEmpty())
        {
            return Status{ErrorCode::NotFound};
        }
        if (!DirectoryExists(PathJoin(engineRoot, u8"Code").AsView()))
        {
            LOG_ERROR(u8"Export", u8"native dev build needs the engine checkout at '{}'",
                      engineRoot);
            return Status{ErrorCode::NotFound};
        }
#if PLATFORM_WINDOWS
        // The same contract as the ship build, with the same narrow gate: only cl.exe needs vcvars
        // set in this process; clang and clang-cl find MSVC on their own.
        if (ShipCompilerIsMsvcCl() && !GetEnvironmentVariable(u8"VCToolsInstallDir").HasValue())
        {
            LOG_ERROR(u8"Export",
                      u8"native dev build with MSVC needs the Visual Studio build environment: "
                      u8"VCToolsInstallDir is not set - launch the editor/CLI from a "
                      u8"Developer Command Prompt (or a shell that has run vcvars64.bat)");
            return Status{ErrorCode::NotFound};
        }
#endif
        const String nativeDir = PathJoin(project.Directory(), u8"Native");
        const String target =
            detail::NativeTargetFromModulePath(project.Settings().nativeModule.AsView());
        if (target.IsEmpty() || !DirectoryExists(nativeDir.AsView()))
        {
            return Status{ErrorCode::InvalidArgument};
        }
        const String suffix =
            Format(u8"-Dev-{}", detail::ShipOutputSuffix(project.Settings().name.AsView())
                                    .AsView()
                                    .Data() + 6); // reuse the sanitizer, swap the prefix
        const String devDir = PathJoin(
            project.Directory(),
            Format(u8"{}/native-dev", engine::project::kProjectCacheDir).AsView());

        const String defCompiler =
            Format(u8"-DCMAKE_CXX_COMPILER={}",
                   StringView(reinterpret_cast<const char8_t*>(BUILDSYSTEM_CXX_COMPILER)));
        const String defGameDir = Format(u8"-DENGINE_GAME_NATIVE_DIR={}", nativeDir);
        const String defGameTarget = Format(u8"-DENGINE_GAME_NATIVE_TARGET={}", target);
        const String defSuffix = Format(u8"-DBUILDSYSTEM_OUTPUT_SUFFIX={}", suffix);
        const StringView cfgArgs[] = {u8"-S",
                                      engineRoot,
                                      u8"-B",
                                      devDir.AsView(),
                                      u8"-G",
                                      u8"Ninja",
                                      defCompiler.AsView(),
                                      u8"-DCMAKE_BUILD_TYPE=Debug",
                                      u8"-DENGINE_SHARED_LIBS=ON",
                                      defGameDir.AsView(),
                                      defGameTarget.AsView(),
                                      defSuffix.AsView()};
        const ProcessResult configured =
            RunProcess(cmakePath, Span<const StringView>(cfgArgs, 12));
        if (!configured.Ok())
        {
            LOG_ERROR(u8"Export", u8"native dev configure failed ({}):\n{}",
                      configured.exitCode, configured.output);
            return Status{ErrorCode::Internal};
        }
        const StringView buildArgs[] = {u8"--build", devDir.AsView(), u8"--target",
                                        target.AsView(), u8"-j", u8"4"};
        const ProcessResult built = RunProcess(cmakePath, Span<const StringView>(buildArgs, 6));
        if (!built.Ok())
        {
            LOG_ERROR(u8"Export", u8"native dev build failed ({}) - tail:\n{}",
                      built.exitCode, built.output);
            return Status{ErrorCode::Internal};
        }
        // Success means the .so is at the manifest path (the Native/CMakeLists output-directory rule).
        const String modulePath =
            PathJoin(project.Directory(), project.Settings().nativeModule.AsView());
        return FileExists(modulePath.AsView()) ? Status{} : Status{ErrorCode::NotFound};
    }

    Status ApplyPresetDisplay(StringView outDir, const ExportPreset& preset)
    {
        if (!preset.overridesRender && !preset.overridesWindow && !preset.overridesOrientation)
        {
            return Status{};
        }
        foundation::vfs::NativeFileSystem mount(outDir, editor::EditorRootAllocator());
        engine::project::ProjectSettings dist;
        if (!engine::project::LoadProjectSettings(mount, dist, engine::project::kDistManifestFile).IsOk())
        {
            LOG_ERROR(u8"Export", u8"the dist manifest did not read back for the preset's display");
            return Status{ErrorCode::Internal};
        }
        if (preset.overridesRender)
        {
            dist.renderWidth = preset.renderWidth;
            dist.renderHeight = preset.renderHeight;
            dist.renderFit = preset.renderFit;
        }
        if (preset.overridesWindow)
        {
            dist.windowWidth = preset.windowWidth;
            dist.windowHeight = preset.windowHeight;
            dist.windowMode = preset.windowMode;
            dist.windowResizable = preset.windowResizable;
        }
        if (preset.overridesOrientation)
        {
            dist.orientation = preset.orientation;
        }
        if (!engine::project::SaveProjectSettings(*mount.AsWritable(), dist, engine::project::kDistManifestFile)
                 .IsOk())
        {
            LOG_ERROR(u8"Export", u8"failed to write the preset's display into the dist manifest");
            return Status{ErrorCode::Internal};
        }
        return Status{};
    }

    Status ExportOne(EditorProject& project, const ExportPreset& preset,
                     const TemplateRegistry& templates, BuilderRegistry& builders,
                     StringView outRoot, StringView dataRoot, bool rebuild, ExportResult* outResult,
                     const ExportProgress& onProgress, bool cook,
                     const HashMap<Guid, Array<byte>>* sceneStreams,
                     const SceneReferenceScanner* scanner,
                     const Array<Guid>* precomputedReachableRoots)
    {
        const ExportTemplate* tmpl = templates.Resolve(preset);
        if (tmpl == nullptr)
        {
            LOG_ERROR(u8"Export",
                               u8"no export template for preset '{}' (platform '{}') - import one",
                               preset.name, preset.platform);
            return Status{ErrorCode::NotFound};
        }

        ExportResult result;

        // Soft engine-version match, like EditorProject::Open's project-manifest check: a template
        // built against a different engine version may be binary-incompatible with the cooked
        // content, but that is not known, so warn and keep going rather than block. An empty version
        // means an older or hand-written template with no stamp; skip the check.
        if (!tmpl->engineVersion.IsEmpty() &&
            tmpl->engineVersion != engine::project::kEngineVersionString)
        {
            LOG_WARNING(u8"Export",
                                 u8"template '{}' was built against engine {} but this build is {} "
                                 u8"- exporting anyway",
                                 tmpl->id, tmpl->engineVersion,
                                 engine::project::kEngineVersionString);
            result.engineVersionWarning = String(u8"Template '");
            result.engineVersionWarning += tmpl->id;
            result.engineVersionWarning += u8"' targets engine ";
            result.engineVersionWarning += tmpl->engineVersion;
            result.engineVersionWarning += u8" (this build is ";
            result.engineVersionWarning += engine::project::kEngineVersionString;
            result.engineVersionWarning += u8").";
        }

        const String subdir = preset.outputSubdir.IsEmpty()
                                  ? detail::SanitizeName(preset.name.AsView())
                                  : String(preset.outputSubdir.AsView());
        result.outputDir = PathJoin(outRoot, subdir.AsView());

        // Ensure the output dir (and outRoot) exist before the content pipeline writes into it.
        (void)CreateDirectories(result.outputDir.AsView());

        // Closure pruning is opt-in per preset. It needs the scene-to-asset edges discovered, either
        // by a live scene-reference scanner (the CLI, which loads scenes on its own single thread) or
        // by a precomputed reachable-root set (the editor, which pre-scans on the main thread and
        // passes the guids into this background job, since scene loading is main-thread only).
        // Without either, content must not be dropped silently, so fall back to packing everything
        // with a warning.
        const bool haveScanner = (scanner != nullptr && *scanner);
        const bool havePrecomputed = (precomputedReachableRoots != nullptr);
        bool prune = preset.pruneToReachable;
        if (prune && !haveScanner && !havePrecomputed)
        {
            LOG_WARNING(
                u8"Export",
                u8"preset '{}' requests pruning but no scene-reference scanner or precomputed root "
                u8"set was supplied - exporting everything",
                preset.name);
            prune = false;
        }

        // The content variants this preset ships: the single host pak for desktop, BC and ASTC for
        // Web. They are passed into every content path so the pack produces one pak per variant.
        const Array<ContentVariant> variants =
            VariantsForPlatform(project, preset.platform.AsView());
        const Span<const ContentVariant> variantSpan(variants.Data(), variants.Size());

        Status contentStatus;
        if (prune)
        {
            // Seed the roots, expand the scene-graph edges, take the PlanFor closure, then cook,
            // stage and pack only that. The scene-edge expansion is either precomputed (the editor's
            // main-thread pre-scan) or run inline through the scanner (CLI). `seeds` is still
            // recomputed here for the report (metadata only, safe in the background); it drives the
            // display, while planRoots drives the cook and the pack.
            const Array<ExportRoot> seeds = CollectExportRoots(project);
            const Array<Guid> planRoots = havePrecomputed
                                              ? *precomputedReachableRoots
                                              : ExpandReachableRoots(project, seeds, *scanner);
            Array<Guid> reachableList;
            contentStatus = CookReachable(project, builders,
                                          Span<const Guid>(planRoots.Data(), planRoots.Size()),
                                          cook, rebuild, result.content, reachableList, onProgress);
            // Variant target cooks (web) after the host closure cook; no-op for desktop.
            if (contentStatus.IsOk())
            {
                contentStatus =
                    CookVariantTargets(project, builders, variantSpan, rebuild, onProgress);
            }
            if (contentStatus.IsOk())
            {
                HashMap<Guid, u8> reachable;
                for (const Guid& g : reachableList)
                {
                    reachable.InsertOrAssign(g, u8(1));
                }
                contentStatus =
                    ExportContent(project, result.outputDir.AsView(), result.content, onProgress,
                                  sceneStreams, &reachable, &seeds, &result.pruning, variantSpan,
                                  preset.includeShaderSource);
            }
        }
        else if (cook)
        {
            contentStatus = ExportProject(project, result.outputDir.AsView(), builders, rebuild,
                                          &result.content, onProgress, sceneStreams, variantSpan,
                                          preset.includeShaderSource);
        }
        else
        {
            // The editor already cooked the host through CookService; the variant targets still
            // need cooking.
            contentStatus = CookVariantTargets(project, builders, variantSpan, rebuild, onProgress);
            if (contentStatus.IsOk())
            {
                contentStatus =
                    ExportContent(project, result.outputDir.AsView(), result.content, onProgress,
                                  sceneStreams, nullptr, nullptr, nullptr, variantSpan,
                                  preset.includeShaderSource);
            }
        }
        if (!contentStatus.IsOk() || !ApplyPresetDisplay(result.outputDir.AsView(), preset).IsOk())
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
        // Player: <template dir>/<playerBinary> is copied to <outDir>/<name>. The name is the
        // preset's playerName (else the template binary), with the target platform's executable
        // extension ensured, since Windows needs .exe or the OS will not launch it.
        //
        // For native projects the player is not the template's: it is Engine.GamePlayer, freshly
        // built with the game's native module statically linked (for Web presets too, as the wasm
        // build). Ship players ignore the manifest's dlopen module, since the static plugin takes
        // precedence.
        const bool nativeShip = !project.Settings().nativeModule.IsEmpty();
        const bool nativeWeb = nativeShip && (preset.platform.AsView().StartsWith(u8"Web") ||
                                              preset.platform.AsView().StartsWith(u8"Wasm"));
        String shipPlayerPath;
        if (nativeShip)
        {
            const Status shipStatus = BuildShipPlayer(project, preset.config.AsView(), nativeWeb,
                                                      onProgress, shipPlayerPath);
            if (!shipStatus.IsOk())
            {
                if (outResult != nullptr)
                {
                    *outResult = result;
                }
                return shipStatus;
            }
        }
        // Web native ships as Engine.GamePlayer.{html,js,wasm}: the trio cross-references by
        // basename, so a preset playerName cannot rename it (desktop renames freely).
        const String outName =
            nativeWeb ? String(u8"Engine.GamePlayer.html")
                      : detail::PlayerOutputName(preset.platform.AsView(),
                                                 preset.playerName.AsView(),
                                                 nativeShip ? StringView(u8"Engine.GamePlayer")
                                                            : tmpl->playerBinary.AsView());
        const StringView shipPlayerName =
            nativeWeb ? StringView(u8"Engine.GamePlayer.html") : StringView(u8"Engine.GamePlayer");
        String shipPlayerDir;
        if (nativeShip)
        {
            // Split the built player's absolute path into (dir, name) for the copy helper.
            const StringView full = shipPlayerPath.AsView();
            for (usize i = full.Size(); i > 0; --i)
            {
                if (full.Data()[i - 1] == '/')
                {
                    shipPlayerDir = String(StringView(full.Data(), i - 1));
                    break;
                }
            }
        }
        const bool playerStaged =
            nativeShip
                ? detail::CopyFilePreserving(shipPlayerDir.AsView(), shipPlayerName,
                                             result.outputDir.AsView(), outName.AsView())
                : detail::CopyFilePreserving(tmpl->directory.AsView(), tmpl->playerBinary.AsView(),
                                             result.outputDir.AsView(), outName.AsView());
        if (!playerStaged)
        {
            LOG_ERROR(u8"Export", u8"failed to stage player '{}' from {}",
                               nativeShip ? StringView(u8"Engine.GamePlayer")
                                          : tmpl->playerBinary.AsView(),
                               nativeShip ? shipPlayerPath.AsView() : tmpl->id.AsView());
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return Status{ErrorCode::Internal};
        }
        ++result.filesStaged;
        if (nativeWeb)
        {
            // The rest of the ship trio (the js glue and the wasm), with the same basenames.
            const StringView trio[] = {StringView(u8"Engine.GamePlayer.js"),
                                       StringView(u8"Engine.GamePlayer.wasm")};
            for (StringView part : trio)
            {
                if (!detail::CopyFilePreserving(shipPlayerDir.AsView(), part,
                                                result.outputDir.AsView(), part))
                {
                    LOG_ERROR(u8"Export", u8"failed to stage web ship part '{}' from '{}'", part,
                              shipPlayerDir);
                    if (outResult != nullptr)
                    {
                        *outResult = result;
                    }
                    return Status{ErrorCode::Internal};
                }
                ++result.filesStaged;
            }
        }

        // Cooked engine shaders: Data/Engine/Shaders/shaders.dpak (and Data/.dataroot) beside the
        // player, so the dist renders with no runtime compiler, found by the same data-root
        // discovery every executable uses. A cook failure is fatal, since a dist without shaders
        // cannot render.
        if (onProgress)
        {
            onProgress(u8"Cooking shaders...", 0.95f);
        }
        {
            u32 shaderVariants = 0;
            const Status packStatus = StageShaderPack(
                result.outputDir.AsView(), dataRoot, preset.platform.AsView(), project,
                Span<const Guid>(result.content.shaders.Data(), result.content.shaders.Size()),
                shaderVariants);
            if (!packStatus.IsOk())
            {
                if (outResult != nullptr)
                {
                    *outResult = result;
                }
                return Status{ErrorCode::Internal};
            }
            result.filesStaged += 2; // the pack and the marker
            LOG_INFO(u8"Export", u8"staged Data/{} ({} variants)", shaders::kShaderPackPath,
                     shaderVariants);
        }

        if (onProgress && !tmpl->sidecars.IsEmpty())
        {
            onProgress(u8"Staging runtime libs...", 0.96f);
        }
        // Template sidecars (runtime libraries) from the template directory. A dist renders from its
        // cooked shader pack and never compiles shaders. The player build does not list DXC, but an
        // older template still might, so DXC libraries are dropped here and the dist ships no
        // dxcompiler or dxil.
        for (const String& sidecar : tmpl->sidecars)
        {
            if (nativeWeb && (sidecar.AsView().EndsWith(u8".js") ||
                              sidecar.AsView().EndsWith(u8".wasm")))
            {
                continue; // the ship build provided the trio; never overwrite it with the template's
            }
            if (IsDxcRuntimeLib(sidecar.AsView()))
            {
                LOG_INFO(u8"Export", u8"omitting DXC sidecar '{}' (dist renders from the "
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
                LOG_WARNING(u8"Export", u8"sidecar '{}' not found in template '{}'",
                                     sidecar, tmpl->id);
            }
        }

        // Symbols (PDB, DWARF) are left out of the dist by default; stage them only when the preset
        // opts in. sidecars[] always stage; symbols[] are gated here.
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
                    LOG_WARNING(u8"Export",
                                         u8"symbol file '{}' not found in template '{}'", symbol,
                                         tmpl->id);
                }
            }
        }

        // Preset additionalFiles (game extras, relative to the project) go to <outDir>/<basename>.
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
                LOG_WARNING(u8"Export", u8"additional file '{}' not found", extra);
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
                     StringView outRoot, StringView dataRoot, bool rebuild,
                     const ExportProgress& onProgress, bool cook,
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
            if (ExportOne(project, preset, templates, builders, outRoot, dataRoot, rebuild, &result,
                          scoped, cook, sceneStreams, scanner, precomputedReachableRoots)
                    .IsOk())
            {
                ++ok;
                LOG_INFO(u8"Export", u8"exported '{}' -> {} ({} files staged)",
                                  preset.name, result.outputDir, result.filesStaged);
            }
            else
            {
                LOG_ERROR(u8"Export", u8"preset '{}' failed", preset.name);
            }
        }
        return (ok == n) ? Status{} : Status{ErrorCode::Internal};
    }
}
