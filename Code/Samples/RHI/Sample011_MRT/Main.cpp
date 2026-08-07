#include <new>
/// Sample011 - Multiple Render Targets. Ported from Sedulous Sample011_MRT.
/// Pass 1: Renders triangles to 2 render targets (color + brightness).
/// Pass 2: Composites both side-by-side via fullscreen triangle.

#include <cstdint>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.samples.framework;
import draconic.rhi.vulkan;

namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

class MRTSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override { return u8"Sample011 - MRT"; }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnResize(draconic::foundation::u32 /*w*/, draconic::foundation::u32 /*h*/) override
    {
        createRenderTargets();
    }
    void OnShutdown() override;

private:
    static constexpr const char8_t kGBufShader[] = u8R"(
        struct VSInput { float3 Position : TEXCOORD0; float4 Color : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float4 Color : COLOR0; };
        struct PSOutput { float4 Color : SV_TARGET0; float4 Brightness : SV_TARGET1; };
        PSInput VSMain(VSInput i) { PSInput o; o.Position = float4(i.Position,1); o.Color = i.Color; return o; }
        PSOutput PSMain(PSInput i) { PSOutput o; o.Color = i.Color;
            float lum = dot(i.Color.rgb, float3(0.299,0.587,0.114));
            o.Brightness = float4(lum,lum,lum,1); return o; }
    )";
    static constexpr const char8_t kCompShader[] = u8R"(
        Texture2D gColorTex : register(t0, space0);
        Texture2D gBrightTex : register(t1, space0);
        SamplerState gSampler : register(s0, space0);
        struct PSInput { float4 Position : SV_POSITION; float2 TexCoord : TEXCOORD0; };
        PSInput VSMain(uint vid : SV_VertexID) { PSInput o;
            float2 uv = float2((vid << 1) & 2, vid & 2);
            o.Position = float4(uv * 2.0 - 1.0, 0, 1); o.TexCoord = float2(uv.x, 1.0 - uv.y); return o; }
        float4 PSMain(PSInput i) : SV_TARGET {
            float2 uv = i.TexCoord;
            if (uv.x < 0.5) return gColorTex.Sample(gSampler, float2(uv.x*2, uv.y));
            else return gBrightTex.Sample(gSampler, float2((uv.x-0.5)*2, uv.y)); }
    )";
    static constexpr float kVerts[] = {
        -.5f, -.5f, 0, 1,   .2f, .2f, 1, .5f,  -.5f, 0, 1,   .2f, .2f, 1,
        0,    .6f,  0, 1,   .8f, .2f, 1, -.3f, -.3f, 0, .2f, .3f, 1,   1,
        .7f,  -.1f, 0, .2f, .3f, 1,   1, .2f,  .5f,  0, .2f, .8f, 1,   1,
    };
    static constexpr draconic::foundation::u16 kIdx[] = {0, 1, 2, 3, 4, 5};

    void createRenderTargets();

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_gbVs = nullptr, *m_gbPs = nullptr, *m_compVs = nullptr,
                      *m_compPs = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::Sampler* m_sampler = nullptr;
    rhi::PipelineLayout *m_gbPl = nullptr, *m_compPl = nullptr;
    rhi::RenderPipeline *m_gbPipe = nullptr, *m_compPipe = nullptr;
    rhi::BindGroupLayout* m_compBgl = nullptr;
    rhi::BindGroup* m_compBg = nullptr;
    rhi::Texture *m_colorRT = nullptr, *m_brightRT = nullptr;
    rhi::TextureView *m_colorRTView = nullptr, *m_brightRTView = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

void MRTSample::createRenderTargets()
{
    if (m_compBg)
    {
        m_device->DestroyBindGroup(m_compBg);
        m_compBg = nullptr;
    }
    if (m_colorRTView)
    {
        m_device->DestroyTextureView(m_colorRTView);
        m_colorRTView = nullptr;
    }
    if (m_colorRT)
    {
        m_device->DestroyTexture(m_colorRT);
        m_colorRT = nullptr;
    }
    if (m_brightRTView)
    {
        m_device->DestroyTextureView(m_brightRTView);
        m_brightRTView = nullptr;
    }
    if (m_brightRT)
    {
        m_device->DestroyTexture(m_brightRT);
        m_brightRT = nullptr;
    }

    rhi::TextureDesc td{};
    td.format = rhi::TextureFormat::RGBA8Unorm;
    td.width = m_width;
    td.height = m_height;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
    m_device->CreateTexture(td, m_colorRT);
    m_device->CreateTexture(td, m_brightRT);
    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::RGBA8Unorm;
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    m_device->CreateTextureView(m_colorRT, tvd, m_colorRTView);
    m_device->CreateTextureView(m_brightRT, tvd, m_brightRTView);

    rhi::BindGroupEntry bgE[3] = {rhi::BindGroupEntry::TextureEntry(m_colorRTView),
                                  rhi::BindGroupEntry::TextureEntry(m_brightRTView),
                                  rhi::BindGroupEntry::SamplerEntry(m_sampler)};
    rhi::BindGroupDesc bgd{};
    bgd.layout = m_compBgl;
    bgd.entries = draconic::foundation::Span<const rhi::BindGroupEntry>(bgE, 3);
    m_device->CreateBindGroup(bgd, m_compBg);
}

draconic::foundation::Status MRTSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8;
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kGBufShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"GBufVS",
                                            m_gbVs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kGBufShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"GBufPS",
                                            m_gbPs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kCompShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"CompVS",
                                            m_compVs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kCompShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"CompPS",
                                            m_compPs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::BufferDesc vbd{};
    vbd.size = sizeof(kVerts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kIdx);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kVerts), sizeof(kVerts)));
    batch->WriteBuffer(m_ib, 0, Span<const u8>(reinterpret_cast<const u8*>(kIdx), sizeof(kIdx)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    rhi::SamplerDesc sd{};
    sd.minFilter = rhi::FilterMode::Nearest;
    sd.magFilter = rhi::FilterMode::Nearest;
    sd.addressU = rhi::AddressMode::ClampToEdge;
    sd.addressV = rhi::AddressMode::ClampToEdge;
    if (m_device->CreateSampler(sd, m_sampler) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // GBuffer pipeline (empty layout, 2 color targets).
    rhi::PipelineLayoutDesc gpld{};
    if (m_device->CreatePipelineLayout(gpld, m_gbPl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x4, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 28;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
    rhi::ColorTargetState gbCt[2] = {{.format = rhi::TextureFormat::RGBA8Unorm,
                                      .blend = {},
                                      .writeMask = rhi::ColorWriteMask::All},
                                     {.format = rhi::TextureFormat::RGBA8Unorm,
                                      .blend = {},
                                      .writeMask = rhi::ColorWriteMask::All}};
    rhi::RenderPipelineDesc grpd{};
    grpd.layout = m_gbPl;
    grpd.vertex.shader = {m_gbVs, u8"VSMain", rhi::ShaderStage::Vertex};
    grpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
    grpd.fragment = rhi::FragmentState{};
    grpd.fragment->shader = {m_gbPs, u8"PSMain", rhi::ShaderStage::Fragment};
    grpd.fragment->targets = Span<const rhi::ColorTargetState>(gbCt, 2);
    if (m_device->CreateRenderPipeline(grpd, m_gbPipe) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Composite BGL + pipeline (3 bindings: 2 textures + 1 sampler).
    rhi::BindGroupLayoutEntry cE[3] = {
        rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
        rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
        rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
    };
    rhi::BindGroupLayoutDesc cBgld{};
    cBgld.entries = Span<const rhi::BindGroupLayoutEntry>(cE, 3);
    if (m_device->CreateBindGroupLayout(cBgld, m_compBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupLayout* cSets[1] = {m_compBgl};
    rhi::PipelineLayoutDesc cpld{};
    cpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(cSets, 1);
    if (m_device->CreatePipelineLayout(cpld, m_compPl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::ColorTargetState compCt{};
    compCt.format = m_swapChain->Format();
    rhi::RenderPipelineDesc crpd{};
    crpd.layout = m_compPl;
    crpd.vertex.shader = {m_compVs, u8"VSMain", rhi::ShaderStage::Vertex};
    crpd.fragment = rhi::FragmentState{};
    crpd.fragment->shader = {m_compPs, u8"PSMain", rhi::ShaderStage::Fragment};
    crpd.fragment->targets = Span<const rhi::ColorTargetState>(&compCt, 1);
    if (m_device->CreateRenderPipeline(crpd, m_compPipe) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    createRenderTargets();
    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

void MRTSample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;

    // Pass 1: render to 2 RTs.
    enc->TransitionTexture(m_colorRT, rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    enc->TransitionTexture(m_brightRT, rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    rhi::ColorAttachment ca2[2];
    ca2[0].view = m_colorRTView;
    ca2[0].loadOp = rhi::LoadOp::Clear;
    ca2[0].storeOp = rhi::StoreOp::Store;
    ca2[0].clearValue = rhi::ClearColor(0.1f, 0.1f, 0.15f, 1);
    ca2[1].view = m_brightRTView;
    ca2[1].loadOp = rhi::LoadOp::Clear;
    ca2[1].storeOp = rhi::StoreOp::Store;
    ca2[1].clearValue = rhi::ClearColor::Black();
    rhi::RenderPassDesc rpd1{};
    rpd1.colorAttachments.Add(ca2[0]);
    rpd1.colorAttachments.Add(ca2[1]);
    auto* rp1 = enc->BeginRenderPass(rpd1);
    rp1->SetPipeline(m_gbPipe);
    rp1->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp1->SetScissor(0, 0, m_width, m_height);
    rp1->SetVertexBuffer(0, m_vb, 0);
    rp1->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp1->DrawIndexed(6);
    rp1->End();
    enc->TransitionTexture(m_colorRT, rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::ShaderRead);
    enc->TransitionTexture(m_brightRT, rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::ShaderRead);

    // Pass 2: composite to swap chain.
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    rhi::ColorAttachment ca1{};
    ca1.view = m_swapChain->CurrentTextureView();
    ca1.loadOp = rhi::LoadOp::Clear;
    ca1.storeOp = rhi::StoreOp::Store;
    ca1.clearValue = rhi::ClearColor::Black();
    rhi::RenderPassDesc rpd2{};
    rpd2.colorAttachments.Add(ca1);
    auto* rp2 = enc->BeginRenderPass(rpd2);
    rp2->SetPipeline(m_compPipe);
    rp2->SetBindGroup(0, m_compBg);
    rp2->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp2->SetScissor(0, 0, m_width, m_height);
    rp2->Draw(3);
    rp2->End();
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void MRTSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_compPipe)
        m_device->DestroyRenderPipeline(m_compPipe);
    if (m_compPl)
        m_device->DestroyPipelineLayout(m_compPl);
    if (m_compBg)
        m_device->DestroyBindGroup(m_compBg);
    if (m_compBgl)
        m_device->DestroyBindGroupLayout(m_compBgl);
    if (m_gbPipe)
        m_device->DestroyRenderPipeline(m_gbPipe);
    if (m_gbPl)
        m_device->DestroyPipelineLayout(m_gbPl);
    if (m_brightRTView)
        m_device->DestroyTextureView(m_brightRTView);
    if (m_brightRT)
        m_device->DestroyTexture(m_brightRT);
    if (m_colorRTView)
        m_device->DestroyTextureView(m_colorRTView);
    if (m_colorRT)
        m_device->DestroyTexture(m_colorRT);
    if (m_sampler)
        m_device->DestroySampler(m_sampler);
    if (m_ib)
        m_device->DestroyBuffer(m_ib);
    if (m_vb)
        m_device->DestroyBuffer(m_vb);
    if (m_compPs)
        m_device->DestroyShaderModule(m_compPs);
    if (m_compVs)
        m_device->DestroyShaderModule(m_compVs);
    if (m_gbPs)
        m_device->DestroyShaderModule(m_gbPs);
    if (m_gbVs)
        m_device->DestroyShaderModule(m_gbVs);
    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    MRTSample app;
    return app.Run(argc, argv);
}
