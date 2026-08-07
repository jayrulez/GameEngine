/// draconic.rhi.webgpu:render_pipeline - RenderPipeline over WGPURenderPipeline.
///
/// One honest narrowing: FillMode::Wireframe has no WebGPU shape (polygon mode is
/// not in the API) - NotSupported, callers keep their debug-wireframe toggles off
/// this backend. depthBias rides the depth-stencil state as WebGPU defines it.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:render_pipeline;

import draconic.foundation;
import draconic.rhi;
import :api;
import :conversions;
import :pipeline_layout;
import :shader_module;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuRenderPipeline final : public RenderPipeline
    {
    public:
        Status Initialize(const WebGpuApi& api, WGPUDevice device,
                          const RenderPipelineDesc& pipelineDesc)
        {
            m_api = &api;
            if (pipelineDesc.layout == nullptr || pipelineDesc.vertex.shader.module == nullptr)
            {
                return ErrorCode::InvalidArgument;
            }
            if (pipelineDesc.primitive.fillMode == FillMode::Wireframe)
            {
                return ErrorCode::NotSupported; // no polygon mode in WebGPU
            }

            WGPURenderPipelineDescriptor wgpuDesc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            wgpuDesc.label = ToWgpuStringView(pipelineDesc.label);
            auto* pipelineLayout = static_cast<WebGpuPipelineLayout*>(pipelineDesc.layout);
            wgpuDesc.layout = pipelineLayout->Handle();
            m_pushConstants = pipelineLayout->EmulationInfo();

            // ---- Vertex stage + buffers ----
            String vertexEntry(pipelineDesc.vertex.shader.entryPoint);
            wgpuDesc.vertex.module =
                static_cast<WebGpuShaderModule*>(pipelineDesc.vertex.shader.module)->Handle();
            wgpuDesc.vertex.entryPoint = ToWgpuStringView(vertexEntry.AsView());

            Array<WGPUVertexBufferLayout> vertexBuffers;
            Array<Array<WGPUVertexAttribute>> attributeStorage;
            for (const VertexBufferLayout& bufferLayout : pipelineDesc.vertex.buffers)
            {
                Array<WGPUVertexAttribute> attributes;
                for (const VertexAttribute& attribute : bufferLayout.attributes)
                {
                    WGPUVertexAttribute wgpuAttribute = WGPU_VERTEX_ATTRIBUTE_INIT;
                    wgpuAttribute.format = ToWgpuVertexFormat(attribute.format);
                    wgpuAttribute.offset = attribute.offset;
                    wgpuAttribute.shaderLocation = attribute.shaderLocation;
                    attributes.PushBack(wgpuAttribute);
                }
                attributeStorage.PushBack(Move(attributes));

                WGPUVertexBufferLayout wgpuLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
                wgpuLayout.stepMode = ToWgpuVertexStepMode(bufferLayout.stepMode);
                wgpuLayout.arrayStride = bufferLayout.stride;
                wgpuLayout.attributeCount = attributeStorage.Back().Size();
                wgpuLayout.attributes = attributeStorage.Back().Data();
                vertexBuffers.PushBack(wgpuLayout);
            }
            wgpuDesc.vertex.bufferCount = vertexBuffers.Size();
            wgpuDesc.vertex.buffers = vertexBuffers.Data();

            // ---- Primitive ----
            wgpuDesc.primitive.topology =
                ToWgpuPrimitiveTopology(pipelineDesc.primitive.topology);
            wgpuDesc.primitive.frontFace = ToWgpuFrontFace(pipelineDesc.primitive.frontFace);
            wgpuDesc.primitive.cullMode = ToWgpuCullMode(pipelineDesc.primitive.cullMode);
            // WebGPU strip topologies REQUIRE a strip index format; lists must leave it
            // Undefined. UInt32 matches the engine's index buffers.
            const bool isStrip =
                pipelineDesc.primitive.topology == PrimitiveTopology::LineStrip ||
                pipelineDesc.primitive.topology == PrimitiveTopology::TriangleStrip;
            wgpuDesc.primitive.stripIndexFormat =
                isStrip ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Undefined;
            wgpuDesc.primitive.unclippedDepth = !pipelineDesc.primitive.depthClipEnabled;

            // ---- Depth/stencil ----
            WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
            if (pipelineDesc.depthStencil.HasValue())
            {
                const DepthStencilState& ds = pipelineDesc.depthStencil.Value();
                depthStencil.format = ToWgpuTextureFormat(ds.format);
                depthStencil.depthWriteEnabled =
                    ds.depthWriteEnabled ? WGPUOptionalBool_True : WGPUOptionalBool_False;
                depthStencil.depthCompare = ds.depthTestEnabled
                                                ? ToWgpuCompareFunction(ds.depthCompare)
                                                : WGPUCompareFunction_Always;
                depthStencil.stencilReadMask = ds.stencilEnabled ? ds.stencilReadMask : 0;
                depthStencil.stencilWriteMask = ds.stencilEnabled ? ds.stencilWriteMask : 0;
                depthStencil.stencilFront.compare =
                    ToWgpuCompareFunction(ds.stencilFront.compare);
                depthStencil.stencilFront.failOp =
                    ToWgpuStencilOperation(ds.stencilFront.failOp);
                depthStencil.stencilFront.depthFailOp =
                    ToWgpuStencilOperation(ds.stencilFront.depthFailOp);
                depthStencil.stencilFront.passOp =
                    ToWgpuStencilOperation(ds.stencilFront.passOp);
                depthStencil.stencilBack.compare =
                    ToWgpuCompareFunction(ds.stencilBack.compare);
                depthStencil.stencilBack.failOp = ToWgpuStencilOperation(ds.stencilBack.failOp);
                depthStencil.stencilBack.depthFailOp =
                    ToWgpuStencilOperation(ds.stencilBack.depthFailOp);
                depthStencil.stencilBack.passOp = ToWgpuStencilOperation(ds.stencilBack.passOp);
                depthStencil.depthBias = ds.depthBias;
                depthStencil.depthBiasSlopeScale = ds.depthBiasSlopeScale;
                depthStencil.depthBiasClamp = ds.depthBiasClamp;
                wgpuDesc.depthStencil = &depthStencil;
            }

            // ---- Multisample ----
            wgpuDesc.multisample.count = pipelineDesc.multisample.count;
            wgpuDesc.multisample.mask = pipelineDesc.multisample.mask;
            wgpuDesc.multisample.alphaToCoverageEnabled =
                pipelineDesc.multisample.alphaToCoverageEnabled;

            // ---- Fragment stage + color targets ----
            WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
            String fragmentEntry;
            Array<WGPUColorTargetState> targets;
            Array<WGPUBlendState> blendStorage;
            if (pipelineDesc.fragment.HasValue())
            {
                const FragmentState& fs = pipelineDesc.fragment.Value();
                fragmentEntry = String(fs.shader.entryPoint);
                fragment.module =
                    static_cast<WebGpuShaderModule*>(fs.shader.module)->Handle();
                fragment.entryPoint = ToWgpuStringView(fragmentEntry.AsView());

                // Reserve so blend pointers stay stable while targets fill.
                blendStorage.Reserve(fs.targets.Size());
                for (const ColorTargetState& target : fs.targets)
                {
                    WGPUColorTargetState wgpuTarget = WGPU_COLOR_TARGET_STATE_INIT;
                    wgpuTarget.format = ToWgpuTextureFormat(target.format);
                    wgpuTarget.writeMask = ToWgpuColorWriteMask(target.writeMask);
                    if (target.blend.HasValue())
                    {
                        const BlendState& blend = target.blend.Value();
                        WGPUBlendState wgpuBlend = WGPU_BLEND_STATE_INIT;
                        wgpuBlend.color.srcFactor = ToWgpuBlendFactor(blend.color.srcFactor);
                        wgpuBlend.color.dstFactor = ToWgpuBlendFactor(blend.color.dstFactor);
                        wgpuBlend.color.operation = ToWgpuBlendOperation(blend.color.operation);
                        wgpuBlend.alpha.srcFactor = ToWgpuBlendFactor(blend.alpha.srcFactor);
                        wgpuBlend.alpha.dstFactor = ToWgpuBlendFactor(blend.alpha.dstFactor);
                        wgpuBlend.alpha.operation = ToWgpuBlendOperation(blend.alpha.operation);
                        blendStorage.PushBack(wgpuBlend);
                        wgpuTarget.blend = &blendStorage.Back();
                    }
                    targets.PushBack(wgpuTarget);
                }
                fragment.targetCount = targets.Size();
                fragment.targets = targets.Data();
                wgpuDesc.fragment = &fragment;
            }

            m_pipeline = api.wgpuDeviceCreateRenderPipeline(device, &wgpuDesc);
            return m_pipeline != nullptr ? Status(ErrorCode::Ok) : Status(ErrorCode::Unknown);
        }

        void Release()
        {
            if (m_pipeline != nullptr)
            {
                m_api->wgpuRenderPipelineRelease(m_pipeline);
                m_pipeline = nullptr;
            }
        }

        [[nodiscard]] WGPURenderPipeline Handle() const { return m_pipeline; }
        [[nodiscard]] const PushConstantEmulation& PushConstants() const { return m_pushConstants; }

    private:
        const WebGpuApi* m_api = nullptr;
        WGPURenderPipeline m_pipeline = nullptr;
        PushConstantEmulation m_pushConstants;
    };
}
