// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RenderGraph - :types partition
//
// Core value types: resource/pass handles, pass and access enums, subresource
// ranges. Access types map to RHI ResourceState, the currency the barrier solver
// works in.

module;
#include "Core/Prelude.h"

export module foundation.rendergraph:types;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rendergraph
{
    namespace rhi = foundation::rhi;

    // Handle to a graph resource (texture or buffer); generation-checked for staleness.
    struct RGHandle
    {
        u32 index = 0xFFFFFFFFu;
        u32 generation = 0;

        [[nodiscard]] static constexpr RGHandle Invalid() noexcept
        {
            return RGHandle{0xFFFFFFFFu, 0};
        }
        [[nodiscard]] constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }
    };

    [[nodiscard]] constexpr bool operator==(RGHandle a, RGHandle b) noexcept
    {
        return a.index == b.index && a.generation == b.generation;
    }
    [[nodiscard]] constexpr bool operator!=(RGHandle a, RGHandle b) noexcept { return !(a == b); }

    // Handle to a graph pass.
    struct PassHandle
    {
        u32 index = 0xFFFFFFFFu;

        [[nodiscard]] static constexpr PassHandle Invalid() noexcept
        {
            return PassHandle{0xFFFFFFFFu};
        }
        [[nodiscard]] constexpr bool IsValid() const noexcept { return index != 0xFFFFFFFFu; }
    };

    [[nodiscard]] constexpr bool operator==(PassHandle a, PassHandle b) noexcept
    {
        return a.index == b.index;
    }
    [[nodiscard]] constexpr bool operator!=(PassHandle a, PassHandle b) noexcept
    {
        return a.index != b.index;
    }

    enum class RGPassType : u8
    {
        Render,
        Compute,
        Copy
    };

    // Type of resource access declared by a pass.
    enum class RGAccessType : u8
    {
        // Reads
        ReadTexture,
        ReadBuffer,
        ReadDepthStencil,
        ReadCopySrc,
        // Sample a depth texture in a shader: like ReadTexture (sampled, not an attachment) but the
        // layout must be DepthStencilRead (DEPTH_STENCIL_READ_ONLY_OPTIMAL), not ShaderRead.
        SampleDepthStencil,
        // Writes
        WriteColorTarget,
        WriteDepthTarget,
        WriteStorage,
        WriteCopyDst,
        // Read and write
        ReadWriteStorage,
        ReadWriteDepthTarget,
        ReadWriteColorTarget,
    };

    [[nodiscard]] constexpr bool IsRead(RGAccessType type) noexcept
    {
        switch (type)
        {
        case RGAccessType::ReadTexture:
        case RGAccessType::ReadBuffer:
        case RGAccessType::ReadDepthStencil:
        case RGAccessType::SampleDepthStencil:
        case RGAccessType::ReadCopySrc:
        case RGAccessType::ReadWriteStorage:
        case RGAccessType::ReadWriteDepthTarget:
        case RGAccessType::ReadWriteColorTarget:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] constexpr bool IsWrite(RGAccessType type) noexcept
    {
        switch (type)
        {
        case RGAccessType::WriteColorTarget:
        case RGAccessType::WriteDepthTarget:
        case RGAccessType::WriteStorage:
        case RGAccessType::WriteCopyDst:
        case RGAccessType::ReadWriteStorage:
        case RGAccessType::ReadWriteDepthTarget:
        case RGAccessType::ReadWriteColorTarget:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] constexpr rhi::ResourceState ToResourceState(RGAccessType type) noexcept
    {
        using RS = rhi::ResourceState;
        switch (type)
        {
        case RGAccessType::ReadTexture:
            return RS::ShaderRead;
        case RGAccessType::ReadBuffer:
            return RS::ShaderRead;
        case RGAccessType::ReadDepthStencil:
            return RS::DepthStencilRead;
        case RGAccessType::SampleDepthStencil:
            return RS::DepthStencilRead;
        case RGAccessType::ReadCopySrc:
            return RS::CopySrc;
        case RGAccessType::WriteColorTarget:
            return RS::RenderTarget;
        case RGAccessType::WriteDepthTarget:
            return RS::DepthStencilWrite;
        case RGAccessType::WriteStorage:
            return RS::ShaderWrite;
        case RGAccessType::WriteCopyDst:
            return RS::CopyDst;
        case RGAccessType::ReadWriteStorage:
            return RS::ShaderWrite | RS::ShaderRead;
        case RGAccessType::ReadWriteDepthTarget:
            return RS::DepthStencilWrite;
        case RGAccessType::ReadWriteColorTarget:
            return RS::RenderTarget;
        }
        return RS::Undefined;
    }

    // Subresource range for fine-grained access tracking (e.g. shadow cascades).
    // A count of 0 means "all remaining from the base".
    struct RGSubresourceRange
    {
        u32 baseMipLevel = 0;
        u32 mipLevelCount = 0;
        u32 baseArrayLayer = 0;
        u32 arrayLayerCount = 0;

        [[nodiscard]] static constexpr RGSubresourceRange All() noexcept
        {
            return RGSubresourceRange{};
        }

        [[nodiscard]] constexpr bool IsAll() const noexcept
        {
            return baseMipLevel == 0 && mipLevelCount == 0 && baseArrayLayer == 0 &&
                   arrayLayerCount == 0;
        }

        [[nodiscard]] constexpr bool Overlaps(RGSubresourceRange other, u32 totalMips = 1,
                                              u32 totalLayers = 1) const noexcept
        {
            const u32 myMipEnd = mipLevelCount == 0 ? totalMips : baseMipLevel + mipLevelCount;
            const u32 otherMipEnd =
                other.mipLevelCount == 0 ? totalMips : other.baseMipLevel + other.mipLevelCount;
            const u32 myLayerEnd =
                arrayLayerCount == 0 ? totalLayers : baseArrayLayer + arrayLayerCount;
            const u32 otherLayerEnd = other.arrayLayerCount == 0
                                          ? totalLayers
                                          : other.baseArrayLayer + other.arrayLayerCount;

            const bool mipOverlap = baseMipLevel < otherMipEnd && other.baseMipLevel < myMipEnd;
            const bool layerOverlap =
                baseArrayLayer < otherLayerEnd && other.baseArrayLayer < myLayerEnd;
            return mipOverlap && layerOverlap;
        }
    };

    [[nodiscard]] constexpr bool operator==(RGSubresourceRange a, RGSubresourceRange b) noexcept
    {
        return a.baseMipLevel == b.baseMipLevel && a.mipLevelCount == b.mipLevelCount &&
               a.baseArrayLayer == b.baseArrayLayer && a.arrayLayerCount == b.arrayLayerCount;
    }
    [[nodiscard]] constexpr bool operator!=(RGSubresourceRange a, RGSubresourceRange b) noexcept
    {
        return !(a == b);
    }

    // A single resource access declared by a pass.
    struct RGResourceAccess
    {
        RGHandle handle = RGHandle::Invalid();
        RGAccessType type = RGAccessType::ReadTexture;
        RGSubresourceRange subresource;

        [[nodiscard]] bool IsRead() const noexcept { return rendergraph::IsRead(type); }
        [[nodiscard]] bool IsWrite() const noexcept { return rendergraph::IsWrite(type); }
        [[nodiscard]] rhi::ResourceState ToResourceState() const noexcept
        {
            return rendergraph::ToResourceState(type);
        }
    };

    // How transient resource dimensions resolve relative to the graph output.
    enum class SizeMode : u8
    {
        FullSize,
        HalfSize,
        QuarterSize,
        Custom
    };

    enum class RGResourceLifetime : u8
    {
        Transient,
        Persistent,
        Imported
    };

    enum class RGResourceType : u8
    {
        Texture,
        Buffer
    };
}
