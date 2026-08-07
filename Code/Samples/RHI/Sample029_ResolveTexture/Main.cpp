#include <new>
/// Sample029 -- ResolveTexture (Explicit 4x MSAA). Ported from Sedulous Sample029_ResolveTexture.
/// Demonstrates explicit MSAA resolve via CommandEncoder::resolveTexture().
/// Unlike Sample010 (which uses ColorAttachment.resolveTarget for automatic
/// render-pass resolve), this sample renders to a 4x MSAA target and then
/// manually resolves to the swapchain using the resolveTexture command.

#include <cstdint>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.samples.framework;
import draconic.rhi.vulkan;

namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

class ResolveTextureSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample029 - ResolveTexture (Explicit 4x MSAA)";
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnResize(draconic::foundation::u32 w, draconic::foundation::u32 h) override
    {
        recreateMsaaTarget(w, h);
    }
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct VSInput {
            float3 Position : TEXCOORD0;
            float4 Color    : TEXCOORD1;
        };
        struct PSInput {
            float4 Position : SV_POSITION;
            float4 Color    : COLOR0;
        };
        PSInput VSMain(VSInput input) {
            PSInput output;
            output.Position = float4(input.Position, 1.0);
            output.Color = input.Color;
            return output;
        }
        float4 PSMain(PSInput input) : SV_TARGET {
            return input.Color;
        }
    )";

    // Star shape: great for showing MSAA on diagonal edges.
    // Each vertex: float3 Position, float4 Color (stride = 28).
    static constexpr float kVerts[] = {
        // Center
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        // Outer tips (radius 0.7)
        0.0f,
        0.7f,
        0.0f,
        1.0f,
        0.2f,
        0.2f,
        1.0f,
        0.665f,
        0.216f,
        0.0f,
        0.2f,
        1.0f,
        0.2f,
        1.0f,
        0.411f,
        -0.566f,
        0.0f,
        0.2f,
        0.3f,
        1.0f,
        1.0f,
        -0.411f,
        -0.566f,
        0.0f,
        1.0f,
        1.0f,
        0.2f,
        1.0f,
        -0.665f,
        0.216f,
        0.0f,
        1.0f,
        0.2f,
        1.0f,
        1.0f,
        // Inner notches (radius 0.25)
        0.238f,
        0.327f,
        0.0f,
        0.8f,
        0.7f,
        0.5f,
        1.0f,
        0.385f,
        -0.125f,
        0.0f,
        0.5f,
        0.8f,
        0.7f,
        1.0f,
        0.0f,
        -0.405f,
        0.0f,
        0.5f,
        0.5f,
        0.9f,
        1.0f,
        -0.385f,
        -0.125f,
        0.0f,
        0.9f,
        0.8f,
        0.5f,
        1.0f,
        -0.238f,
        0.327f,
        0.0f,
        0.9f,
        0.5f,
        0.8f,
        1.0f,
    };

    static constexpr draconic::foundation::u16 kIdx[] = {
        0, 1, 6, 0, 6, 2, 0, 2, 7, 0, 7, 3, 0, 3, 8, 0, 8, 4, 0, 4, 9, 0, 9, 5, 0, 5, 10, 0, 10, 1,
    };

    static constexpr draconic::foundation::u32 kSamples = 4;

    void recreateMsaaTarget(draconic::foundation::u32 w, draconic::foundation::u32 h);

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::Texture* m_msaaTex = nullptr;
    rhi::TextureView* m_msaaView = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

void ResolveTextureSample::recreateMsaaTarget(draconic::foundation::u32 w, draconic::foundation::u32 h)
{
    if (m_msaaView)
    {
        m_device->DestroyTextureView(m_msaaView);
        m_msaaView = nullptr;
    }
    if (m_msaaTex)
    {
        m_device->DestroyTexture(m_msaaTex);
        m_msaaTex = nullptr;
    }
    // Need RenderTarget (to draw into) and CopySrc (source for resolveTexture).
    rhi::TextureDesc td{};
    td.format = m_swapChain->Format();
    td.width = w;
    td.height = h;
    td.sampleCount = kSamples;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
    td.label = u8"MsaaRT";
    m_device->CreateTexture(td, m_msaaTex);
    rhi::TextureViewDesc tvd{};
    tvd.format = m_swapChain->Format();
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    m_device->CreateTextureView(m_msaaTex, tvd, m_msaaView);
}

draconic::foundation::Status ResolveTextureSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8;
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"ResolveVS",
                                            m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain",
                                            u8"ResolvePS", m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Vertex and index buffers.
    rhi::BufferDesc vbd{};
    vbd.size = sizeof(kVerts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    vbd.label = u8"ResolveVB";
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kIdx);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    ibd.label = u8"ResolveIB";
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kVerts), sizeof(kVerts)));
    batch->WriteBuffer(m_ib, 0, Span<const u8>(reinterpret_cast<const u8*>(kIdx), sizeof(kIdx)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    // Pipeline layout (no bind groups).
    rhi::PipelineLayoutDesc pld{};
    pld.label = u8"ResolvePL";
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // MSAA render target.
    recreateMsaaTarget(m_width, m_height);

    // Render pipeline with multisample count = 4.
    rhi::VertexAttribute attrs[2] = {
        {rhi::VertexFormat::Float32x3, 0, 0},
        {rhi::VertexFormat::Float32x4, 12, 1},
    };
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 28;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();

    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_pl;
    rpd.label = u8"ResolvePipeline";
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
    rpd.fragment = rhi::FragmentState{};
    rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
    rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
    rpd.multisample.count = kSamples;
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

void ResolveTextureSample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;

    // === Step 1: Render star into MSAA texture ===
    enc->TransitionTexture(m_msaaTex, rhi::ResourceState::CopySrc,
                           rhi::ResourceState::RenderTarget);

    rhi::ColorAttachment ca{};
    ca.view = m_msaaView;
    ca.resolveTarget = nullptr; // No auto-resolve -- we do it manually.
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);
    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->DrawIndexed(30);
    rp->End();

    // === Step 2: Transition for resolve ===
    // MSAA texture: RenderTarget -> CopySrc
    enc->TransitionTexture(m_msaaTex, rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::CopySrc);
    // Swapchain: Present -> CopyDst
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Present,
                           rhi::ResourceState::CopyDst);

    // === Step 3: Explicit resolve MSAA -> swapchain ===
    enc->ResolveTexture(m_msaaTex, m_swapChain->CurrentTexture());

    // === Step 4: Transition swapchain back to Present ===
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::CopyDst,
                           rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void ResolveTextureSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_msaaView)
        m_device->DestroyTextureView(m_msaaView);
    if (m_msaaTex)
        m_device->DestroyTexture(m_msaaTex);
    if (m_ps)
        m_device->DestroyShaderModule(m_ps);
    if (m_vs)
        m_device->DestroyShaderModule(m_vs);
    if (m_ib)
        m_device->DestroyBuffer(m_ib);
    if (m_vb)
        m_device->DestroyBuffer(m_vb);
    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    ResolveTextureSample app;
    return app.Run(argc, argv);
}
