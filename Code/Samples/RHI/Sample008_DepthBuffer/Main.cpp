// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample008 - Depth Buffer.
/// Three overlapping quads at different Z depths demonstrate depth testing.
/// Red (z=0.2, far), green (z=0.5, mid) and blue (z=0.8, near) are drawn far to near in the
/// engine's reverse-Z convention (rhi::depth: nearer is larger, and the clear is the far plane,
/// 0); the depth buffer gives correct visibility.

#include <cstdint>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

class DepthBufferSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override { return u8"Sample008 - Depth Buffer"; }

protected:
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnResize(foundation::core::u32 w, foundation::core::u32 h) override { recreateDepth(w, h); }
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct VSInput
        {
            float3 Position : TEXCOORD0;
            float4 Color    : TEXCOORD1;
        };
        struct PSInput
        {
            float4 Position : SV_POSITION;
            float4 Color    : COLOR0;
        };
        PSInput VSMain(VSInput input)
        {
            PSInput output;
            output.Position = float4(input.Position, 1.0);
            output.Color = input.Color;
            return output;
        }
        float4 PSMain(PSInput input) : SV_TARGET
        {
            return input.Color;
        }
    )";

    // Three overlapping quads drawn in order: red (far), green (middle), blue (near).
    // Stride: 7 floats per vertex (pos xyz, color rgba).
    static constexpr float kVerts[] = {
        // Quad 0: red, large, behind (z=0.2), drawn first.
        -0.6f,
        -0.6f,
        0.2f,
        1.0f,
        0.2f,
        0.2f,
        1.0f,
        0.4f,
        -0.6f,
        0.2f,
        1.0f,
        0.2f,
        0.2f,
        1.0f,
        0.4f,
        0.6f,
        0.2f,
        1.0f,
        0.2f,
        0.2f,
        1.0f,
        -0.6f,
        0.6f,
        0.2f,
        1.0f,
        0.2f,
        0.2f,
        1.0f,
        // Quad 1: green, overlaps red, closer (z=0.5), drawn second.
        -0.2f,
        -0.4f,
        0.5f,
        0.2f,
        1.0f,
        0.2f,
        1.0f,
        0.6f,
        -0.4f,
        0.5f,
        0.2f,
        1.0f,
        0.2f,
        1.0f,
        0.6f,
        0.4f,
        0.5f,
        0.2f,
        1.0f,
        0.2f,
        1.0f,
        -0.2f,
        0.4f,
        0.5f,
        0.2f,
        1.0f,
        0.2f,
        1.0f,
        // Quad 2: blue, overlaps both, nearest (z=0.8), drawn third.
        -0.4f,
        -0.7f,
        0.8f,
        0.2f,
        0.3f,
        1.0f,
        1.0f,
        0.2f,
        -0.7f,
        0.8f,
        0.2f,
        0.3f,
        1.0f,
        1.0f,
        0.2f,
        0.7f,
        0.8f,
        0.2f,
        0.3f,
        1.0f,
        1.0f,
        -0.4f,
        0.7f,
        0.8f,
        0.2f,
        0.3f,
        1.0f,
        1.0f,
    };
    static constexpr foundation::core::u16 kIdx[] = {
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
    };

    void recreateDepth(foundation::core::u32 w, foundation::core::u32 h);

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::Texture* m_depthTex = nullptr;
    rhi::TextureView* m_depthView = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
};

void DepthBufferSample::recreateDepth(foundation::core::u32 w, foundation::core::u32 h)
{
    if (m_depthView)
    {
        m_device->DestroyTextureView(m_depthView);
        m_depthView = nullptr;
    }
    if (m_depthTex)
    {
        m_device->DestroyTexture(m_depthTex);
        m_depthTex = nullptr;
    }

    rhi::TextureDesc td = rhi::TextureDesc::DepthBuffer(rhi::TextureFormat::Depth24PlusStencil8, w,
                                                        h, 1, u8"DepthTex");
    m_device->CreateTexture(td, m_depthTex);
    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::Depth24PlusStencil8;
    tvd.dimension = rhi::TextureViewDimension::Texture2D;
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    m_device->CreateTextureView(m_depthTex, tvd, m_depthView);
}

foundation::core::Status DepthBufferSample::OnInit()
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

    // Pipeline layout (empty: no bind groups needed).
    rhi::PipelineLayoutDesc pld{};
    if (m_device->CreatePipelineLayout(pld, m_pl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    recreateDepth(m_width, m_height);

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
    rpd.depthStencil = rhi::DepthStencilState{};
    rpd.depthStencil->format = rhi::TextureFormat::Depth24PlusStencil8;
    rpd.depthStencil->depthWriteEnabled = true;
    rpd.depthStencil->depthCompare = rhi::depth::Nearer();
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    return foundation::core::ErrorCode::Ok;
}

void DepthBufferSample::OnRender()
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
    enc->TransitionTexture(m_depthTex, rhi::ResourceState::Undefined,
                           rhi::ResourceState::DepthStencilWrite);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    rhi::DepthStencilAttachment dsa{};
    dsa.view = m_depthView;
    dsa.depthLoadOp = rhi::LoadOp::Clear;
    dsa.depthStoreOp = rhi::StoreOp::Store;
    dsa.depthClearValue = rhi::depth::ClearValue();
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    rpd.depthStencilAttachment = dsa;
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    // Draw all 3 quads; the depth buffer decides visibility.
    rp->DrawIndexed(18);
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

void DepthBufferSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_depthView)
        m_device->DestroyTextureView(m_depthView);
    if (m_depthTex)
        m_device->DestroyTexture(m_depthTex);
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
    DepthBufferSample app;
    return app.Run(argc, argv);
}
