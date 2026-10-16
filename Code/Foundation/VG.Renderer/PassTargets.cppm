// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG.Renderer::PassTargets - :pass_targets partition.
//
// VGPassTargets: the targets VG's quality pass draws into, which the host provides (the
// stencil-then-cover design): a multisampled colour target resolved into the backbuffer, for edge
// anti-aliasing, and a stencil attachment, for complex fills and clip paths. Every host drawing
// VG with those features needs the same pair at its window's size, so they live here once: the
// UI runtime's windows and the samples share it. A renderer meant for this pass is initialized
// with ConfigFor's sample count and stencil format.

module;
#include "Core/Prelude.h"

export module foundation.vg.renderer:pass_targets;

import foundation.core;
import foundation.rhi;
import foundation.vg; // VGBlendSpace
import :renderer;     // VGTargetConfig, PickStencilCapableFormat

using namespace foundation::core;

export namespace foundation::vg::renderer
{
    class VGPassTargets
    {
    public:
        static constexpr u32 kDefaultSamples = 4;

        VGPassTargets() noexcept = default;
        VGPassTargets(const VGPassTargets&) = delete;
        VGPassTargets& operator=(const VGPassTargets&) = delete;
        ~VGPassTargets() { Destroy(); }

        /// Picks the stencil format `device` takes at `samples` (none: the targets stay off and
        /// the host draws the plain pass). Call once, before the renderer is initialized.
        void Configure(foundation::rhi::Device& device, u32 samples = kDefaultSamples)
        {
            Destroy();
            m_device = &device;
            m_samples = samples;
            m_stencilFormat = PickStencilCapableFormat(device, samples);
        }

        /// The renderer's target config for this pass: the sample count and stencil format when
        /// the targets exist, a plain pass's otherwise.
        [[nodiscard]] VGTargetConfig ConfigFor(foundation::vg::VGBlendSpace blendSpace =
                                                   foundation::vg::VGBlendSpace::Linear) const noexcept
        {
            VGTargetConfig config;
            if (IsValid())
            {
                config.sampleCount = m_samples;
                config.depthStencilFormat = m_stencilFormat;
            }
            config.blendSpace = blendSpace;
            return config;
        }

        /// (Re)creates both targets at `width` x `height` for a colour target of `colorFormat`.
        /// Any failure releases both: a stencil pipeline without its attachment is an invalid
        /// pass, so the host falls back to the plain pass as a unit.
        [[nodiscard]] bool Create(foundation::rhi::TextureFormat colorFormat, u32 width, u32 height)
        {
            namespace rhi = foundation::rhi;
            ReleaseTargets();
            if (m_device == nullptr || width == 0 || height == 0 || m_stencilFormat == rhi::TextureFormat::Undefined)
                return false;
            rhi::TextureDesc cd = rhi::TextureDesc::RenderTarget(colorFormat, width, height);
            cd.sampleCount = m_samples;
            cd.label = u8"VG MSAA color";
            rhi::TextureDesc dd{};
            dd.dimension = rhi::TextureDimension::Texture2D;
            dd.format = m_stencilFormat;
            dd.width = width;
            dd.height = height;
            dd.depth = 1;
            dd.usage = rhi::TextureUsage::DepthStencil;
            dd.sampleCount = m_samples;
            dd.label = u8"VG stencil";
            if (!m_device->CreateTexture(cd, m_color).IsOk() || !m_device->CreateTexture(dd, m_stencil).IsOk() ||
                !m_device->CreateTextureView(m_color, rhi::TextureViewDesc{}, m_colorView).IsOk() ||
                !m_device->CreateTextureView(m_stencil, rhi::TextureViewDesc{}, m_stencilView).IsOk())
            {
                ReleaseTargets();
                return false;
            }
            m_colorFormat = colorFormat;
            m_width = width;
            m_height = height;
            return true;
        }

        /// Whether the targets exist at another size than `width` x `height` (a resize is due; the
        /// host waits for the GPU to finish with them first).
        [[nodiscard]] bool NeedsResize(u32 width, u32 height) const noexcept
        {
            return m_colorView != nullptr && width != 0 && height != 0 && (width != m_width || height != m_height);
        }

        /// Recreates the targets when the size changed (call outside an open frame, after the GPU
        /// is done with the old ones). False when the targets are off.
        bool Resize(u32 width, u32 height)
        {
            if (m_colorView == nullptr)
                return false;
            if (width == m_width && height == m_height)
                return true;
            return Create(m_colorFormat, width, height);
        }

        void Destroy()
        {
            ReleaseTargets();
            m_stencilFormat = foundation::rhi::TextureFormat::Undefined;
        }

        /// Whether a stencil format was found (the renderer is to be configured for the pass).
        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_device != nullptr && m_stencilFormat != foundation::rhi::TextureFormat::Undefined;
        }
        /// Whether the targets exist at `width` x `height`, so this frame can take the quality pass.
        [[nodiscard]] bool Matches(u32 width, u32 height) const noexcept
        {
            return m_colorView != nullptr && m_width == width && m_height == height;
        }

        /// Transitions both targets for a fresh pass (their old contents are discarded) and
        /// describes it: the multisampled colour cleared to `clear` and resolved into
        /// `resolveTarget`, the stencil cleared to 0 as stencil-then-cover expects.
        [[nodiscard]] foundation::rhi::RenderPassDesc BeginPass(foundation::rhi::CommandEncoder& encoder,
                                                                foundation::rhi::TextureView* resolveTarget,
                                                                foundation::rhi::ClearColor clear,
                                                                const char8_t* label = u8"VG (msaa+stencil)") const
        {
            namespace rhi = foundation::rhi;
            encoder.TransitionTexture(m_color, rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget);
            encoder.TransitionTexture(m_stencil, rhi::ResourceState::Undefined, rhi::ResourceState::DepthStencilWrite);
            rhi::ColorAttachment color{};
            color.view = m_colorView;
            color.resolveTarget = resolveTarget;
            color.loadOp = rhi::LoadOp::Clear;
            color.storeOp = rhi::StoreOp::DontCare; // resolved; the multisampled texels can go
            color.clearValue = clear;
            rhi::DepthStencilAttachment ds{};
            ds.view = m_stencilView;
            ds.depthLoadOp = rhi::LoadOp::Clear;
            ds.depthStoreOp = rhi::StoreOp::DontCare;
            ds.stencilLoadOp = rhi::LoadOp::Clear;
            ds.stencilStoreOp = rhi::StoreOp::DontCare;
            ds.stencilClearValue = 0;
            rhi::RenderPassDesc desc{};
            desc.colorAttachments.Add(color);
            desc.depthStencilAttachment = ds;
            desc.label = label;
            return desc;
        }

        [[nodiscard]] u32 Samples() const noexcept { return m_samples; }
        [[nodiscard]] foundation::rhi::TextureFormat StencilFormat() const noexcept { return m_stencilFormat; }

    private:
        foundation::rhi::Device* m_device = nullptr; // borrowed
        foundation::rhi::Texture* m_color = nullptr;
        foundation::rhi::TextureView* m_colorView = nullptr;
        foundation::rhi::Texture* m_stencil = nullptr;
        foundation::rhi::TextureView* m_stencilView = nullptr;
        foundation::rhi::TextureFormat m_colorFormat = foundation::rhi::TextureFormat::Undefined;
        foundation::rhi::TextureFormat m_stencilFormat = foundation::rhi::TextureFormat::Undefined;
        u32 m_samples = kDefaultSamples;
        u32 m_width = 0;
        u32 m_height = 0;

        void ReleaseTargets()
        {
            if (m_device != nullptr)
            {
                if (m_colorView != nullptr)
                    m_device->DestroyTextureView(m_colorView);
                if (m_color != nullptr)
                    m_device->DestroyTexture(m_color);
                if (m_stencilView != nullptr)
                    m_device->DestroyTextureView(m_stencilView);
                if (m_stencil != nullptr)
                    m_device->DestroyTexture(m_stencil);
            }
            m_colorView = nullptr;
            m_color = nullptr;
            m_stencilView = nullptr;
            m_stencil = nullptr;
            m_width = 0;
            m_height = 0;
        }
    };
}
