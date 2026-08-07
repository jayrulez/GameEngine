#include <new>
/// Sample023 -- Cube Map & Comparison Sampler. Ported from Sedulous Sample023_CubeMap.
/// Demonstrates cube map textures and comparison samplers.
/// Renders a fullscreen quad that samples a procedural cube map (skybox),
/// plus a second pass with a depth texture sampled via comparison sampler
/// to demonstrate shadow-map-style sampling.

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

class CubeMapSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample023 - Cube Map & Comparison Sampler";
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    draconic::foundation::Status createCubeMap();
    draconic::foundation::Status createDepthTexture();

    // Skybox shader: fullscreen quad -> ray direction -> cube map lookup
    static constexpr const char8_t kSkyboxShader[] = u8R"(
        TextureCube<float4> gCubeMap : register(t0, space0);
        SamplerState gSampler : register(s0, space0);

        struct PushConstants
        {
            float Time;
            float AspectRatio;
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
            // Fullscreen triangle
            float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
            output.Position = float4(uv * 2.0 - 1.0, 0.5, 1.0);
            output.UV = uv;
            return output;
        }

        float4 PSMain(PSInput input) : SV_TARGET
        {
            // Convert UV to ray direction
            float2 ndc = input.UV * 2.0 - 1.0;
            ndc.x *= pc.AspectRatio;
            ndc.y = -ndc.y;

            // Simple rotation around Y
            float c = cos(pc.Time * 0.3);
            float s = sin(pc.Time * 0.3);

            float3 dir = normalize(float3(ndc.x, ndc.y, 1.0));
            float3 rotDir = float3(dir.x * c + dir.z * s, dir.y, -dir.x * s + dir.z * c);

            return gCubeMap.Sample(gSampler, rotDir);
        }
    )";

    // Shadow test shader: renders a quad, samples a depth texture with comparison sampler
    static constexpr const char8_t kShadowShader[] = u8R"(
        Texture2D<float> gShadowMap : register(t0, space0);
        SamplerComparisonState gShadowSampler : register(s0, space0);

        struct PushConstants
        {
            float Time;
            float AspectRatio;
            float2 _pad;
        };

        [[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0, space1);

        struct VSInput
        {
            float3 Position : TEXCOORD0;
            float2 TexCoord : TEXCOORD1;
        };

        struct PSInput
        {
            float4 Position : SV_POSITION;
            float2 TexCoord : TEXCOORD0;
        };

        PSInput VSMain(VSInput input)
        {
            PSInput output;
            output.Position = float4(input.Position, 1.0);
            output.TexCoord = input.TexCoord;
            return output;
        }

        float4 PSMain(PSInput input) : SV_TARGET
        {
            // Compare at varying depth based on time for animated shadow boundary
            float compareValue = 0.5 + 0.4 * sin(pc.Time);
            float shadow = gShadowMap.SampleCmpLevelZero(gShadowSampler, input.TexCoord, compareValue);
            float3 litColor = float3(0.9, 0.85, 0.7);
            float3 shadowColor = float3(0.1, 0.1, 0.2);
            float3 color = lerp(shadowColor, litColor, shadow);
            return float4(color, 1.0);
        }
    )";

    struct PushData
    {
        float time;
        float aspectRatio;
        float _pad0;
        float _pad1;
    };

    shaders::Compiler* m_compiler = nullptr;

    // Skybox resources
    rhi::ShaderModule* m_skyboxVs = nullptr;
    rhi::ShaderModule* m_skyboxPs = nullptr;
    rhi::Texture* m_cubeTexture = nullptr;
    rhi::TextureView* m_cubeView = nullptr;
    rhi::Sampler* m_linearSampler = nullptr;
    rhi::BindGroupLayout* m_skyboxBgl = nullptr;
    rhi::BindGroup* m_skyboxBg = nullptr;
    rhi::PipelineLayout* m_skyboxPl = nullptr;
    rhi::RenderPipeline* m_skyboxPipeline = nullptr;

    // Shadow comparison resources
    rhi::ShaderModule* m_shadowVs = nullptr;
    rhi::ShaderModule* m_shadowPs = nullptr;
    rhi::Texture* m_depthTexture = nullptr;
    rhi::TextureView* m_depthView = nullptr;
    rhi::Sampler* m_comparisonSampler = nullptr;
    rhi::Buffer* m_quadVb = nullptr;
    rhi::BindGroupLayout* m_shadowBgl = nullptr;
    rhi::BindGroup* m_shadowBg = nullptr;
    rhi::PipelineLayout* m_shadowPl = nullptr;
    rhi::RenderPipeline* m_shadowPipeline = nullptr;

    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status CubeMapSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Compile skybox shaders
    if (samples::framework::CompileToModule(m_compiler, m_device, kSkyboxShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"SkyboxVS",
                                            m_skyboxVs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(
            m_compiler, m_device, kSkyboxShader, shaders::ShaderStage::Fragment, u8"PSMain",
            u8"SkyboxPS", m_skyboxPs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Compile shadow shaders
    if (samples::framework::CompileToModule(m_compiler, m_device, kShadowShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"ShadowVS",
                                            m_shadowVs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(
            m_compiler, m_device, kShadowShader, shaders::ShaderStage::Fragment, u8"PSMain",
            u8"ShadowPS", m_shadowPs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Create procedural cube map (6 faces, 64x64, each a solid color)
    if (createCubeMap() != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Create depth texture for comparison sampler (gradient)
    if (createDepthTexture() != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Create samplers
    {
        rhi::SamplerDesc sd{};
        sd.minFilter = rhi::FilterMode::Linear;
        sd.magFilter = rhi::FilterMode::Linear;
        sd.label = u8"LinearSampler";
        if (m_device->CreateSampler(sd, m_linearSampler) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }
    {
        rhi::SamplerDesc sd{};
        sd.minFilter = rhi::FilterMode::Linear;
        sd.magFilter = rhi::FilterMode::Linear;
        sd.compare = rhi::CompareFunction::LessEqual;
        sd.label = u8"ComparisonSampler";
        if (m_device->CreateSampler(sd, m_comparisonSampler) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Skybox bind group layout: cube texture + sampler
    {
        rhi::BindGroupLayoutEntry entries[2] = {
            rhi::BindGroupLayoutEntry::SampledTexture(
                0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment,
                rhi::TextureViewDimension::TextureCube),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment)};
        rhi::BindGroupLayoutDesc bgld{};
        bgld.entries = Span<const rhi::BindGroupLayoutEntry>(entries, 2);
        bgld.label = u8"SkyboxBGL";
        if (m_device->CreateBindGroupLayout(bgld, m_skyboxBgl) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Skybox bind group
    {
        rhi::BindGroupEntry entries[2] = {rhi::BindGroupEntry::TextureEntry(m_cubeView),
                                          rhi::BindGroupEntry::SamplerEntry(m_linearSampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_skyboxBgl;
        bgd.entries = Span<const rhi::BindGroupEntry>(entries, 2);
        bgd.label = u8"SkyboxBG";
        if (m_device->CreateBindGroup(bgd, m_skyboxBg) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Skybox pipeline layout
    {
        rhi::BindGroupLayout* sets[1] = {m_skyboxBgl};
        rhi::PushConstantRange pcr{};
        pcr.stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
        pcr.offset = 0;
        pcr.size = sizeof(PushData);
        rhi::PushConstantRange pushRanges[1] = {pcr};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 1);
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>(pushRanges, 1);
        pld.label = u8"SkyboxPL";
        if (m_device->CreatePipelineLayout(pld, m_skyboxPl) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Skybox pipeline (fullscreen triangle, no vertex input)
    {
        rhi::ColorTargetState ct{};
        ct.format = m_swapChain->Format();
        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_skyboxPl;
        rpd.vertex.shader = {m_skyboxVs, u8"VSMain", rhi::ShaderStage::Vertex};
        rpd.fragment = rhi::FragmentState{};
        rpd.fragment->shader = {m_skyboxPs, u8"PSMain", rhi::ShaderStage::Fragment};
        rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
        rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        rpd.label = u8"SkyboxPipeline";
        if (m_device->CreateRenderPipeline(rpd, m_skyboxPipeline) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Shadow test quad vertices (bottom-right corner overlay)
    {
        float quadVerts[] = {// pos xyz, uv
                             0.3f, -0.9f, 0.0f, 0.0f, 1.0f, 0.9f, -0.9f, 0.0f, 1.0f, 1.0f,
                             0.9f, -0.3f, 0.0f, 1.0f, 0.0f, 0.3f, -0.9f, 0.0f, 0.0f, 1.0f,
                             0.9f, -0.3f, 0.0f, 1.0f, 0.0f, 0.3f, -0.3f, 0.0f, 0.0f, 0.0f};

        u32 vbSize = sizeof(quadVerts);
        rhi::BufferDesc bd{};
        bd.size = vbSize;
        bd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
        bd.memory = rhi::MemoryLocation::GpuOnly;
        bd.label = u8"ShadowQuadVB";
        if (m_device->CreateBuffer(bd, m_quadVb) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;

        rhi::TransferBatch* batch = nullptr;
        m_graphicsQueue->CreateTransferBatch(batch);
        batch->WriteBuffer(m_quadVb, 0,
                           Span<const u8>(reinterpret_cast<const u8*>(quadVerts), vbSize));
        batch->Submit();
        m_graphicsQueue->DestroyTransferBatch(batch);
    }

    // Shadow bind group layout: depth texture + comparison sampler
    {
        rhi::BindGroupLayoutEntry entries[2];
        entries[0] = rhi::BindGroupLayoutEntry::SampledTexture(
            0, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2D);
        entries[0].textureSampleType = rhi::TextureSampleType::Depth; // SampleCmp source
        entries[1] = {};
        entries[1].binding = 0;
        entries[1].visibility = rhi::ShaderStage::Fragment;
        entries[1].type = rhi::BindingType::ComparisonSampler;
        rhi::BindGroupLayoutDesc bgld{};
        bgld.entries = Span<const rhi::BindGroupLayoutEntry>(entries, 2);
        bgld.label = u8"ShadowBGL";
        if (m_device->CreateBindGroupLayout(bgld, m_shadowBgl) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Shadow bind group
    {
        rhi::BindGroupEntry entries[2] = {rhi::BindGroupEntry::TextureEntry(m_depthView),
                                          rhi::BindGroupEntry::SamplerEntry(m_comparisonSampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_shadowBgl;
        bgd.entries = Span<const rhi::BindGroupEntry>(entries, 2);
        bgd.label = u8"ShadowBG";
        if (m_device->CreateBindGroup(bgd, m_shadowBg) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Shadow pipeline layout
    {
        rhi::BindGroupLayout* sets[1] = {m_shadowBgl};
        rhi::PushConstantRange pcr{};
        pcr.stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
        pcr.offset = 0;
        pcr.size = sizeof(PushData);
        rhi::PushConstantRange pushRanges[1] = {pcr};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 1);
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>(pushRanges, 1);
        pld.label = u8"ShadowPL";
        if (m_device->CreatePipelineLayout(pld, m_shadowPl) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    // Shadow pipeline
    {
        rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                         {rhi::VertexFormat::Float32x2, 12, 1}};
        rhi::VertexBufferLayout vbl{};
        vbl.stride = 20;
        vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);

        rhi::ColorTargetState ct{};
        ct.format = m_swapChain->Format();

        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_shadowPl;
        rpd.vertex.shader = {m_shadowVs, u8"VSMain", rhi::ShaderStage::Vertex};
        rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
        rpd.fragment = rhi::FragmentState{};
        rpd.fragment->shader = {m_shadowPs, u8"PSMain", rhi::ShaderStage::Fragment};
        rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
        rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        rpd.label = u8"ShadowPipeline";
        if (m_device->CreateRenderPipeline(rpd, m_shadowPipeline) != draconic::foundation::ErrorCode::Ok)
            return draconic::foundation::ErrorCode::Unknown;
    }

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

draconic::foundation::Status CubeMapSample::createCubeMap()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    constexpr u32 faceSize = 64;
    constexpr u32 BytesPerPixel = 4;
    constexpr u32 faceBytes = faceSize * faceSize * BytesPerPixel;

    // Create cube map texture: 2D with 6 array layers
    rhi::TextureDesc td{};
    td.dimension = rhi::TextureDimension::Texture2D;
    td.format = rhi::TextureFormat::RGBA8UnormSrgb;
    td.width = faceSize;
    td.height = faceSize;
    td.arrayLayerCount = 6;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    td.label = u8"CubeMapTex";
    if (m_device->CreateTexture(td, m_cubeTexture) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Create cube view
    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::RGBA8UnormSrgb;
    tvd.dimension = rhi::TextureViewDimension::TextureCube;
    tvd.baseMipLevel = 0;
    tvd.mipLevelCount = 1;
    tvd.baseArrayLayer = 0;
    tvd.arrayLayerCount = 6;
    if (m_device->CreateTextureView(m_cubeTexture, tvd, m_cubeView) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Generate 6 face colors: +X red, -X cyan, +Y green, -Y magenta, +Z blue, -Z yellow
    u8 faceColors[6][4] = {
        {200, 60, 60, 255},  // +X: red
        {60, 200, 200, 255}, // -X: cyan
        {60, 200, 60, 255},  // +Y: green
        {200, 60, 200, 255}, // -Y: magenta
        {60, 60, 200, 255},  // +Z: blue
        {200, 200, 60, 255}  // -Z: yellow
    };

    u8 stagingBuf[faceBytes];
    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);

    for (int face = 0; face < 6; face++)
    {
        // Fill face with gradient from face color to white at center
        for (u32 y = 0; y < faceSize; y++)
        {
            for (u32 x = 0; x < faceSize; x++)
            {
                float fx = (static_cast<float>(x) / static_cast<float>(faceSize)) * 2.0f - 1.0f;
                float fy = (static_cast<float>(y) / static_cast<float>(faceSize)) * 2.0f - 1.0f;
                float dist = std::min(1.0f, std::sqrt(fx * fx + fy * fy));
                float t = 1.0f - dist * 0.5f;

                u32 idx = (y * faceSize + x) * BytesPerPixel;
                stagingBuf[idx + 0] = static_cast<u8>(faceColors[face][0] * t + 40 * (1.0f - t));
                stagingBuf[idx + 1] = static_cast<u8>(faceColors[face][1] * t + 40 * (1.0f - t));
                stagingBuf[idx + 2] = static_cast<u8>(faceColors[face][2] * t + 40 * (1.0f - t));
                stagingBuf[idx + 3] = 255;
            }
        }

        rhi::TextureDataLayout layout{};
        layout.bytesPerRow = faceSize * BytesPerPixel;
        layout.rowsPerImage = faceSize;
        batch->WriteTexture(m_cubeTexture, Span<const u8>(stagingBuf, faceBytes), layout,
                            rhi::Extent3D{faceSize, faceSize, 1}, 0, static_cast<u32>(face));
    }

    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);
    return draconic::foundation::ErrorCode::Ok;
}

draconic::foundation::Status CubeMapSample::createDepthTexture()
{
    using draconic::foundation::Status;

    constexpr draconic::foundation::u32 texSize = 64;

    rhi::TextureDesc td{};
    td.dimension = rhi::TextureDimension::Texture2D;
    td.format = rhi::TextureFormat::Depth32Float;
    td.width = texSize;
    td.height = texSize;
    td.arrayLayerCount = 1;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
    td.label = u8"ShadowDepthTex";
    if (m_device->CreateTexture(td, m_depthTexture) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::Depth32Float;
    tvd.dimension = rhi::TextureViewDimension::Texture2D;
    if (m_device->CreateTextureView(m_depthTexture, tvd, m_depthView) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // We'll render a gradient depth in a render pass
    // For simplicity, just clear to 0.5 so the comparison sampler has something to compare against
    // (A real sample would render shadow casters here)

    return draconic::foundation::ErrorCode::Ok;
}

void CubeMapSample::OnRender()
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

    f32 aspect = static_cast<f32>(m_width) / static_cast<f32>(m_height);

    // Render a depth value into the shadow depth texture
    enc->TransitionTexture(m_depthTexture, rhi::ResourceState::Undefined,
                           rhi::ResourceState::DepthStencilWrite);
    {
        rhi::DepthStencilAttachment dsa{};
        dsa.view = m_depthView;
        dsa.depthLoadOp = rhi::LoadOp::Clear;
        dsa.depthStoreOp = rhi::StoreOp::Store;
        dsa.depthClearValue = 0.5f;
        rhi::RenderPassDesc rpd{};
        rpd.depthStencilAttachment = dsa;
        auto* rp = enc->BeginRenderPass(rpd);
        rp->End();
    }

    // Transition depth texture from DepthStencilWrite -> ShaderRead for sampling
    enc->TransitionTexture(m_depthTexture, rhi::ResourceState::DepthStencilWrite,
                           rhi::ResourceState::ShaderRead);

    // Transition swapchain
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0.0f, 1.0f);
    rp->SetScissor(0, 0, m_width, m_height);

    // Draw skybox (fullscreen triangle, no VB needed - SV_VertexID)
    rp->SetPipeline(m_skyboxPipeline);
    rp->SetBindGroup(0, m_skyboxBg);
    PushData pc{};
    pc.time = m_totalTime;
    pc.aspectRatio = aspect;
    rp->SetPushConstants(rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0, sizeof(PushData),
                         &pc);
    rp->Draw(3);

    // Draw shadow comparison overlay quad
    rp->SetPipeline(m_shadowPipeline);
    rp->SetBindGroup(0, m_shadowBg);
    rp->SetPushConstants(rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0, sizeof(PushData),
                         &pc);
    rp->SetVertexBuffer(0, m_quadVb, 0);
    rp->Draw(6);

    rp->End();

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);

    // Transition depth texture back to DepthStencilWrite for next frame
    enc->TransitionTexture(m_depthTexture, rhi::ResourceState::ShaderRead,
                           rhi::ResourceState::DepthStencilWrite);

    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void CubeMapSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_shadowPipeline)
        m_device->DestroyRenderPipeline(m_shadowPipeline);
    if (m_skyboxPipeline)
        m_device->DestroyRenderPipeline(m_skyboxPipeline);
    if (m_shadowPl)
        m_device->DestroyPipelineLayout(m_shadowPl);
    if (m_skyboxPl)
        m_device->DestroyPipelineLayout(m_skyboxPl);
    if (m_shadowBg)
        m_device->DestroyBindGroup(m_shadowBg);
    if (m_skyboxBg)
        m_device->DestroyBindGroup(m_skyboxBg);
    if (m_shadowBgl)
        m_device->DestroyBindGroupLayout(m_shadowBgl);
    if (m_skyboxBgl)
        m_device->DestroyBindGroupLayout(m_skyboxBgl);
    if (m_comparisonSampler)
        m_device->DestroySampler(m_comparisonSampler);
    if (m_linearSampler)
        m_device->DestroySampler(m_linearSampler);
    if (m_quadVb)
        m_device->DestroyBuffer(m_quadVb);
    if (m_depthView)
        m_device->DestroyTextureView(m_depthView);
    if (m_depthTexture)
        m_device->DestroyTexture(m_depthTexture);
    if (m_cubeView)
        m_device->DestroyTextureView(m_cubeView);
    if (m_cubeTexture)
        m_device->DestroyTexture(m_cubeTexture);
    if (m_shadowPs)
        m_device->DestroyShaderModule(m_shadowPs);
    if (m_shadowVs)
        m_device->DestroyShaderModule(m_shadowVs);
    if (m_skyboxPs)
        m_device->DestroyShaderModule(m_skyboxPs);
    if (m_skyboxVs)
        m_device->DestroyShaderModule(m_skyboxVs);
    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    CubeMapSample app;
    return app.Run(argc, argv);
}
