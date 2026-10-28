// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Hosting - :picture_baker partition: GpuPictureBaker, the GUI's IPictureBaker on the GPU.
//
// A picture the GUI asks for (an SVG at a size in device pixels) is queued; the host's frame
// records the queue (Record, on the frame's own command encoder, before the passes that sample
// it): each picture drawn through a VG renderer of its own into a texture of its own, which every
// window's renderer (its targets) is told to sample under the picture's key
// (RegisterExternalTexture), so a picture baked once shows in any window. Nothing is
// read back, so nothing waits: the frame that asked draws the picture as paths, and the next one
// draws it from the texture. The texture holds premultiplied colour in the canvas's blend space,
// as a layer's target does, so the GUI draws it with VGContext::DrawImagePremultiplied.
//
// A bake no draw has asked for in kKeepFrames frames goes (the GUI's renderer forgets it first):
// that long unasked, no frame still in flight samples it. A window that goes takes its renderer
// out of the targets first (RemoveTarget), which forgets every bake. Bakes draw without stencil fills (the
// bake targets have no depth-stencil), as a context without a stencil-capable renderer does.
//
// Small pictures (icons) go into an atlas instead (AtlasRegion): pages of kAtlasPageSize pixels,
// each picture in a place of its own with a transparent gutter round it, so pictures drawn one
// after another from a page are one draw. The pictures asked for since the last frame are drawn
// into their pages, one pass a page, each clipped to its place; a mask picture is drawn white, to
// take its tint when it is drawn. A page only fills: when no page has room, the atlas is packed
// anew from the pictures asked for in the last kRecentFrames frames, its pages cleared and those
// pictures drawn again (meanwhile drawn as paths), and one more page is added while there are
// fewer than kMostAtlasPages.

module;
#include "Core/Prelude.h"

export module foundation.gui.hosting:picture_baker;

import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.fonts;
import foundation.vg;
import foundation.vg.svg;
import foundation.vg.renderer;
import foundation.gui;
import :atlas_packer;

using namespace foundation::core;
namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace image = foundation::image;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    /// What a baker's renderer draws with: the VG shaders the GUI's renderer has (gradients and
    /// boxes as it draws them; none of them but the first two required), the format and blend
    /// space of the GUI's target (a bake holds colour as that target's layers do), and the frames
    /// in flight.
    struct PictureBakerSetup
    {
        rhi::ShaderModule* VertexShader = nullptr;
        rhi::ShaderModule* FragmentShader = nullptr;
        rhi::ShaderModule* GradientRadialShader = nullptr;
        rhi::ShaderModule* GradientConicShader = nullptr;
        rhi::ShaderModule* DistanceFieldShader = nullptr;
        rhi::TextureFormat Format = rhi::TextureFormat::RGBA8UnormSrgb;
        vg::VGBlendSpace BlendSpace = vg::VGBlendSpace::Linear;
        i32 FrameCount = 2;
    };

    /// GpuPictureBaker::kAtlasGutter, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kGpuPictureBakerAtlasGutter = 1;

    /// GpuPictureBaker::kAtlasPageSize, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kGpuPictureBakerAtlasPageSize = 1024;

    /// GpuPictureBaker::kKeepFrames, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u64 kGpuPictureBakerKeepFrames = 600;

    /// GpuPictureBaker::kMostAtlasPages, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr usize kGpuPictureBakerMostAtlasPages = 4;

    /// GpuPictureBaker::kRecentFrames, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u64 kGpuPictureBakerRecentFrames = 60;

    class GpuPictureBaker final : public IPictureBaker
    {
    public:
        /// Frames a bake stays after a draw last asked for it.
        static constexpr u64 kKeepFrames = kGpuPictureBakerKeepFrames;
        /// An atlas page's side, the gutter round each picture on it, and the most pages.
        static constexpr u32 kAtlasPageSize = kGpuPictureBakerAtlasPageSize;
        static constexpr u32 kAtlasGutter = kGpuPictureBakerAtlasGutter;
        static constexpr usize kMostAtlasPages = kGpuPictureBakerMostAtlasPages;
        /// Packing a full atlas anew keeps the pictures asked for in this many frames.
        static constexpr u64 kRecentFrames = kGpuPictureBakerRecentFrames;

        /// `fonts` draws an SVG's text. The renderers that sample the bakes are its targets.
        GpuPictureBaker(IAllocator& allocator, rhi::Device& device, foundation::fonts::IFontService* fonts) noexcept
            : m_allocator(allocator), m_device(device), m_renderer(allocator), m_canvas(fonts), m_targets(allocator),
              m_entries(allocator), m_pages(allocator), m_atlas(allocator)
        {
        }
        ~GpuPictureBaker() override { Dispose(); }
        GpuPictureBaker(const GpuPictureBaker&) = delete;
        GpuPictureBaker& operator=(const GpuPictureBaker&) = delete;

        [[nodiscard]] Status Initialize(const PictureBakerSetup& setup)
        {
            if (setup.VertexShader == nullptr || setup.FragmentShader == nullptr)
                return Status{ErrorCode::InvalidArgument};
            m_format = setup.Format;
            vg::renderer::VGTargetConfig config;
            config.blendSpace = setup.BlendSpace;
            if (Status status = m_renderer.Initialize(m_device, *setup.VertexShader, *setup.FragmentShader, setup.Format,
                                                      setup.FrameCount, setup.DistanceFieldShader, setup.GradientRadialShader,
                                                      setup.GradientConicShader, config);
                !status.IsOk())
                return status;
            m_canvas.SetPerPixelGradients(setup.GradientRadialShader != nullptr && setup.GradientConicShader != nullptr);
            return Status{ErrorCode::Ok};
        }

        /// Forgets every bake (its targets first) and releases the renderer. The GPU must be done
        /// with them (a host disposes after its device is idle).
        void Dispose()
        {
            for (const UniquePtr<Entry>& entry : m_entries)
                Release(*entry);
            m_entries.Clear();
            for (const UniquePtr<AtlasPage>& page : m_pages)
                Release(*page);
            m_pages.Clear();
            m_atlas.Clear();
            m_targets.Clear();
            if (m_renderer.IsInitialized())
                m_renderer.Dispose();
        }

        /// A renderer that samples the bakes (a window's): every bake drawn is registered with it,
        /// those drawn before too.
        /// The fonts an SVG's text draws with from now on (the host's changed: a project's own).
        void SetFontService(foundation::fonts::IFontService* fonts) noexcept { m_canvas.SetFontService(fonts); }

        void AddTarget(vg::renderer::VGRenderer& target)
        {
            for (vg::renderer::VGRenderer* known : m_targets)
                if (known == &target)
                    return;
            m_targets.PushBack(&target);
            for (const UniquePtr<Entry>& entry : m_entries)
                if (entry->View != nullptr)
                    target.RegisterExternalTexture(entry->Key.Get(), entry->View);
            for (const UniquePtr<AtlasPage>& page : m_pages)
                if (page->View != nullptr)
                    target.RegisterExternalTexture(page->Key.Get(), page->View);
        }
        /// A renderer that goes (its window's): it forgets every bake, and is told of no more.
        void RemoveTarget(vg::renderer::VGRenderer& target)
        {
            for (usize i = 0; i < m_targets.Size(); ++i)
                if (m_targets[i] == &target)
                {
                    for (const UniquePtr<Entry>& entry : m_entries)
                        if (entry->View != nullptr)
                            target.UnregisterExternalTexture(entry->Key.Get());
                    for (const UniquePtr<AtlasPage>& page : m_pages)
                        if (page->View != nullptr)
                            target.UnregisterExternalTexture(page->Key.Get());
                    m_targets.RemoveAt(i);
                    return;
                }
        }

        const image::ImageData* Baked(const foundation::vg::svg::SVGDocument& document, u32 width, u32 height) override
        {
            for (const UniquePtr<Entry>& entry : m_entries)
                if (entry->Document == &document && entry->Width == width && entry->Height == height)
                {
                    entry->LastAsked = m_frame;
                    return entry->View != nullptr ? entry->Key.Get() : nullptr;
                }
            UniquePtr<Entry> entry = MakeUnique<Entry>(m_allocator);
            entry->Document = &document;
            entry->Width = width;
            entry->Height = height;
            entry->LastAsked = m_frame;
            entry->Key = MakeUnique<image::ImageDataRef>(m_allocator, width, height);
            m_entries.PushBack(Move(entry));
            return nullptr;
        }

        Optional<PictureRegion> AtlasRegion(const foundation::vg::svg::SVGDocument& document, u32 width, u32 height, bool mask) override
        {
            if (width == 0 || height == 0 || width > kLargestAtlasPicture || height > kLargestAtlasPicture)
                return {};
            for (const UniquePtr<AtlasEntry>& entry : m_atlas)
                if (entry->Document == &document && entry->Width == width && entry->Height == height && entry->Mask == mask)
                {
                    entry->LastAsked = m_frame;
                    if (!entry->Baked)
                        return {};
                    return PictureRegion{m_pages[entry->Page]->Key.Get(),
                                         Rectangle{static_cast<f32>(entry->X), static_cast<f32>(entry->Y), static_cast<f32>(width),
                                                   static_cast<f32>(height)}};
                }
            UniquePtr<AtlasEntry> entry = MakeUnique<AtlasEntry>(m_allocator);
            entry->Document = &document;
            entry->Width = width;
            entry->Height = height;
            entry->Mask = mask;
            entry->LastAsked = m_frame;
            if (!Place(*entry) && !(PackAnew() && Place(*entry)))
                return {}; // no room even packed anew: drawn as paths
            m_atlas.PushBack(Move(entry));
            return {};
        }

        [[nodiscard]] u64 Generation() const override { return m_generation; }

        /// Draws the pictures asked for since the last frame into textures, on `encoder` before the
        /// frame's passes that sample them, after the host waited for `frameIndex`'s last use (as
        /// it does before its own renderer's BeginFrame); and lets the bakes go that no draw has
        /// asked for in kKeepFrames frames. True when any baked: the GUI redraws (Generation).
        bool Record(rhi::CommandEncoder& encoder, i32 frameIndex)
        {
            ++m_frame;
            for (usize i = m_entries.Size(); i > 0; --i)
                if (m_frame - m_entries[i - 1]->LastAsked > kKeepFrames)
                {
                    Release(*m_entries[i - 1]);
                    m_entries.RemoveAt(i - 1);
                }
            for (usize i = m_atlas.Size(); i > 0; --i)
                if (m_frame - m_atlas[i - 1]->LastAsked > kKeepFrames)
                    m_atlas.RemoveAt(i - 1); // its place stays empty until the atlas is packed anew
            m_packedThisFrame = false;
            bool begun = false;
            bool baked = false;
            const auto begin = [&]()
            {
                if (!begun)
                {
                    m_renderer.BeginFrame(frameIndex);
                    begun = true;
                }
            };
            for (const UniquePtr<Entry>& entry : m_entries)
            {
                if (entry->View != nullptr || !m_renderer.IsInitialized())
                    continue;
                begin();
                baked = Bake(encoder, frameIndex, *entry) || baked;
            }
            for (usize page = 0; page < m_pages.Size() && m_renderer.IsInitialized(); ++page)
            {
                bool waiting = false;
                for (const UniquePtr<AtlasEntry>& entry : m_atlas)
                    waiting = waiting || (entry->Page == page && !entry->Baked);
                if (!waiting)
                    continue;
                begin();
                baked = BakePage(encoder, frameIndex, page) || baked;
            }
            if (baked)
                ++m_generation;
            return baked;
        }

        /// The bakes kept (drawn or waiting).
        [[nodiscard]] usize BakeCount() const noexcept { return m_entries.Size(); }
        /// The atlas's pictures kept (drawn or waiting) and its pages.
        [[nodiscard]] usize AtlasPictureCount() const noexcept { return m_atlas.Size(); }
        [[nodiscard]] usize AtlasPageCount() const noexcept { return m_pages.Size(); }

    private:
        struct Entry
        {
            const foundation::vg::svg::SVGDocument* Document = nullptr; // the key, with the size
            u32 Width = 0;
            u32 Height = 0;
            u64 LastAsked = 0;
            UniquePtr<image::ImageDataRef> Key; // what the GUI's renderer knows the texture by
            rhi::Texture* Texture = nullptr;
            rhi::TextureView* View = nullptr; // set once drawn
        };

        struct AtlasPage
        {
            explicit AtlasPage(IAllocator& allocator) : Packer(allocator, kAtlasPageSize, kAtlasPageSize) {}
            ShelfPacker Packer;
            UniquePtr<image::ImageDataRef> Key; // what the GUI's renderer knows the page by
            rhi::Texture* Texture = nullptr;
            rhi::TextureView* View = nullptr;
            bool Clear = true; // drawn into next with the page cleared first (new, or packed anew)
        };
        struct AtlasEntry
        {
            const foundation::vg::svg::SVGDocument* Document = nullptr; // the key, with the size and mask
            u32 Width = 0;
            u32 Height = 0;
            bool Mask = false;
            u64 LastAsked = 0;
            usize Page = 0;
            u32 X = 0; // its place on the page, inside its gutter
            u32 Y = 0;
            bool Baked = false;
        };

        // A place on a page for `entry`, a page added when none has room and the atlas may grow.
        bool Place(AtlasEntry& entry)
        {
            const u32 width = entry.Width + 2 * kAtlasGutter, height = entry.Height + 2 * kAtlasGutter;
            for (usize i = 0; i <= m_pages.Size(); ++i)
            {
                if (i == m_pages.Size())
                {
                    if (m_pages.Size() >= kMostAtlasPages)
                        return false;
                    UniquePtr<AtlasPage> page = MakeUnique<AtlasPage>(m_allocator, m_allocator);
                    page->Key = MakeUnique<image::ImageDataRef>(m_allocator, kAtlasPageSize, kAtlasPageSize);
                    m_pages.PushBack(Move(page));
                }
                if (const Optional<PackedPlace> at = m_pages[i]->Packer.Place(width, height); at.HasValue())
                {
                    entry.Page = i;
                    entry.X = at.Value().X + kAtlasGutter;
                    entry.Y = at.Value().Y + kAtlasGutter;
                    entry.Baked = false;
                    return true;
                }
            }
            return false;
        }

        // The atlas full: its pages emptied, the pictures asked for lately placed again (to be
        // drawn again), the rest forgotten. Once a frame at most; false when it already was.
        bool PackAnew()
        {
            if (m_packedThisFrame)
                return false;
            m_packedThisFrame = true;
            for (const UniquePtr<AtlasPage>& page : m_pages)
            {
                page->Packer.Reset();
                page->Clear = true;
            }
            for (usize i = m_atlas.Size(); i > 0; --i)
                if (m_frame - m_atlas[i - 1]->LastAsked > kRecentFrames || !Place(*m_atlas[i - 1]))
                    m_atlas.RemoveAt(i - 1);
            return true;
        }

        // The pictures waiting on page `index`, drawn into it in one pass, each clipped to its place.
        bool BakePage(rhi::CommandEncoder& encoder, i32 frameIndex, usize index)
        {
            AtlasPage& page = *m_pages[index];
            const bool fresh = page.Texture == nullptr;
            if (fresh)
            {
                rhi::TextureDesc desc{};
                desc.dimension = rhi::TextureDimension::Texture2D;
                desc.format = m_format;
                desc.width = kAtlasPageSize;
                desc.height = kAtlasPageSize;
                desc.depth = 1;
                desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
                desc.label = u8"picture atlas";
                if (!m_device.CreateTexture(desc, page.Texture).IsOk() || page.Texture == nullptr ||
                    !m_device.CreateTextureView(page.Texture, rhi::TextureViewDesc{}, page.View).IsOk() || page.View == nullptr)
                {
                    Release(page);
                    return false;
                }
            }
            m_canvas.Clear();
            for (const UniquePtr<AtlasEntry>& entry : m_atlas)
            {
                if (entry->Page != index || entry->Baked)
                    continue;
                const Rectangle place{static_cast<f32>(entry->X), static_cast<f32>(entry->Y), static_cast<f32>(entry->Width),
                                      static_cast<f32>(entry->Height)};
                m_canvas.PushClipRect(place);
                vg::svg::SVGRenderer::Render(m_canvas, *entry->Document, place, entry->Mask ? Optional<Color>{Color::White} : Optional<Color>{});
                m_canvas.PopClip();
            }
            const vg::renderer::VGRenderSlice slice = m_renderer.Prepare(m_canvas.GetBatch(), frameIndex, kAtlasPageSize, kAtlasPageSize);
            encoder.TransitionTexture(page.Texture, fresh ? rhi::ResourceState::Undefined : rhi::ResourceState::ShaderRead,
                                      rhi::ResourceState::RenderTarget);
            rhi::ColorAttachment color{};
            color.view = page.View;
            color.loadOp = page.Clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
            color.storeOp = rhi::StoreOp::Store;
            color.clearValue = rhi::ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            rhi::RenderPassDesc pass{};
            pass.colorAttachments.Add(color);
            pass.label = u8"picture atlas";
            if (rhi::RenderPassEncoder* encoderPass = encoder.BeginRenderPass(pass))
            {
                if (slice.isValid)
                    m_renderer.Render(*encoderPass, kAtlasPageSize, kAtlasPageSize, frameIndex, slice);
                encoderPass->End();
            }
            encoder.TransitionTexture(page.Texture, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderRead);
            page.Clear = false;
            if (fresh)
                for (vg::renderer::VGRenderer* target : m_targets)
                    target->RegisterExternalTexture(page.Key.Get(), page.View);
            for (const UniquePtr<AtlasEntry>& entry : m_atlas)
                if (entry->Page == index)
                    entry->Baked = true;
            return true;
        }

        void Release(AtlasPage& page)
        {
            if (page.View != nullptr)
            {
                for (vg::renderer::VGRenderer* target : m_targets)
                    target->UnregisterExternalTexture(page.Key.Get());
                m_device.DestroyTextureView(page.View);
                page.View = nullptr;
            }
            if (page.Texture != nullptr)
            {
                m_device.DestroyTexture(page.Texture);
                page.Texture = nullptr;
            }
        }

        bool Bake(rhi::CommandEncoder& encoder, i32 frameIndex, Entry& entry)
        {
            rhi::TextureDesc desc{};
            desc.dimension = rhi::TextureDimension::Texture2D;
            desc.format = m_format;
            desc.width = entry.Width;
            desc.height = entry.Height;
            desc.depth = 1;
            desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
            desc.label = u8"picture bake";
            if (!m_device.CreateTexture(desc, entry.Texture).IsOk() || entry.Texture == nullptr ||
                !m_device.CreateTextureView(entry.Texture, rhi::TextureViewDesc{}, entry.View).IsOk() || entry.View == nullptr)
            {
                Release(entry);
                return false;
            }
            m_canvas.Clear();
            vg::svg::SVGRenderer::Render(m_canvas, *entry.Document,
                                         Rectangle{0.0f, 0.0f, static_cast<f32>(entry.Width), static_cast<f32>(entry.Height)});
            const vg::renderer::VGRenderSlice slice = m_renderer.Prepare(m_canvas.GetBatch(), frameIndex, entry.Width, entry.Height);
            encoder.TransitionTexture(entry.Texture, rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget);
            rhi::ColorAttachment color{};
            color.view = entry.View;
            color.loadOp = rhi::LoadOp::Clear;
            color.storeOp = rhi::StoreOp::Store;
            color.clearValue = rhi::ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            rhi::RenderPassDesc pass{};
            pass.colorAttachments.Add(color);
            pass.label = u8"picture bake";
            if (rhi::RenderPassEncoder* encoderPass = encoder.BeginRenderPass(pass))
            {
                if (slice.isValid)
                    m_renderer.Render(*encoderPass, entry.Width, entry.Height, frameIndex, slice);
                encoderPass->End();
            }
            encoder.TransitionTexture(entry.Texture, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderRead);
            for (vg::renderer::VGRenderer* target : m_targets)
                target->RegisterExternalTexture(entry.Key.Get(), entry.View);
            return true;
        }

        void Release(Entry& entry)
        {
            if (entry.View != nullptr)
            {
                for (vg::renderer::VGRenderer* target : m_targets)
                    target->UnregisterExternalTexture(entry.Key.Get());
                m_device.DestroyTextureView(entry.View);
                entry.View = nullptr;
            }
            if (entry.Texture != nullptr)
            {
                m_device.DestroyTexture(entry.Texture);
                entry.Texture = nullptr;
            }
        }

        IAllocator& m_allocator;
        rhi::Device& m_device;
        vg::renderer::VGRenderer m_renderer; // the bakes' own: its frame buffers are not the GUI's
        vg::VGContext m_canvas;
        rhi::TextureFormat m_format = rhi::TextureFormat::RGBA8UnormSrgb;
        Array<vg::renderer::VGRenderer*> m_targets; // the windows' renderers, which sample the bakes
        Array<UniquePtr<Entry>> m_entries;
        Array<UniquePtr<AtlasPage>> m_pages;
        Array<UniquePtr<AtlasEntry>> m_atlas;
        bool m_packedThisFrame = false;
        u64 m_frame = 0;
        u64 m_generation = 0;
    };
}
