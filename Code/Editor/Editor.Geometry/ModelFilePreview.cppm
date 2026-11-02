// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Geometry - :model_file_preview partition.
//
// The import window's preview of a model file (Specs/import-window.md W4): the model as the model
// viewer shows it, live in a PreviewViewport (orbit, zoom, Frame), with its clips (a picker, play
// and pause) and its skeleton drawn over it on request; and the facts the file gives (meshes,
// triangles, materials, textures, bones, clips). It opens through Editor.ModelPreview's OpenModel
// into a cache folder under the project's cache: a model opened before binds from there, and one
// that must be read is read as a task on the editor's pool, then cooked and bound on the main
// thread, which owns the resources. Without a host (a test) there is no view, but the model still
// opens and its facts show. The window drives it each frame (IImportPreviewFrames).
//
// The interface names no render or scene type (they are the body's): gcc's module merger fails
// when a heavy translation unit imports an interface that pulls in engine.render.

module;
#include "Core/Prelude.h"

export module editor.geometry:model_file_preview;

import foundation.core;
import foundation.model; // Model (what the importer read, cooked rather than read again)
import foundation.runtime.client;
import foundation.graphics;
import foundation.gui;
import editor.core;
import pipeline.importer;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class ModelFilePreview final : public IImportPreview, public app::IImportPreviewFrames, public IImportPartPreview
    {
    public:
        /// Opens `path` with its cooked files under `cacheRoot`: bound at once when opened before,
        /// else cooked on `tasks` from `prepared` (the importer's read, a pipeline::LoadedModel,
        /// held by the task while it cooks), else read and cooked there. `services` give the host
        /// and GUI the view needs (none: no view). `options` (the file's import settings, which
        /// outlive this) say what the import leaves out: materials left out draw the default.
        ModelFilePreview(StringView path, StringView cacheRoot, TaskPool& tasks, IAllocator& allocator, const app::AssetPageServices& services,
                         Object* prepared = nullptr, const pipeline::ImportOptions* options = nullptr);
        ModelFilePreview(const ModelFilePreview&) = delete;
        ModelFilePreview& operator=(const ModelFilePreview&) = delete;
        ~ModelFilePreview() override;

        [[nodiscard]] foundation::gui::Node& View() override;
        [[nodiscard]] Span<const String> Facts() const override;
        /// Stops the cook, which reads the importer's model, and waits for it.
        void StopReading() override;
        /// The file's settings changed: the model drawn as it will import (materials left out draw
        /// the default material).
        void OnSettingsChanged() override;

        /// A part alone: a mesh by itself, a material on a sphere, the skeleton's bones over the
        /// model, a clip playing on it, a texture's picture in place of the model. False for a
        /// collision shape (the preview's cook makes none), and for a texture when the model was not
        /// read here (bound from the preview's cache). Asked before the model is in, it is shown once
        /// it is.
        bool ShowPart(const pipeline::ImportPlanEntry& part) override;
        void ShowWhole() override;
        /// The skeleton's bones in a tree; choosing one draws the bones with it apart.
        [[nodiscard]] RefPtr<foundation::gui::Node> PartView(const pipeline::ImportPlanEntry& part) override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame) override;

        // === For tests ===
        /// Whether the model opened (cooked or from the cache, and bound).
        [[nodiscard]] bool IsOpen() const;
        /// Whether its parts are loading (it binds once they are in).
        [[nodiscard]] bool IsLoading() const;
        /// Whether a texture's picture shows in place of the model.
        [[nodiscard]] bool ShowsPicture() const;
        /// Whether the model draws the default material for a material left out of the import.
        [[nodiscard]] bool DrawsDefaultMaterial() const;
        /// Whether the model draws a material without a texture left out of the import.
        [[nodiscard]] bool DrawsWithoutTextures() const;
        /// The bone chosen in the skeleton's tree (-1: none), and choosing one as the tree does.
        [[nodiscard]] i32 ChosenBone() const;
        void ChooseBone(i32 bone);
        /// Runs its pool and its loading to the end, as frames would, without a host.
        void FinishLoading();
        /// The clips the picker lists, by name.
        [[nodiscard]] Span<const String> Clips() const;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };

    /// Previews the model files an importer lists by one asset type, cooking each into a folder
    /// under the open project's cache.
    class ModelFilePreviewFactory final : public IImportPreviewFactory
    {
    public:
        ModelFilePreviewFactory(const TypeInfo& type, EditorContext& context, const app::AssetPageServicesSource& services)
            : m_type(&type), m_context(&context), m_services(&services)
        {
        }
        [[nodiscard]] const TypeInfo* AssetType() const override { return m_type; }
        [[nodiscard]] UniquePtr<IImportPreview> Create(const ImportPreviewSource& file, TaskPool& tasks, IAllocator& allocator) override;

    private:
        const TypeInfo* m_type;
        EditorContext* m_context;
        const app::AssetPageServicesSource* m_services;
    };

    /// Registers the model preview for the files the model importer lists (its manifests).
    void RegisterModelFilePreview(EditorContext& context, const app::AssetPageServicesSource& services);

    /// Where the import window's model previews cook, under a project's cache folder.
    inline constexpr const char8_t* kModelPreviewCacheFolder = u8"ModelPreviews";
}
