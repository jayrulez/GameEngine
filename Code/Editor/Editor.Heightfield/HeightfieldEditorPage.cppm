// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Heightfield - the `editor.heightfield` module.
//
// HeightfieldEditorPage: the inspect + author surface over a HeightfieldAsset. A heightfield IS an
// image of heights, so the preview is a 2D grayscale height image (permanent, by asset identity -
// the texture/image page precedent, NOT a placeholder awaiting 3D; the 3D preview belongs to the
// terrain asset page). On the GUI from the image pages' markup (Data/Editor/ImagePage.xml;
// Documentation/Specs/editor-gui.md, 5.4, layer 2): the source's heights from its lowest to its
// highest as black to white (so low relief shows, as its thumbnail does), the pointer's height read
// in metres, beside the authored fields (grid size, world footprint, Y range) and the source's
// facts, read only. Sculpt brushes live in the scene viewport, not here. Save writes the object
// back and requests a re-cook so bound heightfield products (colliders, later terrain) hot-swap.

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
        Array<u16> m_samples; // the source's, row by row
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

    /// The page (asking for its services when it is made) and the heightfield thumbnails.
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
