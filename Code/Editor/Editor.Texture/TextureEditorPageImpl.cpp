// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Texture - the `editor.texture` module (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.texture;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.texture;
import texture.pipeline;
import texture.compression;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import foundation.rhi;
import editor.app;

using namespace foundation::core;
namespace image = foundation::image;
namespace gui = foundation::gui;

namespace editor
{
    namespace
    {
        // The display name of a policy-resolved cooked format (the "Cooks to:" row).
        [[nodiscard]] StringView CookedFormatName(foundation::rhi::TextureFormat format)
        {
            switch (format)
            {
            case foundation::rhi::TextureFormat::BC1RGBAUnorm: return u8"BC1 (opaque)";
            case foundation::rhi::TextureFormat::BC1RGBAUnormSrgb: return u8"BC1 sRGB (opaque)";
            case foundation::rhi::TextureFormat::BC4RUnorm: return u8"BC4 (single channel)";
            case foundation::rhi::TextureFormat::BC5RGUnorm: return u8"BC5 (normal RG)";
            case foundation::rhi::TextureFormat::BC7RGBAUnorm: return u8"BC7";
            case foundation::rhi::TextureFormat::BC7RGBAUnormSrgb: return u8"BC7 sRGB";
            case foundation::rhi::TextureFormat::ASTC4x4Unorm: return u8"ASTC 4x4";
            case foundation::rhi::TextureFormat::ASTC4x4UnormSrgb: return u8"ASTC 4x4 sRGB";
            case foundation::rhi::TextureFormat::RGBA32Float: return u8"RGBA32F (uncompressed)";
            default: return u8"uncompressed";
            }
        }

        // A decoded source converted to RGBA8 for the picture, or empty when it cannot be. An HDR
        // source is clamped, since a faithful preview would need an exposure control, which this
        // page does not have.
        image::Image ToPreview(const image::Image& src)
        {
            const u32 w = src.Width();
            const u32 h = src.Height();
            if (w == 0 || h == 0)
                return image::Image{};
            if (src.Format() == image::PixelFormat::RGBA8)
                return image::Image(w, h, image::PixelFormat::RGBA8, src.PixelData());
            if (src.Format() == image::PixelFormat::RGBA32F)
            {
                const Span<const u8> raw = src.PixelData();
                const usize texels = static_cast<usize>(w) * h * 4;
                if (raw.Size() < texels * sizeof(f32))
                    return image::Image{};
                const f32* in = reinterpret_cast<const f32*>(raw.Data());
                image::Image out(w, h, image::PixelFormat::RGBA8);
                Span<u8> bytes = out.PixelDataMut();
                for (usize i = 0; i < texels; ++i)
                    bytes[i] = static_cast<u8>(Clamp(in[i], 0.0f, 1.0f) * 255.0f + 0.5f);
                return out;
            }
            return src.ConvertFormat(image::PixelFormat::RGBA8);
        }

        constexpr TextureEditorPage::Preset kPresets[] = {
            {u8"UI", &pipeline::TextureAsset::SetupForUI},
            {u8"Sprite", &pipeline::TextureAsset::SetupForSprite},
            {u8"3D Surface", &pipeline::TextureAsset::SetupFor3D},
            {u8"Normal Map", &pipeline::TextureAsset::SetupForNormalMap},
            {u8"Data Mask", &pipeline::TextureAsset::SetupForDataMask},
            {u8"Equirect Sky", &pipeline::TextureAsset::SetupForEquirectangularSkybox},
            {u8"Cubemap Sky", &pipeline::TextureAsset::SetupForCubemapSkybox},
        };
    } // namespace

    Span<const TextureEditorPage::Preset> TextureEditorPage::Presets()
    {
        return Span<const Preset>(kPresets, sizeof(kPresets) / sizeof(kPresets[0]));
    }

    TextureEditorPage::TextureEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::TextureAsset::StaticType(), kPath)
    {
        if (Content())
        {
            m_preview = MakeUnique<app::ImagePreview>(Allocator(), *Content());
            AddPresets();
        }
        LoadPreview(instance);
        ShowFacts();
    }

    void TextureEditorPage::AddPresets()
    {
        // After the bar's separator, the page's own controls follow Save, Undo and Redo.
        gui::Node* own = Find<gui::Node>(u8"page-own");
        gui::Node* bar = own != nullptr ? own->GetParent() : nullptr;
        if (bar == nullptr || LoadFailed())
            return;
        m_presets = MakeRef<gui::ToolMenuButton>(Allocator());
        m_presets->SetId(u8"texture-presets");
        m_presets->SetText(u8"Preset");
        m_presets->SetTooltip(u8"Set what a kind of texture needs (usage, colour space, shape, filters, wraps, mips) in one step");
        TextureEditorPage* self = this;
        for (const Preset& preset : Presets())
        {
            const StringView label = preset.label;
            (void)m_presets->GetMenu().AddItem(label, [self, label]() { (void)self->ApplyPreset(label); });
        }
        bar->AddChildAt(m_presets.Get(), bar->GetChildIndex(own) + 1);
    }

    bool TextureEditorPage::ApplyPreset(StringView label)
    {
        for (const Preset& preset : Presets())
        {
            if (preset.label != label)
                continue;
            void (pipeline::TextureAsset::*setup)() = preset.setup;
            EditWhole(u8"texture_preset", [setup](ISerializable& object) { (static_cast<pipeline::TextureAsset&>(object).*setup)(); });
            return true;
        }
        return false;
    }

    bool TextureEditorPage::FollowEdit(StringView path, ISerializable& object)
    {
        // Usage is the main choice and the colour space follows it (data is linear, colour is
        // sRGB), so the two change as one step. Colour Space keeps its own row for the rare
        // override.
        if (path != StringView(u8"usage"))
            return false;
        auto& asset = static_cast<pipeline::TextureAsset&>(object);
        asset.colorSpace = asset.usage == texcomp::TextureUsage::Color ? image::ImageColorSpace::Srgb : image::ImageColorSpace::Linear;
        return true;
    }

    void TextureEditorPage::LoadPreview(foundation::content::Instance& instance)
    {
        const pipeline::TextureAsset* asset = ObjectAs<pipeline::TextureAsset>();
        if (asset == nullptr)
            return;
        // Embedded (from a model's import): the "pixels" stream, raw RGBA8 at embeddedWidth by
        // embeddedHeight.
        if (asset->fileName.IsEmpty() && asset->embeddedWidth > 0 && asset->embeddedHeight > 0)
        {
            const usize expected = static_cast<usize>(asset->embeddedWidth) * asset->embeddedHeight * 4;
            UniquePtr<IStream> stream = instance.ReadData(u8"pixels");
            image::Image pixels(asset->embeddedWidth, asset->embeddedHeight, image::PixelFormat::RGBA8);
            if (!stream || stream->Size() < static_cast<i64>(expected) || stream->Read(pixels.PixelDataMut().Data(), expected) != expected)
            {
                if (m_preview)
                    m_preview->ShowNone(u8"Its embedded pixels did not read.");
                return;
            }
            m_sourceFormat = image::PixelFormat::RGBA8;
            if (m_preview)
                m_preview->Show(Move(pixels));
            return;
        }
        if (asset->fileName.IsEmpty() || Context().Project() == nullptr)
        {
            if (m_preview)
                m_preview->ShowNone(u8"This texture names no source file.");
            return;
        }
        // An external file (the +X face stands in for a cubemap's).
        const String path = PathJoin(Context().Project()->SourcesRoot().AsView(), asset->fileName.View());
        image::Image source;
        image::Image shown;
        if (image::io::LoadImage(path.AsView(), source).IsOk())
            shown = ToPreview(source);
        if (shown.Width() == 0)
        {
            LOG_WARNING(u8"Editor", u8"texture source missing or undecodable: {}", path);
            if (m_preview)
                m_preview->ShowNone(Format(u8"{} is missing or did not decode.", asset->fileName.View()).AsView());
            return;
        }
        m_sourceFormat = source.Format();
        if (m_preview)
            m_preview->Show(Move(shown));
    }

    void TextureEditorPage::ShowFacts()
    {
        const pipeline::TextureAsset* asset = ObjectAs<pipeline::TextureAsset>();
        if (asset == nullptr)
            return;
        // Data stored as sRGB warps its values; colour stored as linear is unusual.
        const bool dataAsSrgb = asset->usage != texcomp::TextureUsage::Color && asset->colorSpace == image::ImageColorSpace::Srgb;
        const bool colorAsLinear = asset->usage == texcomp::TextureUsage::Color && asset->colorSpace == image::ImageColorSpace::Linear;
        SetNotice(u8"colorSpace", dataAsSrgb      ? StringView(u8"Normal, mask and HDR maps are data: sRGB will warp their values. Set the colour space to Linear.")
                                  : colorAsLinear ? StringView(u8"Colour stored as Linear is unusual (fine for data authored as an image, a LUT).")
                                                  : StringView{});
        if (asset->fileName.IsEmpty() && asset->embeddedWidth > 0)
            SetFact(u8"Pixels", u8"Embedded in the asset (a model's import)");
        if (!asset->sourceHint.IsEmpty())
            SetFact(u8"Imported From", asset->sourceHint.AsView());
        const image::Image* pixels = m_preview && m_preview->View().HasImage() ? &m_preview->View().Pixels() : nullptr;
        if (pixels == nullptr)
            return;
        SetFact(u8"Size", Format(u8"{} x {}  ({})", pixels->Width(), pixels->Height(),
                                 m_sourceFormat == image::PixelFormat::RGBA32F ? StringView(u8"HDR") : StringView(u8"LDR"))
                              .AsView());
        // The mip chain the cook makes: down to 1x1 when it makes a chain at all.
        u32 mips = 1;
        if (asset->generateMipmaps)
            for (u32 w = pixels->Width(), h = pixels->Height(); w > 1 || h > 1; ++mips)
            {
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
        SetFact(u8"Mip Levels", Format(u8"{}", mips).AsView());
        SetFact(u8"Cooks To", ResolvedFormatText().AsView());
    }

    String TextureEditorPage::ResolvedFormatText() const
    {
        const pipeline::TextureAsset* asset = ObjectAs<pipeline::TextureAsset>();
        if (asset == nullptr)
            return String(u8"-");
        // The same inputs the cook uses: the sRGB flag, the level-0 alpha scan and the source
        // size. An HDR source resolves through the policy's HDR branch, which stays uncompressed
        // until BC6H is supported.
        const bool srgb = asset->colorSpace == image::ImageColorSpace::Srgb;
        const image::Image* pixels = m_preview && m_preview->View().HasImage() ? &m_preview->View().Pixels() : nullptr;
        const u32 width = pixels != nullptr ? pixels->Width() : 0;
        const u32 height = pixels != nullptr ? pixels->Height() : 0;
        bool hasAlpha = false;
        if (pixels != nullptr)
        {
            const Span<const u8> px = pixels->PixelData();
            for (usize i = 0; i + 3 < px.Size() && !hasAlpha; i += 4)
                hasAlpha = px[i + 3] != 255;
        }
        const foundation::rhi::TextureFormat uncompressed =
            m_sourceFormat == image::PixelFormat::RGBA32F ? foundation::rhi::TextureFormat::RGBA32Float : foundation::rhi::TextureFormat::RGBA8Unorm;
        const bool multiChannel = pixels != nullptr && texcomp::HasDistinctChannels(pixels->PixelData().Data(), width, height);
        const foundation::rhi::TextureFormat desktop = texcomp::ResolveCompressedFormat(
            asset->usage, srgb, hasAlpha, multiChannel, asset->compression, width, height, texcomp::DesktopProfile(), uncompressed);
        const foundation::rhi::TextureFormat mobile = texcomp::ResolveCompressedFormat(
            asset->usage, srgb, hasAlpha, multiChannel, asset->compression, width, height, texcomp::MobileProfile(), uncompressed);
        if (desktop == mobile)
            return String(CookedFormatName(desktop));
        return Format(u8"{}  ·  mobile: {}", CookedFormatName(desktop), CookedFormatName(mobile));
    }

    const TypeInfo* TextureEditorPageFactory::PrimaryType() const
    {
        return &pipeline::TextureAsset::StaticType();
    }

    UniquePtr<EditorPage> TextureEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<TextureEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
