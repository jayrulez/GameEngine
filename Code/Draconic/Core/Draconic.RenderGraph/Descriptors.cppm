// Draconic::RenderGraph - :descriptors partition
//
// Render-graph resource descriptors (transient texture/buffer) and pass target
// attachments. Ported from Sedulous.RenderGraph (Descriptors.bf). RGTextureDesc
// resolves size-relative-to-output and converts to an RHI TextureDesc.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:descriptors;

import draconic.foundation;
import draconic.rhi;
import :types;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace rhi = draconic::rhi;

    // Describes a transient texture resource in the graph.
    struct RGTextureDesc
    {
        rhi::TextureFormat format = rhi::TextureFormat::Undefined;
        SizeMode sizeMode = SizeMode::FullSize;
        u32 width = 0; // used only when sizeMode == Custom
        u32 height = 0;
        u32 arrayLayerCount = 1;
        u32 mipLevelCount = 1;
        u32 sampleCount = 1;
        rhi::TextureUsage usage = rhi::TextureUsage::None;

        RGTextureDesc() = default;
        RGTextureDesc(rhi::TextureFormat fmt, SizeMode mode = SizeMode::FullSize) noexcept
            : format(fmt), sizeMode(mode)
        {
        }
        RGTextureDesc(rhi::TextureFormat fmt, u32 w, u32 h) noexcept
            : format(fmt), sizeMode(SizeMode::Custom), width(w), height(h)
        {
        }

        // Resolves actual dimensions from the graph output size.
        void Resolve(u32 outputWidth, u32 outputHeight) noexcept
        {
            switch (sizeMode)
            {
            case SizeMode::FullSize:
                width = Max(1u, outputWidth);
                height = Max(1u, outputHeight);
                break;
            case SizeMode::HalfSize:
                width = Max(1u, outputWidth / 2u);
                height = Max(1u, outputHeight / 2u);
                break;
            case SizeMode::QuarterSize:
                width = Max(1u, outputWidth / 4u);
                height = Max(1u, outputHeight / 4u);
                break;
            case SizeMode::Custom:
                width = Max(1u, width);
                height = Max(1u, height);
                break; // never a zero-extent target
            }
        }

        [[nodiscard]] rhi::TextureDesc ToTextureDesc(StringView label) const
        {
            rhi::TextureDesc desc{};
            desc.format = format;
            desc.width = width;
            desc.height = height;
            desc.arrayLayerCount = arrayLayerCount;
            desc.mipLevelCount = mipLevelCount;
            desc.sampleCount = sampleCount;
            desc.usage = usage;
            desc.label = label;
            return desc;
        }
    };

    // Describes a transient buffer resource in the graph.
    struct RGBufferDesc
    {
        u64 size = 0;
        rhi::BufferUsage usage = rhi::BufferUsage::None;
    };

    // Color target attachment for a render pass.
    struct RGColorTarget
    {
        RGHandle handle = RGHandle::Invalid();
        rhi::LoadOp loadOp = rhi::LoadOp::Clear;
        rhi::StoreOp storeOp = rhi::StoreOp::Store;
        rhi::ClearColor clearValue = rhi::ClearColor::Black();
        RGSubresourceRange subresource;
    };

    // Depth/stencil target attachment for a render pass.
    struct RGDepthTarget
    {
        RGHandle handle = RGHandle::Invalid();
        rhi::LoadOp depthLoadOp = rhi::LoadOp::Clear;
        rhi::StoreOp depthStoreOp = rhi::StoreOp::Store;
        f32 depthClearValue = 1.0f;
        bool readOnly = false;
        rhi::LoadOp stencilLoadOp = rhi::LoadOp::DontCare;
        rhi::StoreOp stencilStoreOp = rhi::StoreOp::DontCare;
        u32 stencilClearValue = 0;
        RGSubresourceRange subresource;
    };

    // Configuration for the render graph.
    struct RenderGraphConfig
    {
        i32 frameBufferCount = 2; // multi-buffering slots (typically 2 or 3)
    };
}
