// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Image - the `editor.image` module (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.image;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import image.pipeline;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace image = foundation::image;

namespace editor
{
    namespace
    {
        [[nodiscard]] StringView PixelFormatLabel(image::PixelFormat format)
        {
            switch (format)
            {
            case image::PixelFormat::R8:
                return u8"R8";
            case image::PixelFormat::RG8:
                return u8"RG8";
            case image::PixelFormat::RGB8:
                return u8"RGB8";
            case image::PixelFormat::RGBA8:
                return u8"RGBA8";
            case image::PixelFormat::RGBA32F:
                return u8"RGBA32F (HDR)";
            default:
                return u8"?";
            }
        }
    } // namespace

    ImageEditorPage::ImageEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::ImageAsset::StaticType(), kPath)
    {
        if (Content())
            m_preview = MakeUnique<app::ImagePreview>(Allocator(), *Content());
        LoadPreview();
    }

    void ImageEditorPage::LoadPreview()
    {
        const pipeline::ImageAsset* asset = ObjectAs<pipeline::ImageAsset>();
        if (asset == nullptr)
            return;
        if (asset->fileName.IsEmpty() || Context().Project() == nullptr)
        {
            if (m_preview)
                m_preview->ShowNone(u8"This image names no source file.");
            return;
        }
        const String path = PathJoin(Context().Project()->SourcesRoot().AsView(), asset->fileName.View());
        image::Image source;
        if (!image::io::LoadImage(path.AsView(), source).IsOk() || source.Width() == 0 || source.Height() == 0)
        {
            LOG_WARNING(u8"Editor", u8"image source missing or undecodable: {}", path);
            if (m_preview)
                m_preview->ShowNone(Format(u8"{} is missing or did not decode.", asset->fileName.View()).AsView());
            return;
        }
        SetFact(u8"Dimensions", Format(u8"{} x {}", source.Width(), source.Height()).AsView());
        SetFact(u8"Pixel Format", PixelFormatLabel(source.Format()));
        SetFact(u8"Data Size", Format(u8"{} KiB", source.PixelData().Size() / 1024).AsView());
        // The display is 8-bit: converted through the image library (floats clamp).
        if (m_preview)
            m_preview->Show(source.Format() == image::PixelFormat::RGBA8 ? Move(source) : source.ConvertFormat(image::PixelFormat::RGBA8));
    }

    const TypeInfo* ImageEditorPageFactory::PrimaryType() const
    {
        return &pipeline::ImageAsset::StaticType();
    }

    UniquePtr<EditorPage> ImageEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<ImageEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
