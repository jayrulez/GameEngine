// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// RHI - :counters partition
//
// What a device has recorded since it was made and the GPU memory it holds now
// (Device::Counters), for the profiler's counters. Each backend keeps a
// DeviceCounterBlock and bumps it where it records and allocates; the block is plain atomics, so
// encoders on any recording thread (bundles are built on workers) may bump it. Relaxed: they
// are tallies, nothing is published through them.

module;
#include "Core/Prelude.h"

export module foundation.rhi:counters;

import foundation.core;
import :enums;
import :texture_format;
import :descriptors;

using namespace foundation::core;

export namespace foundation::rhi
{
    /// A device's counts: totals since it was made (a reader takes differences per frame) and
    /// what it holds now.
    struct DeviceCounters
    {
        u64 draws = 0;          // draw calls of every kind; a bundle's each time it is executed
        u64 dispatches = 0;     // compute dispatches (and ray traces)
        u64 renderPasses = 0;
        u64 computePasses = 0;
        u64 pipelineBinds = 0;  // SetPipeline, render and compute
        u64 bindGroupBinds = 0; // SetBindGroup, render and compute
        u64 transferBytes = 0;  // written through transfer batches
        // Held now: the GPU memory of the buffers and textures this device made (render targets,
        // depth included, apart), and how many of each.
        u64 bufferBytes = 0;
        u64 textureBytes = 0;
        u64 renderTargetBytes = 0;
        u64 buffers = 0;
        u64 textures = 0;
    };

    /// The bytes a texture takes, from its description: every mip of every layer at every
    /// sample. For a backend that cannot ask its driver (Vulkan asks); a driver may pad more.
    [[nodiscard]] inline u64 EstimateTextureBytes(const TextureDesc& desc) noexcept
    {
        const u32 layers = desc.dimension == TextureDimension::Texture3D ? 1u : Max(desc.arrayLayerCount, 1u);
        u64 total = 0;
        u32 w = Max(desc.width, 1u), h = Max(desc.height, 1u), d = Max(desc.depth, 1u);
        for (u32 mip = 0; mip < Max(desc.mipLevelCount, 1u); ++mip)
        {
            const u64 level = IsCompressed(desc.format)
                                  ? static_cast<u64>(CompressedLevelBytes(desc.format, w, h))
                                  : static_cast<u64>(w) * h * BytesPerPixel(desc.format);
            total += level * d;
            w = Max(w / 2u, 1u);
            h = Max(h / 2u, 1u);
            d = Max(d / 2u, 1u);
        }
        return total * layers * Max(desc.sampleCount, 1u);
    }

    /// Whether a texture is a render target for the counters (colour or depth attachment).
    [[nodiscard]] constexpr bool CountsAsRenderTarget(const TextureDesc& desc) noexcept
    {
        return (desc.usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil)) != TextureUsage::None;
    }

    /// The counts a backend keeps for Device::Counters.
    class DeviceCounterBlock
    {
    public:
        void Draws(u64 count = 1) noexcept { m_draws.fetch_add(count, MemoryOrder::Relaxed); }
        void Dispatches(u64 count = 1) noexcept { m_dispatches.fetch_add(count, MemoryOrder::Relaxed); }
        void RenderPass() noexcept { m_renderPasses.fetch_add(1, MemoryOrder::Relaxed); }
        void ComputePass() noexcept { m_computePasses.fetch_add(1, MemoryOrder::Relaxed); }
        void PipelineBinds(u64 count = 1) noexcept { m_pipelineBinds.fetch_add(count, MemoryOrder::Relaxed); }
        void BindGroupBinds(u64 count = 1) noexcept { m_bindGroupBinds.fetch_add(count, MemoryOrder::Relaxed); }
        void Transfer(u64 bytes) noexcept { m_transferBytes.fetch_add(bytes, MemoryOrder::Relaxed); }

        void BufferMade(u64 bytes) noexcept
        {
            m_bufferBytes.fetch_add(bytes, MemoryOrder::Relaxed);
            m_buffers.fetch_add(1, MemoryOrder::Relaxed);
        }
        void BufferFreed(u64 bytes) noexcept
        {
            m_bufferBytes.fetch_sub(bytes, MemoryOrder::Relaxed);
            m_buffers.fetch_sub(1, MemoryOrder::Relaxed);
        }
        void TextureMade(u64 bytes, bool renderTarget) noexcept
        {
            if (renderTarget)
            {
                m_renderTargetBytes.fetch_add(bytes, MemoryOrder::Relaxed);
            }
            else
            {
                m_textureBytes.fetch_add(bytes, MemoryOrder::Relaxed);
            }
            m_textures.fetch_add(1, MemoryOrder::Relaxed);
        }
        void TextureFreed(u64 bytes, bool renderTarget) noexcept
        {
            if (renderTarget)
            {
                m_renderTargetBytes.fetch_sub(bytes, MemoryOrder::Relaxed);
            }
            else
            {
                m_textureBytes.fetch_sub(bytes, MemoryOrder::Relaxed);
            }
            m_textures.fetch_sub(1, MemoryOrder::Relaxed);
        }

        [[nodiscard]] DeviceCounters Snapshot() const noexcept
        {
            DeviceCounters c;
            c.draws = m_draws.load(MemoryOrder::Relaxed);
            c.dispatches = m_dispatches.load(MemoryOrder::Relaxed);
            c.renderPasses = m_renderPasses.load(MemoryOrder::Relaxed);
            c.computePasses = m_computePasses.load(MemoryOrder::Relaxed);
            c.pipelineBinds = m_pipelineBinds.load(MemoryOrder::Relaxed);
            c.bindGroupBinds = m_bindGroupBinds.load(MemoryOrder::Relaxed);
            c.transferBytes = m_transferBytes.load(MemoryOrder::Relaxed);
            c.bufferBytes = m_bufferBytes.load(MemoryOrder::Relaxed);
            c.textureBytes = m_textureBytes.load(MemoryOrder::Relaxed);
            c.renderTargetBytes = m_renderTargetBytes.load(MemoryOrder::Relaxed);
            c.buffers = m_buffers.load(MemoryOrder::Relaxed);
            c.textures = m_textures.load(MemoryOrder::Relaxed);
            return c;
        }

    private:
        Atomic<u64> m_draws{0};
        Atomic<u64> m_dispatches{0};
        Atomic<u64> m_renderPasses{0};
        Atomic<u64> m_computePasses{0};
        Atomic<u64> m_pipelineBinds{0};
        Atomic<u64> m_bindGroupBinds{0};
        Atomic<u64> m_transferBytes{0};
        Atomic<u64> m_bufferBytes{0};
        Atomic<u64> m_textureBytes{0};
        Atomic<u64> m_renderTargetBytes{0};
        Atomic<u64> m_buffers{0};
        Atomic<u64> m_textures{0};
    };

    /// What a render bundle recorded, added to the device's counts each time the bundle runs
    /// (a bundle is recorded once and replayed). Its own encoder's thread only.
    struct RecordedCounts
    {
        u64 draws = 0;
        u64 pipelineBinds = 0;
        u64 bindGroupBinds = 0;

        void AddTo(DeviceCounterBlock& block) const noexcept
        {
            block.Draws(draws);
            block.PipelineBinds(pipelineBinds);
            block.BindGroupBinds(bindGroupBinds);
        }
    };
}
