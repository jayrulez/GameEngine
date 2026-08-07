#include <new>
/// Sample004 - Compute Shader (Animated Point Grid).
/// Ported from Sedulous Sample004_Compute.

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

class ComputeSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample004 - Compute (Animated Point Grid)";
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
    static constexpr const char8_t kComputeSrc[] = u8R"(
        cbuffer Params : register(b0, space0) { float Time; uint NumPoints; float Spacing; float Padding; };
        struct Vertex { float PosX, PosY, PosZ, ColR, ColG, ColB; };
        RWStructuredBuffer<Vertex> gVertices : register(u0, space0);
        [numthreads(64, 1, 1)]
        void CSMain(uint3 dtid : SV_DispatchThreadID) {
            uint idx = dtid.x; if (idx >= NumPoints) return;
            uint gridSize = (uint)sqrt((float)NumPoints);
            uint row = idx / gridSize, col = idx % gridSize;
            float fx = ((float)col / (float)(gridSize-1))*2.0 - 1.0;
            float fz = ((float)row / (float)(gridSize-1))*2.0 - 1.0;
            float dist = sqrt(fx*fx + fz*fz);
            float fy = sin(dist*6.0 - Time*2.0) * 0.15;
            gVertices[idx].PosX = fx; gVertices[idx].PosY = fy; gVertices[idx].PosZ = fz;
            gVertices[idx].ColR = fx*0.5+0.5; gVertices[idx].ColG = fy*2.0+0.5; gVertices[idx].ColB = fz*0.5+0.5;
        }
    )";
    static constexpr const char8_t kRenderSrc[] = u8R"(
        cbuffer ViewProj : register(b0, space0) { row_major float4x4 VP; };
        struct VSInput { float3 Position : TEXCOORD0; float3 Color : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float3 Color : COLOR0;
                         [[vk::builtin("PointSize")]] float PointSize : PSIZE; };
        PSInput VSMain(VSInput i) { PSInput o; o.Position = mul(float4(i.Position,1), VP); o.Color = i.Color; o.PointSize = 1.0; return o; }
        float4 PSMain(PSInput i) : SV_TARGET { return float4(i.Color, 1.0); }
    )";

    static constexpr draconic::foundation::u32 kGrid = 64, kNumPts = kGrid * kGrid, kVertSz = 24,
                                         kBufSz = kNumPts * kVertSz;

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_cs = nullptr, *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vtxBuf = nullptr, *m_paramsBuf = nullptr, *m_vpBuf = nullptr;
    void *m_paramsMapped = nullptr, *m_vpMapped = nullptr;
    rhi::BindGroupLayout *m_compBgl = nullptr, *m_renBgl = nullptr;
    rhi::BindGroup *m_compBg = nullptr, *m_renBg = nullptr;
    rhi::PipelineLayout *m_compPl = nullptr, *m_renPl = nullptr;
    rhi::ComputePipeline* m_compPipe = nullptr;
    rhi::RenderPipeline* m_renPipe = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
    samples::framework::DepthBuffer m_depthBuf;
};

draconic::foundation::Status ComputeSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span, draconic::foundation::u8, draconic::foundation::u32;
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kComputeSrc,
                                            shaders::ShaderStage::Compute, u8"CSMain", u8"CS",
                                            m_cs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kRenderSrc,
                                            shaders::ShaderStage::Vertex, u8"VSMain", u8"VS",
                                            m_vs) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (samples::framework::CompileToModule(m_compiler, m_device, kRenderSrc,
                                            shaders::ShaderStage::Fragment, u8"PSMain", u8"PS",
                                            m_ps) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Buffers.
    rhi::BufferDesc vbd{};
    vbd.size = kBufSz;
    vbd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vtxBuf) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc pbd{};
    pbd.size = 16;
    pbd.usage = rhi::BufferUsage::Uniform;
    pbd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(pbd, m_paramsBuf) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_paramsMapped = m_paramsBuf->Map();
    rhi::BufferDesc vpd{};
    vpd.size = 64;
    vpd.usage = rhi::BufferUsage::Uniform;
    vpd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(vpd, m_vpBuf) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_vpMapped = m_vpBuf->Map();

    // Compute BGL + BG + PL + pipeline.
    rhi::BindGroupLayoutEntry cE[2] = {
        rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Compute),
        rhi::BindGroupLayoutEntry::StorageBuffer(0, rhi::ShaderStage::Compute, false)};
    rhi::BindGroupLayoutDesc cBgld{};
    cBgld.entries = Span<const rhi::BindGroupLayoutEntry>(cE, 2);
    if (m_device->CreateBindGroupLayout(cBgld, m_compBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupEntry cBgE[2] = {rhi::BindGroupEntry::BufferEntry(m_paramsBuf, 0, 16),
                                   rhi::BindGroupEntry::BufferEntry(m_vtxBuf, 0, kBufSz)};
    rhi::BindGroupDesc cBgd{};
    cBgd.layout = m_compBgl;
    cBgd.entries = Span<const rhi::BindGroupEntry>(cBgE, 2);
    if (m_device->CreateBindGroup(cBgd, m_compBg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupLayout* cSets[1] = {m_compBgl};
    rhi::PipelineLayoutDesc cPld{};
    cPld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(cSets, 1);
    if (m_device->CreatePipelineLayout(cPld, m_compPl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::ComputePipelineDesc cpd{};
    cpd.layout = m_compPl;
    cpd.compute = {m_cs, u8"CSMain", rhi::ShaderStage::Compute};
    if (m_device->CreateComputePipeline(cpd, m_compPipe) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Render BGL + BG + PL + pipeline.
    rhi::BindGroupLayoutEntry rE[1] = {
        rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex)};
    rhi::BindGroupLayoutDesc rBgld{};
    rBgld.entries = Span<const rhi::BindGroupLayoutEntry>(rE, 1);
    if (m_device->CreateBindGroupLayout(rBgld, m_renBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupEntry rBgE[1] = {rhi::BindGroupEntry::BufferEntry(m_vpBuf, 0, 64)};
    rhi::BindGroupDesc rBgd{};
    rBgd.layout = m_renBgl;
    rBgd.entries = Span<const rhi::BindGroupEntry>(rBgE, 1);
    if (m_device->CreateBindGroup(rBgd, m_renBg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupLayout* rSets[1] = {m_renBgl};
    rhi::PipelineLayoutDesc rPld{};
    rPld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(rSets, 1);
    if (m_device->CreatePipelineLayout(rPld, m_renPl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    m_depthBuf.Recreate(m_device, m_width, m_height);

    rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                     {rhi::VertexFormat::Float32x3, 12, 1}};
    rhi::VertexBufferLayout vbl{};
    vbl.stride = kVertSz;
    vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    rhi::RenderPipelineDesc rpd{};
    rpd.layout = m_renPl;
    rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
    rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
    rpd.fragment = rhi::FragmentState{};
    rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
    rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
    rpd.primitive.topology = rhi::PrimitiveTopology::PointList;
    rpd.depthStencil = rhi::DepthStencilState{};
    rpd.depthStencil->format = rhi::TextureFormat::Depth24PlusStencil8;
    rpd.depthStencil->depthCompare = rhi::CompareFunction::Less;
    if (m_device->CreateRenderPipeline(rpd, m_renPipe) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    return draconic::foundation::ErrorCode::Ok;
}

void ComputeSample::OnRender()
{
    using draconic::foundation::u32, draconic::foundation::f32, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    // Update params.
    u32 numPts = kNumPts;
    f32 params[4] = {m_totalTime, 0, 1.0f, 0};
    std::memcpy(&params[1], &numPts, 4);
    std::memcpy(m_paramsMapped, params, 16);

    // Update VP.
    f32 aspect = static_cast<f32>(m_width) / static_cast<f32>(m_height);
    f32 camAngle = m_totalTime * 0.3f, camDist = 2.5f;
    Float4x4 view = Float4x4::LookAtRH(
        draconic::foundation::Float3{std::sin(camAngle) * camDist, 1.2f, std::cos(camAngle) * camDist},
        draconic::foundation::Float3{0, 0, 0}, draconic::foundation::Float3{0, 1, 0});
    Float4x4 proj =
        Float4x4::PerspectiveFovRH(draconic::foundation::DegreesToRadians(45.0f), aspect, 0.1f, 100.0f);
    Float4x4 vp = view * proj;
    std::memcpy(m_vpMapped, vp.Data(), 64);

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != draconic::foundation::ErrorCode::Ok || !enc)
        return;

    // Compute pass.
    rhi::BufferBarrier bb{};
    bb.buffer = m_vtxBuf;
    bb.oldState = rhi::ResourceState::VertexBuffer;
    bb.newState = rhi::ResourceState::ShaderWrite;
    rhi::BarrierGroup bg1{};
    bg1.bufferBarriers = Span<const rhi::BufferBarrier>(&bb, 1);
    enc->Barrier(bg1);
    auto* cp = enc->BeginComputePass(u8"GenerateVertices");
    cp->SetPipeline(m_compPipe);
    cp->SetBindGroup(0, m_compBg);
    cp->Dispatch((kNumPts + 63) / 64);
    cp->End();
    bb.oldState = rhi::ResourceState::ShaderWrite;
    bb.newState = rhi::ResourceState::VertexBuffer;
    enc->Barrier(bg1);

    // Render pass.
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);
    enc->TransitionTexture(m_depthBuf.texture, rhi::ResourceState::Undefined,
                           rhi::ResourceState::DepthStencilWrite);
    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear;
    ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    rhi::DepthStencilAttachment dsa{};
    dsa.view = m_depthBuf.view;
    dsa.depthLoadOp = rhi::LoadOp::Clear;
    dsa.depthStoreOp = rhi::StoreOp::Store;
    dsa.depthClearValue = 1.0f;
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    rpd.depthStencilAttachment = dsa;
    auto* rp = enc->BeginRenderPass(rpd);
    rp->SetPipeline(m_renPipe);
    rp->SetBindGroup(0, m_renBg);
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vtxBuf, 0);
    rp->Draw(kNumPts);
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

void ComputeSample::OnShutdown()
{
    m_depthBuf.Destroy(m_device);
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_renPipe)
        m_device->DestroyRenderPipeline(m_renPipe);
    if (m_renPl)
        m_device->DestroyPipelineLayout(m_renPl);
    if (m_renBg)
        m_device->DestroyBindGroup(m_renBg);
    if (m_renBgl)
        m_device->DestroyBindGroupLayout(m_renBgl);
    if (m_compPipe)
        m_device->DestroyComputePipeline(m_compPipe);
    if (m_compPl)
        m_device->DestroyPipelineLayout(m_compPl);
    if (m_compBg)
        m_device->DestroyBindGroup(m_compBg);
    if (m_compBgl)
        m_device->DestroyBindGroupLayout(m_compBgl);
    if (m_vpBuf)
        m_device->DestroyBuffer(m_vpBuf);
    if (m_paramsBuf)
        m_device->DestroyBuffer(m_paramsBuf);
    if (m_vtxBuf)
        m_device->DestroyBuffer(m_vtxBuf);
    if (m_ps)
        m_device->DestroyShaderModule(m_ps);
    if (m_vs)
        m_device->DestroyShaderModule(m_vs);
    if (m_cs)
        m_device->DestroyShaderModule(m_cs);
    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    ComputeSample app;
    return app.Run(argc, argv);
}
