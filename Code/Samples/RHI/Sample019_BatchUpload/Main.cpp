#include <new>
/// Sample019 -- Batch Upload (Async Transfer). Ported from Sedulous Sample019_BatchUpload.
/// Demonstrates batched GPU uploads using TransferBatch with async fence signaling.
/// Uploads a vertex buffer, index buffer, and a procedural texture in a single
/// batched transfer with submitAsync, then renders a textured quad once the
/// upload fence signals completion.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.samples.framework;
import draconic.rhi.vulkan;

namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

class BatchUploadSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample019 - Batch Upload (Async Transfer)";
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    draconic::foundation::Status doBatchUpload();

    static constexpr const char8_t kShader[] = u8R"(
        Texture2D gTexture : register(t0, space0);
        SamplerState gSampler : register(s0, space0);

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

        cbuffer Transform : register(b0, space0)
        {
            float Time;
            float Pad0;
            float Pad1;
            float Pad2;
        };

        PSInput VSMain(VSInput input)
        {
            PSInput output;
            // Gentle rotation
            float c = cos(Time * 0.5);
            float s = sin(Time * 0.5);
            float3 p = input.Position;
            float x = p.x * c - p.y * s;
            float y = p.x * s + p.y * c;
            output.Position = float4(x, y, p.z, 1.0);
            output.TexCoord = input.TexCoord;
            return output;
        }

        float4 PSMain(PSInput input) : SV_TARGET
        {
            return gTexture.Sample(gSampler, input.TexCoord);
        }
    )";

    static constexpr draconic::foundation::u32 kTexSize = 128;

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule* m_vs = nullptr;
    rhi::ShaderModule* m_ps = nullptr;

    rhi::Buffer* m_vb = nullptr;
    rhi::Buffer* m_ib = nullptr;
    rhi::Texture* m_tex = nullptr;
    rhi::TextureView* m_texView = nullptr;
    rhi::Sampler* m_sampler = nullptr;

    rhi::Buffer* m_transformBuf = nullptr;
    void* m_transformMapped = nullptr;

    rhi::BindGroupLayout* m_bgl = nullptr;
    rhi::BindGroup* m_bg = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_frameFence = nullptr;
    draconic::foundation::u64 m_frameFenceVal = 0;

    // Upload tracking
    rhi::Fence* m_uploadFence = nullptr;
    draconic::foundation::u64 m_uploadFenceVal = 0;
    bool m_uploadComplete = false;
    float m_uploadStartTime = 0.0f;
};

draconic::foundation::Status BatchUploadSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"BatchVS",
                                            m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"BatchPS",
                                            m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Vertex buffer: 4 vertices x (pos3 + uv2) x 4 = 80 bytes
    rhi::BufferDesc vbd{};
    vbd.size = 80;
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    vbd.label = u8"BatchVB";
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Index buffer: 6 uint16 = 12 bytes
    rhi::BufferDesc ibd{};
    ibd.size = 12;
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    ibd.label = u8"BatchIB";
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Texture
    rhi::TextureDesc td{};
    td.format = rhi::TextureFormat::RGBA8Unorm;
    td.width = kTexSize;
    td.height = kTexSize;
    td.mipLevelCount = 1;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    td.label = u8"BatchTex";
    if (m_device->CreateTexture(td, m_tex) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::RGBA8Unorm;
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    if (m_device->CreateTextureView(m_tex, tvd, m_texView) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::SamplerDesc sd{};
    sd.minFilter = rhi::FilterMode::Linear;
    sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = rhi::AddressMode::Repeat;
    sd.addressV = rhi::AddressMode::Repeat;
    sd.label = u8"BatchSampler";
    if (m_device->CreateSampler(sd, m_sampler) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Transform UBO
    rhi::BufferDesc tbd{};
    tbd.size = 16;
    tbd.usage = rhi::BufferUsage::Uniform;
    tbd.memory = rhi::MemoryLocation::CpuToGpu;
    tbd.label = u8"BatchTransform";
    if (m_device->CreateBuffer(tbd, m_transformBuf) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_transformMapped = m_transformBuf->Map();

    // Bind group layout: UBO + texture + sampler
    rhi::BindGroupLayoutEntry bglEntries[3] = {
        rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex),
        rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
        rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
    };
    rhi::BindGroupLayoutDesc bgld{};
    bgld.entries = Span<const rhi::BindGroupLayoutEntry>(bglEntries, 3);
    bgld.label = u8"BatchBGL";
    if (m_device->CreateBindGroupLayout(bgld, m_bgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::BindGroupEntry bgEntries[3] = {
        rhi::BindGroupEntry::BufferEntry(m_transformBuf, 0, 16),
        rhi::BindGroupEntry::TextureEntry(m_texView),
        rhi::BindGroupEntry::SamplerEntry(m_sampler),
    };
    rhi::BindGroupDesc bgd{};
    bgd.layout = m_bgl;
    bgd.entries = Span<const rhi::BindGroupEntry>(bgEntries, 3);
    bgd.label = u8"BatchBG";
    if (m_device->CreateBindGroup(bgd, m_bg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Pipeline layout
    rhi::BindGroupLayout* bgls[1] = {m_bgl};
    rhi::PipelineLayoutDesc pld{};
    pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(bgls, 1);
    pld.label = u8"BatchPL";
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Render pipeline
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
    rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
    rpd.label = u8"BatchPipeline";
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_frameFence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Upload fence
    if (m_device->CreateFence(0, m_uploadFence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // === Batch upload: VB + IB + texture in one submission ===
    if (doBatchUpload() != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    return draconic::foundation::ErrorCode::Ok;
}

draconic::foundation::Status BatchUploadSample::doBatchUpload()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;

    m_uploadStartTime = m_totalTime;

    rhi::TransferBatch* transfer = nullptr;
    if (m_graphicsQueue->CreateTransferBatch(transfer) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Vertex data: quad
    float verts[20] = {
        -0.6f, 0.6f,  0.0f, 0.0f, 0.0f, 0.6f,  0.6f,  0.0f, 1.0f, 0.0f,
        0.6f,  -0.6f, 0.0f, 1.0f, 1.0f, -0.6f, -0.6f, 0.0f, 0.0f, 1.0f,
    };
    transfer->WriteBuffer(m_vb, 0, Span<const u8>(reinterpret_cast<const u8*>(verts), 80));

    // Index data
    draconic::foundation::u16 indices[6] = {0, 1, 2, 0, 2, 3};
    transfer->WriteBuffer(m_ib, 0, Span<const u8>(reinterpret_cast<const u8*>(indices), 12));

    // Texture data: procedural mandelbrot-ish pattern
    u32 texBytes = kTexSize * kTexSize * 4;
    auto* pixels = new u8[texBytes];

    for (u32 y = 0; y < kTexSize; y++)
    {
        for (u32 x = 0; x < kTexSize; x++)
        {
            float cr = static_cast<float>(x) / static_cast<float>(kTexSize) * 3.0f - 2.0f;
            float ci = static_cast<float>(y) / static_cast<float>(kTexSize) * 2.4f - 1.2f;
            float zr = 0, zi = 0;
            int iter = 0;
            for (iter = 0; iter < 64; iter++)
            {
                float zr2 = zr * zr - zi * zi + cr;
                float zi2 = 2.0f * zr * zi + ci;
                zr = zr2;
                zi = zi2;
                if (zr * zr + zi * zi > 4.0f)
                    break;
            }

            u32 off = (y * kTexSize + x) * 4;
            if (iter == 64)
            {
                pixels[off] = 10;
                pixels[off + 1] = 10;
                pixels[off + 2] = 30;
                pixels[off + 3] = 255;
            }
            else
            {
                float t = static_cast<float>(iter) / 64.0f;
                pixels[off] = static_cast<u8>(t * 200 + 55);
                pixels[off + 1] = static_cast<u8>(t * t * 255);
                pixels[off + 2] = static_cast<u8>(std::sqrt(t) * 255);
                pixels[off + 3] = 255;
            }
        }
    }

    rhi::TextureDataLayout layout{};
    layout.bytesPerRow = kTexSize * 4;
    layout.rowsPerImage = kTexSize;
    transfer->WriteTexture(m_tex, Span<const u8>(pixels, texBytes), layout,
                           rhi::Extent3D{kTexSize, kTexSize, 1});

    delete[] pixels;

    // Async submit - signals fence when GPU transfer completes
    m_uploadFenceVal = 1;
    if (transfer->SubmitAsync(m_uploadFence, m_uploadFenceVal) != draconic::foundation::ErrorCode::Ok)
    {
        m_graphicsQueue->DestroyTransferBatch(transfer);
        return draconic::foundation::ErrorCode::Unknown;
    }

    std::printf("Batch upload submitted asynchronously (VB: 80B, IB: 12B, Tex: %uB)\n", texBytes);
    m_graphicsQueue->DestroyTransferBatch(transfer);
    return draconic::foundation::ErrorCode::Ok;
}

void BatchUploadSample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::Span;

    if (m_frameFenceVal > 0)
        m_frameFence->Wait(m_frameFenceVal, ~0ull);

    // Check if async upload has completed
    if (!m_uploadComplete)
    {
        if (m_uploadFence->CompletedValue() >= m_uploadFenceVal)
        {
            m_uploadComplete = true;
            std::printf("Batch upload completed! Rendering enabled.\n");
        }
    }

    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    // Update transform
    float transform[4] = {m_totalTime, 0, 0, 0};
    std::memcpy(m_transformMapped, transform, 16);

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
    ca.clearValue = rhi::ClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    auto* rp = enc->BeginRenderPass(rpd);

    if (m_uploadComplete)
    {
        rp->SetPipeline(m_pipeline);
        rp->SetBindGroup(0, m_bg);
        rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0.0f, 1.0f);
        rp->SetScissor(0, 0, m_width, m_height);
        rp->SetVertexBuffer(0, m_vb, 0);
        rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
        rp->DrawIndexed(6);
    }

    rp->End();

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    m_frameFenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_frameFence, m_frameFenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void BatchUploadSample::OnShutdown()
{
    if (m_transformBuf && m_transformMapped)
        m_transformBuf->Unmap();

    if (m_uploadFence)
        m_device->DestroyFence(m_uploadFence);
    if (m_frameFence)
        m_device->DestroyFence(m_frameFence);
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
    if (m_texView)
        m_device->DestroyTextureView(m_texView);
    if (m_tex)
        m_device->DestroyTexture(m_tex);
    if (m_transformBuf)
        m_device->DestroyBuffer(m_transformBuf);
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
    BatchUploadSample app;
    return app.Run(argc, argv);
}
