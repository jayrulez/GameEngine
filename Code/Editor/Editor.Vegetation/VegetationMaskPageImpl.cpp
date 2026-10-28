// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - the `:mask_page` partition (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.vegetation;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.vegetation.resource; // kVegetationMaskStream, kMaxMaskPlanes
import vegetation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace image = foundation::image;

namespace editor
{
    namespace gui = foundation::gui;

    namespace
    {
        // "Plane 1 (red)": an imported mask's planes are its image's channels.
        String PlaneName(u32 plane, bool imported)
        {
            static constexpr StringView kChannels[] = {u8"red", u8"green", u8"blue", u8"alpha"};
            if (imported && plane < 4)
                return Format(u8"Plane {} ({})", plane + 1, kChannels[plane]);
            return Format(u8"Plane {}", plane + 1);
        }
    }

    VegetationMaskPage::VegetationMaskPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::VegetationMaskAsset::StaticType(), kPath), m_densities(context.Allocator())
    {
        ReadPlanes();
        VegetationMaskPage* self = this;
        // What the cook allows: a painted mask's size is its planes', an imported one's all its image's.
        Properties().SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                   {
                                       const bool size = path == StringView(u8"width") || path == StringView(u8"height");
                                       if (size || path == StringView(u8"planeCount"))
                                           info.ReadOnly = info.ReadOnly || self->m_imported || (size && self->m_painted);
                                       if (path == StringView(u8"planeCount"))
                                       {
                                           info.HasRange = true;
                                           info.Min = 1.0;
                                           info.Max = static_cast<f64>(foundation::vegetation::kMaxMaskPlanes);
                                       }
                                   });
        if (gui::IPropertySourceClient* client = Properties().GetClient())
            client->OnPropertiesChanged(); // the grid listed the rows before the hook was set
        if (Content())
        {
            m_preview = MakeUnique<app::ImagePreview>(Allocator(), *Content());
            m_preview->SetDescribe([self](i32 x, i32 y)
                                   {
                                       u8 density = 0;
                                       if (!self->DensityAt(x, y, density))
                                           return Format(u8"x {}  y {}", x, y);
                                       return Format(u8"x {}  y {}  ·  {}%", x, y, (static_cast<u32>(density) * 100u + 127u) / 255u);
                                   });
            // The plane in the channel's place: a plane is one value, not colours.
            if (gui::Node* channel = Find<gui::Node>(u8"image-channel"))
            {
                channel->SetDisplayed(false);
                m_planeChoice = MakeRef<gui::ComboBox>(Allocator());
                m_planeChoice->SetId(u8"mask-plane");
                m_planeChoice->AddClass(u8"image-channel");
                m_planeChoice->SetTooltip(u8"The density plane shown: one per vegetation layer that places by this mask");
                (void)m_planeChoice->AddEventListener(gui::EventType::Changed, [self](const gui::Event&)
                                                      { self->ShowPlane(static_cast<u32>(Max(0, self->m_planeChoice->GetSelectedIndex()))); });
                if (gui::Node* bar = channel->GetParent())
                    bar->AddChildAt(m_planeChoice.Get(), bar->GetChildIndex(channel) + 1);
            }
        }
        ShowChoices();
        ShowPlanePicture();
        ShowFacts();
    }

    void VegetationMaskPage::ReadPlanes()
    {
        m_densities.Clear();
        m_width = 0;
        m_height = 0;
        m_planeCount = 0;
        m_painted = false;
        m_imported = false;
        const pipeline::VegetationMaskAsset* asset = ObjectAs<pipeline::VegetationMaskAsset>();
        if (asset == nullptr || Context().Project() == nullptr)
            return;
        if (!asset->fileName.IsEmpty())
        {
            // Imported: its image at its own size, a plane per channel, as the cook decodes it.
            m_imported = true;
            const String path = PathJoin(Context().Project()->SourcesRoot().AsView(), asset->fileName.View());
            image::Image source;
            if (!image::io::LoadImage(path.AsView(), source).IsOk() || source.Format() != image::PixelFormat::RGBA8 || source.Width() == 0)
            {
                LOG_WARNING(u8"Editor", u8"vegetation mask source missing or not 8-bit RGBA: {}", path);
                return;
            }
            m_width = static_cast<i32>(source.Width());
            m_height = static_cast<i32>(source.Height());
            m_planeCount = 4;
            const usize texels = static_cast<usize>(m_width) * static_cast<usize>(m_height);
            m_densities.Resize(texels * 4u);
            const Span<const u8> rgba = source.PixelData();
            for (u32 plane = 0; plane < 4; ++plane)
                for (usize i = 0; i < texels; ++i)
                    m_densities[plane * texels + i] = rgba[i * 4u + plane];
            m_painted = true;
            return;
        }
        // Painted: its sidecar, at the asset's size (absent: never painted, nothing grows yet).
        m_width = Max(asset->width, 1);
        m_height = Max(asset->height, 1);
        m_planeCount = Clamp<u32>(asset->planeCount, 1u, foundation::vegetation::kMaxMaskPlanes);
        const usize planeBytes = static_cast<usize>(m_width) * static_cast<usize>(m_height);
        m_densities.Resize(planeBytes * m_planeCount, u8{0});
        foundation::content::Instance* instance = Context().Project()->SourceDb().GetInstance(InstanceId());
        UniquePtr<IStream> stream = instance != nullptr ? instance->ReadData(foundation::vegetation::kVegetationMaskStream) : UniquePtr<IStream>{};
        const i64 size = stream ? stream->Size() : 0;
        if (size <= 0 || static_cast<usize>(size) % planeBytes != 0)
            return;
        // The planes it has (the count may have changed since: the cook keeps those that remain).
        Array<u8> painted(Allocator());
        painted.Resize(static_cast<usize>(size));
        if (stream->Read(painted.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
            return;
        m_painted = true;
        MemCopy(m_densities.Data(), painted.Data(), Min(painted.Size(), m_densities.Size()));
    }

    bool VegetationMaskPage::DensityAt(i32 x, i32 y, u8& out) const
    {
        if (x < 0 || y < 0 || x >= m_width || y >= m_height || m_plane >= m_planeCount || m_densities.IsEmpty())
            return false;
        const usize planeBytes = static_cast<usize>(m_width) * static_cast<usize>(m_height);
        out = m_densities[m_plane * planeBytes + static_cast<usize>(y) * static_cast<usize>(m_width) + static_cast<usize>(x)];
        return true;
    }

    void VegetationMaskPage::ShowPlane(u32 plane)
    {
        m_plane = m_planeCount > 0 ? Min(plane, m_planeCount - 1) : 0u;
        if (m_planeChoice && m_planeChoice->GetSelectedIndex() != static_cast<i32>(m_plane))
            m_planeChoice->SetSelectedIndex(static_cast<i32>(m_plane));
        ShowPlanePicture();
        ShowFacts();
    }

    void VegetationMaskPage::ShowPlanePicture()
    {
        if (!m_preview)
            return;
        if (m_imported && m_densities.IsEmpty())
        {
            const pipeline::VegetationMaskAsset* asset = ObjectAs<pipeline::VegetationMaskAsset>();
            m_preview->ShowNone(Format(u8"{} is missing or is not an 8-bit RGBA image.", asset != nullptr ? asset->fileName.View() : StringView{}).AsView());
            return;
        }
        if (!m_painted)
        {
            m_preview->ShowNone(u8"Nothing painted yet: the scene view's Paint Vegetation tool paints it, over a terrain whose vegetation uses it.");
            return;
        }
        // None to full as black to white.
        const usize planeBytes = static_cast<usize>(m_width) * static_cast<usize>(m_height);
        image::Image grey(static_cast<u32>(m_width), static_cast<u32>(m_height), image::PixelFormat::RGBA8);
        Span<u8> rgba = grey.PixelDataMut();
        const u8* plane = m_densities.Data() + m_plane * planeBytes;
        for (usize i = 0; i < planeBytes; ++i)
        {
            rgba[i * 4 + 0] = plane[i];
            rgba[i * 4 + 1] = plane[i];
            rgba[i * 4 + 2] = plane[i];
            rgba[i * 4 + 3] = 255;
        }
        m_preview->Show(Move(grey));
    }

    void VegetationMaskPage::ShowChoices()
    {
        if (!m_planeChoice)
            return;
        String items;
        for (u32 plane = 0; plane < Max(m_planeCount, 1u); ++plane)
        {
            if (!items.IsEmpty())
                items.Append(StringView(u8"|"));
            items.Append(PlaneName(plane, m_imported).AsView());
        }
        m_planeChoice->SetItemsText(items);
        m_planeChoice->SetSelectedIndex(static_cast<i32>(m_plane));
    }

    void VegetationMaskPage::ShowFacts()
    {
        if (m_imported)
            SetFact(u8"Source Size", m_width > 0 ? Format(u8"{} x {}", m_width, m_height).AsView() : StringView(u8"Not read"));
        SetFact(u8"Painted", m_painted ? StringView(u8"Yes") : StringView(u8"Not yet"));
        if (m_painted && m_plane < m_planeCount)
        {
            // How much of the plane shown lets anything grow.
            const usize planeBytes = static_cast<usize>(m_width) * static_cast<usize>(m_height);
            const u8* plane = m_densities.Data() + m_plane * planeBytes;
            usize covered = 0;
            for (usize i = 0; i < planeBytes; ++i)
                covered += plane[i] != 0 ? 1u : 0u;
            SetFact(u8"Covered", Format(u8"{}%", planeBytes > 0 ? (covered * 100u + planeBytes / 2u) / planeBytes : 0u).AsView());
        }
        else
            SetFact(u8"Covered", u8"None");
        SetNotice(u8"size", m_painted && !m_imported ? StringView(u8"Its width and height are fixed once painted: its planes are kept at that size.")
                                                     : StringView{});
    }

    void VegetationMaskPage::OnObjectChanged()
    {
        // A plane count changed (or its undo): the planes as the cook would now read them.
        const u32 shown = m_plane;
        ReadPlanes();
        m_plane = m_planeCount > 0 ? Min(shown, m_planeCount - 1) : 0u;
        ShowChoices();
        ShowPlanePicture();
        ShowFacts();
    }

    const TypeInfo* VegetationMaskPageFactory::PrimaryType() const
    {
        return &pipeline::VegetationMaskAsset::StaticType();
    }

    UniquePtr<EditorPage> VegetationMaskPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<VegetationMaskPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterVegetationMaskEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(editor::EditorRootAllocator().New<VegetationMaskPageFactory>(services), editor::EditorRootAllocator()));
    }
}
