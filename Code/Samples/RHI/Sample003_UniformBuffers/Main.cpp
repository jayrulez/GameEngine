#include <new>
/// Sample003 - Rotating Cube with Uniform Buffers + Push Constants.
/// Ported from Sedulous Sample003_UniformBuffers.

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
using draconic::foundation::Float4x4;

class UniformBufferSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample003 - Rotating Cube (Uniform Buffers)";
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnResize(draconic::foundation::u32 w, draconic::foundation::u32 h) override
    {
        m_depthBuf.Recreate(m_device, w, h);
    }
    void OnShutdown() override;

private:
    static constexpr const char8_t kShaderSource[] = u8R"(
        cbuffer UBO : register(b0, space0) { row_major float4x4 MVP; };
        struct PushData { float4 Tint; };
        [[vk::push_constant]] ConstantBuffer<PushData> gPush : register(b0, space1);
        struct VSInput { float3 Position : TEXCOORD0; float3 Color : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float3 Color : COLOR0; };
        PSInput VSMain(VSInput input) {
            PSInput o; o.Position = mul(float4(input.Position, 1.0), MVP); o.Color = input.Color; return o;
        }
        float4 PSMain(PSInput input) : SV_TARGET { return float4(input.Color * gPush.Tint.rgb, 1.0); }
    )";

    static constexpr float kCubeVerts[] = {
        -0.5f, -0.5f, -0.5f, 1, 0, 0, 0.5f,  -0.5f, -0.5f, 0,   1,   0,
        0.5f,  0.5f,  -0.5f, 0, 0, 1, -0.5f, 0.5f,  -0.5f, 1,   1,   0,
        -0.5f, -0.5f, 0.5f,  1, 0, 1, 0.5f,  -0.5f, 0.5f,  0,   1,   1,
        0.5f,  0.5f,  0.5f,  1, 1, 1, -0.5f, 0.5f,  0.5f,  .5f, .5f, .5f,
    };
    static constexpr draconic::foundation::u16 kCubeIdx[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 4, 7, 3, 4, 3, 0,
        1, 2, 6, 1, 6, 5, 3, 7, 6, 3, 6, 2, 4, 0, 1, 4, 1, 5,
    };

    shaders::Compiler* m_compiler = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr, *m_ub = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::BindGroupLayout* m_bgl = nullptr;
    rhi::BindGroup* m_bg = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
    void* m_ubMapped = nullptr;
    samples::framework::DepthBuffer m_depthBuf;
};

draconic::foundation::Status UniformBufferSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8;

    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShaderSource,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"CubeVS",
                                            m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShaderSource,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"CubePS",
                                            m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Buffers.
    rhi::BufferDesc vbd{};
    vbd.size = sizeof(kCubeVerts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kCubeIdx);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ubd{};
    ubd.size = 64;
    ubd.usage = rhi::BufferUsage::Uniform;
    ubd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(ubd, m_ub) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_ubMapped = m_ub->Map();

    // Upload.
    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kCubeVerts), sizeof(kCubeVerts)));
    batch->WriteBuffer(m_ib, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kCubeIdx), sizeof(kCubeIdx)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    // Bind group layout + group.
    rhi::BindGroupLayoutEntry bglE[1] = {rhi::BindGroupLayoutEntry::UniformBuffer(
        0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment)};
    rhi::BindGroupLayoutDesc bgld{};
    bgld.entries = Span<const rhi::BindGroupLayoutEntry>(bglE, 1);
    if (m_device->CreateBindGroupLayout(bgld, m_bgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::BindGroupEntry bgE[1] = {rhi::BindGroupEntry::BufferEntry(m_ub, 0, 64)};
    rhi::BindGroupDesc bgd{};
    bgd.layout = m_bgl;
    bgd.entries = Span<const rhi::BindGroupEntry>(bgE, 1);
    if (m_device->CreateBindGroup(bgd, m_bg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Pipeline layout with push constants.
    rhi::BindGroupLayout* sets[1] = {m_bgl};
    rhi::PushConstantRange pc{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0, 16};
    rhi::PipelineLayoutDesc pld{};
    pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 1);
    pld.pushConstantRanges = Span<const rhi::PushConstantRange>(&pc, 1);
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Depth buffer.
    m_depthBuf.Recreate(m_device, m_width, m_height);

    // Render pipeline.
    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x3, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 24;
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
    rpd.primitive = {rhi::PrimitiveTopology::TriangleList, rhi::FrontFace::CW, rhi::CullMode::Back};
    rpd.depthStencil = rhi::DepthStencilState{};
    rpd.depthStencil->format = rhi::TextureFormat::Depth24PlusStencil8;
    rpd.depthStencil->depthCompare = rhi::CompareFunction::Less;
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

void UniformBufferSample::OnRender()
{
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    // Update MVP.
    draconic::foundation::f32 aspect =
        static_cast<draconic::foundation::f32>(m_width) / static_cast<draconic::foundation::f32>(m_height);
    draconic::foundation::f32 angle = m_totalTime * 1.2f;
    Float4x4 model = (Float4x4::RotationY(angle) * Float4x4::RotationX(angle * 0.7f));
    Float4x4 view =
        Float4x4::LookAtRH(draconic::foundation::Float3{0, 1.5f, -3}, draconic::foundation::Float3{0, 0, 0},
                           draconic::foundation::Float3{0, 1, 0});
    Float4x4 proj =
        Float4x4::PerspectiveFovRH(draconic::foundation::DegreesToRadians(45.0f), aspect, 0.1f, 100.0f);
    Float4x4 mvp = model * view * proj;
    std::memcpy(m_ubMapped, mvp.Data(), 64);

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    enc->TransitionTexture(m_depthBuf.texture, rhi::ResourceState::Undefined,
                           rhi::ResourceState::DepthStencilWrite);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    rhi::DepthStencilAttachment dsa{};
    dsa.view = m_depthBuf.view;
    dsa.depthLoadOp = rhi::LoadOp::Clear;
    dsa.depthStoreOp = rhi::StoreOp::Store;
    dsa.depthClearValue = 1.0f;
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    rpd.depthStencilAttachment = dsa;

    auto* rp = enc->BeginRenderPass(rpd);
    rp->SetPipeline(m_pipeline);
    rp->SetBindGroup(0, m_bg);
    draconic::foundation::f32 pulse = (std::sin(m_totalTime * 2.0f) * 0.3f + 0.7f);
    draconic::foundation::f32 tint[4] = {pulse, pulse, pulse, 1.0f};
    rp->SetPushConstants(rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0, 16, tint);
    rp->SetViewport(0, 0, static_cast<draconic::foundation::f32>(m_width),
                    static_cast<draconic::foundation::f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->DrawIndexed(36);
    rp->End();

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(draconic::foundation::Span<rhi::CommandBuffer* const>(cbs, 1), m_fence,
                            m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void UniformBufferSample::OnShutdown()
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
    UniformBufferSample app;
    return app.Run(argc, argv);
}
