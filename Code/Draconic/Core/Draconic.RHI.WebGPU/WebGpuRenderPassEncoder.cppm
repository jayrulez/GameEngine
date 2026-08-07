/// draconic.rhi.webgpu:render_pass_encoder - RenderPassEncoder over WGPURenderPassEncoder.
///
/// SetPushConstants takes native immediates where available (see :pipeline_layout), else the
/// uniform-buffer fallback (:push_constant_emulator) - the shadow is flushed and bound before
/// each draw. Multi-draw indirect unrolls into single indirect draws (core WebGPU has one-draw
/// indirect). Occlusion queries require RenderPassDesc.occlusionQuerySet declared at pass begin;
/// Begin/End then carry only the index. Timestamps also ride the pass descriptor.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:render_pass_encoder;

import draconic.foundation;
import draconic.rhi;
import :api;
import :bind_group;
import :buffer;
import :render_pipeline;
import :render_bundle_encoder;
import :push_constant_emulator;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuRenderPassEncoder final : public RenderPassEncoder
    {
    public:
        void Begin(const WebGpuApi& api, WGPUDevice device, WGPURenderPassEncoder encoder)
        {
            m_api = &api;
            m_encoder = encoder;
            m_pushConstants.Begin(api, device);
        }

        void SetPipeline(RenderPipeline* pipeline) override
        {
            auto* wgpuPipeline = static_cast<WebGpuRenderPipeline*>(pipeline);
            m_api->wgpuRenderPassEncoderSetPipeline(m_encoder, wgpuPipeline->Handle());
            m_pushConstants.SetPipeline(wgpuPipeline->PushConstants());
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynamicOffsets) override
        {
            m_api->wgpuRenderPassEncoderSetBindGroup(
                m_encoder, index, static_cast<WebGpuBindGroup*>(group)->Handle(),
                dynamicOffsets.Size(), dynamicOffsets.Data());
        }

        void SetPushConstants(ShaderStage, u32 offset, u32 size, const void* data) override
        {
            // Emulating pipeline: fold into the shadow (bound before the next draw). Otherwise
            // the pipeline declared native immediates - issue them directly.
            if (!m_pushConstants.Write(offset, size, data) &&
                m_api->wgpuRenderPassEncoderSetImmediates != nullptr)
            {
                m_api->wgpuRenderPassEncoderSetImmediates(m_encoder, offset, data, size);
            }
        }

        void SetVertexBuffer(u32 slot, Buffer* buffer, u64 offset) override
        {
            m_api->wgpuRenderPassEncoderSetVertexBuffer(
                m_encoder, slot, static_cast<WebGpuBuffer*>(buffer)->Handle(), offset,
                WGPU_WHOLE_SIZE);
        }

        void SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset) override
        {
            m_api->wgpuRenderPassEncoderSetIndexBuffer(
                m_encoder, static_cast<WebGpuBuffer*>(buffer)->Handle(),
                format == IndexFormat::UInt16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                offset, WGPU_WHOLE_SIZE);
        }

        void SetViewport(f32 x, f32 y, f32 width, f32 height, f32 minDepth, f32 maxDepth) override
        {
            m_api->wgpuRenderPassEncoderSetViewport(m_encoder, x, y, width, height, minDepth,
                                                    maxDepth);
        }

        void SetScissor(i32 x, i32 y, u32 width, u32 height) override
        {
            m_api->wgpuRenderPassEncoderSetScissorRect(m_encoder, static_cast<u32>(x),
                                                       static_cast<u32>(y), width, height);
        }

        void SetBlendConstant(f32 r, f32 g, f32 b, f32 a) override
        {
            const WGPUColor color{r, g, b, a};
            m_api->wgpuRenderPassEncoderSetBlendConstant(m_encoder, &color);
        }

        void SetStencilReference(u32 reference) override
        {
            m_api->wgpuRenderPassEncoderSetStencilReference(m_encoder, reference);
        }

        void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override
        {
            FlushPushConstants();
            m_api->wgpuRenderPassEncoderDraw(m_encoder, vertexCount, instanceCount, firstVertex,
                                             firstInstance);
        }

        void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 baseVertex,
                         u32 firstInstance) override
        {
            FlushPushConstants();
            m_api->wgpuRenderPassEncoderDrawIndexed(m_encoder, indexCount, instanceCount,
                                                    firstIndex, baseVertex, firstInstance);
        }

        void DrawIndirect(Buffer* buffer, u64 offset, u32 drawCount, u32 stride) override
        {
            FlushPushConstants();
            const WGPUBuffer handle = static_cast<WebGpuBuffer*>(buffer)->Handle();
            for (u32 i = 0; i < drawCount; ++i) // core WebGPU: one draw per indirect call
            {
                m_api->wgpuRenderPassEncoderDrawIndirect(m_encoder, handle,
                                                         offset + static_cast<u64>(i) * stride);
            }
        }

        void DrawIndexedIndirect(Buffer* buffer, u64 offset, u32 drawCount, u32 stride) override
        {
            FlushPushConstants();
            const WGPUBuffer handle = static_cast<WebGpuBuffer*>(buffer)->Handle();
            for (u32 i = 0; i < drawCount; ++i)
            {
                m_api->wgpuRenderPassEncoderDrawIndexedIndirect(
                    m_encoder, handle, offset + static_cast<u64>(i) * stride);
            }
        }

        void ExecuteBundles(Span<RenderBundle* const> bundles) override
        {
            WGPURenderBundle handles[16];
            usize count = 0;
            for (RenderBundle* bundle : bundles)
            {
                if (count == 16)
                {
                    m_api->wgpuRenderPassEncoderExecuteBundles(m_encoder, count, handles);
                    count = 0;
                }
                handles[count++] = static_cast<WebGpuRenderBundle*>(bundle)->Handle();
            }
            if (count > 0)
            {
                m_api->wgpuRenderPassEncoderExecuteBundles(m_encoder, count, handles);
            }
        }

        void WriteTimestamp(QuerySet*, u32) override
        {
            // Pass-interior timestamps have no WebGPU shape; begin/end-of-pass writes
            // ride the pass descriptor (RenderPassDesc.timestampQuerySet).
        }

        void BeginOcclusionQuery(QuerySet*, u32 index) override
        {
            // The set itself was declared at pass begin (RenderPassDesc.
            // occlusionQuerySet); WebGPU only takes the index here.
            m_api->wgpuRenderPassEncoderBeginOcclusionQuery(m_encoder, index);
        }
        void EndOcclusionQuery(QuerySet*, u32) override
        {
            m_api->wgpuRenderPassEncoderEndOcclusionQuery(m_encoder);
        }

        void End() override
        {
            m_api->wgpuRenderPassEncoderEnd(m_encoder);
            // AFTER End: the pass commands have taken their references, so the emulated
            // uniform buffers/bind groups can be freed.
            m_pushConstants.Release();
            m_api->wgpuRenderPassEncoderRelease(m_encoder);
            m_encoder = nullptr;
        }

    private:
        /// Upload + bind any pending emulated push-constant block before a draw. No-op for
        /// pipelines that use native immediates.
        void FlushPushConstants()
        {
            i32 group = -1;
            WGPUBindGroup bindGroup = nullptr;
            if (m_pushConstants.FlushBeforeDraw(group, bindGroup))
            {
                m_api->wgpuRenderPassEncoderSetBindGroup(
                    m_encoder, static_cast<u32>(group), bindGroup, 0, nullptr);
            }
        }

        const WebGpuApi* m_api = nullptr;
        WGPURenderPassEncoder m_encoder = nullptr;
        PushConstantEmulator m_pushConstants;
    };
}
