/// draconic.rhi.webgpu:sampler - Sampler over WGPUSampler.
///
/// Two narrowings vs the RHI desc (both logged nowhere - they are static, documented
/// facts of the platform): ClampToBorder becomes ClampToEdge (no border sampling in
/// core WebGPU; borderColor is ignored with it), and mipLodBias does not exist (bias
/// belongs to the shader sample instruction in WGSL).

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:sampler;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuSampler final : public Sampler
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device, const SamplerDesc& samplerDesc)
        {
            m_api = &api;
            desc = samplerDesc;

            WGPUSamplerDescriptor wgpuDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(samplerDesc.label);
            wgpuDesc.addressModeU = ToWgpuAddressMode(samplerDesc.addressU);
            wgpuDesc.addressModeV = ToWgpuAddressMode(samplerDesc.addressV);
            wgpuDesc.addressModeW = ToWgpuAddressMode(samplerDesc.addressW);
            wgpuDesc.magFilter = ToWgpuFilterMode(samplerDesc.magFilter);
            wgpuDesc.minFilter = ToWgpuFilterMode(samplerDesc.minFilter);
            wgpuDesc.mipmapFilter = ToWgpuMipmapFilterMode(samplerDesc.mipmapFilter);
            wgpuDesc.lodMinClamp = samplerDesc.minLod;
            wgpuDesc.lodMaxClamp = samplerDesc.maxLod;
            if (samplerDesc.compare.HasValue())
            {
                wgpuDesc.compare = ToWgpuCompareFunction(samplerDesc.compare.Value());
            }
            // WebGPU: anisotropy > 1 requires all-linear filtering; clamp to the valid shape.
            const bool allLinear = samplerDesc.minFilter == FilterMode::Linear &&
                                   samplerDesc.magFilter == FilterMode::Linear &&
                                   samplerDesc.mipmapFilter == MipmapFilterMode::Linear;
            wgpuDesc.maxAnisotropy = allLinear && samplerDesc.maxAnisotropy > 0
                                         ? samplerDesc.maxAnisotropy
                                         : 1;

            m_sampler = api.wgpuDeviceCreateSampler(device, &wgpuDesc);
            return m_sampler != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_sampler != nullptr)
            {
                m_api->wgpuSamplerRelease(m_sampler);
                m_sampler = nullptr;
            }
        }

        [[nodiscard]] WGPUSampler Handle() const { return m_sampler; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUSampler m_sampler = nullptr;
    };
}
