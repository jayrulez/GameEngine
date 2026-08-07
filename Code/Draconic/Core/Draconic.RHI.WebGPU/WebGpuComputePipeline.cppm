/// draconic.rhi.webgpu:compute_pipeline - ComputePipeline over WGPUComputePipeline.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:compute_pipeline;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;
import :pipeline_layout;
import :shader_module;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuComputePipeline final : public ComputePipeline
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device,
                          const ComputePipelineDesc& pipelineDesc)
        {
            m_api = &api;
            if (pipelineDesc.layout == nullptr || pipelineDesc.compute.module == nullptr)
            {
                return ErrorCode::InvalidArgument;
            }

            String entry(pipelineDesc.compute.entryPoint);
            WGPUComputePipelineDescriptor wgpuDesc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(pipelineDesc.label);
            auto* pipelineLayout = static_cast<WebGpuPipelineLayout*>(pipelineDesc.layout);
            wgpuDesc.layout = pipelineLayout->Handle();
            m_pushConstants = pipelineLayout->EmulationInfo();
            wgpuDesc.compute.module =
                static_cast<WebGpuShaderModule*>(pipelineDesc.compute.module)->Handle();
            wgpuDesc.compute.entryPoint = ToWgpuStringView(entry.AsView());

            m_pipeline = api.wgpuDeviceCreateComputePipeline(device, &wgpuDesc);
            return m_pipeline != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_pipeline != nullptr)
            {
                m_api->wgpuComputePipelineRelease(m_pipeline);
                m_pipeline = nullptr;
            }
        }

        [[nodiscard]] WGPUComputePipeline Handle() const { return m_pipeline; }
        [[nodiscard]] const PushConstantEmulation& PushConstants() const { return m_pushConstants; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUComputePipeline m_pipeline = nullptr;
        PushConstantEmulation m_pushConstants;
    };
}
