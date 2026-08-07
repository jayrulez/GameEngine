#include <new>
/// Sample001 - Triangle. Ported from Sedulous Sample001_Triangle.
/// Renders a colored triangle using vertex buffer + render pipeline.

#include <cstdio>
#include <cstdint>
#include <span>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.samples.framework;
import draconic.rhi.vulkan;

namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

class TriangleSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override { return u8"Sample001 - Triangle"; }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr const char8_t kShaderSource[] = u8R"(
        struct VSInput {
            float3 Position : TEXCOORD0;
            float3 Color    : TEXCOORD1;
        };
        struct PSInput {
            float4 Position : SV_POSITION;
            float3 Color    : TEXCOORD0;
        };
        PSInput VSMain(VSInput input) {
            PSInput output;
            output.Position = float4(input.Position, 1.0);
            output.Color = input.Color;
            return output;
        }
        float4 PSMain(PSInput input) : SV_TARGET {
            return float4(input.Color, 1.0);
        }
    )";

    static constexpr float kVertexData[] = {
        0.0f, 0.5f, 0.0f, 1.0f,  0.0f,  0.0f, 0.5f, -0.5f, 0.0f,
        0.0f, 1.0f, 0.0f, -0.5f, -0.5f, 0.0f, 0.0f, 0.0f,  1.0f,
    };

    shaders::Compiler* m_compiler = nullptr;
    rhi::Buffer* m_vertexBuf = nullptr;
    rhi::ShaderModule* m_vs = nullptr;
    rhi::ShaderModule* m_ps = nullptr;
    rhi::BindGroupLayout* m_bgl = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status TriangleSample::OnInit()
{
    using draconic::foundation::Status;

    // Shader compiler.
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (samples::framework::CompileToModule(m_compiler, m_device, kShaderSource,
                                            shaders::ShaderStage::Vertex, u8"VSMain",
                                            u8"TriangleVS", m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShaderSource,
                                            shaders::ShaderStage::Fragment, u8"PSMain",
                                            u8"TrianglePS", m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Vertex buffer.
    rhi::BufferDesc bd{};
    bd.size = sizeof(kVertexData);
    bd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    bd.memory = rhi::MemoryLocation::GpuOnly;
    bd.label = u8"TriangleVB";
    if (m_device->CreateBuffer(bd, m_vertexBuf) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Upload.
    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(
        m_vertexBuf, 0,
        draconic::foundation::Span<const draconic::foundation::u8>(
            reinterpret_cast<const draconic::foundation::u8*>(kVertexData), sizeof(kVertexData)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    // Pipeline layout (empty).
    rhi::BindGroupLayoutDesc bglDesc{};
    bglDesc.label = u8"EmptyBGL";
    if (m_device->CreateBindGroupLayout(bglDesc, m_bgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::PipelineLayoutDesc pld{};
    rhi::BindGroupLayout* sets[1] = {m_bgl};
    pld.bindGroupLayouts = draconic::foundation::Span<rhi::BindGroupLayout* const>(sets, 1);
    pld.label = u8"TrianglePL";
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Render pipeline.
    rhi::VertexAttribute attrs[2] = {
        {rhi::VertexFormat::Float32x3, 0, 0},
        {rhi::VertexFormat::Float32x3, 12, 1},
    };
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 24;
    vbl.attributes = draconic::foundation::Span<const rhi::VertexAttribute>(attrs, 2);

    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    ct.writeMask = rhi::ColorWriteMask::All;

    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_pl;
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.vertex.buffers = draconic::foundation::Span<const rhi::VertexBufferLayout>(&vbl, 1);
    rpd.fragment = rhi::FragmentState{};
    rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
    rpd.fragment->targets = draconic::foundation::Span<const rhi::ColorTargetState>(&ct, 1);
    rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
    rpd.label = u8"TrianglePipeline";
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Command pool + fence.
    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    return draconic::foundation::ErrorCode::Ok;
}

void TriangleSample::OnRender()
{
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);

    auto* rp = enc->BeginRenderPass(rpd);
    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<draconic::foundation::f32>(m_width),
                    static_cast<draconic::foundation::f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vertexBuf, 0);
    rp->Draw(3);
    rp->End();

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(draconic::foundation::Span<rhi::CommandBuffer* const>(cbs, 1), m_fence,
                            m_fenceVal);

    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void TriangleSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_bgl)
        m_device->DestroyBindGroupLayout(m_bgl);
    if (m_ps)
        m_device->DestroyShaderModule(m_ps);
    if (m_vs)
        m_device->DestroyShaderModule(m_vs);
    if (m_vertexBuf)
        m_device->DestroyBuffer(m_vertexBuf);
    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    TriangleSample app;
    return app.Run(argc, argv);
}
