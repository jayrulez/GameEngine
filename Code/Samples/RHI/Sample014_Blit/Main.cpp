#include <new>
/// Sample014 - Blit (Scaled Copy). Ported from Sedulous Sample014_Blit.
/// Renders a spinning triangle to a small 128x128 offscreen texture, then blits
/// it to the full swapchain (scaled up with linear filtering).

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

class BlitSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override { return u8"Sample014 - Blit (Scaled Copy)"; }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct VSInput { float3 Position : TEXCOORD0; float4 Color : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float4 Color : COLOR0; };
        PSInput VSMain(VSInput i) { PSInput o; o.Position = float4(i.Position,1); o.Color = i.Color; return o; }
        float4 PSMain(PSInput i) : SV_TARGET { return i.Color; }
    )";
    static constexpr draconic::foundation::u32 kOffscreenSize = 128;

    void updateTriangle();

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer* m_vb = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::Texture* m_offscreenTex = nullptr;
    rhi::TextureView* m_offscreenView = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status BlitSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8;
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

    // Triangle VB (CpuToGpu for per-frame rotation updates).
    rhi::BufferDesc vbd{};
    vbd.size = 84;
    vbd.usage = rhi::BufferUsage::Vertex;
    vbd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::PipelineLayoutDesc pld{};
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Offscreen render target.
    rhi::TextureDesc td{};
    td.format = m_swapChain->Format();
    td.width = kOffscreenSize;
    td.height = kOffscreenSize;
    td.mipLevelCount = 1;
    td.usage =
        rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc | rhi::TextureUsage::Sampled;
    if (m_device->CreateTexture(td, m_offscreenTex) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::TextureViewDesc tvd{};
    tvd.format = m_swapChain->Format();
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    if (m_device->CreateTextureView(m_offscreenTex, tvd, m_offscreenView) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x4, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 28;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_pl;
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
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

void BlitSample::updateTriangle()
{
    float angle = m_totalTime * 2.0f;
    float c = std::cos(angle), s = std::sin(angle);
    float basePos[6] = {0.0f, 0.5f, 0.433f, -0.25f, -0.433f, -0.25f};
    float colors[12] = {1, 0.2f, 0.2f, 1, 0.2f, 1, 0.2f, 1, 0.2f, 0.4f, 1, 1};
    float verts[21];
    for (int i = 0; i < 3; ++i)
    {
        float x = basePos[i * 2], y = basePos[i * 2 + 1];
        verts[i * 7 + 0] = x * c - y * s;
        verts[i * 7 + 1] = x * s + y * c;
        verts[i * 7 + 2] = 0.0f;
        verts[i * 7 + 3] = colors[i * 4];
        verts[i * 7 + 4] = colors[i * 4 + 1];
        verts[i * 7 + 5] = colors[i * 4 + 2];
        verts[i * 7 + 6] = colors[i * 4 + 3];
    }
    void* mapped = m_vb->Map();
    if (mapped)
    {
        std::memcpy(mapped, verts, 84);
        m_vb->Unmap();
    }
}

void BlitSample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    updateTriangle();

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;

    // Pass 1: Render spinning triangle to offscreen texture.
    enc->TransitionTexture(m_offscreenTex, rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    {
        rhi::ColorAttachment ca{};
        ca.view = m_offscreenView;
        ca.loadOp = rhi::LoadOp::Clear;
        ca.storeOp = rhi::StoreOp::Store;
        ca.clearValue = rhi::ClearColor(0.15f, 0.1f, 0.2f, 1.0f);
        rhi::RenderPassDesc rpd{};
        rpd.colorAttachments.Add(ca);
        auto* rp = enc->BeginRenderPass(rpd);
        rp->SetPipeline(m_pipeline);
        rp->SetViewport(0, 0, static_cast<f32>(kOffscreenSize), static_cast<f32>(kOffscreenSize), 0,
                        1);
        rp->SetScissor(0, 0, kOffscreenSize, kOffscreenSize);
        rp->SetVertexBuffer(0, m_vb, 0);
        rp->Draw(3);
        rp->End();
    }
    enc->TransitionTexture(m_offscreenTex, rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::CopySrc);

    // Pass 2: Blit offscreen (128x128) to full swapchain (scaled up with linear filtering).
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::CopyDst);
    enc->Blit(m_offscreenTex, m_swapChain->CurrentTexture());
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::CopyDst,
                           rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void BlitSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_offscreenView)
        m_device->DestroyTextureView(m_offscreenView);
    if (m_offscreenTex)
        m_device->DestroyTexture(m_offscreenTex);
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
    BlitSample app;
    return app.Run(argc, argv);
}
