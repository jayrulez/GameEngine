/// draconic.rhi.webgpu:compute_pass_encoder - ComputePassEncoder over WGPUComputePassEncoder.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:compute_pass_encoder;

import draconic.foundation;
import draconic.rhi;
import :api;
import :bind_group;
import :buffer;
import :compute_pipeline;
import :push_constant_emulator;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuComputePassEncoder final : public ComputePassEncoder
    {
    public:
        void Begin(const WebGpuApi& api, WGPUDevice device, WGPUComputePassEncoder encoder)
        {
            m_api = &api;
            m_encoder = encoder;
            m_pushConstants.Begin(api, device);
        }

        void SetPipeline(ComputePipeline* pipeline) override
        {
            auto* wgpuPipeline = static_cast<WebGpuComputePipeline*>(pipeline);
            m_api->wgpuComputePassEncoderSetPipeline(m_encoder, wgpuPipeline->Handle());
            m_pushConstants.SetPipeline(wgpuPipeline->PushConstants());
        }

        void SetBindGroup(u32 index, BindGroup* group, Span<const u32> dynamicOffsets) override
        {
            m_api->wgpuComputePassEncoderSetBindGroup(
                m_encoder, index, static_cast<WebGpuBindGroup*>(group)->Handle(),
                dynamicOffsets.Size(), dynamicOffsets.Data());
        }

        void SetPushConstants(ShaderStage, u32 offset, u32 size, const void* data) override
        {
            // Emulating pipeline: fold into the shadow (bound before the next dispatch).
            // Otherwise the pipeline declared native immediates - issue them directly.
            if (!m_pushConstants.Write(offset, size, data) &&
                m_api->wgpuComputePassEncoderSetImmediates != nullptr)
            {
                m_api->wgpuComputePassEncoderSetImmediates(m_encoder, offset, data, size);
            }
        }

        void Dispatch(u32 x, u32 y, u32 z) override
        {
            FlushPushConstants();
            m_api->wgpuComputePassEncoderDispatchWorkgroups(m_encoder, x, y, z);
        }

        void DispatchIndirect(Buffer* buffer, u64 offset) override
        {
            FlushPushConstants();
            m_api->wgpuComputePassEncoderDispatchWorkgroupsIndirect(
                m_encoder, static_cast<WebGpuBuffer*>(buffer)->Handle(), offset);
        }

        void ComputeBarrier() override
        {
            // WebGPU tracks hazards automatically - dispatch ordering within a pass is
            // already dependency-correct.
        }

        void WriteTimestamp(QuerySet*, u32) override
        {
            // Pass-interior timestamps have no WebGPU shape (begin/end-of-pass only).
        }

        void End() override
        {
            m_api->wgpuComputePassEncoderEnd(m_encoder);
            // AFTER End: the pass commands hold their references now, so the emulated
            // uniform buffers/bind groups can be freed.
            m_pushConstants.Release();
            m_api->wgpuComputePassEncoderRelease(m_encoder);
            m_encoder = nullptr;
        }

    private:
        /// Upload + bind any pending emulated push-constant block before a dispatch. No-op for
        /// pipelines that use native immediates.
        void FlushPushConstants()
        {
            i32 group = -1;
            WGPUBindGroup bindGroup = nullptr;
            if (m_pushConstants.FlushBeforeDraw(group, bindGroup))
            {
                m_api->wgpuComputePassEncoderSetBindGroup(
                    m_encoder, static_cast<u32>(group), bindGroup, 0, nullptr);
            }
        }

        const WebGpuApi* m_api = nullptr;
        WGPUComputePassEncoder m_encoder = nullptr;
        PushConstantEmulator m_pushConstants;
    };
}
