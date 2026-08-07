/// draconic.rhi.webgpu:blit_helper - the internal fullscreen blit pass.
///
/// WebGPU has no vkCmdBlitImage: scaling blits and mip generation are a render pass
/// that samples the source into the destination (the same approach DX12 backends
/// take). The shader is WGSL - an INTERNAL pipeline with its own compact layout, no
/// DXC and no register shifts involved. Pipelines cache per destination format.
///
/// Serves color formats only: depth blits would need a depth-output variant (no
/// consumer yet), and 3D textures would need per-slice passes - both fail honestly.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:blit_helper;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuBlitHelper final
    {
    public:
        void Initialize(const WebGpuApi& api, WGPUDevice device)
        {
            m_api = &api;
            m_device = device;
        }

        void Release()
        {
            for (usize i = 0; i < m_pipelines.Size(); ++i)
            {
                m_api->wgpuRenderPipelineRelease(m_pipelines[i].pipeline);
            }
            m_pipelines.Clear();
            if (m_pipelineLayout != nullptr)
            {
                m_api->wgpuPipelineLayoutRelease(m_pipelineLayout);
                m_pipelineLayout = nullptr;
            }
            if (m_bindGroupLayout != nullptr)
            {
                m_api->wgpuBindGroupLayoutRelease(m_bindGroupLayout);
                m_bindGroupLayout = nullptr;
            }
            if (m_sampler != nullptr)
            {
                m_api->wgpuSamplerRelease(m_sampler);
                m_sampler = nullptr;
            }
            if (m_shaderModule != nullptr)
            {
                m_api->wgpuShaderModuleRelease(m_shaderModule);
                m_shaderModule = nullptr;
            }
        }

        /// Records a fullscreen sample of `sourceView` into `destinationView` on the
        /// given command encoder. Views are consumed by reference only; the caller
        /// releases them. False when the destination format has no blit pipeline.
        bool Blit(WGPUCommandEncoder encoder, WGPUTextureView sourceView,
                  WGPUTextureView destinationView, WGPUTextureFormat destinationFormat)
        {
            const WGPURenderPipeline pipeline = PipelineForFormat(destinationFormat);
            if (pipeline == nullptr)
            {
                return false;
            }

            WGPUBindGroupEntry entries[2];
            entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[0].binding = 0;
            entries[0].textureView = sourceView;
            entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
            entries[1].binding = 1;
            entries[1].sampler = m_sampler;
            WGPUBindGroupDescriptor groupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            groupDesc.layout = m_bindGroupLayout;
            groupDesc.entryCount = 2;
            groupDesc.entries = entries;
            const WGPUBindGroup group = m_api->wgpuDeviceCreateBindGroup(m_device, &groupDesc);

            WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            color.view = destinationView;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            passDesc.colorAttachmentCount = 1;
            passDesc.colorAttachments = &color;

            const WGPURenderPassEncoder pass =
                m_api->wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
            m_api->wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            m_api->wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
            m_api->wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            m_api->wgpuRenderPassEncoderEnd(pass);
            m_api->wgpuRenderPassEncoderRelease(pass);
            m_api->wgpuBindGroupRelease(group);
            return true;
        }

    private:
        struct FormatPipeline
        {
            WGPUTextureFormat format = WGPUTextureFormat_Undefined;
            WGPURenderPipeline pipeline = nullptr;
        };

        void EnsureCommon()
        {
            if (m_shaderModule != nullptr)
            {
                return;
            }

            static constexpr char kBlitWgsl[] =
                "@group(0) @binding(0) var sourceTexture : texture_2d<f32>;\n"
                "@group(0) @binding(1) var sourceSampler : sampler;\n"
                "struct VertexOutput {\n"
                "  @builtin(position) position : vec4f,\n"
                "  @location(0) uv : vec2f,\n"
                "}\n"
                "@vertex fn vertexMain(@builtin(vertex_index) index : u32) -> VertexOutput {\n"
                "  var output : VertexOutput;\n"
                "  let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));\n"
                "  output.position = vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
                "  output.uv = vec2f(uv.x, 1.0 - uv.y);\n"
                "  return output;\n"
                "}\n"
                "@fragment fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {\n"
                "  return textureSampleLevel(sourceTexture, sourceSampler, input.uv, 0.0);\n"
                "}\n";

            WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
            wgsl.code = WGPUStringView{kBlitWgsl, sizeof(kBlitWgsl) - 1};
            WGPUShaderModuleDescriptor moduleDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            moduleDesc.nextInChain = &wgsl.chain;
            m_shaderModule = m_api->wgpuDeviceCreateShaderModule(m_device, &moduleDesc);

            WGPUSamplerDescriptor samplerDesc = WGPU_SAMPLER_DESCRIPTOR_INIT;
            samplerDesc.magFilter = WGPUFilterMode_Linear;
            samplerDesc.minFilter = WGPUFilterMode_Linear;
            m_sampler = m_api->wgpuDeviceCreateSampler(m_device, &samplerDesc);

            WGPUBindGroupLayoutEntry entries[2];
            entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            entries[0].binding = 0;
            entries[0].visibility = WGPUShaderStage_Fragment;
            entries[0].texture.sampleType = WGPUTextureSampleType_Float;
            entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
            entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            entries[1].binding = 1;
            entries[1].visibility = WGPUShaderStage_Fragment;
            entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
            WGPUBindGroupLayoutDescriptor layoutDesc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            layoutDesc.entryCount = 2;
            layoutDesc.entries = entries;
            m_bindGroupLayout = m_api->wgpuDeviceCreateBindGroupLayout(m_device, &layoutDesc);

            WGPUPipelineLayoutDescriptor pipelineLayoutDesc =
                WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            pipelineLayoutDesc.bindGroupLayoutCount = 1;
            pipelineLayoutDesc.bindGroupLayouts = &m_bindGroupLayout;
            m_pipelineLayout =
                m_api->wgpuDeviceCreatePipelineLayout(m_device, &pipelineLayoutDesc);
        }

        WGPURenderPipeline PipelineForFormat(WGPUTextureFormat format)
        {
            if (format == WGPUTextureFormat_Undefined)
            {
                return nullptr;
            }
            for (usize i = 0; i < m_pipelines.Size(); ++i)
            {
                if (m_pipelines[i].format == format)
                {
                    return m_pipelines[i].pipeline;
                }
            }
            EnsureCommon();
            if (m_shaderModule == nullptr)
            {
                return nullptr;
            }

            WGPURenderPipelineDescriptor pipelineDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pipelineDesc.layout = m_pipelineLayout;
            pipelineDesc.vertex.module = m_shaderModule;
            pipelineDesc.vertex.entryPoint =
                WGPUStringView{"vertexMain", sizeof("vertexMain") - 1};
            WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
            target.format = format;
            WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
            fragment.module = m_shaderModule;
            fragment.entryPoint = WGPUStringView{"fragmentMain", sizeof("fragmentMain") - 1};
            fragment.targetCount = 1;
            fragment.targets = &target;
            pipelineDesc.fragment = &fragment;

            const WGPURenderPipeline pipeline =
                m_api->wgpuDeviceCreateRenderPipeline(m_device, &pipelineDesc);
            if (pipeline != nullptr)
            {
                m_pipelines.PushBack(FormatPipeline{format, pipeline});
            }
            return pipeline;
        }

        const WebGpuApi* m_api = nullptr;
        WGPUDevice m_device = nullptr;
        WGPUShaderModule m_shaderModule = nullptr;
        WGPUSampler m_sampler = nullptr;
        WGPUBindGroupLayout m_bindGroupLayout = nullptr;
        WGPUPipelineLayout m_pipelineLayout = nullptr;
        Array<FormatPipeline> m_pipelines;
    };
}
