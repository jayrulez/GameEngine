/// Vulkan implementation of ComputePassEncoder.
/// Ported from Sedulous.RHI.Vulkan/VulkanComputePassEncoder.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "VkIncludes.h"

export module draconic.rhi.vulkan:compute_pass_encoder;

import draconic.foundation;
import draconic.rhi;
import :conversions;
import :buffer;
import :bind_group;
import :compute_pipeline;
import :pipeline_layout;
import :query_set;

using namespace draconic::foundation;

export namespace draconic::rhi::vk
{

    class VkComputePassEncoderImpl : public ComputePassEncoder
    {
    public:
        VkComputePassEncoderImpl(VkCommandBuffer cmdBuf) : m_cmdBuf(cmdBuf) {}

        void SetPipeline(ComputePipeline* pipeline) override
        {
            m_current = static_cast<VkComputePipelineImpl*>(pipeline);
            if (m_current)
                vkCmdBindPipeline(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_current->handle());
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynOffsets) override
        {
            auto* bg = static_cast<VkBindGroupImpl*>(group);
            if (!bg || !m_current || !m_current->vkLayout())
                return;
            VkDescriptorSet set = bg->handle();
            vkCmdBindDescriptorSets(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_current->vkLayout()->handle(), index, 1, &set,
                                    static_cast<u32>(dynOffsets.Size()), dynOffsets.Data());
        }

        void SetPushConstants(ShaderStage stages, u32 offset, u32 size, const void* data) override
        {
            if (!m_current || !m_current->vkLayout())
                return;
            vkCmdPushConstants(m_cmdBuf, m_current->vkLayout()->handle(),
                               toVkShaderStageFlags(stages), offset, size, data);
        }

        void Dispatch(u32 x, u32 y, u32 z) override { vkCmdDispatch(m_cmdBuf, x, y, z); }

        void DispatchIndirect(Buffer* buffer, u64 offset) override
        {
            auto* vkBuf = static_cast<VkBufferImpl*>(buffer);
            if (vkBuf)
                vkCmdDispatchIndirect(m_cmdBuf, vkBuf->handle(), offset);
        }

        void ComputeBarrier() override
        {
            VkMemoryBarrier2 mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            VkDependencyInfo di{};
            di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(m_cmdBuf, &di);
        }

        void WriteTimestamp(QuerySet* qs, u32 index) override
        {
            auto* q = static_cast<VkQuerySetImpl*>(qs);
            if (q)
                vkCmdWriteTimestamp(m_cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, q->handle(),
                                    index);
        }

        void End() override { m_current = nullptr; }

    private:
        VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
        VkComputePipelineImpl* m_current = nullptr;
    };

} // namespace draconic::rhi::vk
