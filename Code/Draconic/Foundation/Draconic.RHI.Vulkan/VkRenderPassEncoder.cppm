/// Vulkan implementation of RenderPassEncoder + MeshShaderPassExt.
/// Ported from Sedulous.RHI.Vulkan/VulkanRenderPassEncoder.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "VkIncludes.h"

export module draconic.rhi.vulkan:render_pass_encoder;

import draconic.foundation;
import draconic.rhi;
import :conversions;
import :buffer;
import :bind_group;
import :render_pipeline;
import :compute_pipeline;
import :pipeline_layout;
import :query_set;
import :mesh_pipeline;
import :render_bundle_encoder;

using namespace draconic::foundation;

export namespace draconic::rhi::vk
{

    class VkRenderPassEncoderImpl : public RenderPassEncoder, public MeshShaderPassExt
    {
    public:
        MeshShaderPassExt* AsMeshShaderExt() noexcept override { return this; }
        VkRenderPassEncoderImpl(VkCommandBuffer cmdBuf, VkDevice device)
            : m_cmdBuf(cmdBuf), m_device(device)
        {
        }

        // ---- RenderPassEncoder ----

        void SetPipeline(RenderPipeline* pipeline) override
        {
            m_currentPipeline = static_cast<VkRenderPipelineImpl*>(pipeline);
            m_currentMeshPipeline = nullptr;
            if (m_currentPipeline)
                vkCmdBindPipeline(m_cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_currentPipeline->handle());
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynOffsets) override
        {
            auto* bg = static_cast<VkBindGroupImpl*>(group);
            auto* layout = getCurrentLayout();
            if (!bg || !layout)
                return;
            VkDescriptorSet set = bg->handle();
            vkCmdBindDescriptorSets(m_cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->handle(),
                                    index, 1, &set, static_cast<u32>(dynOffsets.Size()),
                                    dynOffsets.Data());
        }

        void SetPushConstants(ShaderStage stages, u32 offset, u32 size, const void* data) override
        {
            auto* layout = getCurrentLayout();
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

        void SetViewport(f32 x, f32 y, f32 w, f32 h, f32 minDepth, f32 maxDepth) override
        {
            // Flip Y via negative height to match DX12 coordinate system.
            VkViewport vp{};
            vp.x = x;
            vp.y = y + h;
            vp.width = w;
            vp.height = -h;
            vp.minDepth = minDepth;
            vp.maxDepth = maxDepth;
            vkCmdSetViewport(m_cmdBuf, 0, 1, &vp);
        }

        void SetScissor(i32 x, i32 y, u32 w, u32 h) override
        {
            VkRect2D sc{};
            sc.offset = {x, y};
            sc.extent = {w, h};
            vkCmdSetScissor(m_cmdBuf, 0, 1, &sc);
        }

        void SetBlendConstant(f32 r, f32 g, f32 b, f32 a) override
        {
            f32 c[4] = {r, g, b, a};
            vkCmdSetBlendConstants(m_cmdBuf, c);
        }

        void SetStencilReference(u32 ref) override
        {
            vkCmdSetStencilReference(m_cmdBuf, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
        }

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override
        {
            vkCmdDraw(m_cmdBuf, vertexCount, instanceCount, firstVertex, firstInstance);
        }

        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 baseVertex,
                         u32 firstInstance) override
        {
            vkCmdDrawIndexed(m_cmdBuf, indexCount, instanceCount, firstIndex, baseVertex,
                             firstInstance);
        }

        void DrawIndirect(Buffer* buf, u64 offset, u32 drawCount, u32 stride) override
        {
            auto* vkBuf = static_cast<VkBufferImpl*>(buf);
            if (!vkBuf)
                return;
            vkCmdDrawIndirect(m_cmdBuf, vkBuf->handle(), offset, drawCount,
                              stride > 0 ? stride : 16);
        }

        void DrawIndexedIndirect(Buffer* buf, u64 offset, u32 drawCount, u32 stride) override
        {
            auto* vkBuf = static_cast<VkBufferImpl*>(buf);
            if (!vkBuf)
                return;
            vkCmdDrawIndexedIndirect(m_cmdBuf, vkBuf->handle(), offset, drawCount,
                                     stride > 0 ? stride : 20);
        }

        void WriteTimestamp(QuerySet* qs, u32 index) override
        {
            auto* q = static_cast<VkQuerySetImpl*>(qs);
            if (q)
                vkCmdWriteTimestamp(m_cmdBuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, q->handle(),
                                    index);
        }

        void BeginOcclusionQuery(QuerySet* qs, u32 index) override
        {
            auto* q = static_cast<VkQuerySetImpl*>(qs);
            if (q)
                vkCmdBeginQuery(m_cmdBuf, q->handle(), index, 0);
        }

        void EndOcclusionQuery(QuerySet* qs, u32 index) override
        {
            auto* q = static_cast<VkQuerySetImpl*>(qs);
            if (q)
                vkCmdEndQuery(m_cmdBuf, q->handle(), index);
        }

        void ExecuteBundles(Span<RenderBundle* const> bundles) override
        {
            if (bundles.IsEmpty())
                return;
            Array<VkCommandBuffer> secs(bundles.Size());
            for (usize i = 0; i < bundles.Size(); ++i)
                secs[i] = static_cast<VkRenderBundleImpl*>(bundles[i])->handle();
            vkCmdExecuteCommands(m_cmdBuf, static_cast<u32>(secs.Size()), secs.Data());
        }

        void End() override
        {
            vkCmdEndRendering(m_cmdBuf);
            m_currentPipeline = nullptr;
            m_currentMeshPipeline = nullptr;
        }

        // ---- MeshShaderPassExt ----

        void SetMeshPipeline(MeshPipeline* pipeline) override;
        void DrawMeshTasks(u32 gx, u32 gy, u32 gz) override;
        void DrawMeshTasksIndirect(Buffer* buf, u64 offset, u32 drawCount, u32 stride) override;
        void DrawMeshTasksIndirectCount(Buffer* buf, u64 offset, Buffer* countBuf, u64 countOffset,
                                        u32 maxDrawCount, u32 stride) override;

    private:
        VkPipelineLayoutImpl* getCurrentLayout();

        VkCommandBuffer m_cmdBuf = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkRenderPipelineImpl* m_currentPipeline = nullptr;
        VkMeshPipelineImpl* m_currentMeshPipeline = nullptr;

        // Cached device-level mesh shader function pointers.
        PFN_vkCmdDrawMeshTasksEXT m_pfnDrawMesh = nullptr;
        PFN_vkCmdDrawMeshTasksIndirectEXT m_pfnDrawMeshIndirect = nullptr;
        PFN_vkCmdDrawMeshTasksIndirectCountEXT m_pfnDrawMeshIndCount = nullptr;
    };

} // namespace draconic::rhi::vk
