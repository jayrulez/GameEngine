/// Draconic::Render - the `:taa` partition.
///
/// Temporal anti-aliasing resolve (ported from Sedulous taa.frag.hlsl). Blends the current jittered
/// HDR frame with the reprojected history: closest-depth motion selection, a YCoCg variance clip, a
/// Catmull-Rom history sample, a luma/motion-adaptive blend, and a depth-disocclusion reject. Runs in
/// linear HDR after the scene is composed (opaque+sky+transparent) and before bloom/tonemap. Per-view
/// color-history ping-pong (persistent), managed here; jitter is applied to the projection by the caller.
///
/// Depth-disocclusion (ported from Sedulous, hardens ghost-on-reveal): compares this frame's linearized
/// depth against the previous frame's depth at the reprojected historyUV, rejecting history on a large
/// relative mismatch (a surface revealed/occluded). We avoid a separate prev-depth ping-pong by carrying
/// the previous frame's LINEAR depth in the color-history texture's alpha channel (unused downstream);
/// linear depth in half-float keeps ~0.05% relative precision everywhere vs the 10% reject threshold.

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
    Status TaaPass::Initialize()
    {

        // set 0: current(t0) history(t1) motion(t2) depth(t3) + point(s0) linear(s1).
        rhi::BindGroupLayoutEntry e[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(3, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(1, rhi::ShaderStage::Fragment),
        };
        // WebGPU annotations (Vulkan/DX12 ignore): t3 receives the DEPTH view,
        // sampled only through the point sampler s0.
        e[3].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        e[4].samplerNonFiltering = true;
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{e, 6};
        if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayout* layouts[] = {m_layout};
        rhi::PushConstantRange pc{};
        pc.stages = rhi::ShaderStage::Fragment;
        pc.offset = 0;
        pc.size = sizeof(TaaPush);
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        auto sampler = [this](rhi::FilterMode f, rhi::Sampler*& out)
        {
            rhi::SamplerDesc s{};
            s.minFilter = f;
            s.magFilter = f;
            s.mipmapFilter = f == rhi::FilterMode::Nearest
                                 ? rhi::MipmapFilterMode::Nearest
                                 : rhi::MipmapFilterMode::Linear; // see AO note
            s.addressU = rhi::AddressMode::ClampToEdge;
            s.addressV = rhi::AddressMode::ClampToEdge;
            s.addressW = rhi::AddressMode::ClampToEdge;
            return m_device->CreateSampler(s, out).IsOk();
        };
        if (!sampler(rhi::FilterMode::Nearest, m_pointSampler) ||
            !sampler(rhi::FilterMode::Linear, m_linearSampler))
        {
            return Status{ErrorCode::Unknown};
        }

        m_pipeline = MakePipeline();
        if (m_pipeline == nullptr)
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    rendergraph::RGHandle TaaPass::DeclareTaa(rendergraph::RenderGraph& graph,
                                              rendergraph::RGHandle current,
                                              rendergraph::RGHandle motion,
                                              rendergraph::RGHandle depth, u32 viewIndex, u32 w,
                                              u32 h, f32 blendFactor, f32 varianceGamma,
                                              f32 motionScale, f32 nearPlane, f32 farPlane)
    {
        if (viewIndex >= kMaxViews || w == 0 || h == 0)
        {
            return current;
        }
        // Hot reload: rebuild the pipeline when the shader changed (GPU idled on reload).
        const u64 shaderVersion = m_shaders->Version(u8"taa");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
            }
            m_pipeline = MakePipeline();
            m_pipelineShaderVersion = shaderVersion;
        }
        if (m_pipeline == nullptr)
        {
            return current;
        }
        ViewHistory& hist = m_views[viewIndex];
        if (!EnsureHistory(hist, w, h))
        {
            return current;
        }

        const u32 cur = hist.cur, prev = cur ^ 1u;
        const rendergraph::RGHandle resolved = graph.CreateTransient(
            u8"taa.resolved", rendergraph::RGTextureDesc(kHistoryFormat, w, h));
        const rendergraph::RGHandle histPrev =
            graph.ImportTarget(u8"taa.histPrev", hist.tex[prev], hist.view[prev],
                               rhi::ResourceState::ShaderRead, hist.state[prev]);
        hist.state[prev] = rhi::ResourceState::ShaderRead;
        const rendergraph::RGHandle histCur =
            graph.ImportTarget(u8"taa.histCur", hist.tex[cur], hist.view[cur],
                               rhi::ResourceState::RenderTarget, hist.state[cur]);
        hist.state[cur] = rhi::ResourceState::RenderTarget;

        TaaPush push{};
        push.texelSize = Float2{1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h)};
        push.blendFactor = blendFactor;
        push.historyValid = hist.valid ? 1.0f : 0.0f;
        push.varianceGamma = varianceGamma;
        push.motionScale = motionScale;
        push.nearPlane = nearPlane;
        push.farPlane = (farPlane > nearPlane) ? farPlane : 1000.0f;

        rhi::TextureView* histPrevView = hist.view[prev];
        graph.AddRenderPass(
            u8"taa",
            [this, &graph, current, motion, depth, histPrev, resolved, histCur, histPrevView,
             push](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, resolved, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.SetColorTarget(1, histCur, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(current);
                b.ReadTexture(
                    histPrev); // last frame's history -> ShaderRead (ordered after its write)
                b.ReadTexture(motion);
                b.ReadTexture(depth);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, current, motion, depth, histPrevView,
                     push](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(
                            graph.GetTextureView(current), histPrevView,
                            graph.GetTextureView(motion), graph.GetTextureView(depth),
                            graph.GetTextureGeneration(current));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(TaaPush), &push);
                        rp.Draw(3, 1, 0, 0);
                    });
            });

        hist.cur = prev; // this frame's output (cur) becomes next frame's history-to-read
        hist.valid = true;
        return resolved;
    }

    bool TaaPass::EnsureHistory(ViewHistory& hist, u32 w, u32 h)
    {
        if (hist.tex[0] != nullptr && hist.w == w && hist.h == h)
        {
            return true;
        }
        DestroyHistory(hist);
        for (u32 i = 0; i < 2; ++i)
        {
            rhi::TextureDesc td{};
            td.format = kHistoryFormat;
            td.width = w;
            td.height = h;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
            td.label = u8"taa.history";
            if (!m_device->CreateTexture(td, hist.tex[i]).IsOk())
            {
                hist.tex[i] = nullptr;
                DestroyHistory(hist);
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = kHistoryFormat;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(hist.tex[i], vd, hist.view[i]).IsOk())
            {
                hist.view[i] = nullptr;
                DestroyHistory(hist);
                return false;
            }
            hist.state[i] = rhi::ResourceState::Undefined;
        }
        hist.w = w;
        hist.h = h;
        hist.cur = 0;
        hist.valid = false; // size changed -> history stale
        return true;
    }

    void TaaPass::DestroyHistory(ViewHistory& hist)
    {
        for (u32 i = 0; i < 2; ++i)
        {
            if (hist.view[i])
            {
                m_device->DestroyTextureView(hist.view[i]);
                hist.view[i] = nullptr;
            }
            if (hist.tex[i])
            {
                m_device->DestroyTexture(hist.tex[i]);
                hist.tex[i] = nullptr;
            }
        }
        hist.w = hist.h = 0;
        hist.valid = false;
    }

    rhi::RenderPipeline* TaaPass::MakePipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"taa", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"taa", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        rhi::ColorTargetState targets[2] = {};
        targets[0].format = kHistoryFormat; // resolved
        targets[1].format = kHistoryFormat; // history
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{targets, 2};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"taa";
        rhi::RenderPipeline* p = nullptr;
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return nullptr;
        }
        return p;
    }

    rhi::BindGroup* TaaPass::EnsureBindGroup(rhi::TextureView* cur, rhi::TextureView* histPrev,
                                             rhi::TextureView* motion, rhi::TextureView* depth,
                                             u64 generation)
    {
        if (cur == nullptr || histPrev == nullptr || motion == nullptr || depth == nullptr)
        {
            return nullptr;
        }
        if (Entry* e = m_bindGroups.Find(histPrev))
        {
            if (e->gen == generation && e->cur == cur && e->bg != nullptr)
            {
                return e->bg;
            }
            if (e->bg != nullptr)
            {
                m_device->DestroyBindGroup(e->bg);
                e->bg = nullptr;
            }
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(cur),
            rhi::BindGroupEntry::TextureEntry(histPrev),
            rhi::BindGroupEntry::TextureEntry(motion),
            rhi::BindGroupEntry::TextureEntry(depth),
            rhi::BindGroupEntry::SamplerEntry(m_pointSampler),
            rhi::BindGroupEntry::SamplerEntry(m_linearSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 6};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_bindGroups.InsertOrAssign(histPrev, Entry{bg, cur, generation});
        return bg;
    }

    void TaaPass::Shutdown()
    {
        for (auto& kv : m_bindGroups)
        {
            if (kv.value.bg != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_bindGroups.Clear();
        for (u32 v = 0; v < kMaxViews; ++v)
        {
            DestroyHistory(m_views[v]);
        }
        if (m_pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_pointSampler != nullptr)
        {
            m_device->DestroySampler(m_pointSampler);
            m_pointSampler = nullptr;
        }
        if (m_linearSampler != nullptr)
        {
            m_device->DestroySampler(m_linearSampler);
            m_linearSampler = nullptr;
        }
        if (m_pipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = nullptr;
        }
        if (m_layout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
    }
}
