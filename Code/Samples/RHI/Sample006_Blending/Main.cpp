// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample006 - Alpha Blending.
/// Renders overlapping semi-transparent colored quads.

#include <cstdint>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

class BlendingSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override { return u8"Sample006 - Alpha Blending"; }

protected:
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct VSInput { float3 Position : TEXCOORD0; float4 Color : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float4 Color : COLOR0; };
        PSInput VSMain(VSInput i) { PSInput o; o.Position = float4(i.Position, 1.0); o.Color = i.Color; return o; }
        float4 PSMain(PSInput i) : SV_TARGET { return i.Color; }
    )";
    // 4 quads: an opaque background and 3 overlapping translucent quads.
    static constexpr float kVerts[] = {
        -.9f, -.9f, .5f,  .15f, .15f, .2f,  1,   .9f, -.9f, .5f,  .15f, .15f, .2f,  1,    .9f, .9f,
        .5f,  .15f, .15f, .2f,  1,    -.9f, .9f, .5f, .15f, .15f, .2f,  1,    -.6f, -.4f, .3f, 1,
        .2f,  .2f,  .5f,  .1f,  -.4f, .3f,  1,   .2f, .2f,  .5f,  .1f,  .4f,  .3f,  1,    .2f, .2f,
        .5f,  -.6f, .4f,  .3f,  1,    .2f,  .2f, .5f, -.3f, -.5f, .2f,  .2f,  1,    .2f,  .5f, .4f,
        -.5f, .2f,  .2f,  1,    .2f,  .5f,  .4f, .3f, .2f,  .2f,  1,    .2f,  .5f,  -.3f, .3f, .2f,
        .2f,  1,    .2f,  .5f,  -.1f, -.3f, .1f, .2f, .3f,  1,    .5f,  .6f,  -.3f, .1f,  .2f, .3f,
        1,    .5f,  .6f,  .5f,  .1f,  .2f,  .3f, 1,   .5f,  -.1f, .5f,  .1f,  .2f,  .3f,  1,   .5f,
    };
    static constexpr foundation::core::u16 kIdx[] = {0, 1, 2,  0, 2,  3,  4,  5,  6,  4,  6,  7,
                                                   8, 9, 10, 8, 10, 11, 12, 13, 14, 12, 14, 15};

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline *m_opaquePipe = nullptr, *m_blendPipe = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
};

foundation::core::Status BlendingSample::OnInit()
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

    // Empty pipeline layout.
    rhi::PipelineLayoutDesc pld{};
    if (m_device->CreatePipelineLayout(pld, m_pl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x4, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 28;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
    rhi::ColorTargetState ctOpaque{};
    ctOpaque.format = m_swapChain->Format();
    rhi::ColorTargetState ctBlend{};
    ctBlend.format = m_swapChain->Format();
    ctBlend.blend = rhi::BlendState::AlphaBlend();

    // Opaque pipeline (for background quad).
    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_pl;
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
    rpd.fragment = rhi::FragmentState{};
    rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
    rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ctOpaque, 1);
    if (m_device->CreateRenderPipeline(rpd, m_opaquePipe) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Blend pipeline (for translucent quads).
    rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ctBlend, 1);
    if (m_device->CreateRenderPipeline(rpd, m_blendPipe) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    return foundation::core::ErrorCode::Ok;
}

void BlendingSample::OnRender()
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

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.05f, 0.05f, 0.08f, 1);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    // Draw background opaque.
    rp->SetPipeline(m_opaquePipe);
    rp->DrawIndexed(6, 1, 0, 0, 0);
    // Draw 3 translucent quads.
    rp->SetPipeline(m_blendPipe);
    rp->DrawIndexed(18, 1, 6, 0, 0);
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

void BlendingSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_blendPipe)
        m_device->DestroyRenderPipeline(m_blendPipe);
    if (m_opaquePipe)
        m_device->DestroyRenderPipeline(m_opaquePipe);
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
    BlendingSample app;
    return app.Run(argc, argv);
}
