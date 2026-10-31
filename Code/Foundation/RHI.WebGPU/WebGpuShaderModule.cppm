// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:shader_module - ShaderModule over WGPUShaderModule.
///
/// Desktop dev loop: the ShaderModuleDesc carries the same DXC-produced SPIR-V the
/// Vulkan backend consumes, ingested through the standard WGPUShaderSourceSPIRV chained
/// struct (gated on the ShaderSourceSPIRV instance feature the backend requests at
/// creation; browsers never expose it). The browser path uses the cook-time WGSL: same
/// desc, but the bytes are WGSL text, ingested through the standard WGSL chained
/// struct. The discriminator is the SPIR-V magic in the first word.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:shader_module;

import foundation.core;
import foundation.rhi;
import :api;
import :conversions;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuShaderModule final : public ShaderModule
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device, const ShaderModuleDesc& desc)
        {
            m_api = &api;

            const bool isSpirv = desc.code.Size() >= 4 &&
                                 *reinterpret_cast<const u32*>(desc.code.Data()) == 0x07230203u;
            if (isSpirv)
            {
                if (!api.spirvIngestion)
                {
                    return ErrorCode::NotSupported; // browser: SPIR-V never ingests
                }
                WGPUShaderSourceSPIRV spirv = WGPU_SHADER_SOURCE_SPIRV_INIT;
                spirv.codeSize = static_cast<u32>(desc.code.Size() / 4);
                spirv.code = reinterpret_cast<const u32*>(desc.code.Data());
                WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
                moduleDesc.label = ToWgpuStringView(desc.label);
                moduleDesc.nextInChain = &spirv.chain;
                m_module = api.wgpuDeviceCreateShaderModule(device, &moduleDesc);
            }
            else
            {
                // WGSL text (the shader cook's browser output).
                WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
                wgsl.code = WGPUStringView{reinterpret_cast<const char*>(desc.code.Data()),
                                           desc.code.Size()};
                WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
                moduleDesc.label = ToWgpuStringView(desc.label);
                moduleDesc.nextInChain = &wgsl.chain;
                m_module = api.wgpuDeviceCreateShaderModule(device, &moduleDesc);
            }
            return m_module != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_module != nullptr)
            {
                m_api->wgpuShaderModuleRelease(m_module);
                m_module = nullptr;
            }
        }

        [[nodiscard]] WGPUShaderModule Handle() const { return m_module; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPUShaderModule m_module = nullptr;
    };
}
