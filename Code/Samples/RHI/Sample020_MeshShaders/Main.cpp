#include <new>
/// Sample020 -- Mesh Shaders (Rotating Triangle). Ported from Sedulous Sample020_MeshShaders.
/// Demonstrates mesh shader pipeline: a rotating triangle generated entirely in the mesh shader.

#include <cstdio>

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.samples.framework;
import draconic.rhi.vulkan;

namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

struct PushData
{
    float time;
    float aspectRatio;
    float pad0;
    float pad1;
};

class MeshShaderSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    draconic::foundation::StringView Title() const override
    {
        return u8"Sample020 - Mesh Shaders (Rotating Triangle)";
    }

protected:
    rhi::DeviceFeatures RequiredFeatures() const override
    {
        rhi::DeviceFeatures f{};
        f.meshShaders = true;
        return f;
    }
    draconic::foundation::Status OnInit() override;
    void OnRender() override;
    void OnShutdown() override;

private:
    static constexpr const char8_t kMeshShaderSource[] = u8R"(
        struct PushConstants
        {
            float Time;
            float AspectRatio;
            float Pad0, Pad1;
        };

        [[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0, space0);

        struct MeshOutput
        {
            float4 Position : SV_POSITION;
            float3 Color    : TEXCOORD0;
        };

        [outputtopology("triangle")]
        [numthreads(1, 1, 1)]
        void MSMain(out vertices MeshOutput verts[3], out indices uint3 tris[1])
        {
            SetMeshOutputCounts(3, 1);

            float angle = pc.Time * 0.5;
            float c = cos(angle);
            float s = sin(angle);

            float2 positions[3] = {
                float2( 0.0,  0.5),
                float2(-0.5, -0.5),
                float2( 0.5, -0.5)
            };

            float3 colors[3] = {
                float3(1.0, 0.0, 0.0),
                float3(0.0, 1.0, 0.0),
                float3(0.0, 0.0, 1.0)
            };

            for (uint i = 0; i < 3; i++)
            {
                float2 p = positions[i];
                float2 rotated = float2(p.x * c - p.y * s, p.x * s + p.y * c);
                rotated.x /= pc.AspectRatio;

                verts[i].Position = float4(rotated, 0.0, 1.0);
                verts[i].Color = colors[i];
            }

            tris[0] = uint3(0, 1, 2);
        }
    )";

    static constexpr const char8_t kFragmentShaderSource[] = u8R"(
        struct PSInput
        {
            float4 Position : SV_POSITION;
            float3 Color    : TEXCOORD0;
        };

        float4 PSMain(PSInput input) : SV_TARGET
        {
            return float4(input.Color, 1.0);
        }
    )";

    shaders::Compiler* m_compiler = nullptr;
    rhi::ShaderModule* m_meshModule = nullptr;
    rhi::ShaderModule* m_fragModule = nullptr;
    rhi::PipelineLayout* m_pipelineLayout = nullptr;
    rhi::MeshPipeline* m_meshPipeline = nullptr;
    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    draconic::foundation::u64 m_fenceVal = 0;
};

draconic::foundation::Status MeshShaderSample::OnInit()
{
    using draconic::foundation::Status, draconic::foundation::Span;

    // Check mesh shader support.
    if (!m_device->features.meshShaders)
    {
        std::fprintf(stderr, "ERROR: Mesh shaders are not supported by this device/backend\n");
        return draconic::foundation::ErrorCode::Unknown;
    }

    // Shader compiler.
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Compile mesh shader (SM 6.5 required for mesh shaders).
    if (samples::framework::CompileToModule(m_compiler, m_device, kMeshShaderSource,
                                            shaders::ShaderStage::Mesh, u8"MSMain", u8"MeshShader",
                                            u8"6_5", m_meshModule) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Compile fragment shader.
    if (samples::framework::CompileToModule(
            m_compiler, m_device, kFragmentShaderSource, shaders::ShaderStage::Fragment, u8"PSMain",
            u8"FragmentShader", m_fragModule) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Pipeline layout with push constants.
    rhi::PushConstantRange pushRange{};
    pushRange.stages = rhi::ShaderStage::Mesh;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushData);

    rhi::PipelineLayoutDesc pld{};
    pld.pushConstantRanges = Span<const rhi::PushConstantRange>(&pushRange, 1);
    pld.label = u8"MeshPipelineLayout";
    if (m_device->CreatePipelineLayout(pld, m_pipelineLayout) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Create mesh pipeline.
    rhi::ColorTargetState ct{};
    ct.format = m_swapChain->Format();
    ct.writeMask = rhi::ColorWriteMask::All;

    rhi::MeshPipelineDesc mpd{};
    mpd.layout = m_pipelineLayout;
    mpd.mesh = {m_meshModule, u8"MSMain", rhi::ShaderStage::Mesh};
    mpd.fragment = rhi::FragmentState{};
    mpd.fragment->shader = {m_fragModule, u8"PSMain", rhi::ShaderStage::Fragment};
    mpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
    mpd.colorTargets = Span<const rhi::ColorTargetState>(&ct, 1);
    mpd.label = u8"MeshShaderPipeline";
    if (m_device->CreateMeshPipeline(mpd, m_meshPipeline) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    // Command pool and fence.
    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != draconic::foundation::ErrorCode::Ok)
        return draconic::foundation::ErrorCode::Unknown;

    return draconic::foundation::ErrorCode::Ok;
}

void MeshShaderSample::OnRender()
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

    // Barrier: present -> render target.
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::RenderTarget);

    // Begin render pass.
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

    // Draw with mesh shader.
    if (auto* meshPass = rp->AsMeshShaderExt())
    {
        meshPass->SetMeshPipeline(m_meshPipeline);

        // Push constants (must be after pipeline is bound).
        PushData pushData{};
        pushData.time = m_totalTime;
        pushData.aspectRatio = static_cast<float>(m_width) / static_cast<float>(m_height);
        pushData.pad0 = 0.0f;
        pushData.pad1 = 0.0f;
        rp->SetPushConstants(rhi::ShaderStage::Mesh, 0, sizeof(PushData), &pushData);

        meshPass->DrawMeshTasks(1);
    }

    rp->End();

    // Barrier: render target -> present.
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget,
                           rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void MeshShaderSample::OnShutdown()
{
    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);
    if (m_meshPipeline)
        m_device->DestroyMeshPipeline(m_meshPipeline);
    if (m_pipelineLayout)
        m_device->DestroyPipelineLayout(m_pipelineLayout);
    if (m_fragModule)
        m_device->DestroyShaderModule(m_fragModule);
    if (m_meshModule)
        m_device->DestroyShaderModule(m_meshModule);
    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    MeshShaderSample app;
    return app.Run(argc, argv);
}
