// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Geometry - :model_file_preview partition.
//
// The import window's preview of a model file (Specs/import-window.md W4): the model as the model
// viewer shows it, live in a PreviewViewport (orbit, zoom, Frame), with its clips (a picker, play
// and pause) and its skeleton drawn over it on request; and the facts the file gives (meshes,
// triangles, materials, textures, bones, clips). It opens through Editor.ModelPreview's OpenModel
// into a cache folder under the project's cache: a model opened before binds from there, and one
// that must be read is read on the ImportPreviewReader's worker, then cooked and bound on the main
// thread, which owns the resources. Without a host (a test) there is no view, but the model still
// opens and its facts show. The window drives it each frame (IImportPreviewFrames).
//
// The interface names no render, scene or model type (they are the body's): gcc's module merger
// fails when a heavy translation unit imports an interface that pulls in engine.render.

module;
#include "Core/Prelude.h"

export module editor.geometry:model_file_preview;

import foundation.core;
import foundation.runtime.client;
import foundation.graphics;
import foundation.gui;
import editor.core;
import pipeline.importer;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class ModelFilePreview final : public IImportPreview, public app::IImportPreviewFrames
    {
    public:
        /// Opens `path` with its cooked files under `cacheRoot`: bound at once when opened before,
        /// else read on `reader`. `services` give the host and GUI the view needs (none: no view).
        ModelFilePreview(StringView path, StringView cacheRoot, ImportPreviewReader& reader, IAllocator& allocator, const app::AssetPageServices& services);
        ModelFilePreview(const ModelFilePreview&) = delete;
        ModelFilePreview& operator=(const ModelFilePreview&) = delete;
        ~ModelFilePreview() override;

        [[nodiscard]] foundation::gui::Node& View() override;
        [[nodiscard]] Span<const String> Facts() const override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost& host, foundation::graphics::FrameContext& frame) override;

        // === For tests ===
        /// Whether the model opened (cooked or from the cache, and bound).
        [[nodiscard]] bool IsOpen() const;
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
        [[nodiscard]] UniquePtr<IImportPreview> Create(StringView path, const pipeline::ImportOptions* options, ImportPreviewReader& reader,
                                                       IAllocator& allocator) override;

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
