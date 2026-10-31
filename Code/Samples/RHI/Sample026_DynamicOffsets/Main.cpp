// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample026 - Dynamic Offsets & Blend Constants.
/// Demonstrates dynamic uniform buffer offsets and blend constants.
/// Draws 4 quads, each reading from a different offset in one shared UBO.
/// Uses setBlendConstant with BlendFactor::Constant for per-frame color modulation.

#include <cmath>
#include <cstdint>
#include <cstring>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

class DynamicOffsetSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override
    {
        return u8"Sample026 - Dynamic Offsets & Blend Constants";
    }

protected:
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    void updateUBO();

    static constexpr const char8_t kShader[] = u8R"(
        cbuffer ObjectData : register(b0, space0)
        {
            float4 TintColor;
            float4 OffsetScale; // xy=offset, zw=scale
        };

        struct VSInput
        {
            float3 Position : TEXCOORD0;
        };

        struct PSInput
        {
            float4 Position : SV_POSITION;
        };

        PSInput VSMain(VSInput input)
        {
            PSInput output;
            float2 pos = input.Position.xy * OffsetScale.zw + OffsetScale.xy;
            output.Position = float4(pos, input.Position.z, 1.0);
            return output;
        }

        float4 PSMain(PSInput input) : SV_TARGET
        {
            return TintColor;
        }
    )";

    struct ObjectData
    {
        float tintColor[4];
        float offsetScale[4];
        // Pad to 256-byte alignment (D3D12 CBV minimum).
        float _pad[56];
    };

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr, *m_ub = nullptr;
    rhi::BindGroupLayout* m_bgl = nullptr;
    rhi::BindGroup* m_bg = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
};

foundation::core::Status DynamicOffsetSample::OnInit()
{
    using foundation::core::Status, foundation::core::Span, foundation::core::u8, foundation::core::u32;
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"DynVS",
                                            m_vs) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"DynPS",
                                            m_ps) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Unit quad vertices (will be transformed by UBO data).
    static constexpr float verts[] = {
        -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
    };
    static constexpr foundation::core::u16 indices[] = {0, 1, 2, 0, 2, 3};

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

    // Uniform buffer: 4 ObjectData structs (256 bytes each = 1024 total).
    rhi::BufferDesc ubd{};
    ubd.size = 256 * 4;
    ubd.usage = rhi::BufferUsage::Uniform;
    ubd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(ubd, m_ub) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Initialize UBO data.
    updateUBO();

    // Bind group layout with dynamic offset UBO.
    rhi::BindGroupLayoutEntry entry = rhi::BindGroupLayoutEntry::UniformBuffer(
        0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
    entry.hasDynamicOffset = true;
    rhi::BindGroupLayoutEntry entries[1] = {entry};
    rhi::BindGroupLayoutDesc bgld{};
    bgld.entries = Span<const rhi::BindGroupLayoutEntry>(entries, 1);
    if (m_device->CreateBindGroupLayout(bgld, m_bgl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Bind group (bind the whole buffer, dynamic offset selects the slice).
    rhi::BindGroupEntry bgEntries[1] = {rhi::BindGroupEntry::BufferEntry(m_ub, 0, 256)};
    rhi::BindGroupDesc bgd{};
    bgd.layout = m_bgl;
    bgd.entries = Span<const rhi::BindGroupEntry>(bgEntries, 1);
    if (m_device->CreateBindGroup(bgd, m_bg) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Pipeline layout.
    rhi::BindGroupLayout* sets[1] = {m_bgl};
    rhi::PipelineLayoutDesc pld{};
    pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 1);
    if (m_device->CreatePipelineLayout(pld, m_pl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Pipeline with blend constant support.
    rhi::VertexAttribute attrs[1] = {{rhi::VertexFormat::Float32x3, 0, 0}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 12;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 1);

    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    ct.writeMask = rhi::ColorWriteMask::All;
    ct.blend = rhi::BlendState{
        {rhi::BlendFactor::Constant, rhi::BlendFactor::OneMinusConstant, rhi::BlendOperation::Add},
        {rhi::BlendFactor::One, rhi::BlendFactor::Zero, rhi::BlendOperation::Add}};

    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_pl;
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
    rpd.fragment = rhi::FragmentState{};
    rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
    rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    return foundation::core::ErrorCode::Ok;
}

void DynamicOffsetSample::updateUBO()
{
    void* mapped = m_ub->Map();
    if (!mapped)
        return;

    // 4 objects at different positions with different colors.
    ObjectData objs[4] = {};

    // Red, top-left.
    objs[0].tintColor[0] = 1.0f;
    objs[0].tintColor[1] = 0.2f;
    objs[0].tintColor[2] = 0.2f;
    objs[0].tintColor[3] = 1.0f;
    objs[0].offsetScale[0] = -0.45f;
    objs[0].offsetScale[1] = 0.45f;
    objs[0].offsetScale[2] = 0.4f;
    objs[0].offsetScale[3] = 0.4f;

    // Green, top-right.
    objs[1].tintColor[0] = 0.2f;
    objs[1].tintColor[1] = 1.0f;
    objs[1].tintColor[2] = 0.2f;
    objs[1].tintColor[3] = 1.0f;
    objs[1].offsetScale[0] = 0.45f;
    objs[1].offsetScale[1] = 0.45f;
    objs[1].offsetScale[2] = 0.4f;
    objs[1].offsetScale[3] = 0.4f;

    // Blue, bottom-left.
    objs[2].tintColor[0] = 0.2f;
    objs[2].tintColor[1] = 0.3f;
    objs[2].tintColor[2] = 1.0f;
    objs[2].tintColor[3] = 1.0f;
    objs[2].offsetScale[0] = -0.45f;
    objs[2].offsetScale[1] = -0.45f;
    objs[2].offsetScale[2] = 0.4f;
    objs[2].offsetScale[3] = 0.4f;

    // Yellow, bottom-right.
    objs[3].tintColor[0] = 1.0f;
    objs[3].tintColor[1] = 1.0f;
    objs[3].tintColor[2] = 0.2f;
    objs[3].tintColor[3] = 1.0f;
    objs[3].offsetScale[0] = 0.45f;
    objs[3].offsetScale[1] = -0.45f;
    objs[3].offsetScale[2] = 0.4f;
    objs[3].offsetScale[3] = 0.4f;

    std::memcpy(mapped, objs, sizeof(objs));
    m_ub->Unmap();
}

void DynamicOffsetSample::OnRender()
{
    using foundation::core::f32, foundation::core::u32, foundation::core::Span;
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
    ca.clearValue = rhi::ClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);

    // Animate blend constant: pulsing between full visibility and half.
    f32 pulse = 0.5f + 0.5f * std::sin(m_totalTime * 2.0f);
    rp->SetBlendConstant(pulse, pulse, pulse, 1.0f);

    // Draw 4 objects, each at a different dynamic offset.
    for (u32 i = 0; i < 4; i++)
    {
        u32 off[1] = {i * 256};
        rp->SetBindGroup(0, m_bg, Span<const u32>(off, 1));
        rp->DrawIndexed(6);
    }

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

void DynamicOffsetSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_bg)
        m_device->DestroyBindGroup(m_bg);
    if (m_bgl)
        m_device->DestroyBindGroupLayout(m_bgl);
    if (m_ub)
        m_device->DestroyBuffer(m_ub);
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
    DynamicOffsetSample app;
    return app.Run(argc, argv);
}
