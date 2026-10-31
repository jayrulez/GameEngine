// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of RenderBundleEncoder and RenderBundle.
///
/// A render bundle is a secondary VkCommandBuffer recorded with
/// VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT and dynamic-rendering inheritance info (the
/// attachment formats it is compatible with). It is replayed into a primary pass, begun with
/// VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, via vkCmdExecuteCommands
/// (RenderPassEncoder::ExecuteBundles). Because bundles carry no pass-level dynamic state and
/// Vulkan secondaries don't inherit it, the command pool records the desc's viewport and
/// scissor up front. The draw-recording methods mirror VkRenderPassEncoderImpl.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:render_bundle_encoder;

import foundation.core;
import foundation.rhi;
import :conversions;
import :buffer;
import :bind_group;
import :render_pipeline;
import :pipeline_layout;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    // An immutable, replayable secondary command buffer. Valid until the owning pool resets.
    class VkRenderBundleImpl : public RenderBundle
    {
    public:
        VkRenderBundleImpl(VkCommandBuffer cmdBuf, const RecordedCounts& recorded)
            : counts(recorded), m_cmdBuf(cmdBuf)
        {
        }
        [[nodiscard]] VkCommandBuffer handle() const noexcept { return m_cmdBuf; }

        RecordedCounts counts; // what it recorded, counted each time it is executed

    private:
        VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
    };

    // Records draws into a secondary command buffer. The owning command pool allocates and recycles
    // the secondary handle and frees this wrapper (and the bundle it produced) on reset.
    class VkRenderBundleEncoderImpl : public RenderBundleEncoder
    {
    public:
        VkRenderBundleEncoderImpl(VkCommandBuffer cmdBuf, IAllocator& allocator)
            : m_cmdBuf(cmdBuf), m_allocator(allocator)
        {
        }
        ~VkRenderBundleEncoderImpl() override
        {
            m_allocator.Delete(m_bundle);
        } // frees the produced bundle wrapper

        void SetPipeline(RenderPipeline* pipeline) override
        {
            m_currentPipeline = static_cast<VkRenderPipelineImpl*>(pipeline);
            if (m_currentPipeline)
                vkCmdBindPipeline(m_cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_currentPipeline->handle());
            ++m_recorded.pipelineBinds;
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynOffsets) override
        {
            auto* bg = static_cast<VkBindGroupImpl*>(group);
            auto* layout = m_currentPipeline ? m_currentPipeline->vkLayout() : nullptr;
            if (!bg || !layout)
                return;
            VkDescriptorSet set = bg->handle();
            vkCmdBindDescriptorSets(m_cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->handle(),
                                    index, 1, &set, static_cast<u32>(dynOffsets.Size()),
                                    dynOffsets.Data());
            ++m_recorded.bindGroupBinds;
        }

        void SetPushConstants(ShaderStage stages, u32 offset, u32 size, const void* data) override
        {
            auto* layout = m_currentPipeline ? m_currentPipeline->vkLayout() : nullptr;
            if (!layout)
                return;
            vkCmdPushConstants(m_cmdBuf, layout->handle(), toVkShaderStageFlags(stages), offset,
                               size, data);
        }

        void SetVertexBuffer(u32 slot, Buffer* buffer, u64 offset) override
        {
            auto* vkBuf = static_cast<VkBufferImpl*>(buffer);
            if (!vkBuf)
                return;
            VkBuffer handle = vkBuf->handle();
            vkCmdBindVertexBuffers(m_cmdBuf, slot, 1, &handle, &offset);
        }

        void SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset) override
        {
            auto* vkBuf = static_cast<VkBufferImpl*>(buffer);
            if (!vkBuf)
                return;
            vkCmdBindIndexBuffer(m_cmdBuf, vkBuf->handle(), offset, toVkIndexType(format));
        }

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override
        {
            vkCmdDraw(m_cmdBuf, vertexCount, instanceCount, firstVertex, firstInstance);
            ++m_recorded.draws;
        }

        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 baseVertex,
                         u32 firstInstance) override
        {
            vkCmdDrawIndexed(m_cmdBuf, indexCount, instanceCount, firstIndex, baseVertex,
                             firstInstance);
            ++m_recorded.draws;
        }

        void DrawIndirect(Buffer* buf, u64 offset, u32 drawCount, u32 stride) override
        {
            auto* vkBuf = static_cast<VkBufferImpl*>(buf);
            if (!vkBuf)
                return;
            vkCmdDrawIndirect(m_cmdBuf, vkBuf->handle(), offset, drawCount,
                              stride > 0 ? stride : 16);
            m_recorded.draws += drawCount;
        }

        void DrawIndexedIndirect(Buffer* buf, u64 offset, u32 drawCount, u32 stride) override
        {
            auto* vkBuf = static_cast<VkBufferImpl*>(buf);
            if (!vkBuf)
                return;
            vkCmdDrawIndexedIndirect(m_cmdBuf, vkBuf->handle(), offset, drawCount,
                                     stride > 0 ? stride : 20);
            m_recorded.draws += drawCount;
        }

        // End recording; the produced bundle (owned by the pool) wraps the secondary buffer.
        RenderBundle* Finish() override
        {
            if (m_finished)
            {
                return m_bundle;
            }
            m_finished = true;
            vkEndCommandBuffer(m_cmdBuf);
            m_bundle = m_allocator.New<VkRenderBundleImpl>(m_cmdBuf, m_recorded);
            return m_bundle;
        }

        [[nodiscard]] VkRenderBundleImpl* producedBundle() const noexcept { return m_bundle; }

    private:
        VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
        IAllocator& m_allocator;
        VkRenderPipelineImpl* m_currentPipeline = nullptr;
        VkRenderBundleImpl* m_bundle = nullptr;
        bool m_finished = false;
        RecordedCounts m_recorded;
    };

} // namespace foundation::rhi::vk
