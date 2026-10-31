// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample010 - MSAA.
/// Renders a triangle with 4x MSAA, resolving to the swap chain.

#include <cstdint>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

class MSAASample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override { return u8"Sample010 - MSAA (4x)"; }

protected:
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnResize(foundation::core::u32 w, foundation::core::u32 h) override { recreateMSAA(w, h); }
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct VSInput { float3 Position : TEXCOORD0; float3 Color : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float3 Color : COLOR0; };
        PSInput VSMain(VSInput i) { PSInput o; o.Position = float4(i.Position,1); o.Color = i.Color; return o; }
        float4 PSMain(PSInput i) : SV_TARGET { return float4(i.Color, 1.0); }
    )";
    static constexpr float kVerts[] = {0, .7f, 0, 1,    0,    0, .7f, -.5f, 0,
                                       0, 1,   0, -.7f, -.5f, 0, 0,   0,    1};
    static constexpr foundation::core::u16 kIdx[] = {0, 1, 2};
    static constexpr foundation::core::u32 kSamples = 4;

    void recreateMSAA(foundation::core::u32 w, foundation::core::u32 h);

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::Texture* m_msaaTex = nullptr;
    rhi::TextureView* m_msaaView = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
};

void MSAASample::recreateMSAA(foundation::core::u32 w, foundation::core::u32 h)
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
    rhi::TextureDesc td =
        rhi::TextureDesc::RenderTarget(m_swapChain->Format(), w, h, kSamples, u8"MSAATarget");
    m_device->CreateTexture(td, m_msaaTex);
    rhi::TextureViewDesc tvd{};
    tvd.format = m_swapChain->Format();
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    m_device->CreateTextureView(m_msaaTex, tvd, m_msaaView);
}

foundation::core::Status MSAASample::OnInit()
{
    using foundation::core::Status, foundation::core::Span, foundation::core::u8;
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"VS",
                                            m_vs) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"PS",
                                            m_ps) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::BufferDesc vbd{};
    vbd.size = sizeof(kVerts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vb) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kIdx);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(ibd, m_ib) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kVerts), sizeof(kVerts)));
    batch->WriteBuffer(m_ib, 0, Span<const u8>(reinterpret_cast<const u8*>(kIdx), sizeof(kIdx)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    rhi::PipelineLayoutDesc pld{};
    if (m_device->CreatePipelineLayout(pld, m_pl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x3, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 24;
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
    rpd.multisample.count = kSamples;
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    recreateMSAA(m_width, m_height);
    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    return foundation::core::ErrorCode::Ok;
}

void MSAASample::OnRender()
{
    using foundation::core::f32, foundation::core::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != foundation::core::ErrorCode::Ok)
        return;
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != foundation::core::ErrorCode::Ok || !enc)
        return;
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    enc->TransitionTexture(m_msaaTex, rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    // Render into MSAA target, resolve to swap chain.
    rhi::ColorAttachment ca{};
    ca.view = m_msaaView;
    ca.resolveTarget = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.08f, 0.08f, 0.12f, 1);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);
    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->DrawIndexed(3);
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

void MSAASample::OnShutdown()
{
    if (m_msaaView)
        m_device->DestroyTextureView(m_msaaView);
    if (m_msaaTex)
        m_device->DestroyTexture(m_msaaTex);
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
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
    MSAASample app;
    return app.Run(argc, argv);
}
