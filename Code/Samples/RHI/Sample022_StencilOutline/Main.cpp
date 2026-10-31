// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample022 - Stencil Outline.
/// Demonstrates stencil buffer operations for object outlining.
/// Pass 1: Draw solid hexagon, write stencil = 1.
/// Pass 2: Draw scaled-up hexagon, only where stencil != 1 (outline effect).

#include <cstdint>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

class StencilOutlineSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override { return u8"Sample022 - Stencil Outline"; }

protected:
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnResize(foundation::core::u32 w, foundation::core::u32 h) override
    {
        recreateDepthStencil(w, h);
    }
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct PushConstants
        {
            float Scale;
            float AspectRatio;
            float Time;
            float _pad;
        };

        [[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0, space0);

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
            float2 pos = input.Position.xy * pc.Scale;
            pos.x /= pc.AspectRatio;
            // Gentle rotation
            float c = cos(pc.Time * 0.5);
            float s = sin(pc.Time * 0.5);
            float2 rotated = float2(pos.x * c - pos.y * s, pos.x * s + pos.y * c);
            output.Position = float4(rotated, input.Position.z, 1.0);
            output.Color = input.Color;
            return output;
        }

        float4 PSMain(PSInput input) : SV_TARGET
        {
            return input.Color;
        }
    )";

    struct PushData
    {
        float scale;
        float aspectRatio;
        float time;
        float _pad;
    };

    // Hexagon: center and 6 outer vertices.
    // Stride: 7 floats per vertex (pos xyz, color rgba).
    static constexpr float kVerts[] = {
        // Center
        0.0f,
        0.0f,
        0.5f,
        0.9f,
        0.9f,
        0.9f,
        1.0f,
        // Outer vertices (radius 0.6)
        0.6f,
        0.0f,
        0.5f,
        0.3f,
        0.6f,
        1.0f,
        1.0f,
        0.3f,
        0.52f,
        0.5f,
        0.3f,
        1.0f,
        0.6f,
        1.0f,
        -0.3f,
        0.52f,
        0.5f,
        1.0f,
        1.0f,
        0.3f,
        1.0f,
        -0.6f,
        0.0f,
        0.5f,
        1.0f,
        0.6f,
        0.3f,
        1.0f,
        -0.3f,
        -0.52f,
        0.5f,
        1.0f,
        0.3f,
        0.6f,
        1.0f,
        0.3f,
        -0.52f,
        0.5f,
        0.6f,
        0.3f,
        1.0f,
        1.0f,
    };
    static constexpr foundation::core::u16 kIdx[] = {
        0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 6, 0, 6, 1,
    };

    void recreateDepthStencil(foundation::core::u32 w, foundation::core::u32 h);

    shaders::Compiler* m_compiler = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_stencilWritePipeline = nullptr;
    rhi::RenderPipeline* m_stencilTestPipeline = nullptr;
    rhi::Texture* m_depthStencilTex = nullptr;
    rhi::TextureView* m_depthStencilView = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
};

void StencilOutlineSample::recreateDepthStencil(foundation::core::u32 w, foundation::core::u32 h)
{
    if (m_depthStencilView)
    {
        m_device->DestroyTextureView(m_depthStencilView);
        m_depthStencilView = nullptr;
    }
    if (m_depthStencilTex)
    {
        m_device->DestroyTexture(m_depthStencilTex);
        m_depthStencilTex = nullptr;
    }

    rhi::TextureDesc td = rhi::TextureDesc::DepthBuffer(rhi::TextureFormat::Depth24PlusStencil8, w,
                                                        h, 1, u8"StencilDSTex");
    m_device->CreateTexture(td, m_depthStencilTex);
    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::Depth24PlusStencil8;
    tvd.dimension = rhi::TextureViewDimension::Texture2D;
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    m_device->CreateTextureView(m_depthStencilTex, tvd, m_depthStencilView);
}

foundation::core::Status StencilOutlineSample::OnInit()
{
    using foundation::core::Status, foundation::core::Span, foundation::core::u8;

    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"StencilVS",
                                            m_vs) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain",
                                            u8"StencilPS", m_ps) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Vertex & index buffers.
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

    // Pipeline layout with push constants (no bind groups).
    rhi::PushConstantRange pcRange{rhi::ShaderStage::Vertex, 0, sizeof(PushData)};
    rhi::PipelineLayoutDesc pld{};
    pld.pushConstantRanges = Span<const rhi::PushConstantRange>(&pcRange, 1);
    if (m_device->CreatePipelineLayout(pld, m_pl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    recreateDepthStencil(m_width, m_height);

    // Shared vertex layout.
    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x4, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 28;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();

    // Pipeline 1: stencil write. Draw solid, always pass depth, write stencil = ref (1).
    {
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
        rpd.depthStencil->depthCompare = rhi::CompareFunction::Always;
        rpd.depthStencil->stencilEnabled = true;
        rpd.depthStencil->stencilReadMask = 0xFF;
        rpd.depthStencil->stencilWriteMask = 0xFF;
        rpd.depthStencil->stencilFront = {rhi::CompareFunction::Always, rhi::StencilOperation::Keep,
                                          rhi::StencilOperation::Keep,
                                          rhi::StencilOperation::Replace};
        rpd.depthStencil->stencilBack = {rhi::CompareFunction::Always, rhi::StencilOperation::Keep,
                                         rhi::StencilOperation::Keep,
                                         rhi::StencilOperation::Replace};
        if (m_device->CreateRenderPipeline(rpd, m_stencilWritePipeline) !=
            foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    // Pipeline 2: stencil test. Draw the outline only where stencil != 1.
    {
        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_pl;
        rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
        rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
        rpd.fragment = rhi::FragmentState{};
        rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
        rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
        rpd.depthStencil = rhi::DepthStencilState{};
        rpd.depthStencil->format = rhi::TextureFormat::Depth24PlusStencil8;
        rpd.depthStencil->depthWriteEnabled = false;
        rpd.depthStencil->depthCompare = rhi::CompareFunction::Always;
        rpd.depthStencil->stencilEnabled = true;
        rpd.depthStencil->stencilReadMask = 0xFF;
        rpd.depthStencil->stencilWriteMask = 0x00;
        rpd.depthStencil->stencilFront = {rhi::CompareFunction::NotEqual,
                                          rhi::StencilOperation::Keep, rhi::StencilOperation::Keep,
                                          rhi::StencilOperation::Keep};
        rpd.depthStencil->stencilBack = {rhi::CompareFunction::NotEqual,
                                         rhi::StencilOperation::Keep, rhi::StencilOperation::Keep,
                                         rhi::StencilOperation::Keep};
        if (m_device->CreateRenderPipeline(rpd, m_stencilTestPipeline) !=
            foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    return foundation::core::ErrorCode::Ok;
}

void StencilOutlineSample::OnRender()
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
    enc->TransitionTexture(m_depthStencilTex, rhi::ResourceState::Undefined,
                           rhi::ResourceState::DepthStencilWrite);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.08f, 0.08f, 0.12f, 1.0f);

    rhi::DepthStencilAttachment dsa{};
    dsa.view = m_depthStencilView;
    dsa.depthLoadOp = rhi::LoadOp::Clear;
    dsa.depthStoreOp = rhi::StoreOp::Store;
    dsa.depthClearValue = rhi::depth::ClearValue();
    dsa.stencilLoadOp = rhi::LoadOp::Clear;
    dsa.stencilStoreOp = rhi::StoreOp::Store;
    dsa.stencilClearValue = 0;

    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    rpd.depthStencilAttachment = dsa;
    auto* rp = enc->BeginRenderPass(rpd);

    f32 aspect = static_cast<f32>(m_width) / static_cast<f32>(m_height);

    // Pass 1: Draw solid hexagon, write stencil = 1.
    rp->SetPipeline(m_stencilWritePipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0.0f, 1.0f);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->SetStencilReference(1);
    PushData pc1{1.0f, aspect, m_totalTime, 0.0f};
    rp->SetPushConstants(rhi::ShaderStage::Vertex, 0, sizeof(PushData), &pc1);
    rp->DrawIndexed(18);

    // Pass 2: Draw scaled-up hexagon, only where stencil != 1 (outline ring).
    rp->SetPipeline(m_stencilTestPipeline);
    rp->SetStencilReference(1);
    PushData pc2{1.15f, aspect, m_totalTime, 0.0f};
    rp->SetPushConstants(rhi::ShaderStage::Vertex, 0, sizeof(PushData), &pc2);
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

void StencilOutlineSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_stencilTestPipeline)
        m_device->DestroyRenderPipeline(m_stencilTestPipeline);
    if (m_stencilWritePipeline)
        m_device->DestroyRenderPipeline(m_stencilWritePipeline);
    if (m_depthStencilView)
        m_device->DestroyTextureView(m_depthStencilView);
    if (m_depthStencilTex)
        m_device->DestroyTexture(m_depthStencilTex);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
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
    StencilOutlineSample app;
    return app.Run(argc, argv);
}
