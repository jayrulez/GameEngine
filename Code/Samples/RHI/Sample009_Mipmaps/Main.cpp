// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample009 - Mipmaps.
/// Textured quad that recedes into the distance showing mip level selection.
/// Uses generateMipmaps() to generate the mip chain from a base texture.

#include <cmath>
#include <cstdint>
#include <cstring>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
using foundation::core::Float4x4;

class MipmapSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override { return u8"Sample009 - Mipmaps"; }

protected:
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnResize(foundation::core::u32 w, foundation::core::u32 h) override
    {
        m_depthBuf.Recreate(m_device, w, h);
    }
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        Texture2D gTexture : register(t0, space0);
        SamplerState gSampler : register(s0, space0);
        cbuffer UBO : register(b0, space1) { row_major float4x4 MVP; };
        struct VSInput { float3 Position : TEXCOORD0; float2 TexCoord : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float2 TexCoord : TEXCOORD0; };
        PSInput VSMain(VSInput i) { PSInput o; o.Position = mul(float4(i.Position,1), MVP); o.TexCoord = i.TexCoord; return o; }
        float4 PSMain(PSInput i) : SV_TARGET { return gTexture.Sample(gSampler, i.TexCoord); }
    )";
    // Receding floor plane.
    static constexpr float kVerts[] = {-4, 0, 0,   0, 0,  4,  0, 0,   8, 0,
                                       4,  0, -20, 8, 10, -4, 0, -20, 0, 10};
    static constexpr foundation::core::u16 kIdx[] = {0, 1, 2, 0, 2, 3};

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr, *m_ub = nullptr;
    void* m_ubMapped = nullptr;
    rhi::Texture* m_tex = nullptr;
    rhi::TextureView* m_texView = nullptr;
    rhi::Sampler* m_sampler = nullptr;
    rhi::BindGroupLayout *m_texBgl = nullptr, *m_uboBgl = nullptr;
    rhi::BindGroup *m_texBg = nullptr, *m_uboBg = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
    samples::framework::DepthBuffer m_depthBuf;
};

foundation::core::Status MipmapSample::OnInit()
{
    using foundation::core::Status, foundation::core::Span, foundation::core::u8, foundation::core::u32;
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

    // Buffers.
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
    rhi::BufferDesc ubd{};
    ubd.size = 64;
    ubd.usage = rhi::BufferUsage::Uniform;
    ubd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(ubd, m_ub) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    m_ubMapped = m_ub->Map();

    // Checkerboard base texture 256x256 with 9 mip levels.
    constexpr u32 tw = 256, th = 256, mipCount = 9;
    u8 texPixels[tw * th * 4];
    for (u32 y = 0; y < th; ++y)
        for (u32 x = 0; x < tw; ++x)
        {
            bool c = ((x / 16) + (y / 16)) % 2 == 0;
            u32 i = (y * tw + x) * 4;
            texPixels[i] = c ? 255 : 30;
            texPixels[i + 1] = c ? 255 : 30;
            texPixels[i + 2] = c ? 255 : 200;
            texPixels[i + 3] = 255;
        }
    rhi::TextureDesc td{};
    td.format = rhi::TextureFormat::RGBA8Unorm;
    td.width = tw;
    td.height = th;
    td.mipLevelCount = mipCount;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc |
               rhi::TextureUsage::CopyDst | rhi::TextureUsage::RenderTarget;
    if (m_device->CreateTexture(td, m_tex) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Upload the base mip and generate the others.
    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kVerts), sizeof(kVerts)));
    batch->WriteBuffer(m_ib, 0, Span<const u8>(reinterpret_cast<const u8*>(kIdx), sizeof(kIdx)));
    rhi::TextureDataLayout layout{};
    layout.bytesPerRow = tw * 4;
    layout.rowsPerImage = th;
    batch->WriteTexture(m_tex, Span<const u8>(texPixels, sizeof(texPixels)), layout,
                        rhi::Extent3D{tw, th, 1});
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    // Generate mipmaps then transition to shader-read.
    rhi::CommandPool* tmpPool = nullptr;
    m_device->CreateCommandPool(rhi::QueueType::Graphics, tmpPool);
    rhi::CommandEncoder* enc = nullptr;
    tmpPool->CreateEncoder(enc);
    enc->GenerateMipmaps(m_tex);
    // generateMipmaps leaves all mips in TRANSFER_SRC.
    // Transition all mips to SHADER_READ for sampling.
    rhi::TextureBarrier tb{};
    tb.texture = m_tex;
    tb.oldState = rhi::ResourceState::CopySrc;
    tb.newState = rhi::ResourceState::ShaderRead;
    tb.baseMipLevel = 0;
    tb.mipLevelCount = mipCount;
    tb.baseArrayLayer = 0;
    tb.arrayLayerCount = 1;
    rhi::BarrierGroup bg{};
    bg.textureBarriers = Span<const rhi::TextureBarrier>(&tb, 1);
    enc->Barrier(bg);
    rhi::CommandBuffer* cb = enc->Finish();
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1));
    m_graphicsQueue->WaitIdle();
    tmpPool->DestroyEncoder(enc);
    m_device->DestroyCommandPool(tmpPool);

    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::RGBA8Unorm;
    tvd.mipLevelCount = mipCount;
    tvd.arrayLayerCount = 1;
    if (m_device->CreateTextureView(m_tex, tvd, m_texView) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    rhi::SamplerDesc sd{};
    sd.minFilter = rhi::FilterMode::Linear;
    sd.magFilter = rhi::FilterMode::Linear;
    sd.mipmapFilter = rhi::MipmapFilterMode::Linear;
    sd.maxLod = static_cast<foundation::core::f32>(mipCount);
    if (m_device->CreateSampler(sd, m_sampler) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // Bind groups: set 0 holds the texture and sampler, set 1 the UBO.
    rhi::BindGroupLayoutEntry tE[2] = {
        rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
        rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment)};
    rhi::BindGroupLayoutDesc tBgld{};
    tBgld.entries = Span<const rhi::BindGroupLayoutEntry>(tE, 2);
    if (m_device->CreateBindGroupLayout(tBgld, m_texBgl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    rhi::BindGroupEntry tBgE[2] = {rhi::BindGroupEntry::TextureEntry(m_texView),
                                   rhi::BindGroupEntry::SamplerEntry(m_sampler)};
    rhi::BindGroupDesc tBgd{};
    tBgd.layout = m_texBgl;
    tBgd.entries = Span<const rhi::BindGroupEntry>(tBgE, 2);
    if (m_device->CreateBindGroup(tBgd, m_texBg) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::BindGroupLayoutEntry uE[1] = {
        rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex)};
    rhi::BindGroupLayoutDesc uBgld{};
    uBgld.entries = Span<const rhi::BindGroupLayoutEntry>(uE, 1);
    if (m_device->CreateBindGroupLayout(uBgld, m_uboBgl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    rhi::BindGroupEntry uBgE[1] = {rhi::BindGroupEntry::BufferEntry(m_ub, 0, 64)};
    rhi::BindGroupDesc uBgd{};
    uBgd.layout = m_uboBgl;
    uBgd.entries = Span<const rhi::BindGroupEntry>(uBgE, 1);
    if (m_device->CreateBindGroup(uBgd, m_uboBg) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    rhi::BindGroupLayout* sets[2] = {m_texBgl, m_uboBgl};
    rhi::PipelineLayoutDesc pld{};
    pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 2);
    if (m_device->CreatePipelineLayout(pld, m_pl) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    m_depthBuf.Recreate(m_device, m_width, m_height);

    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x2, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 20;
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

void MipmapSample::OnRender()
{
    using foundation::core::f32, foundation::core::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != foundation::core::ErrorCode::Ok)
        return;
    f32 aspect = static_cast<f32>(m_width) / static_cast<f32>(m_height);
    Float4x4 view =
        Float4x4::LookAtRH(foundation::core::Float3{0, 2, 2}, foundation::core::Float3{0, 0, -5},
                           foundation::core::Float3{0, 1, 0});
    Float4x4 proj =
        Float4x4::PerspectiveFovRH(foundation::core::DegreesToRadians(60.0f), aspect, 0.1f, 100.0f);
    Float4x4 mvp = view * proj;
    std::memcpy(m_ubMapped, mvp.Data(), 64);

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != foundation::core::ErrorCode::Ok || !enc)
        return;
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    enc->TransitionTexture(m_depthBuf.texture, rhi::ResourceState::Undefined,
                           rhi::ResourceState::DepthStencilWrite);
    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.1f, 0.1f, 0.15f, 1);
    rhi::DepthStencilAttachment dsa{};
    dsa.view = m_depthBuf.view;
    dsa.depthLoadOp = rhi::LoadOp::Clear;
    dsa.depthStoreOp = rhi::StoreOp::Store;
    dsa.depthClearValue = rhi::depth::ClearValue();
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    rpd.depthStencilAttachment = dsa;
    auto* rp = enc->BeginRenderPass(rpd);
    rp->SetPipeline(m_pipeline);
    rp->SetBindGroup(0, m_texBg);
    rp->SetBindGroup(1, m_uboBg);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->DrawIndexed(6);
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

void MipmapSample::OnShutdown()
{
    m_depthBuf.Destroy(m_device);
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_uboBg)
        m_device->DestroyBindGroup(m_uboBg);
    if (m_uboBgl)
        m_device->DestroyBindGroupLayout(m_uboBgl);
    if (m_texBg)
        m_device->DestroyBindGroup(m_texBg);
    if (m_texBgl)
        m_device->DestroyBindGroupLayout(m_texBgl);
    if (m_sampler)
        m_device->DestroySampler(m_sampler);
    if (m_texView)
        m_device->DestroyTextureView(m_texView);
    if (m_tex)
        m_device->DestroyTexture(m_tex);
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
    MipmapSample app;
    return app.Run(argc, argv);
}
