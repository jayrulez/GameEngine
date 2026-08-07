/// Draconic::Render - the `:sky` partition.
///
/// Draws the environment as the visible background: a fullscreen triangle at the far plane, depth-
/// tested (LessEqual, no write) against the forward depth so it only fills pixels no geometry covered,
/// reconstructing a world-space view ray per pixel (inverse view-proj) and sampling the env cubemap.
/// Runs after the forward pass, into the same (HDR) color target, before tonemap - so the sky is in
/// the linear working space and gets tonemapped with the scene.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data;        // kGVelocityFormat (sky writes camera-motion velocity for TAA)

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    Status SkyPass::Initialize()
    {
        rhi::BindGroupLayoutEntry uboE = rhi::BindGroupLayoutEntry::UniformBuffer(
            0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry texE = rhi::BindGroupLayoutEntry::SampledTexture(
            0, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::TextureCube);
        rhi::BindGroupLayoutEntry sampE =
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry set0[] = {uboE, texE, sampE};
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{set0, 3};
        if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BindGroupLayout* layouts[] = {m_layout};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Linear;
        ss.magFilter = rhi::FilterMode::Linear;
        ss.mipmapFilter = rhi::MipmapFilterMode::Linear;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"sky.sampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void SkyPass::DeclareSky(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                             rendergraph::RGHandle velocity, rendergraph::RGHandle depth,
                             rendergraph::RGHandle envH, rhi::TextureView* envView,
                             rhi::TextureFormat colorFormat, rhi::TextureFormat depthFormat,
                             const Float4x4& invViewProj, const Float4x4& prevViewProj,
                             Float2 jitter, Float2 prevJitter, const Float3& camPos,
                             f32 backgroundIntensity, const Float3& sunDir, f32 sunSize,
                             const Float3& sunColor, f32 sunIntensity, i32 vpX, i32 vpY, u32 vpW,
                             u32 vpH, u32 frameIndex, u32 viewIndex, u64 envUid,
                             rendergraph::RGSubresourceRange colorSub)
    {
        rhi::RenderPipeline* pipeline = EnsurePipeline(colorFormat, depthFormat);
        if (pipeline == nullptr || envView == nullptr)
        {
            return;
        }
        const u32 slot = (viewIndex % kMaxViews) * m_fif + (frameIndex % m_fif);
        const u64 uid = envUid;
        SkyUniform u{};
        u.invViewProj = invViewProj;
        u.prevViewProj = prevViewProj;
        // w = the display-only sky-background multiplier (skyIntensity itself is baked into the cube).
        u.camPosIntensity = Float4{camPos.x, camPos.y, camPos.z, backgroundIntensity};
        u.sunDir = Float4{sunDir.x, sunDir.y, sunDir.z, sunSize};
        u.sunColor = Float4{sunColor.x, sunColor.y, sunColor.z, sunIntensity};
        u.jitter = Float4{jitter.x, jitter.y, prevJitter.x, prevJitter.y};
        // Y-flip targets (WebGPU today; see DxDevice::NeedsClipSpaceYFlip) present the reconstructed sky ray mirrored
        // vertically vs. Vulkan's negative-height viewport; mirror the sampled ray back on those.
        u.skyFlags = Float4{m_device->NeedsClipSpaceYFlip() ? -1.0f : 1.0f, 0.0f, 0.0f, 0.0f};

        graph.AddRenderPass(
            u8"sky",
            [this, color, velocity, depth, envH, envView, pipeline, slot, uid, u, vpX, vpY, vpW,
             vpH, colorSub](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, color, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black(), colorSub);
                b.SetColorTarget(1, velocity, rhi::LoadOp::Load,
                                 rhi::StoreOp::Store); // camera-motion velocity for TAA
                b.SetReadOnlyDepthTarget(depth);       // depth test on, no write
                b.ReadTexture(envH);                   // order precompute -> sky + barrier readable
                b.SetViewport(vpX, vpY, vpW, vpH);
                b.NeverCull();
                b.SetExecute(
                    [this, envView, pipeline, slot, uid, u](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(slot, envView, uid, u);
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.Draw(3, 1, 0, 0);
                    });
            });
    }

    rhi::RenderPipeline* SkyPass::EnsurePipeline(rhi::TextureFormat colorFmt,
                                                 rhi::TextureFormat depthFmt)
    {
        const u64 shaderVersion = m_shaders->Version(u8"sky"); // hot reload rebuilds
        if (m_pipeline != nullptr && m_colorFormat == colorFmt && m_depthFormat == depthFmt &&
            m_pipelineShaderVersion == shaderVersion)
        {
            return m_pipeline;
        }
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"sky", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"sky", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        if (m_pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }

        // 2 targets: color (matches the HDR target) + velocity (camera-motion, for TAA). No blend.
        rhi::ColorTargetState targets[2] = {};
        targets[0].format = colorFmt;
        targets[1].format = kGVelocityFormat;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{targets, 2};
        rhi::DepthStencilState ds{};
        ds.format = depthFmt;
        ds.depthTestEnabled = true;
        ds.depthWriteEnabled = false;
        ds.depthCompare =
            rhi::CompareFunction::LessEqual; // pass at the far plane (background only)

        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.depthStencil = ds;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"sky";
        if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
        {
            m_pipeline = nullptr;
            return nullptr;
        }
        m_colorFormat = colorFmt;
        m_depthFormat = depthFmt;
        m_pipelineShaderVersion = shaderVersion;
        return m_pipeline;
    }

    rhi::BindGroup* SkyPass::EnsureBindGroup(u32 slot, rhi::TextureView* envView, u64 envUid,
                                             const SkyUniform& u)
    {
        if (slot >= kMaxSlots)
        {
            return nullptr;
        }
        Slot& s = m_slots[slot];
        if (s.ubo == nullptr)
        {
            rhi::BufferDesc bd{};
            bd.size = sizeof(SkyUniform);
            bd.usage = rhi::BufferUsage::Uniform;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"sky.ubo";
            if (!m_device->CreateBuffer(bd, s.ubo).IsOk())
            {
                s.ubo = nullptr;
                return nullptr;
            }
        }
        if (void* p = s.ubo->Map())
        {
            MemCopy(p, &u, sizeof(SkyUniform));
            s.ubo->Unmap();
        }
        if (s.bindGroup == nullptr || s.env != envView || s.envUid != envUid)
        {
            if (s.bindGroup != nullptr)
            {
                m_device->DestroyBindGroup(s.bindGroup);
                s.bindGroup = nullptr;
            }
            rhi::BindGroupEntry be[] = {
                rhi::BindGroupEntry::BufferEntry(s.ubo, 0, sizeof(SkyUniform)),
                rhi::BindGroupEntry::TextureEntry(envView),
                rhi::BindGroupEntry::SamplerEntry(m_sampler),
            };
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_layout;
            bgd.entries = Span<const rhi::BindGroupEntry>{be, 3};
            if (!m_device->CreateBindGroup(bgd, s.bindGroup).IsOk())
            {
                s.bindGroup = nullptr;
                return nullptr;
            }
            s.env = envView;
            s.envUid = envUid;
        }
        return s.bindGroup;
    }

    void SkyPass::Shutdown()
    {
        for (u32 i = 0; i < kMaxSlots; ++i)
        {
            if (m_slots[i].bindGroup)
            {
                m_device->DestroyBindGroup(m_slots[i].bindGroup);
                m_slots[i].bindGroup = nullptr;
            }
            if (m_slots[i].ubo)
            {
                m_device->DestroyBuffer(m_slots[i].ubo);
                m_slots[i].ubo = nullptr;
            }
        }
        if (m_pipeline)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_sampler)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
        if (m_pipelineLayout)
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = nullptr;
        }
        if (m_layout)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
    }
}
