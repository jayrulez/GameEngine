// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Image - the `editor.image` module.
//
// ImageEditorPage (the Texture page's source-side sibling): the inspect + intent surface over an
// ImageAsset, the raw source image that Texture assets (and future consumers) build on. On the GUI
// from the image pages' markup (Data/Editor/ImagePage.gml; Documentation/Specs/editor-gui.md, 5.4,
// layer 2): the decoded file (the same decode the cook rides, as RGBA8 for display) to zoom, pan and
// read pixels in, beside the grid: its source file and colour-space intent, then the source's facts
// read only (its size, pixel format and data size). Each edit is one undo step; Save writes the asset and
// asks for a cook so bound products hot-swap.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.image;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import image.pipeline;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class ImageEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ImagePage.gml";

        ImageEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);

        // === For tests ===
        [[nodiscard]] app::ImagePreview* Preview() const noexcept { return m_preview.Get(); }

    private:
        // Decodes the source file and shows it, with its facts.
        void LoadPreview();

        UniquePtr<app::ImagePreview> m_preview;
    };

    class ImageEditorPageFactory final : public IEditorPageFactory
    {
    public:
        explicit ImageEditorPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// A page asks for its services when it is made (the window exists by then).
    inline void RegisterImageEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<ImageEditorPageFactory>(services), editor::EditorRootAllocator()));
    }
}
