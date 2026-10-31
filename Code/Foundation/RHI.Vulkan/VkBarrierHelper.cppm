// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Converts ResourceState to Vulkan synchronization2 stage/access/layout.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:barrier_helper;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    struct StageAccess
    {
        u64 stageMask = 0;
        u64 accessMask = 0;
    };

    /// Restricts a synchronization-2 stage mask to what `queue`'s family can execute. The state
    /// mapping names ALL_GRAPHICS | COMPUTE_SHADER for shader access because it does not know the
    /// queue, and a barrier recorded on a compute-only or transfer-only family with a graphics
    /// stage in it is invalid (VUID-vkCmdPipelineBarrier2-srcStageMask-03849 and friends). Graphics
    /// families execute everything. A mask left empty by the cut (a render-target state on a
    /// compute queue) becomes ALL_COMMANDS, which is valid on every family and merely stronger.
    inline u64 maskStagesForQueue(u64 stages, QueueType queue) noexcept
    {
        if (queue == QueueType::Graphics || stages == 0)
            return stages;
        constexpr u64 kAnyFamily = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT |
                                   VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT |
                                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                   VK_PIPELINE_STAGE_2_CLEAR_BIT;
        constexpr u64 kCompute = kAnyFamily | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT;
        const u64 allowed = (queue == QueueType::Compute) ? kCompute : kAnyFamily;
        const u64 masked = stages & allowed;
        return masked != 0 ? masked : static_cast<u64>(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }

    /// The access half of the cut: an access mask is valid only together with a stage that can
    /// perform it on the recording family. VERTEX_ATTRIBUTE_READ on a compute queue is invalid even
    /// under ALL_COMMANDS, because ALL_COMMANDS expands per family and the compute expansion has no
    /// vertex-input stage (VUID-VkBufferMemoryBarrier2-srcAccessMask-03902), so cutting stages
    /// alone is not enough. An emptied access mask is valid: the barrier is execution-only.
    inline u64 maskAccessForQueue(u64 access, QueueType queue) noexcept
    {
        if (queue == QueueType::Graphics || access == 0)
            return access;
        constexpr u64 kAnyFamily = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |
                                   VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
                                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        constexpr u64 kCompute = kAnyFamily | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_UNIFORM_READ_BIT |
                                 VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                 VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        return access & ((queue == QueueType::Compute) ? kCompute : kAnyFamily);
    }

    /// The whole cut, stage and access as one pair, so the two cannot drift apart.
    inline StageAccess maskForQueue(StageAccess sa, QueueType queue) noexcept
    {
        return StageAccess{maskStagesForQueue(sa.stageMask, queue),
                           maskAccessForQueue(sa.accessMask, queue)};
    }

    inline StageAccess getStageAccess(ResourceState state)
    {
        StageAccess sa{};
        auto s = static_cast<u32>(state);
        auto has = [&](ResourceState f) { return (s & static_cast<u32>(f)) != 0; };

        if (has(ResourceState::VertexBuffer))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
            sa.accessMask |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        }
        if (has(ResourceState::IndexBuffer))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            sa.accessMask |= VK_ACCESS_2_INDEX_READ_BIT;
        }
        if (has(ResourceState::UniformBuffer))
        {
            sa.stageMask |=
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            sa.accessMask |= VK_ACCESS_2_UNIFORM_READ_BIT;
        }
        if (has(ResourceState::ShaderRead))
        {
            sa.stageMask |=
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            sa.accessMask |= VK_ACCESS_2_SHADER_READ_BIT;
        }
        if (has(ResourceState::ShaderWrite))
        {
            sa.stageMask |=
                VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            sa.accessMask |= VK_ACCESS_2_SHADER_WRITE_BIT;
        }
        if (has(ResourceState::RenderTarget))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            sa.accessMask |=
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        }
        if (has(ResourceState::DepthStencilWrite))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            sa.accessMask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        if (has(ResourceState::DepthStencilRead))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            sa.accessMask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        }
        if (has(ResourceState::IndirectArgument))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            sa.accessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        if (has(ResourceState::CopySrc))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            sa.accessMask |= VK_ACCESS_2_TRANSFER_READ_BIT;
        }
        if (has(ResourceState::CopyDst))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            sa.accessMask |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
        if (has(ResourceState::Present))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        }
        if (has(ResourceState::AccelStructRead))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
            sa.accessMask |= VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        }
        if (has(ResourceState::AccelStructWrite))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            sa.accessMask |= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        }
        if (has(ResourceState::General))
        {
            sa.stageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            sa.accessMask |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        }

        if (sa.stageMask == 0)
            sa.stageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        return sa;
    }

    inline VkImageLayout getImageLayout(ResourceState state,
                                        TextureFormat format = TextureFormat::Undefined)
    {
        auto s = static_cast<u32>(state);
        auto has = [&](ResourceState f) { return (s & static_cast<u32>(f)) != 0; };

        if (has(ResourceState::Present))
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (has(ResourceState::RenderTarget))
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (has(ResourceState::DepthStencilWrite))
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (has(ResourceState::DepthStencilRead))
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        if (has(ResourceState::ShaderRead))
        {
            // Depth/stencil textures use DEPTH_STENCIL_READ_ONLY when sampled, not
            // SHADER_READ_ONLY, which is only for color textures.
            if (IsDepthFormat(format))
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (has(ResourceState::ShaderWrite))
            return VK_IMAGE_LAYOUT_GENERAL;
        if (has(ResourceState::CopySrc))
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (has(ResourceState::CopyDst))
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (has(ResourceState::General))
            return VK_IMAGE_LAYOUT_GENERAL;
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }

} // namespace foundation::rhi::vk
