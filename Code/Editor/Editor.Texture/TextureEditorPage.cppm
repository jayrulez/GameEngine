// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Texture - the `editor.texture` module.
//
// TextureEditorPage: the import settings of a TextureAsset, on the GUI from the image pages'
// markup (Data/Editor/ImagePage.xml). It shows the source image for zooming, panning and reading
// pixels, decoded through the same stb path the cook uses so what you see is what gets cooked (a
// model's embedded texture comes from its "pixels" stream). Beside it are the GPU texture
// settings: usage, colour space, compression, shape, sampler filters and wraps, mipmaps and
// anisotropy. Setting usage also sets the colour space (data is linear, colour is sRGB) in one undo
// step, and a notice appears when the two disagree. The presets (UI, Sprite, 3D Surface, Normal
// Map, Data Mask, the skies) are actions in a menu on the page's bar, each one undo step however
// many settings it changes. The cooked format, size and mips are read-only facts. The authored
// TextureAsset is the cook input, so Save writes it back and requests a re-cook, after which every
// live proxy bound to the cooked product hot-swaps.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.texture;
export import :thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.texture;
import texture.pipeline;
import foundation.gui;
import foundation.gui.toolkit; // ToolMenuButton
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class TextureEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ImagePage.xml";

        /// A preset: its menu item's label and the settings it applies.
        struct Preset
        {
            StringView label;
            void (pipeline::TextureAsset::*setup)();
        };
        /// The presets, in menu order.
        [[nodiscard]] static Span<const Preset> Presets();

        TextureEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);

        /// Applies the preset `label` as one undo step; returns false when there is none by that
        /// name.
        bool ApplyPreset(StringView label);
        /// "Cooks to": the compression policy for desktop (and mobile when it differs) from the
        /// asset's usage, colour space and compression and the source's facts.
        [[nodiscard]] String ResolvedFormatText() const;

        // === For tests ===
        [[nodiscard]] app::ImagePreview* Preview() const noexcept { return m_preview.Get(); }
        [[nodiscard]] foundation::gui::ToolMenuButton* PresetButton() const noexcept { return m_presets.Get(); }

    protected:
        bool FollowEdit(StringView path, ISerializable& object) override;
        void OnObjectChanged() override { ShowFacts(); }

    private:
        // Decodes the source image (the external file, or the embedded "pixels" stream) and shows it.
        void LoadPreview(foundation::content::Instance& instance);
        void ShowFacts();
        void AddPresets();

        UniquePtr<app::ImagePreview> m_preview;
        RefPtr<foundation::gui::ToolMenuButton> m_presets;
        foundation::image::PixelFormat m_sourceFormat = foundation::image::PixelFormat::RGBA8; // the format before the preview converts it
    };

    class TextureEditorPageFactory final : public IEditorPageFactory
    {
    public:
        explicit TextureEditorPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// Registers the page (which asks for its services when it is created) and the texture
    /// thumbnails. Each domain has one composition entry point, so Editor.App links no pipeline
    /// libraries.
    inline void RegisterTextureEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<TextureEditorPageFactory>(services), editor::EditorRootAllocator()));
        if (context.Thumbnails() != nullptr)
        {
            RegisterTextureThumbnailGenerator(*context.Thumbnails());
        }
    }
}
