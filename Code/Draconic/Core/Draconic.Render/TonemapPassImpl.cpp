/// Draconic::Render - the `:tonemap` partition.
///
/// The HDR resolve: the forward pass renders linear HDR into a transient (RGBA16F); this fullscreen
/// pass reads it, applies exposure + a tonemap operator + the display OETF, and writes the LDR
/// target. Keeps the renderer in a strict linear working space (docs/design/renderer.md §12) - the
/// foundation IBL/post are designed against. CM1a uses a trivial clamp; CM1b swaps in AgX.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    Status TonemapPass::Initialize()
    {

        // set 0: HDR (t0) + bloom (t1) + AO (t2), all sampled, + a linear sampler (s0).
        rhi::BindGroupLayoutEntry hdrEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry bloomEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry aoEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry sampEntry =
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry entries[] = {hdrEntry, bloomEntry, aoEntry, sampEntry};
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{entries, 4};
        if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayout* layouts[] = {m_layout};
        rhi::PushConstantRange pc{};
        pc.stages = rhi::ShaderStage::Fragment;
        pc.offset = 0;
        pc.size = sizeof(f32) * 10; // exposure + bloom + uvScale.xy + uvOffset.xy + aoStrength +
                                    // debugShowAo + operator + flipSceneY
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Linear;
        ss.magFilter = rhi::FilterMode::Linear;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"tonemap.bloomSampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void TonemapPass::DeclareTonemap(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                                     rendergraph::RGHandle bloom, rendergraph::RGHandle ao,
                                     rendergraph::RGHandle ldr, bool clearColor,
                                     const rhi::ClearColor& clear, rhi::TextureFormat ldrFormat,
                                     i32 vpX, i32 vpY, u32 vpW, u32 vpH, u32 frameIndex,
                                     u32 viewIndex, f32 exposure, f32 bloomIntensity,
                                     Float2 uvScale, Float2 uvOffset, f32 aoStrength,
                                     bool debugShowAo, bool agx, bool sceneYFlipped)
    {
        rhi::RenderPipeline* pipeline = EnsurePipeline(ldrFormat);
        if (pipeline == nullptr)
        {
            return;
        }
        const u32 slot =
            (viewIndex % kMaxViews) * m_framesInFlight + (frameIndex % m_framesInFlight);
        // The scene input is mirrored on Y-flip backends unless the TAA resolve un-mirrored
        // it upstream; tonemap compensates then (see tonemap.ps.hlsl FlipSceneY).
        const bool flipSceneY = sceneYFlipped && m_device->NeedsClipSpaceYFlip();
        const f32 push[10] = {
            exposure,          bloomIntensity, uvScale.x,  uvScale.y,
            uvOffset.x,        uvOffset.y,     aoStrength, debugShowAo ? 1.0f : 0.0f,
            agx ? 1.0f : 0.0f, flipSceneY ? 1.0f : 0.0f};

        const rhi::LoadOp load = clearColor ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
        graph.AddRenderPass(
            u8"tonemap",
            [this, &graph, hdr, bloom, ao, ldr, load, clear, vpX, vpY, vpW, vpH, pipeline, slot,
             push](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, ldr, load, rhi::StoreOp::Store, clear);
                b.ReadTexture(hdr);
                b.ReadTexture(bloom);
                b.ReadTexture(ao);
                b.SetViewport(vpX, vpY, vpW, vpH);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, hdr, bloom, ao, pipeline, slot, push](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(
                            slot, graph.GetTextureView(hdr), graph.GetTextureView(bloom),
                            graph.GetTextureView(ao),
                            graph.GetTextureGeneration(hdr) ^
                                (graph.GetTextureGeneration(bloom) * 1099511628211ull) ^
                                (graph.GetTextureGeneration(ao) * 14695981039346656037ull));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(push), push);
                        rp.Draw(3, 1, 0, 0);
                    });
            });
    }

    rhi::RenderPipeline* TonemapPass::EnsurePipeline(rhi::TextureFormat fmt)
    {
        const u64 shaderVersion = m_shaders->Version(u8"tonemap"); // hot reload rebuilds
        if (m_pipeline != nullptr && m_pipelineFormat == fmt &&
            m_pipelineShaderVersion == shaderVersion)
        {
            return m_pipeline;
        }
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"tonemap", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"tonemap", shaders::ShaderStage::Fragment,
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

        rhi::ColorTargetState color{};
        color.format = fmt;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};

        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"tonemap";
        if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
        {
            m_pipeline = nullptr;
            return nullptr;
        }
        m_pipelineFormat = fmt;
        m_pipelineShaderVersion = shaderVersion;
        return m_pipeline;
    }

    rhi::BindGroup* TonemapPass::EnsureBindGroup(u32 slot, rhi::TextureView* hdrView,
                                                 rhi::TextureView* bloomView,
                                                 rhi::TextureView* aoView, u64 generation)
    {
        if (slot >= kMaxSlots || hdrView == nullptr || bloomView == nullptr || aoView == nullptr)
        {
            return nullptr;
        }
        if (m_bindGroups[slot] != nullptr && m_bgViews[slot] == hdrView &&
            m_bgBloom[slot] == bloomView && m_bgAo[slot] == aoView && m_bgGen[slot] == generation)
        {
            return m_bindGroups[slot];
        }
        if (m_bindGroups[slot] != nullptr)
        {
            m_device->DestroyBindGroup(m_bindGroups[slot]);
            m_bindGroups[slot] = nullptr;
        }
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::TextureEntry(hdrView),
            rhi::BindGroupEntry::TextureEntry(bloomView),
            rhi::BindGroupEntry::TextureEntry(aoView),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 4};
        if (!m_device->CreateBindGroup(bgd, m_bindGroups[slot]).IsOk())
        {
            m_bindGroups[slot] = nullptr;
            return nullptr;
        }
        m_bgViews[slot] = hdrView;
        m_bgBloom[slot] = bloomView;
        m_bgAo[slot] = aoView;
        m_bgGen[slot] = generation;
        return m_bindGroups[slot];
    }

    void TonemapPass::Shutdown()
    {
        for (u32 i = 0; i < kMaxSlots; ++i)
        {
            if (m_bindGroups[i] != nullptr)
            {
                m_device->DestroyBindGroup(m_bindGroups[i]);
                m_bindGroups[i] = nullptr;
            }
        }
        if (m_pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_pipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = nullptr;
        }
        if (m_sampler != nullptr)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
        if (m_layout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
    }
}
