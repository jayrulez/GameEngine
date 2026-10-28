// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Heightfield - the `editor.heightfield` module (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.heightfield;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import heightfield.pipeline;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace image = foundation::image;

namespace editor
{
    namespace
    {
        // "12.34 m", "-0.50 m": a height to the centimetre (Format has no precision of its own).
        String Metres(f32 value)
        {
            const i64 hundredths = static_cast<i64>(Round(Abs(value) * 100.0f));
            const i64 cents = hundredths % 100;
            return Format(u8"{}{}.{}{} m", value < 0.0f && hundredths != 0 ? StringView(u8"-") : StringView{}, hundredths / 100, cents / 10, cents % 10);
        }
    }

    HeightfieldEditorPage::HeightfieldEditorPage(EditorContext& context, app::AssetPageServices services,
                                                 foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::HeightfieldAsset::StaticType(), kPath), m_samples(context.Allocator())
    {
        if (Content())
        {
            m_preview = MakeUnique<app::ImagePreview>(Allocator(), *Content());
            HeightfieldEditorPage* self = this;
            m_preview->SetDescribe([self](i32 x, i32 y)
                                   {
                                       u16 sample = 0;
                                       if (!self->SampleAt(x, y, sample))
                                           return Format(u8"x {}  y {}", x, y);
                                       return Format(u8"x {}  y {}  ·  {}  ({})", x, y, Metres(self->HeightOf(sample)), static_cast<u32>(sample));
                                   });
        }
        LoadPreview();
        ShowFacts();
    }

    bool HeightfieldEditorPage::SampleAt(i32 x, i32 y, u16& out) const
    {
        if (x < 0 || y < 0 || static_cast<u32>(x) >= m_width || static_cast<u32>(y) >= m_height)
            return false;
        out = m_samples[static_cast<usize>(y) * m_width + static_cast<usize>(x)];
        return true;
    }

    f32 HeightfieldEditorPage::HeightOf(u16 sample) const
    {
        const pipeline::HeightfieldAsset* asset = ObjectAs<pipeline::HeightfieldAsset>();
        if (asset == nullptr)
            return 0.0f;
        return asset->minY + (asset->maxY - asset->minY) * (static_cast<f32>(sample) / 65535.0f);
    }

    void HeightfieldEditorPage::LoadPreview()
    {
        const pipeline::HeightfieldAsset* asset = ObjectAs<pipeline::HeightfieldAsset>();
        if (asset == nullptr)
            return;
        if (asset->fileName.IsEmpty() || Context().Project() == nullptr)
        {
            if (m_preview)
                m_preview->ShowNone(u8"A blank heightfield: no source image, its heights flat until sculpted.");
            return;
        }
        const String path = PathJoin(Context().Project()->SourcesRoot().AsView(), asset->fileName.View());
        image::Image source;
        if (!image::io::LoadImage16(path.AsView(), source).IsOk() || source.Width() == 0 || source.Height() == 0)
        {
            LOG_WARNING(u8"Editor", u8"heightfield source missing or undecodable: {}", path);
            if (m_preview)
                m_preview->ShowNone(Format(u8"{} is missing or did not decode.", asset->fileName.View()).AsView());
            return;
        }
        m_width = source.Width();
        m_height = source.Height();
        const usize count = static_cast<usize>(m_width) * m_height;
        const u16* samples = reinterpret_cast<const u16*>(source.PixelData().Data());
        m_samples.Resize(count);
        m_low = 65535;
        m_high = 0;
        for (usize i = 0; i < count; ++i)
        {
            m_samples[i] = samples[i];
            m_low = Min(m_low, samples[i]);
            m_high = Max(m_high, samples[i]);
        }
        // Its lowest to its highest as black to white: a low relief over a few hundred samples
        // shows, where the high byte alone was one flat grey.
        const f32 span = m_high > m_low ? static_cast<f32>(m_high - m_low) : 1.0f;
        image::Image grey(m_width, m_height, image::PixelFormat::RGBA8);
        Span<u8> rgba = grey.PixelDataMut();
        for (usize i = 0; i < count; ++i)
        {
            const u8 g = static_cast<u8>(255.0f * static_cast<f32>(m_samples[i] - m_low) / span + 0.5f);
            rgba[i * 4 + 0] = g;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = g;
            rgba[i * 4 + 3] = 255;
        }
        if (m_preview)
            m_preview->Show(Move(grey));
    }

    void HeightfieldEditorPage::ShowFacts()
    {
        const pipeline::HeightfieldAsset* asset = ObjectAs<pipeline::HeightfieldAsset>();
        if (asset == nullptr)
            return;
        if (m_width == 0)
            return;
        SetFact(u8"Source Size", Format(u8"{} x {}", m_width, m_height).AsView());
        // The source's own range, in metres by the asset's Y range: what the black and the white are.
        SetFact(u8"Lowest", Metres(HeightOf(m_low)).AsView());
        SetFact(u8"Highest", Metres(HeightOf(m_high)).AsView());
    }

    const TypeInfo* HeightfieldEditorPageFactory::PrimaryType() const
    {
        return &pipeline::HeightfieldAsset::StaticType();
    }

    UniquePtr<EditorPage> HeightfieldEditorPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<HeightfieldEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
