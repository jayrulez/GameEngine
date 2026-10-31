// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample015 - GPU Queries.
/// Demonstrates GPU timestamp queries to measure render pass duration.
/// Prints render pass GPU time to console every 2 seconds.

#include <cstdint>
#include <cstdio>
#include <cstring>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

class QuerySample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override { return u8"Sample015 - GPU Queries"; }

protected:
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        struct VSInput { float3 Position : TEXCOORD0; float4 Color : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float4 Color : COLOR0; };
        PSInput VSMain(VSInput i) { PSInput o; o.Position = float4(i.Position,1); o.Color = i.Color; return o; }
        float4 PSMain(PSInput i) : SV_TARGET { return i.Color; }
    )";
    static constexpr float kVerts[] = {
        0.0f, 0.5f, 0.0f, 1.0f,  0.3f,  0.3f, 1.0f, 0.5f, -0.5f, 0.0f, 0.3f,
        1.0f, 0.3f, 1.0f, -0.5f, -0.5f, 0.0f, 0.3f, 0.3f, 1.0f,  1.0f,
    };
    static constexpr foundation::core::u16 kIdx[] = {0, 1, 2};

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::QuerySet* m_tsQuerySet = nullptr;
    rhi::Buffer* m_queryResultBuf = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
    int m_frameCount = 0;
    float m_lastReportTime = 0.0f;
};

foundation::core::Status QuerySample::OnInit()
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
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Timestamp query set: 2 queries (before and after the render pass).
    rhi::QuerySetDesc qsd{};
    qsd.type = rhi::QueryType::Timestamp;
    qsd.count = 2;
    if (m_device->CreateQuerySet(qsd, m_tsQuerySet) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Buffer to receive resolved query results (2 * uint64 = 16 bytes).
    rhi::BufferDesc qbd{};
    qbd.size = 16;
    qbd.usage = rhi::BufferUsage::CopyDst;
    qbd.memory = rhi::MemoryLocation::GpuToCpu;
    if (m_device->CreateBuffer(qbd, m_queryResultBuf) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    return foundation::core::ErrorCode::Ok;
}

void QuerySample::OnRender()
{
    using foundation::core::f32, foundation::core::u64, foundation::core::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);

    // Read back previous frame's query results (after fence wait ensures GPU is done).
    if (m_frameCount > 1)
    {
        void* mapped = m_queryResultBuf->Map();
        if (mapped)
        {
            auto* timestamps = static_cast<u64*>(mapped);
            u64 begin = timestamps[0], end = timestamps[1], delta = end - begin;
            f32 period = m_graphicsQueue->TimestampPeriod();
            f32 gpuTimeUs = static_cast<f32>(delta) * period / 1000.0f;
            if (m_totalTime - m_lastReportTime >= 2.0f)
            {
                std::printf("GPU render pass time: %.2f us (%llu ticks, period=%.2f ns)\n",
                            gpuTimeUs, static_cast<unsigned long long>(delta), period);
                m_lastReportTime = m_totalTime;
            }
            m_queryResultBuf->Unmap();
        }
    }

    if (m_swapChain->AcquireNextImage() != foundation::core::ErrorCode::Ok)
        return;
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != foundation::core::ErrorCode::Ok || !enc)
        return;

    // Reset queries for this frame.
    enc->ResetQuerySet(m_tsQuerySet, 0, 2);

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    // Timestamp before render pass.
    enc->WriteTimestamp(m_tsQuerySet, 0);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
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
    rp->DrawIndexed(3);
    rp->End();

    // Timestamp after render pass.
    enc->WriteTimestamp(m_tsQuerySet, 1);

    // Resolve timestamps to buffer.
    enc->ResolveQuerySet(m_tsQuerySet, 0, 2, m_queryResultBuf, 0);

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);
    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
    m_frameCount++;
}

void QuerySample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_queryResultBuf)
        m_device->DestroyBuffer(m_queryResultBuf);
    if (m_tsQuerySet)
        m_device->DestroyQuerySet(m_tsQuerySet);
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
    QuerySample app;
    return app.Run(argc, argv);
}
