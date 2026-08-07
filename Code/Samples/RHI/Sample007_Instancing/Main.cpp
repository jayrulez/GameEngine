#include <new>
/// Sample007 - Instanced Rendering. Ported from Sedulous Sample007_Instancing.
/// Renders 64 small quads in a grid using instanced draw with per-instance offset + color.
/// Instance buffer is CpuToGpu with per-frame wobble animation.

#include <cmath>
#include <cstdint>
#include <cstring>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.samples.framework;
import draconic.rhi.vulkan;

namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

struct InstanceData
{
    float offset[2];
    float color[4];
};

class InstancingSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample007 - Instanced Rendering";
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct VSInput
        {
            float3 Position  : TEXCOORD0;
            float2 Offset    : TEXCOORD1;
            float4 InstColor : TEXCOORD2;
        };
        struct PSInput
        {
            float4 Position : SV_POSITION;
            float4 Color    : COLOR0;
        };
        PSInput VSMain(VSInput input)
        {
            PSInput output;
            output.Position = float4(input.Position.xy + input.Offset, input.Position.z, 1.0);
            output.Color = input.InstColor;
            return output;
        }
        float4 PSMain(PSInput input) : SV_TARGET
        {
            return input.Color;
        }
    )";
    static constexpr int kInstanceCount = 64;

    // Unit quad vertices (pos only).
    static constexpr float kQuadVerts[] = {
        -0.04f, -0.04f, 0.0f, 0.04f, -0.04f, 0.0f, 0.04f, 0.04f, 0.0f, -0.04f, 0.04f, 0.0f,
    };
    static constexpr draconic::foundation::u16 kQuadIdx[] = {0, 1, 2, 0, 2, 3};

    void updateInstances();

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr, *m_instBuf = nullptr;
    void* m_instMapped = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status InstancingSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::f32,
        draconic::foundation::u32;
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"VS",
                                            m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"PS",
                                            m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Vertex + index buffers (GpuOnly, static quad geometry).
    rhi::BufferDesc vbd{};
    vbd.size = sizeof(kQuadVerts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kQuadIdx);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kQuadVerts), sizeof(kQuadVerts)));
    batch->WriteBuffer(m_ib, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kQuadIdx), sizeof(kQuadIdx)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    // Instance buffer (CpuToGpu for per-frame updates).
    rhi::BufferDesc instBd{};
    instBd.size = kInstanceCount * sizeof(InstanceData);
    instBd.usage = rhi::BufferUsage::Vertex;
    instBd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(instBd, m_instBuf) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_instMapped = m_instBuf->Map();
    if (!m_instMapped)
        return draconic::foundation::ErrorCode::Unknown;

    // Pipeline layout (empty - no bind groups needed).
    rhi::PipelineLayoutDesc pld{};
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Two vertex buffer layouts: slot 0 = per-vertex, slot 1 = per-instance.
    rhi::VertexAttribute vtxAttrs[1] = {{rhi::VertexFormat::Float32x3, 0, 0}};
    rhi::VertexBufferLayout vtxLayout{};
    vtxLayout.stride = 12;
    vtxLayout.stepMode = rhi::VertexStepMode::Vertex;
    vtxLayout.attributes = Span<const rhi::VertexAttribute>(vtxAttrs, 1);

    rhi::VertexAttribute instAttrs[2] = {{rhi::VertexFormat::Float32x2, 0, 1},
                                         {rhi::VertexFormat::Float32x4, 8, 2}};
    rhi::VertexBufferLayout instLayout{};
    instLayout.stride = static_cast<u32>(sizeof(InstanceData));
    instLayout.stepMode = rhi::VertexStepMode::Instance;
    instLayout.attributes = Span<const rhi::VertexAttribute>(instAttrs, 2);

    rhi::VertexBufferLayout layouts[2] = {vtxLayout, instLayout};

    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_pl;
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(layouts, 2);
    rpd.fragment = rhi::FragmentState{};
    rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
    rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

void InstancingSample::updateInstances()
{
    auto* data = static_cast<InstanceData*>(m_instMapped);
    int gridSize = static_cast<int>(std::sqrt(static_cast<float>(kInstanceCount)));

    for (int i = 0; i < kInstanceCount; ++i)
    {
        int row = i / gridSize;
        int col = i % gridSize;

        float spacing = 2.0f / static_cast<float>(gridSize);
        float baseX = -1.0f + spacing * 0.5f + col * spacing;
        float baseY = -1.0f + spacing * 0.5f + row * spacing;

        // Animate: wobble in a circle.
        float phase = m_totalTime * 2.0f + i * 0.3f;
        float wobbleX = std::sin(phase) * 0.02f;
        float wobbleY = std::cos(phase * 1.3f) * 0.02f;

        data[i].offset[0] = baseX + wobbleX;
        data[i].offset[1] = baseY + wobbleY;

        // Color: hue based on index.
        float t = static_cast<float>(i) / static_cast<float>(kInstanceCount);
        constexpr float pi2 = 3.14159265f * 2.0f;
        data[i].color[0] = std::abs(std::sin(t * pi2));
        data[i].color[1] = std::abs(std::sin(t * pi2 + 2.094f));
        data[i].color[2] = std::abs(std::sin(t * pi2 + 4.189f));
        data[i].color[3] = 1.0f;
    }
}

void InstancingSample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    updateInstances();

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
    ca.clearValue = rhi::ClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetVertexBuffer(1, m_instBuf, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->DrawIndexed(6, kInstanceCount);
    rp->End();

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);
    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void InstancingSample::OnShutdown()
{
    if (m_instBuf && m_instMapped)
        m_instBuf->Unmap();
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_instBuf)
        m_device->DestroyBuffer(m_instBuf);
    if (m_ib)
        m_device->DestroyBuffer(m_ib);
    if (m_vb)
        m_device->DestroyBuffer(m_vb);
    if (m_ps)
        m_device->DestroyShaderModule(m_ps);
    if (m_vs)
        m_device->DestroyShaderModule(m_vs);
    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    InstancingSample app;
    return app.Run(argc, argv);
}
