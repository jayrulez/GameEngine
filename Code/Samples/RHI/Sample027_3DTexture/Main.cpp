#include <new>
/// Sample027 -- 3D Texture & 1D LUT. Ported from Sedulous Sample027_3DTexture.
/// Demonstrates 3D textures and 1D textures.
/// Generates a 3D noise volume, renders slices animated over time.
/// Uses a 1D gradient LUT for color mapping.

#include <algorithm>
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

class Texture3DSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample027 - 3D Texture & 1D LUT";
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    draconic::foundation::Status createVolumeTexture();
    draconic::foundation::Status createLUTTexture();

    static constexpr const char8_t kShader[] = u8R"(
        Texture3D<float4> gVolume : register(t0, space0);
        Texture1D<float4> gLUT    : register(t1, space0);
        SamplerState gSampler     : register(s0, space0);

        struct PushConstants
        {
            float SliceZ;
            float Time;
            float2 _pad;
        };

        [[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0, space1);

        struct PSInput
        {
            float4 Position : SV_POSITION;
            float2 UV       : TEXCOORD0;
        };

        PSInput VSMain(uint vertexID : SV_VertexID)
        {
            PSInput output;
            float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
            output.Position = float4(uv * 2.0 - 1.0, 0.5, 1.0);
            output.UV = uv;
            return output;
        }

        float4 PSMain(PSInput input) : SV_TARGET
        {
            // Sample 3D volume at current slice
            float3 uvw = float3(input.UV, pc.SliceZ);
            float density = gVolume.Sample(gSampler, uvw).r;

            // Map density through 1D LUT
            float4 color = gLUT.Sample(gSampler, density);
            return color;
        }
    )";

    struct PushData
    {
        float sliceZ;
        float time;
        float _pad0;
        float _pad1;
    };

    static constexpr draconic::foundation::u32 kVolumeSize = 32;
    static constexpr draconic::foundation::u32 kLUTSize = 64;

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule* m_vs = nullptr;
    rhi::ShaderModule* m_ps = nullptr;

    // 3D volume texture
    rhi::Texture* m_volumeTexture = nullptr;
    rhi::TextureView* m_volumeView = nullptr;

    // 1D LUT texture
    rhi::Texture* m_lutTexture = nullptr;
    rhi::TextureView* m_lutView = nullptr;

    rhi::Sampler* m_sampler = nullptr;
    rhi::BindGroupLayout* m_bgl = nullptr;
    rhi::BindGroup* m_bg = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;

    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status Texture3DSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"Vol3DVS",
                                            m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"Vol3DPS",
                                            m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (createVolumeTexture() != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (createLUTTexture() != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Sampler
    {
        rhi::SamplerDesc sd{};
        sd.minFilter = rhi::FilterMode::Linear;
        sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = rhi::AddressMode::Repeat;
        sd.addressV = rhi::AddressMode::Repeat;
        sd.addressW = rhi::AddressMode::Repeat;
        sd.label = u8"VolSampler";
        if (m_device->CreateSampler(sd, m_sampler) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Bind group layout: 3D tex, 1D tex, sampler
    {
        rhi::BindGroupLayoutEntry entries[3] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment,
                                                      rhi::TextureViewDimension::Texture3D),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment,
                                                      rhi::TextureViewDimension::Texture1D),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment)};
        rhi::BindGroupLayoutDesc bgld{};
        bgld.entries = Span<const rhi::BindGroupLayoutEntry>(entries, 3);
        bgld.label = u8"VolBGL";
        if (m_device->CreateBindGroupLayout(bgld, m_bgl) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Bind group
    {
        rhi::BindGroupEntry entries[3] = {rhi::BindGroupEntry::TextureEntry(m_volumeView),
                                          rhi::BindGroupEntry::TextureEntry(m_lutView),
                                          rhi::BindGroupEntry::SamplerEntry(m_sampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_bgl;
        bgd.entries = Span<const rhi::BindGroupEntry>(entries, 3);
        bgd.label = u8"VolBG";
        if (m_device->CreateBindGroup(bgd, m_bg) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Pipeline layout with push constants
    {
        rhi::BindGroupLayout* sets[1] = {m_bgl};
        rhi::PushConstantRange pcr{};
        pcr.stages = rhi::ShaderStage::Fragment;
        pcr.offset = 0;
        pcr.size = sizeof(PushData);
        rhi::PushConstantRange pushRanges[1] = {pcr};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 1);
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>(pushRanges, 1);
        pld.label = u8"VolPL";
        if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Render pipeline (fullscreen triangle, no vertex input)
    {
        rhi::ColorTargetState ct{};
        ct.format = m_swapChain->Format();
        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_pl;
        rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
        rpd.fragment = rhi::FragmentState{};
        rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
        rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
        rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        rpd.label = u8"VolPipeline";
        if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

draconic::foundation::Status Texture3DSample::createVolumeTexture()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    rhi::TextureDesc td{};
    td.dimension = rhi::TextureDimension::Texture3D;
    td.format = rhi::TextureFormat::R8Unorm;
    td.width = kVolumeSize;
    td.height = kVolumeSize;
    td.depth = kVolumeSize;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    td.label = u8"VolumeTex3D";
    if (m_device->CreateTexture(td, m_volumeTexture) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::R8Unorm;
    tvd.dimension = rhi::TextureViewDimension::Texture3D;
    if (m_device->CreateTextureView(m_volumeTexture, tvd, m_volumeView) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Generate procedural 3D noise data
    constexpr u32 dataSize = kVolumeSize * kVolumeSize * kVolumeSize;
    u8 data[dataSize];

    for (u32 z = 0; z < kVolumeSize; z++)
    {
        for (u32 y = 0; y < kVolumeSize; y++)
        {
            for (u32 x = 0; x < kVolumeSize; x++)
            {
                float fx = static_cast<float>(x) / static_cast<float>(kVolumeSize);
                float fy = static_cast<float>(y) / static_cast<float>(kVolumeSize);
                float fz = static_cast<float>(z) / static_cast<float>(kVolumeSize);

                // Simple 3D pattern: spherical blobs + frequency pattern
                float cx = fx - 0.5f, cy = fy - 0.5f, cz = fz - 0.5f;
                float dist = std::sqrt(cx * cx + cy * cy + cz * cz);
                float sphere = std::max(0.0f, 1.0f - dist * 3.0f);
                float pattern = std::sin(fx * 12.0f) * std::sin(fy * 12.0f) * std::sin(fz * 12.0f);
                float v = std::clamp(sphere + pattern * 0.3f, 0.0f, 1.0f);

                u32 idx = z * kVolumeSize * kVolumeSize + y * kVolumeSize + x;
                data[idx] = static_cast<u8>(v * 255.0f);
            }
        }
    }

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    rhi::TextureDataLayout layout{};
    layout.bytesPerRow = kVolumeSize;
    layout.rowsPerImage = kVolumeSize;
    batch->WriteTexture(m_volumeTexture, Span<const u8>(data, dataSize), layout,
                        rhi::Extent3D{kVolumeSize, kVolumeSize, kVolumeSize});
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    return draconic::foundation::ErrorCode::Ok;
}

draconic::foundation::Status Texture3DSample::createLUTTexture()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    rhi::TextureDesc td{};
    td.dimension = rhi::TextureDimension::Texture1D;
    td.format = rhi::TextureFormat::RGBA8UnormSrgb;
    td.width = kLUTSize;
    td.height = 1;
    td.arrayLayerCount = 1;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    td.label = u8"LUTTex1D";
    if (m_device->CreateTexture(td, m_lutTexture) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::RGBA8UnormSrgb;
    tvd.dimension = rhi::TextureViewDimension::Texture1D;
    if (m_device->CreateTextureView(m_lutTexture, tvd, m_lutView) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Generate gradient LUT: dark blue -> cyan -> green -> yellow -> red -> white
    u8 data[kLUTSize * 4];
    for (u32 i = 0; i < kLUTSize; i++)
    {
        float t = static_cast<float>(i) / static_cast<float>(kLUTSize - 1);
        float r, g, b;
        if (t < 0.2f)
        {
            float s = t / 0.2f;
            r = 0.05f;
            g = 0.05f + s * 0.4f;
            b = 0.3f + s * 0.5f;
        }
        else if (t < 0.4f)
        {
            float s = (t - 0.2f) / 0.2f;
            r = 0.05f;
            g = 0.45f + s * 0.5f;
            b = 0.8f - s * 0.5f;
        }
        else if (t < 0.6f)
        {
            float s = (t - 0.4f) / 0.2f;
            r = s * 0.8f;
            g = 0.95f;
            b = 0.3f - s * 0.3f;
        }
        else if (t < 0.8f)
        {
            float s = (t - 0.6f) / 0.2f;
            r = 0.8f + s * 0.2f;
            g = 0.95f - s * 0.6f;
            b = 0.0f;
        }
        else
        {
            float s = (t - 0.8f) / 0.2f;
            r = 1.0f;
            g = 0.35f + s * 0.65f;
            b = s * 0.8f;
        }

        u32 idx = i * 4;
        data[idx + 0] = static_cast<u8>(r * 255.0f);
        data[idx + 1] = static_cast<u8>(g * 255.0f);
        data[idx + 2] = static_cast<u8>(b * 255.0f);
        data[idx + 3] = 255;
    }

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    rhi::TextureDataLayout layout{};
    layout.bytesPerRow = kLUTSize * 4;
    layout.rowsPerImage = 1;
    batch->WriteTexture(m_lutTexture, Span<const u8>(data, kLUTSize * 4), layout,
                        rhi::Extent3D{kLUTSize, 1, 1});
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    return draconic::foundation::ErrorCode::Ok;
}

void Texture3DSample::OnRender()
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

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.02f, 0.02f, 0.05f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);

    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetPipeline(m_pipeline);
    rp->SetBindGroup(0, m_bg);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0.0f, 1.0f);
    rp->SetScissor(0, 0, m_width, m_height);

    // Animate slice through 3D volume
    float sliceZ = 0.5f + 0.5f * std::sin(m_totalTime * 0.5f);
    PushData pc{};
    pc.sliceZ = sliceZ;
    pc.time = m_totalTime;
    rp->SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(PushData), &pc);

    rp->Draw(3); // Fullscreen triangle

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

void Texture3DSample::OnShutdown()
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
    if (m_sampler)
        m_device->DestroySampler(m_sampler);
    if (m_lutView)
        m_device->DestroyTextureView(m_lutView);
    if (m_lutTexture)
        m_device->DestroyTexture(m_lutTexture);
    if (m_volumeView)
        m_device->DestroyTextureView(m_volumeView);
    if (m_volumeTexture)
        m_device->DestroyTexture(m_volumeTexture);
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
    Texture3DSample app;
    return app.Run(argc, argv);
}
