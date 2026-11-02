// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :image_file_preview partition, its body. See ImageFilePreview.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.app;

import foundation.core;
import foundation.image;
import foundation.image.io;
import foundation.gui;
import foundation.gui.toolkit;
import pipeline.importer; // FileNameOf
import editor.core;
import :image_inspect_view;
import :image_file_preview;

using namespace foundation::core;

namespace editor::app
{
    namespace image = foundation::image;

    namespace
    {
        // The channels as the file stores them, and their depth: "RGBA, 8-bit".
        [[nodiscard]] String ChannelsText(const image::io::ImageFileInfo& info)
        {
            StringView channels = u8"RGBA";
            switch (info.channels)
            {
            case 1: channels = u8"Grey"; break;
            case 2: channels = u8"Grey and alpha"; break;
            case 3: channels = u8"RGB"; break;
            default: break;
            }
            const StringView depth = info.hdr ? StringView(u8"HDR") : info.sixteenBit ? StringView(u8"16-bit") : StringView(u8"8-bit");
            return Format(u8"{}, {}", channels, depth);
        }

        // Whether any pixel is less than opaque (an RGBA8 picture).
        [[nodiscard]] bool UsesAlpha(const image::Image& pixels)
        {
            const Span<const u8> bytes = pixels.PixelData();
            for (usize i = 3; i < bytes.Size(); i += 4)
                if (bytes[i] != 255)
                    return true;
            return false;
        }

        // The bar over the picture and the stage it shows in, with the ids ImagePreview binds.
        [[nodiscard]] RefPtr<gui::Node> BuildView(IAllocator& allocator)
        {
            RefPtr<gui::Panel> root = MakeRef<gui::Panel>(allocator);
            root->AddClass(u8"import-image");
            RefPtr<gui::Panel> bar = MakeRef<gui::Panel>(allocator);
            bar->AddClass(u8"panel-toolbar");
            bar->AddClass(u8"image-bar");
            RefPtr<gui::ToolButton> fit = MakeRef<gui::ToolButton>(allocator);
            fit->SetId(u8"image-fit");
            fit->SetIconSource(String(u8"Editor/Icons/maximize.svg"));
            fit->SetTooltip(u8"Fit the picture (double click)");
            RefPtr<gui::ToolButton> actual = MakeRef<gui::ToolButton>(allocator);
            actual->SetId(u8"image-actual");
            actual->SetText(u8"1:1");
            actual->SetTooltip(u8"One pixel to one pixel");
            RefPtr<gui::Label> zoom = MakeRef<gui::Label>(allocator);
            zoom->SetId(u8"image-zoom");
            zoom->AddClass(u8"mono");
            zoom->AddClass(u8"image-zoom");
            RefPtr<gui::ComboBox> channel = MakeRef<gui::ComboBox>(allocator);
            channel->SetId(u8"image-channel");
            channel->AddClass(u8"image-channel");
            const char8_t* const channels[] = {u8"RGBA", u8"RGB", u8"Red", u8"Green", u8"Blue", u8"Alpha"}; // ImageChannel's order
            for (const char8_t* item : channels)
                channel->AddItem(item);
            channel->SetSelectedIndex(0);
            channel->SetTooltip(u8"The channels shown");
            RefPtr<gui::Label> readout = MakeRef<gui::Label>(allocator);
            readout->SetId(u8"image-readout");
            readout->AddClass(u8"mono");
            readout->AddClass(u8"image-readout");
            bar->AddChild(fit.Get());
            bar->AddChild(actual.Get());
            bar->AddChild(zoom.Get());
            bar->AddChild(channel.Get());
            bar->AddChild(MakeRef<gui::Spacer>(allocator).Get());
            bar->AddChild(readout.Get());
            RefPtr<gui::Panel> stage = MakeRef<gui::Panel>(allocator);
            stage->SetId(u8"image-stage");
            stage->AddClass(u8"image-stage");
            RefPtr<gui::Panel> empty = MakeRef<gui::Panel>(allocator);
            empty->SetId(u8"image-empty");
            empty->AddClass(u8"state-box");
            empty->AddClass(u8"image-empty");
            RefPtr<gui::Label> why = MakeRef<gui::Label>(allocator);
            why->SetId(u8"image-empty-text");
            why->AddClass(u8"state-text");
            empty->AddChild(why.Get());
            root->AddChild(bar.Get());
            root->AddChild(stage.Get());
            root->AddChild(empty.Get());
            return RefPtr<gui::Node>(root);
        }
    }

    ImageFilePreview::ImageFilePreview(StringView path, editor::ImportPreviewReader& reader, IAllocator& allocator)
        : m_root(BuildView(allocator)), m_facts(allocator)
    {
        m_picture = MakeUnique<ImagePreview>(allocator, *m_root);
        m_picture->ShowNone(Format(u8"Reading {}...", pipeline::FileNameOf(path)).AsView());
        RefPtr<Decoded> decoded = MakeRef<Decoded>(allocator);
        ImageFilePreview* self = this;
        // On the worker: only the file and `decoded`, which nothing else touches until it is back.
        m_read = reader.Read([decoded, file = String(path)]()
                             {
                                 image::Image source;
                                 if (!image::io::LoadImage(file.AsView(), source).IsOk() || source.Width() == 0 || source.Height() == 0)
                                     return;
                                 // The display is 8-bit: converted through the image library (floats clamp).
                                 decoded->pixels = source.Format() == image::PixelFormat::RGBA8 ? Move(source) : source.ConvertFormat(image::PixelFormat::RGBA8);
                                 decoded->facts.PushBack(Format(u8"{} × {}", decoded->pixels.Width(), decoded->pixels.Height()));
                                 image::io::ImageFileInfo info;
                                 if (image::io::ReadImageFileInfo(file.AsView(), info))
                                 {
                                     decoded->facts.PushBack(ChannelsText(info));
                                     if (info.channels == 2 || info.channels == 4)
                                         decoded->facts.PushBack(String(UsesAlpha(decoded->pixels) ? StringView(u8"Uses alpha") : StringView(u8"Alpha unused")));
                                 }
                                 decoded->ok = true;
                             },
                             // On the main thread, and only while this preview lives (it cancels the read as it goes).
                             [self, decoded]() { self->Show(*decoded); });
    }

    ImageFilePreview::~ImageFilePreview()
    {
        if (m_read)
            m_read->Cancel();
    }

    void ImageFilePreview::Show(Decoded& decoded)
    {
        if (!decoded.ok)
        {
            m_picture->ShowNone(u8"This file did not decode as a picture.");
        }
        else
        {
            m_facts = Move(decoded.facts);
            m_picture->Show(Move(decoded.pixels));
        }
        if (onChanged)
            onChanged();
    }

    UniquePtr<editor::IImportPreview> ImageFilePreviewFactory::Create(const editor::ImportPreviewSource& file, editor::ImportPreviewReader& reader, IAllocator& allocator)
    {
        return UniquePtr<editor::IImportPreview>(allocator.New<ImageFilePreview>(file.path, reader, allocator), allocator);
    }

    void RegisterImageFilePreview(editor::EditorContext& context, const TypeInfo& type)
    {
        context.ImportPreviews().Register(UniquePtr<editor::IImportPreviewFactory>(
            editor::EditorRootAllocator().New<ImageFilePreviewFactory>(type), editor::EditorRootAllocator()));
    }
}
