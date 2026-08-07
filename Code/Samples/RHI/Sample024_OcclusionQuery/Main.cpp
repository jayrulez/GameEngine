#include <new>
/// Sample024 - Occlusion Queries & Debug Labels. Ported from Sedulous Sample024_OcclusionQuery.
/// Demonstrates occlusion queries and debug labels.
/// Renders an occluder quad, then two test quads behind it with occlusion queries.
/// Prints pixel counts to console. Uses debug labels to mark render sections.

#include <cstdint>
#include <cstdio>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.samples.framework;
import draconic.rhi.vulkan;

namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

class OcclusionQuerySample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample024 - Occlusion Queries & Debug Labels";
    }

protected:
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnResize(draconic::foundation::u32 w, draconic::foundation::u32 h) override { recreateDepth(w, h); }
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

    // Geometry: 3 quads
    // Quad 0: Occluder (opaque gray, z=0.3, center)
    // Quad 1: Test A (red, z=0.7, partially behind occluder)
    // Quad 2: Test B (blue, z=0.7, fully behind occluder)
    static constexpr float kVerts[] = {
        // Quad 0: Occluder - center, near
        -0.3f,
        -0.4f,
        0.3f,
        0.4f,
        0.4f,
        0.4f,
        1.0f,
        0.3f,
        -0.4f,
        0.3f,
        0.4f,
        0.4f,
        0.4f,
        1.0f,
        0.3f,
        0.4f,
        0.3f,
        0.5f,
        0.5f,
        0.5f,
        1.0f,
        -0.3f,
        0.4f,
        0.3f,
        0.5f,
        0.5f,
        0.5f,
        1.0f,

        // Quad 1: Test A - partially occluded (left side visible)
        -0.7f,
        -0.3f,
        0.7f,
        1.0f,
        0.3f,
        0.3f,
        1.0f,
        0.0f,
        -0.3f,
        0.7f,
        1.0f,
        0.3f,
        0.3f,
        1.0f,
        0.0f,
        0.3f,
        0.7f,
        1.0f,
        0.5f,
        0.5f,
        1.0f,
        -0.7f,
        0.3f,
        0.7f,
        1.0f,
        0.5f,
        0.5f,
        1.0f,

        // Quad 2: Test B - fully occluded (behind occluder)
        -0.15f,
        -0.2f,
        0.7f,
        0.3f,
        0.3f,
        1.0f,
        1.0f,
        0.15f,
        -0.2f,
        0.7f,
        0.3f,
        0.3f,
        1.0f,
        1.0f,
        0.15f,
        0.2f,
        0.7f,
        0.5f,
        0.5f,
        1.0f,
        1.0f,
        -0.15f,
        0.2f,
        0.7f,
        0.5f,
        0.5f,
        1.0f,
        1.0f,
    };
    static constexpr draconic::foundation::u16 kIdx[] = {
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
    };

    void recreateDepth(draconic::foundation::u32 w, draconic::foundation::u32 h);

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::Texture* m_depthTex = nullptr;
    rhi::TextureView* m_depthView = nullptr;
    rhi::QuerySet* m_occlusionQuerySet = nullptr;
    rhi::Buffer* m_queryResultBuf = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
    int m_frameCount = 0;
    float m_lastReportTime = 0.0f;
};

void OcclusionQuerySample::recreateDepth(draconic::foundation::u32 w, draconic::foundation::u32 h)
{
    if (m_depthView)
    {
        m_device->DestroyTextureView(m_depthView);
        m_depthView = nullptr;
    }
    if (m_depthTex)
    {
        m_device->DestroyTexture(m_depthTex);
        m_depthTex = nullptr;
    }

    rhi::TextureDesc td = rhi::TextureDesc::DepthBuffer(rhi::TextureFormat::Depth24PlusStencil8, w,
                                                        h, 1, u8"OccDepthTex");
    m_device->CreateTexture(td, m_depthTex);
    rhi::TextureViewDesc tvd{};
    tvd.format = rhi::TextureFormat::Depth24PlusStencil8;
    tvd.dimension = rhi::TextureViewDimension::Texture2D;
    tvd.mipLevelCount = 1;
    tvd.arrayLayerCount = 1;
    m_device->CreateTextureView(m_depthTex, tvd, m_depthView);
}

draconic::foundation::Status OcclusionQuerySample::OnInit()
{
    if (!m_device->features.occlusionQueries)
    {
        std::fprintf(stderr,
                     "ERROR: Occlusion queries are not supported by this device/backend\n");
        return draconic::foundation::ErrorCode::Unknown;
    }

    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8;
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"OccVS",
                                            m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kShader,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"OccPS",
                                            m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::BufferDesc vbd{};
    vbd.size = sizeof(kVerts);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    vbd.label = u8"OccVB";
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kIdx);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    ibd.label = u8"OccIB";
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kVerts), sizeof(kVerts)));
    batch->WriteBuffer(m_ib, 0, Span<const u8>(reinterpret_cast<const u8*>(kIdx), sizeof(kIdx)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    rhi::PipelineLayoutDesc pld{};
    pld.label = u8"OccPL";
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    recreateDepth(m_width, m_height);

    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x4, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = 28;
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
    rpd.depthStencil->depthWriteEnabled = true;
    rpd.depthStencil->depthCompare = rhi::CompareFunction::Less;
    rpd.label = u8"OccPipeline";
    if (m_device->CreateRenderPipeline(rpd, m_pipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Occlusion query set: 2 queries (one per test quad).
    rhi::QuerySetDesc qsd{};
    qsd.type = rhi::QueryType::Occlusion;
    qsd.count = 2;
    qsd.label = u8"OcclusionQS";
    if (m_device->CreateQuerySet(qsd, m_occlusionQuerySet) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Buffer for query results (2 * uint64 = 16 bytes).
    rhi::BufferDesc qbd{};
    qbd.size = 16;
    qbd.usage = rhi::BufferUsage::CopyDst;
    qbd.memory = rhi::MemoryLocation::GpuToCpu;
    qbd.label = u8"OccResultBuf";
    if (m_device->CreateBuffer(qbd, m_queryResultBuf) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

void OcclusionQuerySample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::u64, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);

    // Read back previous frame's occlusion results (after fence wait ensures GPU is done).
    if (m_frameCount > 1)
    {
        void* mapped = m_queryResultBuf->Map();
        if (mapped)
        {
            auto* results = static_cast<u64*>(mapped);
            u64 pixelsA = results[0];
            u64 pixelsB = results[1];

            if (m_totalTime - m_lastReportTime >= 2.0f)
            {
                std::printf("Occlusion: QuadA=%llu pixels, QuadB=%llu pixels (B should be ~0)\n",
                            static_cast<unsigned long long>(pixelsA),
                            static_cast<unsigned long long>(pixelsB));
                m_lastReportTime = m_totalTime;
            }
            m_queryResultBuf->Unmap();
        }
    }

    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;

    // Debug label: frame start.
    enc->InsertDebugLabel(u8"Frame Start", 0, 1, 0);

    // Reset queries for this frame.
    enc->ResetQuerySet(m_occlusionQuerySet, 0, 2);

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    enc->TransitionTexture(m_depthTex, rhi::ResourceState::Undefined,
                           rhi::ResourceState::DepthStencilWrite);

    // Debug label: render pass.
    enc->BeginDebugLabel(u8"Main Render Pass", 0.2f, 0.5f, 1.0f);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    rhi::DepthStencilAttachment dsa{};
    dsa.view = m_depthView;
    dsa.depthLoadOp = rhi::LoadOp::Clear;
    dsa.depthStoreOp = rhi::StoreOp::Store;
    dsa.depthClearValue = 1.0f;
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    rpd.depthStencilAttachment = dsa;
    rpd.occlusionQuerySet = m_occlusionQuerySet; // WebGPU needs it declared at begin
    auto* rp = enc->BeginRenderPass(rpd);

    rp->SetPipeline(m_pipeline);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);

    // Draw occluder first (writes depth).
    rp->DrawIndexed(6, 1, 0, 0, 0);

    // Draw test quad A with occlusion query 0.
    rp->BeginOcclusionQuery(m_occlusionQuerySet, 0);
    rp->DrawIndexed(6, 1, 6, 0, 0);
    rp->EndOcclusionQuery(m_occlusionQuerySet, 0);

    // Draw test quad B with occlusion query 1.
    rp->BeginOcclusionQuery(m_occlusionQuerySet, 1);
    rp->DrawIndexed(6, 1, 12, 0, 0);
    rp->EndOcclusionQuery(m_occlusionQuerySet, 1);

    rp->End();

    enc->EndDebugLabel();

    // Resolve occlusion queries to buffer.
    enc->ResolveQuerySet(m_occlusionQuerySet, 0, 2, m_queryResultBuf, 0);

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);
    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);

    m_frameCount++;
}

void OcclusionQuerySample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_queryResultBuf)
        m_device->DestroyBuffer(m_queryResultBuf);
    if (m_occlusionQuerySet)
        m_device->DestroyQuerySet(m_occlusionQuerySet);
    if (m_pipeline)
        m_device->DestroyRenderPipeline(m_pipeline);
    if (m_pl)
        m_device->DestroyPipelineLayout(m_pl);
    if (m_depthView)
        m_device->DestroyTextureView(m_depthView);
    if (m_depthTex)
        m_device->DestroyTexture(m_depthTex);
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
    OcclusionQuerySample app;
    return app.Run(argc, argv);
}
