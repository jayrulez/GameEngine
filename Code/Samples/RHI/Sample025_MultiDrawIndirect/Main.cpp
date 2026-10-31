// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample025 - Multi-Draw Indirect & Lines.
/// Renders 4 colored quads using a single drawIndexedIndirect call with drawCount=4,
/// then overlays white line wireframes using LineList topology.

#include <cstdint>
#include <cstring>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

struct DrawIndexedIndirectArgs
{
    foundation::core::u32 indexCountPerInstance;
    foundation::core::u32 instanceCount;
    foundation::core::u32 startIndexLocation;
    foundation::core::i32 baseVertexLocation;
    foundation::core::u32 startInstanceLocation;
};

class MultiDrawIndirectSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override
    {
        return u8"Sample025 - Multi-Draw Indirect & Lines";
    }

protected:
    rhi::DeviceFeatures RequiredFeatures() const override
    {
        rhi::DeviceFeatures f{};
        f.multiDrawIndirect = true;
        return f;
    }
    foundation::core::Status OnInit() override;
    void OnRender() override;
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

    foundation::core::Status createGeometry();
    foundation::core::Status createIndirectBuffer();
    foundation::core::Status createLineGeometry();

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr, *m_indirectBuf = nullptr, *m_lineVb = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline *m_fillPipeline = nullptr, *m_linePipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
};

foundation::core::Status MultiDrawIndirectSample::OnInit()
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

    if (createGeometry() != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (createIndirectBuffer() != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (createLineGeometry() != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Pipeline layout (empty: no bind groups needed).
    rhi::PipelineLayoutDesc pld{};
    if (m_device->CreatePipelineLayout(pld, m_pl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Vertex layout: pos float3 and color float4, stride 28.
    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x4, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 28;
    vbl.stepMode = rhi::VertexStepMode::Vertex;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);

    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();

    // Fill pipeline (TriangleList).
    {
        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_pl;
        rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
        rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
        rpd.fragment = rhi::FragmentState{};
        rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
        rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
        rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        if (m_device->CreateRenderPipeline(rpd, m_fillPipeline) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    // Line pipeline (LineList).
    {
        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_pl;
        rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
        rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
        rpd.fragment = rhi::FragmentState{};
        rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
        rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
        rpd.primitive.topology = rhi::PrimitiveTopology::LineList;
        if (m_device->CreateRenderPipeline(rpd, m_linePipeline) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    return foundation::core::ErrorCode::Ok;
}

foundation::core::Status MultiDrawIndirectSample::createGeometry()
{
    using foundation::core::Status, foundation::core::Span, foundation::core::u8, foundation::core::u32;

    // 4 quads at different positions.
    static constexpr float verts[] = {
        // Quad 0: top-left (red)
        -0.9f,
        0.1f,
        0.5f,
        0.8f,
        0.2f,
        0.2f,
        1.0f,
        -0.1f,
        0.1f,
        0.5f,
        0.8f,
        0.2f,
        0.2f,
        1.0f,
        -0.1f,
        0.9f,
        0.5f,
        1.0f,
        0.4f,
        0.4f,
        1.0f,
        -0.9f,
        0.9f,
        0.5f,
        1.0f,
        0.4f,
        0.4f,
        1.0f,

        // Quad 1: top-right (green)
        0.1f,
        0.1f,
        0.5f,
        0.2f,
        0.8f,
        0.2f,
        1.0f,
        0.9f,
        0.1f,
        0.5f,
        0.2f,
        0.8f,
        0.2f,
        1.0f,
        0.9f,
        0.9f,
        0.5f,
        0.4f,
        1.0f,
        0.4f,
        1.0f,
        0.1f,
        0.9f,
        0.5f,
        0.4f,
        1.0f,
        0.4f,
        1.0f,

        // Quad 2: bottom-left (blue)
        -0.9f,
        -0.9f,
        0.5f,
        0.2f,
        0.2f,
        0.8f,
        1.0f,
        -0.1f,
        -0.9f,
        0.5f,
        0.2f,
        0.2f,
        0.8f,
        1.0f,
        -0.1f,
        -0.1f,
        0.5f,
        0.4f,
        0.4f,
        1.0f,
        1.0f,
        -0.9f,
        -0.1f,
        0.5f,
        0.4f,
        0.4f,
        1.0f,
        1.0f,

        // Quad 3: bottom-right (yellow)
        0.1f,
        -0.9f,
        0.5f,
        0.8f,
        0.8f,
        0.2f,
        1.0f,
        0.9f,
        -0.9f,
        0.5f,
        0.8f,
        0.8f,
        0.2f,
        1.0f,
        0.9f,
        -0.1f,
        0.5f,
        1.0f,
        1.0f,
        0.4f,
        1.0f,
        0.1f,
        -0.1f,
        0.5f,
        1.0f,
        1.0f,
        0.4f,
        1.0f,
    };

    static constexpr foundation::core::u16 indices[] = {
        0,  1,  2,  0,  2,  3,  // Quad 0
        4,  5,  6,  4,  6,  7,  // Quad 1
        8,  9,  10, 8,  10, 11, // Quad 2
        12, 13, 14, 12, 14, 15, // Quad 3
    };

    rhi::BufferDesc vbd{};
    vbd.size = sizeof(verts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vb) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::BufferDesc ibd{};
    ibd.size = sizeof(indices);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(ibd, m_ib) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0, Span<const u8>(reinterpret_cast<const u8*>(verts), sizeof(verts)));
    batch->WriteBuffer(m_ib, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(indices), sizeof(indices)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    return foundation::core::ErrorCode::Ok;
}

foundation::core::Status MultiDrawIndirectSample::createIndirectBuffer()
{
    using foundation::core::Status, foundation::core::Span, foundation::core::u8;

    // 4 indirect draw commands, one per quad.
    DrawIndexedIndirectArgs args[4] = {
        {6, 1, 0, 0, 0},
        {6, 1, 6, 0, 0},
        {6, 1, 12, 0, 0},
        {6, 1, 18, 0, 0},
    };

    rhi::BufferDesc bd{};
    bd.size = sizeof(args);
    bd.usage = rhi::BufferUsage::Indirect | rhi::BufferUsage::CopyDst;
    bd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(bd, m_indirectBuf) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_indirectBuf, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(args), sizeof(args)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    return foundation::core::ErrorCode::Ok;
}

foundation::core::Status MultiDrawIndirectSample::createLineGeometry()
{
    using foundation::core::Status, foundation::core::Span, foundation::core::u8;

    // Line wireframes for each quad: 4 edges, so 8 verts per quad, white lines.
    static constexpr float lineVerts[] = {
        // Quad 0 edges
        -0.9f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.9f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.9f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.9f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,

        // Quad 1 edges
        0.1f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.1f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.1f,
        0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.1f,
        0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,

        // Quad 2 edges
        -0.9f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.1f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.9f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.9f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        -0.9f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,

        // Quad 3 edges
        0.1f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.9f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.1f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.1f,
        -0.1f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
        0.1f,
        -0.9f,
        0.4f,
        1.0f,
        1.0f,
        1.0f,
        1.0f,
    };

    rhi::BufferDesc bd{};
    bd.size = sizeof(lineVerts);
    bd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    bd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(bd, m_lineVb) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_lineVb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(lineVerts), sizeof(lineVerts)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    return foundation::core::ErrorCode::Ok;
}

void MultiDrawIndirectSample::OnRender()
{
    using foundation::core::f32, foundation::core::Span, foundation::core::u32;
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
    ca.clearValue = rhi::ClearColor(0.06f, 0.06f, 0.1f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0.0f, 1.0f);
    rp->SetScissor(0, 0, m_width, m_height);

    // Pass 1: Draw all 4 quads with a single multi-draw indirect call.
    rp->SetPipeline(m_fillPipeline);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->DrawIndexedIndirect(m_indirectBuf, 0, 4, static_cast<u32>(sizeof(DrawIndexedIndirectArgs)));

    // Pass 2: Draw line wireframes.
    rp->SetPipeline(m_linePipeline);
    rp->SetVertexBuffer(0, m_lineVb, 0);
    rp->Draw(32); // 4 quads * 4 edges * 2 verts = 32 line verts

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

void MultiDrawIndirectSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_linePipeline)
        m_device->DestroyRenderPipeline(m_linePipeline);
    if (m_fillPipeline)
        m_device->DestroyRenderPipeline(m_fillPipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_ps)
        m_device->DestroyShaderModule(m_ps);
    if (m_vs)
        m_device->DestroyShaderModule(m_vs);
    if (m_lineVb)
        m_device->DestroyBuffer(m_lineVb);
    if (m_indirectBuf)
        m_device->DestroyBuffer(m_indirectBuf);
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
    MultiDrawIndirectSample app;
    return app.Run(argc, argv);
}
