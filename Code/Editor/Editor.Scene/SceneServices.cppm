// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :services partition.
//
// What the scene editor gives the editor besides its pages, none of it UI: the scene and prefab
// document types; the export's seams (a scene's text source transcoded to the binary wire, the
// assets a scene references for the export's closure); and a model import's prefab and scene,
// generated once the import lands. RegisterSceneEditorServices is called by the executable
// beside the pages' registration. Taken out of :page's RegisterSceneEditor as it was, so the
// export and model imports work while the page moves onto the GUI
// (Documentation/Specs/editor-gui.md, 5.3).

module;
#include "Core/Prelude.h"

export module editor.scene:services;

import foundation.core;
import foundation.content;
import foundation.runtime;
import foundation.runtime.client; // IApplicationHost
import engine.composition; // AddAllSceneManagers (the full composition for headless scratch scenes)
import foundation.scene;
import engine.scene;
import foundation.scene.resource;
import editor.core;
import pipeline.importer; // the import framework (a consumer imports it itself)
import scene.pipeline;    // a model import's prefab and scene (GenerateForImport)
import modelimporter;     // ModelImportOptions (the import's prefab and scene toggles)
import :view_settings;    // RegisterSceneViewSettingsType (per-scene grid pref)

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace scene = foundation::scene;

    inline void RegisterSceneEditorServices(EditorContext& context, runtime::IApplicationHost& host)
    {
        GlobalTypeRegistry().Register(scene::SceneDocument::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<scene::SceneDocument>();
        RegisterSceneViewSettingsType();
        GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<scene::PrefabDocument>();

        // Model imports: generate/refresh the hierarchy prefab beside the manifest (the
        // "Generate prefab" import option). Lives here - not in the importer - because it
        // needs scene machinery the importer library (linked by the headless cooker) never
        // links. Re-import reuses the prefab's guid, so placed instances rebuild in every
        // open scene and an open prefab page refreshes like any external asset change.
        EditorContext* editorContext = &context;
        runtime::IApplicationHost* appHost = &host;

        // Export seam: scene/prefab TEXT sources transcode to the binary wire on the main
        // thread before the pack job. The scratch scene is assembled from the full
        // composition - a hand-listed set would silently drop component types.
        context.SceneStreamStager = [](foundation::content::Instance& instance,
                                       Array<byte>& out) -> bool
        {
            if (!engine::IsSceneLike(instance))
            {
                return false;
            }
            UniquePtr<IStream> stream = instance.ReadData(engine::kSceneStream);
            if (stream.Get() == nullptr)
            {
                return false;
            }
            // A transient scratch scene assembled from the full composition (no component type
            // silently skipped by a hand-listed set).
            scene::Scene scratch(editor::EditorRootAllocator(), u8"__export_transcode");
            engine::AddAllSceneManagers(scratch);
            const bool isScene = instance.TypeName() == StringView(u8"SceneDocument");
            Result<Array<byte>> bytes =
                scene::TranscodeSceneStreamToBinary(*stream, scratch, /*includeSettings=*/isScene);
            if (!bytes.HasValue())
            {
                return false;
            }
            out = Move(bytes.Value());
            return true;
        };

        // Export reachability seam: the assets a scene or prefab references, so the export
        // closure can chase the scene->asset edges the cook's read-dep graph can't see. MAIN-THREAD
        // only.
        context.SceneRefScanner =
            [](foundation::content::Instance& instance, foundation::content::ContentDatabase& db,
               Array<Guid>& outResources, Array<Guid>& outPrefabs) -> bool
        {
            if (!engine::IsSceneLike(instance))
            {
                return false;
            }
            return engine::ScanSceneReferences(editor::EditorRootAllocator(), instance, db,
                                               outResources, outPrefabs);
        };

        context.AddImportListener(
            [editorContext, appHost](foundation::content::Instance& instance,
                                     const pipeline::ImportOptions* options)
            {
                // A model's prefab and scene, the pipeline's generation every host runs, then
                // what only the editor does with them.
                pipeline::ModelPrefabResult generated;
                pipeline::ModelPrefabResult generatedScene;
                if (!pipeline::GenerateForImport(editor::EditorRootAllocator(), instance, options,
                                                 generated, generatedScene))
                {
                    return;
                }
                // No options object = the default fresh import, which generates the prefab only.
                const auto* modelOptions =
                    Cast<pipeline::ModelImportOptions>(const_cast<pipeline::ImportOptions*>(options));
                const bool wantPrefab = modelOptions == nullptr || modelOptions->generatePrefab;
                const bool wantScene = modelOptions != nullptr && modelOptions->generateScene;

                if (wantPrefab)
                {
                    if (generated.instance == nullptr)
                    {
                        editorContext->Notify(NoticeKind::Error,
                                              u8"Model prefab generation failed.");
                    }
                    else
                    {
                        if (generated.regenerated)
                        {
                            UniquePtr<IStream> payload = generated.instance->ReadData(u8"scene");
                            if (payload.Get() != nullptr)
                            {
                                Array<byte> bytes;
                                bytes.Resize(static_cast<usize>(payload->Size()));
                                (void)payload->Read(bytes.Data(), bytes.Size());
                                const Guid prefabId = generated.instance->Id();
                                scene::PrefabPayloadResolver resolver{
                                    [editorContext](const Guid& id) -> UniquePtr<IStream>
                                    {
                                        if (editorContext->Project() == nullptr)
                                        {
                                            return UniquePtr<IStream>{};
                                        }
                                        foundation::content::Instance* prefab =
                                            editorContext->Project()->SourceDb().GetInstance(id);
                                        return (prefab != nullptr) ? prefab->ReadData(u8"scene")
                                                                   : UniquePtr<IStream>{};
                                    }};
                                if (auto* scenes =
                                        appHost->Subsystems().GetSubsystem<engine::scene::SceneSubsystem>())
                                {
                                    scenes->ForEachScene(
                                        [&](scene::Scene& scene)
                                        {
                                            const u32 rebuilt = scene::RebuildPrefabInstances(
                                                scene, prefabId,
                                                Span<const byte>{bytes.Data(), bytes.Size()},
                                                &resolver);
                                            if (rebuilt > 0 &&
                                                editorContext->Resources() != nullptr)
                                            {
                                                scene::ResolveSceneResources(
                                                    scene, *editorContext->Resources());
                                            }
                                        });
                                }
                                (void)editorContext->NotifyAssetExternallyModified(prefabId);
                            }
                        }
                        String message(u8"Prefab '");
                        message += generated.instance->Name();
                        message += generated.regenerated
                                       ? StringView(u8"' regenerated (placed instances updated).")
                                       : StringView(u8"' generated.");
                        editorContext->Notify(NoticeKind::Success, message.AsView());
                    }
                }

                if (wantScene)
                {
                    if (generatedScene.instance == nullptr)
                    {
                        editorContext->Notify(NoticeKind::Error,
                                              u8"Model scene generation failed.");
                    }
                    else
                    {
                        // A regenerated scene refreshes any open page editing it (scenes are
                        // referenced/opened, not spawned as instances, so there is nothing to
                        // rebuild in other scenes).
                        if (generatedScene.regenerated)
                        {
                            (void)editorContext->NotifyAssetExternallyModified(
                                generatedScene.instance->Id());
                        }
                        String message(u8"Scene '");
                        message += generatedScene.instance->Name();
                        message += generatedScene.regenerated ? StringView(u8"' regenerated.")
                                                              : StringView(u8"' generated.");
                        editorContext->Notify(NoticeKind::Success, message.AsView());
                    }
                }
            });
    }
}
