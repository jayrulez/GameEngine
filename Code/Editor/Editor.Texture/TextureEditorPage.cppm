// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Texture - the `editor.texture` module.
//
// TextureEditorPage: the import-settings surface over TextureAsset. On the GUI from the image
// pages' markup (Data/Editor/ImagePage.xml; Documentation/Specs/editor-gui.md, 5.4, layer 2): the
// source image (decoded through the same stb path the cook rides, so what you see is what gets
// cooked; a model's embedded texture from its "pixels" stream) to zoom, pan and read pixels in,
// beside the GPU-texture intent: usage, colour space, compression, shape, sampler filters and
// wraps, mipmaps, anisotropy. Usage sets the colour space with it (data is linear, colour sRGB),
// one undo step; a texture whose two disagree says so in a notice. The presets (UI, Sprite, 3D
// Surface, Normal Map, Data Mask, the skies) are actions in a menu on the page's bar, each one
// undo step however many settings it moves. What it cooks to, its size and mips are read-only
// facts. The authored TextureAsset IS the cook input, so Save writes it back and requests a
// re-cook; every live proxy bound to the cooked product then hot-swaps.

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

        /// A preset: what its menu item says, and what it sets.
        struct Preset
        {
            StringView label;
            void (pipeline::TextureAsset::*setup)();
        };
        /// The presets, in their menu's order.
        [[nodiscard]] static Span<const Preset> Presets();

        TextureEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);

        /// Applies the preset `label`, one undo step; false when there is none by that name.
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
        foundation::image::PixelFormat m_sourceFormat = foundation::image::PixelFormat::RGBA8; // before the preview's conversion
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

    /// The page (asking for its services when it is made) and the texture thumbnails: one
    /// composition entry point per domain, no pipeline links in Editor.App.
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
