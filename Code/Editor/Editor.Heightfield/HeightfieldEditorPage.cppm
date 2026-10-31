// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Heightfield - the `editor.heightfield` module.
//
// HeightfieldEditorPage: the page that inspects and authors a HeightfieldAsset. A heightfield is
// an image of heights, so the preview is a 2D grayscale height image, as on the texture and image
// pages; the 3D preview belongs to the terrain asset page. It is built from the image pages' markup
// (Data/Editor/ImagePage.xml). The source's heights show from its lowest to its highest as black to
// white (so low relief shows, as in its thumbnail), and the height under the pointer reads in
// metres. Beside it are the authored fields (grid size, world footprint, Y range) and the source's
// facts, read only. Sculpt brushes live in the scene viewport, not here. Save writes the object
// back and requests a re-cook so bound heightfield products (such as colliders) hot-swap.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.heightfield;

export import :thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import heightfield.pipeline;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class HeightfieldEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ImagePage.xml";

        HeightfieldEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);

        /// The source's height sample at (x, y) (0..65535); false off it.
        bool SampleAt(i32 x, i32 y, u16& out) const;
        /// What a sample is in metres: the asset's Y range spread over the samples.
        [[nodiscard]] f32 HeightOf(u16 sample) const;

        // === For tests ===
        [[nodiscard]] app::ImagePreview* Preview() const noexcept { return m_preview.Get(); }

    protected:
        void OnObjectChanged() override { ShowFacts(); }

    private:
        // Decodes the source heightmap (16-bit) and shows it, with its facts.
        void LoadPreview();
        void ShowFacts();

        UniquePtr<app::ImagePreview> m_preview;
        Array<u16> m_samples; // the source's samples, row by row
        u32 m_width = 0;
        u32 m_height = 0;
        u16 m_low = 0;
        u16 m_high = 0;
    };

    class HeightfieldEditorPageFactory final : public IEditorPageFactory
    {
    public:
        explicit HeightfieldEditorPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// Registers the page (which asks for its services when it is made) and the heightfield
    /// thumbnails.
    inline void RegisterHeightfieldEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<HeightfieldEditorPageFactory>(services), editor::EditorRootAllocator()));
        if (context.Thumbnails() != nullptr)
        {
            RegisterHeightfieldThumbnailGenerator(*context.Thumbnails());
        }
    }
}
