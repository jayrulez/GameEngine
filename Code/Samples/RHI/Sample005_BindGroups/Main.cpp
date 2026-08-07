#include <new>
/// Sample005 - Multiple Bind Groups with Dynamic Offsets.
/// Ported from Sedulous Sample005_BindGroups.
/// 4x4 grid of lit cubes, each with unique color via dynamic offset.

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

class BindGroupSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample005 - Multiple Bind Groups";
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
    static constexpr const char8_t kShader[] = u8R"(
        cbuffer GlobalUBO : register(b0, space0) { row_major float4x4 VP; };
        cbuffer ObjectUBO : register(b0, space1) { row_major float4x4 Model; float4 ObjColor; };
        struct VSInput { float3 Position : TEXCOORD0; float3 Normal : TEXCOORD1; };
        struct PSInput { float4 Position : SV_POSITION; float3 Normal : NORMAL; float4 Color : COLOR; };
        PSInput VSMain(VSInput i) {
            PSInput o;
            float4 wp = mul(float4(i.Position, 1.0), Model);
            o.Position = mul(wp, VP);
            o.Normal = mul(i.Normal, (float3x3)Model);
            o.Color = ObjColor;
            return o;
        }
        float4 PSMain(PSInput i) : SV_TARGET {
            float3 ld = normalize(float3(0.5, 1.0, -0.7));
            float ndotl = max(dot(normalize(i.Normal), ld), 0.0);
            return float4(i.Color.rgb * (0.2 + 0.8 * ndotl), 1.0);
        }
    )";

    static constexpr int kGrid = 4, kObjCount = kGrid * kGrid;
    static constexpr draconic::foundation::u32 kObjStride = 256; // DX12 CBV alignment

    // Cube with face normals (24 verts, 36 indices).
    struct Vert
    {
        float px, py, pz, nx, ny, nz;
    };
    static constexpr Vert kCubeV[24] = {
        {-.5f, -.5f, -.5f, 0, 0, -1}, {.5f, -.5f, -.5f, 0, 0, -1}, {.5f, .5f, -.5f, 0, 0, -1},
        {-.5f, .5f, -.5f, 0, 0, -1},  {.5f, -.5f, .5f, 0, 0, 1},   {-.5f, -.5f, .5f, 0, 0, 1},
        {-.5f, .5f, .5f, 0, 0, 1},    {.5f, .5f, .5f, 0, 0, 1},    {-.5f, -.5f, .5f, -1, 0, 0},
        {-.5f, -.5f, -.5f, -1, 0, 0}, {-.5f, .5f, -.5f, -1, 0, 0}, {-.5f, .5f, .5f, -1, 0, 0},
        {.5f, -.5f, -.5f, 1, 0, 0},   {.5f, -.5f, .5f, 1, 0, 0},   {.5f, .5f, .5f, 1, 0, 0},
        {.5f, .5f, -.5f, 1, 0, 0},    {-.5f, .5f, -.5f, 0, 1, 0},  {.5f, .5f, -.5f, 0, 1, 0},
        {.5f, .5f, .5f, 0, 1, 0},     {-.5f, .5f, .5f, 0, 1, 0},   {-.5f, -.5f, .5f, 0, -1, 0},
        {.5f, -.5f, .5f, 0, -1, 0},   {.5f, -.5f, -.5f, 0, -1, 0}, {-.5f, -.5f, -.5f, 0, -1, 0},
    };
    static constexpr draconic::foundation::u16 kCubeI[36] = {
        0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23};

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule *m_vs = nullptr, *m_ps = nullptr;
    rhi::Buffer *m_vb = nullptr, *m_ib = nullptr, *m_globalUbo = nullptr, *m_objUbo = nullptr;
    void *m_globalMapped = nullptr, *m_objMapped = nullptr;
    rhi::BindGroupLayout *m_globalBgl = nullptr, *m_objBgl = nullptr;
    rhi::BindGroup *m_globalBg = nullptr, *m_objBg = nullptr;
    rhi::PipelineLayout* m_pl = nullptr;
    rhi::RenderPipeline* m_pipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
    samples::framework::DepthBuffer m_depthBuf;
};

draconic::foundation::Status BindGroupSample::OnInit()
{
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
    vbd.size = sizeof(kCubeV);
    vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
    vbd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(vbd, m_vb) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BufferDesc ibd{};
    ibd.size = sizeof(kCubeI);
    ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
    ibd.memory = rhi::MemoryLocation::GpuOnly;
    if (m_device->CreateBuffer(ibd, m_ib) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::TransferBatch* batch = nullptr;
    m_graphicsQueue->CreateTransferBatch(batch);
    batch->WriteBuffer(m_vb, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kCubeV), sizeof(kCubeV)));
    batch->WriteBuffer(m_ib, 0,
                       Span<const u8>(reinterpret_cast<const u8*>(kCubeI), sizeof(kCubeI)));
    batch->Submit();
    m_graphicsQueue->DestroyTransferBatch(batch);

    rhi::BufferDesc gbd{};
    gbd.size = 256;
    gbd.usage = rhi::BufferUsage::Uniform;
    gbd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(gbd, m_globalUbo) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_globalMapped = m_globalUbo->Map();
    rhi::BufferDesc obd{};
    obd.size = kObjCount * kObjStride;
    obd.usage = rhi::BufferUsage::Uniform;
    obd.memory = rhi::MemoryLocation::CpuToGpu;
    if (m_device->CreateBuffer(obd, m_objUbo) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    m_objMapped = m_objUbo->Map();

    // Set 0: global VP.
    rhi::BindGroupLayoutEntry gE[1] = {
        rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex)};
    rhi::BindGroupLayoutDesc gBgld{};
    gBgld.entries = Span<const rhi::BindGroupLayoutEntry>(gE, 1);
    if (m_device->CreateBindGroupLayout(gBgld, m_globalBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupEntry gBgE[1] = {rhi::BindGroupEntry::BufferEntry(m_globalUbo, 0, 64)};
    rhi::BindGroupDesc gBgd{};
    gBgd.layout = m_globalBgl;
    gBgd.entries = Span<const rhi::BindGroupEntry>(gBgE, 1);
    if (m_device->CreateBindGroup(gBgd, m_globalBg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Set 1: per-object with dynamic offset.
    rhi::BindGroupLayoutEntry oE[1] = {rhi::BindGroupLayoutEntry::UniformBuffer(
        0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment)};
    oE[0].hasDynamicOffset = true;
    rhi::BindGroupLayoutDesc oBgld{};
    oBgld.entries = Span<const rhi::BindGroupLayoutEntry>(oE, 1);
    if (m_device->CreateBindGroupLayout(oBgld, m_objBgl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    rhi::BindGroupEntry oBgE[1] = {rhi::BindGroupEntry::BufferEntry(m_objUbo, 0, kObjStride)};
    rhi::BindGroupDesc oBgd{};
    oBgd.layout = m_objBgl;
    oBgd.entries = Span<const rhi::BindGroupEntry>(oBgE, 1);
    if (m_device->CreateBindGroup(oBgd, m_objBg) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Pipeline layout with 2 sets.
    rhi::BindGroupLayout* sets[2] = {m_globalBgl, m_objBgl};
    rhi::PipelineLayoutDesc pld{};
    pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(sets, 2);
    if (m_device->CreatePipelineLayout(pld, m_pl) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    m_depthBuf.Recreate(m_device, m_width, m_height);

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

void BindGroupSample::OnRender()
{
    using draconic::foundation::f32, draconic::foundation::u32, draconic::foundation::Span;
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != draconic::foundation::ErrorCode::Ok)
        return;

    // Update VP.
    f32 aspect = static_cast<f32>(m_width) / static_cast<f32>(m_height);
    f32 camAngle = m_totalTime * 0.3f, camDist = 8.0f;
    Float4x4 view = Float4x4::LookAtRH(
        draconic::foundation::Float3{std::sin(camAngle) * camDist, 5.0f, -std::cos(camAngle) * camDist},
        draconic::foundation::Float3{0, 0, 0}, draconic::foundation::Float3{0, 1, 0});
    Float4x4 proj =
        Float4x4::PerspectiveFovRH(draconic::foundation::DegreesToRadians(45.0f), aspect, 0.1f, 100.0f);
    Float4x4 vp = view * proj;
    std::memcpy(m_globalMapped, vp.Data(), 64);

    // Update per-object.
    static constexpr f32 kColors[kObjCount * 4] = {
        1,   .3f, .3f, 1, .3f, 1,   .3f, 1, .3f, .3f, 1,   1, 1,   1,   .3f, 1,
        1,   .3f, 1,   1, .3f, 1,   1,   1, 1,   .6f, .2f, 1, .6f, .2f, 1,   1,
        .2f, .8f, .6f, 1, .8f, .8f, .8f, 1, .5f, .3f, .1f, 1, .9f, .5f, .7f, 1,
        .4f, .7f, .2f, 1, .2f, .4f, .8f, 1, .8f, .4f, .4f, 1, .6f, .6f, .3f, 1};
    f32 spacing = 2.0f, half = (kGrid - 1) * spacing * 0.5f;
    for (int r = 0; r < kGrid; ++r)
        for (int c = 0; c < kGrid; ++c)
        {
            int idx = r * kGrid + c;
            f32 angle = m_totalTime * (0.5f + idx * 0.1f);
            Float4x4 model = Float4x4::RotationY(angle);
            model.m[3][0] = c * spacing - half;
            model.m[3][1] = 0;
            model.m[3][2] = r * spacing - half;
            auto* dest = static_cast<draconic::foundation::u8*>(m_objMapped) + idx * kObjStride;
            std::memcpy(dest, model.Data(), 64);
            std::memcpy(dest + 64, &kColors[idx * 4], 16);
        }

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
    ca.clearValue = rhi::ClearColor(0.08f, 0.08f, 0.12f, 1);
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
    rp->SetViewport(0, 0, static_cast<f32>(m_width), static_cast<f32>(m_height), 0, 1);
    rp->SetScissor(0, 0, m_width, m_height);
    rp->SetVertexBuffer(0, m_vb, 0);
    rp->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
    rp->SetBindGroup(0, m_globalBg);
    for (int i = 0; i < kObjCount; ++i)
    {
        u32 dynOff = static_cast<u32>(i * kObjStride);
        rp->SetBindGroup(1, m_objBg, Span<const u32>(&dynOff, 1));
        rp->DrawIndexed(36);
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

void BindGroupSample::OnShutdown()
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
    if (m_objBg)
        m_device->DestroyBindGroup(m_objBg);
    if (m_objBgl)
        m_device->DestroyBindGroupLayout(m_objBgl);
    if (m_globalBg)
        m_device->DestroyBindGroup(m_globalBg);
    if (m_globalBgl)
        m_device->DestroyBindGroupLayout(m_globalBgl);
    if (m_objUbo)
        m_device->DestroyBuffer(m_objUbo);
    if (m_globalUbo)
        m_device->DestroyBuffer(m_globalUbo);
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
    BindGroupSample app;
    return app.Run(argc, argv);
}
