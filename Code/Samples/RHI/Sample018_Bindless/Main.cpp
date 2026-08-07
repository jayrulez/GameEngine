#include <new>
/// Sample018 -- Bindless Textures. Ported from Sedulous Sample018_Bindless.
/// Demonstrates bindless texture arrays with material index via push constants.
/// Creates 4 procedural textures, binds them in a bindless array, and renders
/// 4 quads each selecting a different texture via push constant index.

#include <cmath>
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

class BindlessSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override { return u8"Sample018 - Bindless Textures"; }
    rhi::DeviceFeatures RequiredFeatures() const override
    {
        rhi::DeviceFeatures f{};
        f.bindlessDescriptors = true;
        return f;
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    draconic::foundation::Status createTextures();
    void generatePixel(draconic::foundation::u32 texIndex, draconic::foundation::u32 x, draconic::foundation::u32 y,
                       draconic::foundation::u8* rgba);

    static constexpr const char8_t kShader[] = u8R"(
        Texture2D gTextures[] : register(t0, space0);
        SamplerState gSampler : register(s0, space1);

        struct PushData
        {
            uint TextureIndex;
            float OffsetX;
            float OffsetY;
            float Padding;
        };

        [[vk::push_constant]] ConstantBuffer<PushData> gPush : register(b0, space2);

        struct PSInput
        {
            float4 Position : SV_POSITION;
            float2 TexCoord : TEXCOORD0;
        };

        PSInput VSMain(uint vertexID : SV_VertexID)
        {
            // Fullscreen-quad-style: 4 vertices for a unit quad
            float2 positions[4] = {
                float2(-0.4, 0.4),
                float2( 0.4, 0.4),
                float2(-0.4,-0.4),
                float2( 0.4,-0.4)
            };
            float2 uvs[4] = {
                float2(0, 0), float2(1, 0),
                float2(0, 1), float2(1, 1)
            };

            PSInput output;
            float2 pos = positions[vertexID];
            pos.x += gPush.OffsetX;
            pos.y += gPush.OffsetY;
            output.Position = float4(pos, 0.0, 1.0);
            output.TexCoord = uvs[vertexID];
            return output;
        }

        float4 PSMain(PSInput input) : SV_TARGET
        {
            return gTextures[gPush.TextureIndex].Sample(gSampler, input.TexCoord);
        }
    )";

    static constexpr draconic::foundation::u32 kTexSize = 64;
    static constexpr draconic::foundation::u32 kNumTextures = 4;

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule* m_vs = nullptr;
    rhi::ShaderModule* m_ps = nullptr;

    // Textures
    rhi::Texture* m_textures[kNumTextures] = {};
    rhi::TextureView* m_textureViews[kNumTextures] = {};
    rhi::Sampler* m_sampler = nullptr;

    // Bindless bind group (space0: bindless textures)
    rhi::BindGroupLayout* m_bindlessBgl = nullptr;
    rhi::BindGroup* m_bindlessBg = nullptr;

    // Sampler bind group (space1: sampler)
    rhi::BindGroupLayout* m_samplerBgl = nullptr;
    rhi::BindGroup* m_samplerBg = nullptr;

    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status BindlessSample::OnInit()
{
    if (!m_device->features.bindlessDescriptors)
    {
        rhi::LogError("ERROR: Bindless descriptors are not supported by this device/backend");
        return draconic::foundation::ErrorCode::Unknown;
    }

    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain",
                                            u8"BindlessVS", m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain",
                                            u8"BindlessPS", m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Create 4 procedural textures with different patterns
    if (createTextures() != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Sampler
    rhi::SamplerDesc sd{};
    sd.minFilter = rhi::FilterMode::Linear;
    sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = rhi::AddressMode::Repeat;
    sd.addressV = rhi::AddressMode::Repeat;
    sd.label = u8"BindlessSampler";
    if (m_device->CreateSampler(sd, m_sampler) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Bindless BGL (space0): unbounded texture array
    rhi::BindGroupLayoutEntry bindlessEntry{};
    bindlessEntry.binding = 0;
    bindlessEntry.visibility = rhi::ShaderStage::Fragment;
    bindlessEntry.type = rhi::BindingType::BindlessTextures;
    bindlessEntry.textureDimension = rhi::TextureViewDimension::Texture2D;
    bindlessEntry.count = 0xFFFFFFFF;
    rhi::BindGroupLayoutEntry blEntries[1] = {bindlessEntry};
    rhi::BindGroupLayoutDesc blBgld{};
    blBgld.entries = Span<const rhi::BindGroupLayoutEntry>(blEntries, 1);
    blBgld.label = u8"BindlessBGL";
    if (m_device->CreateBindGroupLayout(blBgld, m_bindlessBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Create bindless bind group (no entries at creation - populated via updateBindless)
    rhi::BindGroupDesc blBgd{};
    blBgd.layout = m_bindlessBgl;
    blBgd.label = u8"BindlessBG";
    if (m_device->CreateBindGroup(blBgd, m_bindlessBg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Populate bindless slots
    rhi::BindlessUpdateEntry bindlessUpdates[kNumTextures];
    for (u32 i = 0; i < kNumTextures; ++i)
    {
        bindlessUpdates[i] = {};
        bindlessUpdates[i].layoutIndex = 0;
        bindlessUpdates[i].arrayIndex = i;
        bindlessUpdates[i].textureView = m_textureViews[i];
    }
    m_bindlessBg->UpdateBindless(
        Span<const rhi::BindlessUpdateEntry>(bindlessUpdates, kNumTextures));

    // Sampler BGL (space1)
    rhi::BindGroupLayoutEntry samplerEntry =
        rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
    rhi::BindGroupLayoutEntry sEntries[1] = {samplerEntry};
    rhi::BindGroupLayoutDesc sBgld{};
    sBgld.entries = Span<const rhi::BindGroupLayoutEntry>(sEntries, 1);
    sBgld.label = u8"SamplerBGL";
    if (m_device->CreateBindGroupLayout(sBgld, m_samplerBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::BindGroupEntry sBgEntries[1] = {rhi::BindGroupEntry::SamplerEntry(m_sampler)};
    rhi::BindGroupDesc sBgd{};
    sBgd.layout = m_samplerBgl;
    sBgd.entries = Span<const rhi::BindGroupEntry>(sBgEntries, 1);
    sBgd.label = u8"SamplerBG";
    if (m_device->CreateBindGroup(sBgd, m_samplerBg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Pipeline layout: group 0 = bindless textures, group 1 = sampler, push constants
    rhi::BindGroupLayout* sets[2] = {m_bindlessBgl, m_samplerBgl};
    rhi::PushConstantRange pcr{};
    pcr.stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    pcr.offset = 0;
    pcr.size = 16;
    rhi::PushConstantRange pushRanges[1] = {pcr};
    rhi::PipelineLayoutDesc pld{};
    pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 2);
    pld.pushConstantRanges = Span<const rhi::PushConstantRange>(pushRanges, 1);
    pld.label = u8"BindlessPL";
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Render pipeline (no vertex buffers - SV_VertexID driven)
    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_pl;
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.fragment = rhi::FragmentState{};
    rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
    rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
    rpd.primitive.topology = rhi::PrimitiveTopology::TriangleStrip;
    rpd.label = u8"BindlessPipeline";
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

draconic::foundation::Status BindlessSample::createTextures()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    constexpr u32 rowBytes = kTexSize * 4;
    constexpr u32 texBytes = rowBytes * kTexSize;
    u8 pixels[texBytes];

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);

    for (u32 t = 0; t < kNumTextures; ++t)
    {
        // Generate pattern
        for (u32 y = 0; y < kTexSize; ++y)
        {
            for (u32 x = 0; x < kTexSize; ++x)
            {
                u32 offset = (y * kTexSize + x) * 4;
                generatePixel(t, x, y, &pixels[offset]);
            }
        }

        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = kTexSize;
        td.height = kTexSize;
        td.mipLevelCount = 1;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"BindlessTex";
        if (m_device->CreateTexture(td, m_textures[t]) != draconic::foundation::ErrorCode::Ok)
        {
            m_graphicsQueue->DestroyTransferBatch(batch);
            return draconic::foundation::ErrorCode::Unknown;
        }

        rhi::TextureDataLayout layout{};
        layout.bytesPerRow = rowBytes;
        layout.rowsPerImage = kTexSize;
        batch->WriteTexture(m_textures[t], Span<const u8>(pixels, texBytes), layout,
                            rhi::Extent3D{kTexSize, kTexSize, 1});

        rhi::TextureViewDesc tvd{};
        tvd.format = rhi::TextureFormat::RGBA8Unorm;
        tvd.mipLevelCount = 1;
        tvd.arrayLayerCount = 1;
        if (m_device->CreateTextureView(m_textures[t], tvd, m_textureViews[t]) !=
            draconic::foundation::ErrorCode::Ok)
        {
            m_graphicsQueue->DestroyTransferBatch(batch);
            return draconic::foundation::ErrorCode::Unknown;
        }
    }

    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);
    return draconic::foundation::ErrorCode::Ok;
}

void BindlessSample::generatePixel(draconic::foundation::u32 texIndex, draconic::foundation::u32 x,
                                   draconic::foundation::u32 y, draconic::foundation::u8* rgba)
{
    float fx = static_cast<float>(x) / static_cast<float>(kTexSize);
    float fy = static_cast<float>(y) / static_cast<float>(kTexSize);

    switch (texIndex)
    {
    case 0:
    { // Red/white checkerboard
        bool check = ((x / 8) + (y / 8)) % 2 == 0;
        rgba[0] = check ? 220 : 255;
        rgba[1] = check ? 30 : 255;
        rgba[2] = check ? 30 : 255;
        rgba[3] = 255;
        break;
    }
    case 1:
    { // Green gradient with stripes
        auto g = static_cast<draconic::foundation::u8>(fx * 255.0f);
        bool stripe = (y % 16) < 8;
        rgba[0] = stripe ? 30 : 10;
        rgba[1] = stripe ? g : static_cast<draconic::foundation::u8>(g / 2);
        rgba[2] = stripe ? 50 : 30;
        rgba[3] = 255;
        break;
    }
    case 2:
    { // Blue circles
        float cx = fx - 0.5f, cy = fy - 0.5f;
        float dist = std::sqrt(cx * cx + cy * cy);
        float rings = std::sin(dist * 30.0f) * 0.5f + 0.5f;
        rgba[0] = static_cast<draconic::foundation::u8>(rings * 60);
        rgba[1] = static_cast<draconic::foundation::u8>(rings * 100);
        rgba[2] = static_cast<draconic::foundation::u8>(rings * 255);
        rgba[3] = 255;
        break;
    }
    default:
    { // Yellow/purple diagonal
        float diag = std::sin((fx + fy) * 10.0f) * 0.5f + 0.5f;
        rgba[0] = static_cast<draconic::foundation::u8>(diag * 255 + (1.0f - diag) * 120);
        rgba[1] = static_cast<draconic::foundation::u8>(diag * 220);
        rgba[2] = static_cast<draconic::foundation::u8>((1.0f - diag) * 200);
        rgba[3] = 255;
        break;
    }
    }
}

void BindlessSample::OnRender()
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
    ca.clearValue = rhi::ClearColor(0.08f, 0.06f, 0.12f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetPipeline(m_pipeline);
    rp->SetBindGroup(0, m_bindlessBg);
    rp->SetBindGroup(1, m_samplerBg);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0.0f, 1.0f);
    rp->SetScissor(0, 0, m_width, m_height);

    // Draw 4 quads, each with a different texture index via push constants
    // Layout: 2x2 grid
    float offsets[8] = {-0.45f, 0.45f, 0.45f, 0.45f, -0.45f, -0.45f, 0.45f, -0.45f};

    for (u32 i = 0; i < kNumTextures; ++i)
    {
        u32 pushData[4] = {i, 0, 0, 0};
        std::memcpy(&pushData[1], &offsets[i * 2], 4);
        std::memcpy(&pushData[2], &offsets[i * 2 + 1], 4);
        rp->SetPushConstants(rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0, 16,
                             pushData);
        rp->Draw(4);
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

void BindlessSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_samplerBg)
        m_device->DestroyBindGroup(m_samplerBg);
    if (m_samplerBgl)
        m_device->DestroyBindGroupLayout(m_samplerBgl);
    if (m_bindlessBg)
        m_device->DestroyBindGroup(m_bindlessBg);
    if (m_bindlessBgl)
        m_device->DestroyBindGroupLayout(m_bindlessBgl);
    if (m_sampler)
        m_device->DestroySampler(m_sampler);
    for (int i = kNumTextures - 1; i >= 0; --i)
    {
        if (m_textureViews[i])
            m_device->DestroyTextureView(m_textureViews[i]);
        if (m_textures[i])
            m_device->DestroyTexture(m_textures[i]);
    }
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
    BindlessSample app;
    return app.Run(argc, argv);
}
