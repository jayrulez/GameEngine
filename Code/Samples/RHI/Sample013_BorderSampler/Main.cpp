#include <new>
/// Sample013 - Border Sampler. Ported from Sedulous Sample013_BorderSampler.
/// Demonstrates sampler border colors: TransparentBlack, OpaqueBlack, OpaqueWhite.
/// Three quads with UVs extending beyond [0,1] to show the border region.

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

class BorderSamplerSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override { return u8"Sample013 - Border Sampler"; }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr const char8_t kShader[] = u8R"(
        Texture2D gTexture : register(t0, space0);
        SamplerState gSampler : register(s0, space0);
        cbuffer UBO : register(b0, space1) { float4 QuadOffset; };
        struct VSInput { float3 Position : TEXCOORD0; float2 TexCoord : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float2 TexCoord : TEXCOORD0; };
        PSInput VSMain(VSInput input) {
            PSInput output;
            output.Position = float4(input.Position.xy + QuadOffset.xy, input.Position.z, 1.0);
            output.TexCoord = input.TexCoord;
            return output;
        }
        float4 PSMain(PSInput input) : SV_TARGET {
            return gTexture.Sample(gSampler, input.TexCoord);
        }
    )";

    // Quad with UVs from -0.5 to 1.5 to show border region.
    static constexpr float kQuadVerts[] = {
        -0.25f, -0.25f, 0.0f, -0.5f, -0.5f, 0.25f,  -0.25f, 0.0f, 1.5f,  -0.5f,
        0.25f,  0.25f,  0.0f, 1.5f,  1.5f,  -0.25f, 0.25f,  0.0f, -0.5f, 1.5f,
    };
    static constexpr draconic::foundation::u16 kQuadIdx[] = {0, 1, 2, 0, 2, 3};

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr, *m_ub = nullptr;
    void* m_ubMapped = nullptr;
    rhi::Texture* m_tex = nullptr;
    rhi::TextureView* m_texView = nullptr;
    rhi::Sampler *m_sampTransparent = nullptr, *m_sampOpaqueBlack = nullptr,
                 *m_sampOpaqueWhite = nullptr;
    rhi::BindGroupLayout *m_texBgl = nullptr, *m_uboBgl = nullptr;
    rhi::BindGroup *m_bgTransparent = nullptr, *m_bgOpaqueBlack = nullptr,
                   *m_bgOpaqueWhite = nullptr;
    rhi::BindGroup* m_uboBg = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status BorderSamplerSample::OnInit()
{
    if (!m_device->features.borderSampling)
    {
        rhi::LogError("ERROR: Border sampling (ClampToBorder) is not supported by this "
                      "device/backend");
        return draconic::foundation::ErrorCode::Unknown;
    }

    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;
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

    // Buffers.
    rhi::BufferDesc vbd{};
    vbd.size = sizeof(kQuadVerts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kQuadIdx);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Uniform buffer: 3 slots * 256 bytes (DX12 CBV alignment).
    rhi::BufferDesc ubd{};
    ubd.size = 768;
    ubd.usage = rhi::BufferUsage::Uniform;
    ubd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(ubd, m_ub) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_ubMapped = m_ub->Map();
    // Write all 3 offsets upfront.
    float off0[4] = {-0.55f, 0.0f, 0.0f, 0.0f};
    float off1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float off2[4] = {0.55f, 0.0f, 0.0f, 0.0f};
    std::memcpy(static_cast<u8*>(m_ubMapped), off0, 16);
    std::memcpy(static_cast<u8*>(m_ubMapped) + 256, off1, 16);
    std::memcpy(static_cast<u8*>(m_ubMapped) + 512, off2, 16);

    // 8x8 checkerboard texture (red/white).
    constexpr u32 tw = 8, th = 8;
    u8 texData[tw * th * 4];
    for (u32 y = 0; y < th; ++y)
        for (u32 x = 0; x < tw; ++x)
        {
            u32 i = (y * tw + x) * 4;
            bool white = ((x + y) % 2) == 0;
            texData[i + 0] = white ? 255 : 220;
            texData[i + 1] = white ? 255 : 60;
            texData[i + 2] = white ? 255 : 60;
            texData[i + 3] = 255;
        }
    rhi::TextureDesc td{};
    td.format = rhi::TextureFormat::RGBA8Unorm;
    td.width = tw;
    td.height = th;
    td.mipLevelCount = 1;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    if (m_device->CreateTexture(td, m_tex) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kQuadVerts), sizeof(kQuadVerts)));
    batch->WriteBuffer(m_ib, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kQuadIdx), sizeof(kQuadIdx)));
    rhi::TextureDataLayout layout{};
    layout.bytesPerRow = tw * 4;
    layout.rowsPerImage = th;
    batch->WriteTexture(m_tex, Span<const u8>(texData, sizeof(texData)), layout,
                        rhi::Extent3D{tw, th, 1});
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::RGBA8Unorm;
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    if (m_device->CreateTextureView(m_tex, tvd, m_texView) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Three samplers with ClampToBorder and different border colors.
    auto makeSampler = [&](rhi::SamplerBorderColor bc, rhi::Sampler*& out) -> draconic::foundation::Status
    {
        rhi::SamplerDesc sd{};
        sd.minFilter = rhi::FilterMode::Nearest;
        sd.magFilter = rhi::FilterMode::Nearest;
        sd.addressU = rhi::AddressMode::ClampToBorder;
        sd.addressV = rhi::AddressMode::ClampToBorder;
        sd.addressW = rhi::AddressMode::ClampToBorder;
        sd.borderColor = bc;
        return m_device->CreateSampler(sd, out);
    };
    if (makeSampler(rhi::SamplerBorderColor::TransparentBlack, m_sampTransparent) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (makeSampler(rhi::SamplerBorderColor::OpaqueBlack, m_sampOpaqueBlack) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (makeSampler(rhi::SamplerBorderColor::OpaqueWhite, m_sampOpaqueWhite) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Bind group layout: set 0 = texture + sampler.
    rhi::BindGroupLayoutEntry tE[2] = {
        rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
        rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment)};
    rhi::BindGroupLayoutDesc tBgld{};
    tBgld.entries = Span<const rhi::BindGroupLayoutEntry>(tE, 2);
    if (m_device->CreateBindGroupLayout(tBgld, m_texBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Three bind groups, one per sampler.
    auto makeBG = [&](rhi::Sampler* s, rhi::BindGroup*& out) -> draconic::foundation::Status
    {
        rhi::BindGroupEntry e[2] = {rhi::BindGroupEntry::TextureEntry(m_texView),
                                    rhi::BindGroupEntry::SamplerEntry(s)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_texBgl;
        bgd.entries = Span<const rhi::BindGroupEntry>(e, 2);
        return m_device->CreateBindGroup(bgd, out);
    };
    if (makeBG(m_sampTransparent, m_bgTransparent) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (makeBG(m_sampOpaqueBlack, m_bgOpaqueBlack) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (makeBG(m_sampOpaqueWhite, m_bgOpaqueWhite) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Bind group layout: set 1 = uniform buffer with dynamic offset.
    rhi::BindGroupLayoutEntry uEntry =
        rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex);
    uEntry.hasDynamicOffset = true;
    rhi::BindGroupLayoutEntry uE[1] = {uEntry};
    rhi::BindGroupLayoutDesc uBgld{};
    uBgld.entries = Span<const rhi::BindGroupLayoutEntry>(uE, 1);
    if (m_device->CreateBindGroupLayout(uBgld, m_uboBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupEntry uBgE[1] = {rhi::BindGroupEntry::BufferEntry(m_ub, 0, 16)};
    rhi::BindGroupDesc uBgd{};
    uBgd.layout = m_uboBgl;
    uBgd.entries = Span<const rhi::BindGroupEntry>(uBgE, 1);
    if (m_device->CreateBindGroup(uBgd, m_uboBg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Pipeline layout.
    rhi::BindGroupLayout* sets[2] = {m_texBgl, m_uboBgl};
    rhi::PipelineLayoutDesc pld{};
    pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 2);
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x2, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 20;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    ct.blend = rhi::BlendState::AlphaBlend();
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

void BorderSamplerSample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::u32, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.2f, 0.2f, 0.25f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);

    // Draw 3 quads side by side with different samplers and dynamic UBO offsets.
    rhi::BindGroup* texBGs[3] = {m_bgTransparent, m_bgOpaqueBlack, m_bgOpaqueWhite};
    u32 dynOffsets[3] = {0, 256, 512};
    for (int i = 0; i < 3; ++i)
    {
        rp->SetBindGroup(0, texBGs[i]);
        u32 off[1] = {dynOffsets[i]};
        rp->SetBindGroup(1, m_uboBg, Span<const u32>(off, 1));
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

void BorderSamplerSample::OnShutdown()
{
    if (m_ub && m_ubMapped)
        m_ub->Unmap();
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
    if (m_bgOpaqueWhite)
        m_device->DestroyBindGroup(m_bgOpaqueWhite);
    if (m_bgOpaqueBlack)
        m_device->DestroyBindGroup(m_bgOpaqueBlack);
    if (m_bgTransparent)
        m_device->DestroyBindGroup(m_bgTransparent);
    if (m_texBgl)
        m_device->DestroyBindGroupLayout(m_texBgl);
    if (m_sampOpaqueWhite)
        m_device->DestroySampler(m_sampOpaqueWhite);
    if (m_sampOpaqueBlack)
        m_device->DestroySampler(m_sampOpaqueBlack);
    if (m_sampTransparent)
        m_device->DestroySampler(m_sampTransparent);
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
    BorderSamplerSample app;
    return app.Run(argc, argv);
}
