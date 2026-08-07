/// Draconic::Render - the `:ssr` partition.
///
/// Screen-space reflections. A single fullscreen pass that reflects the lit HDR scene into itself:
/// reconstruct view-space position + normal from the G-buffer, reflect the view ray, march it against
/// the depth buffer, and - on a hit - sample the scene color at the hit and composite it back into the
/// HDR (LERP by a reflectivity weight, so SSR *replaces* the surface's IBL/probe specular rather than
/// adding to it, which avoids double-counting the reflection).
///
/// Inputs (all render-graph transients from the forward G-buffer): scene HDR (t0), depth (t1),
/// octahedral view-normal (t2), material = roughness/metallic (t3). Runs AFTER sky/decals and BEFORE
/// AO + the TAA resolve, so TAA temporally stabilizes the (necessarily noisy) march. Everything is done
/// in view space - the normal is already view-space, so no world round-trip is needed.
///
/// Structurally a sibling of `:ao` (same fullscreen VS, same view reconstruction, same generation-keyed
/// bind-group cache + deferred free), but with a 4-texture bind group and its own march shader.

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
    Status SsrPass::Initialize()
    {

        // Bind-group layout: scene(t0) + depth(t1) + normal(t2) + material(t3) + point sampler(s0) for
        // depth/reconstruction + linear sampler(s1) for the glossy color cone-gather.
        rhi::BindGroupLayoutEntry ge[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(3, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(1, rhi::ShaderStage::Fragment),
        };
        // WebGPU annotations (Vulkan/DX12 ignore): t1 receives the DEPTH view and is
        // sampled only through the point sampler s0 - UnfilterableFloat +
        // non-filtering, per the depth-as-data rule.
        ge[1].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        ge[4].samplerNonFiltering = true;
        rhi::BindGroupLayoutDesc gld{};
        gld.entries = Span<const rhi::BindGroupLayoutEntry>{ge, 6};
        if (!m_device->CreateBindGroupLayout(gld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayout* gl[] = {m_layout};
        rhi::PushConstantRange pcr{};
        pcr.stages = rhi::ShaderStage::Fragment;
        pcr.offset = 0;
        pcr.size = sizeof(SsrPushC);
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{gl, 1};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pcr, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Nearest;
        ss.magFilter = rhi::FilterMode::Nearest;
        ss.mipmapFilter = rhi::MipmapFilterMode::Nearest; // all-nearest (see AO note)
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"ssr.sampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ls{};
        ls.minFilter = rhi::FilterMode::Linear;
        ls.magFilter = rhi::FilterMode::Linear;
        ls.addressU = rhi::AddressMode::ClampToEdge;
        ls.addressV = rhi::AddressMode::ClampToEdge;
        ls.addressW = rhi::AddressMode::ClampToEdge;
        ls.label = u8"ssr.linear";
        if (!m_device->CreateSampler(ls, m_linearSampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        if (!CreateTracePipeline())
        {
            return Status{ErrorCode::Unknown};
        }

        // --- Resolve pipeline (temporal accumulate + composite): reflection(t0) history(t1) velocity(t2)
        //     hdr(t3) + point(s0) linear(s1). MRT out = composited HDR + next-frame reflection history. ---
        rhi::BindGroupLayoutEntry re[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(3, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(1, rhi::ShaderStage::Fragment),
        };
        rhi::BindGroupLayoutDesc rld{};
        rld.entries = Span<const rhi::BindGroupLayoutEntry>{re, 6};
        if (!m_device->CreateBindGroupLayout(rld, m_resolveLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BindGroupLayout* rgl[] = {m_resolveLayout};
        rhi::PushConstantRange rpc{};
        rpc.stages = rhi::ShaderStage::Fragment;
        rpc.offset = 0;
        rpc.size = sizeof(SsrResolvePushC);
        rhi::PipelineLayoutDesc rpld{};
        rpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{rgl, 1};
        rpld.pushConstantRanges = Span<const rhi::PushConstantRange>{&rpc, 1};
        if (!m_device->CreatePipelineLayout(rpld, m_resolvePipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreateResolvePipeline())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    bool SsrPass::CreateTracePipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ssr", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"ssr", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return false;
        }
        rhi::ColorTargetState color{};
        color.format = kHdrFormat;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"ssr";
        if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
        {
            m_pipeline = nullptr;
            return false;
        }
        return true;
    }

    bool SsrPass::CreateResolvePipeline()
    {
        rhi::ShaderModule* rvs = m_shaders->GetVariant(
            u8"ssr_resolve", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        rhi::ShaderModule* rps = m_shaders->GetVariant(
            u8"ssr_resolve", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (rvs == nullptr || rps == nullptr)
        {
            return false;
        }
        rhi::ColorTargetState rtargets[2] = {};
        rtargets[0].format = kHdrFormat; // composited HDR
        rtargets[1].format = kHdrFormat; // reflection history
        rhi::FragmentState rfrag{};
        rfrag.shader = rhi::ProgrammableStage{rps, u8"main", rhi::ShaderStage::Fragment};
        rfrag.targets = Span<const rhi::ColorTargetState>{rtargets, 2};
        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_resolvePipelineLayout;
        rpd.vertex.shader = rhi::ProgrammableStage{rvs, u8"main", rhi::ShaderStage::Vertex};
        rpd.fragment = rfrag;
        rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        rpd.primitive.cullMode = rhi::CullMode::None;
        rpd.label = u8"ssr_resolve";
        if (!m_device->CreateRenderPipeline(rpd, m_resolvePipeline).IsOk())
        {
            m_resolvePipeline = nullptr;
            return false;
        }
        return true;
    }

    rendergraph::RGHandle
    SsrPass::DeclareSsr(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                        rendergraph::RGHandle depth, rendergraph::RGHandle normal,
                        rendergraph::RGHandle material, rendergraph::RGHandle velocity, u32 w,
                        u32 h, i32 vx, i32 vy, u32 vw, u32 vh, const Float4x4& invProj,
                        const Float4x4& proj, const Params& p, u32 viewIndex, u32 frameIndex)
    {
        if (w == 0 || h == 0 || viewIndex >= kMaxViews)
        {
            return hdr;
        }
        // Hot reload: rebuild both pipelines when the shaders changed (GPU idled on reload).
        const u64 shaderVersion =
            m_shaders->Version(u8"ssr") + m_shaders->Version(u8"ssr_resolve");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }
            if (m_resolvePipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_resolvePipeline);
                m_resolvePipeline = nullptr;
            }
            (void)CreateTracePipeline();
            (void)CreateResolvePipeline();
            m_pipelineShaderVersion = shaderVersion;
        }
        if (m_pipeline == nullptr || m_resolvePipeline == nullptr)
        {
            return hdr;
        }
        Tick(frameIndex);
        const f32 fw = static_cast<f32>(w), fh = static_cast<f32>(h);
        const Float2 vpMin{static_cast<f32>(vx) / fw, static_cast<f32>(vy) / fh};
        const Float2 vpSize{static_cast<f32>(vw) / fw, static_cast<f32>(vh) / fh};
        const bool temporalOn = p.temporal;

        // --- Trace: reflection buffer (rgb reflected radiance, a = confidence) ---
        const rendergraph::RGHandle refl =
            graph.CreateTransient(u8"ssr.refl", rendergraph::RGTextureDesc(kHdrFormat, w, h));
        SsrPushC pc{};
        pc.invProj = invProj;
        pc.vpMin = vpMin;
        pc.vpSize = vpSize;
        pc.jitter = Float2{proj(2, 0), proj(2, 1)};
        pc.projXX = proj(0, 0);
        pc.projYY = proj(1, 1);
        pc.thickness = p.thickness;
        pc.intensity = p.intensity;
        pc.edgeFade = (p.edgeFade > 1e-4f) ? p.edgeFade : 1e-4f;
        pc.roughCutoff = p.roughnessCutoff;
        pc.maxSteps = (p.maxSteps > 1) ? p.maxSteps : 1;
        // The dither is STATIC per pixel (see the shader comment - the old frameMod slot now
        // carries the uv<->ndc Y convention).
        pc.ySign = m_device->NeedsClipSpaceYFlip() ? 1.0f : -1.0f;
        pc.debug = p.debug;
        pc.glossy = (p.glossy >= 0.0f) ? p.glossy : 0.0f;
        graph.AddRenderPass(
            u8"ssr.trace",
            [this, &graph, hdr, depth, normal, material, refl, w, h,
             pc](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, refl, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(hdr);
                b.ReadTexture(depth);
                b.ReadTexture(normal);
                b.ReadTexture(material);
                b.SetViewport(0, 0, w, h);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, hdr, depth, normal, material, pc](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(
                            graph.GetTextureView(hdr), graph.GetTextureView(depth),
                            graph.GetTextureView(normal), graph.GetTextureView(material),
                            Combine(graph, hdr, depth, normal, material));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(SsrPushC), &pc);
                        rp.Draw(3, 1, 0, 0);
                    });
            });

        // --- Resolve: reproject + variance-clip + accumulate history, then composite into the HDR ---
        ViewHistory& hist = m_views[viewIndex];
        if (!EnsureHistory(hist, w, h))
        {
            return hdr;
        }
        const u32 cur = hist.cur, prev = cur ^ 1u;
        const rendergraph::RGHandle out =
            graph.CreateTransient(u8"ssr.scene", rendergraph::RGTextureDesc(kHdrFormat, w, h));
        const rendergraph::RGHandle histPrev =
            graph.ImportTarget(u8"ssr.histPrev", hist.tex[prev], hist.view[prev],
                               rhi::ResourceState::ShaderRead, hist.state[prev]);
        hist.state[prev] = rhi::ResourceState::ShaderRead;
        const rendergraph::RGHandle histCur =
            graph.ImportTarget(u8"ssr.histCur", hist.tex[cur], hist.view[cur],
                               rhi::ResourceState::RenderTarget, hist.state[cur]);
        hist.state[cur] = rhi::ResourceState::RenderTarget;

        SsrResolvePushC rpc2{};
        rpc2.vpMin = vpMin;
        rpc2.vpSize = vpSize;
        rpc2.texelSize = Float2{1.0f / fw, 1.0f / fh};
        rpc2.blendFactor = p.historyBlend;
        rpc2.historyValid = hist.valid ? 1.0f : 0.0f;
        rpc2.varianceGamma = p.varianceGamma;
        rpc2.motionScale = p.motionScale;
        rpc2.temporalOn = temporalOn ? 1 : 0;
        rpc2.debug = p.debug;
        rpc2.ghostReject = p.ghostReject;

        rhi::TextureView* histPrevView = hist.view[prev];
        graph.AddRenderPass(
            u8"ssr.resolve",
            [this, &graph, refl, histPrev, velocity, hdr, out, histCur, histPrevView,
             rpc2](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, out, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.SetColorTarget(1, histCur, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(refl);
                b.ReadTexture(histPrev);
                b.ReadTexture(velocity);
                b.ReadTexture(hdr);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, refl, velocity, hdr, histPrevView,
                     rpc2](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureResolveBindGroup(
                            graph.GetTextureView(refl), histPrevView,
                            graph.GetTextureView(velocity), graph.GetTextureView(hdr),
                            graph.GetTextureGeneration(refl) ^
                                (graph.GetTextureGeneration(hdr) * 1099511628211ull));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_resolvePipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(SsrResolvePushC),
                                            &rpc2);
                        rp.Draw(3, 1, 0, 0);
                    });
            });
        hist.cur = prev; // this frame's history output becomes next frame's read
        hist.valid = true;
        return out;
    }

    bool SsrPass::EnsureHistory(ViewHistory& hist, u32 w, u32 h)
    {
        if (hist.tex[0] != nullptr && hist.w == w && hist.h == h)
        {
            return true;
        }
        DestroyHistory(hist);
        for (u32 i = 0; i < 2; ++i)
        {
            rhi::TextureDesc td{};
            td.format = kHdrFormat;
            td.width = w;
            td.height = h;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
            td.label = u8"ssr.history";
            if (!m_device->CreateTexture(td, hist.tex[i]).IsOk())
            {
                hist.tex[i] = nullptr;
                DestroyHistory(hist);
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = kHdrFormat;
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
        hist.valid = false;
        return true;
    }

    void SsrPass::DestroyHistory(ViewHistory& hist)
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

    u64 SsrPass::Combine(rendergraph::RenderGraph& g, rendergraph::RGHandle a,
                         rendergraph::RGHandle b, rendergraph::RGHandle c, rendergraph::RGHandle d)
    {
        u64 k = g.GetTextureGeneration(a);
        k = (k ^ g.GetTextureGeneration(b)) * 1099511628211ull;
        k = (k ^ g.GetTextureGeneration(c)) * 1099511628211ull;
        k = (k ^ g.GetTextureGeneration(d)) * 1099511628211ull;
        return k;
    }

    void SsrPass::Tick(u32 frameIndex)
    {
        if (frameIndex == m_lastFrame)
        {
            return;
        }
        m_lastFrame = frameIndex;
        usize w = 0;
        for (usize i = 0; i < m_retired.Size(); ++i)
        {
            if (m_retired[i].left <= 1)
            {
                m_device->DestroyBindGroup(m_retired[i].bg);
            }
            else
            {
                m_retired[i].left -= 1;
                m_retired[w++] = m_retired[i];
            }
        }
        m_retired.Resize(w);
    }

    rhi::BindGroup* SsrPass::EnsureBindGroup(rhi::TextureView* scene, rhi::TextureView* depth,
                                             rhi::TextureView* normal, rhi::TextureView* material,
                                             u64 generation)
    {
        if (scene == nullptr || depth == nullptr || normal == nullptr || material == nullptr)
        {
            return nullptr;
        }
        if (Entry* e = m_bindGroups.Find(scene))
        {
            if (e->gen == generation && e->depth == depth && e->normal == normal &&
                e->material == material && e->bg != nullptr)
            {
                return e->bg;
            }
            if (e->bg != nullptr)
            {
                m_retired.PushBack(Retired{e->bg, kRetireFrames});
                e->bg = nullptr;
            }
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(scene),
            rhi::BindGroupEntry::TextureEntry(depth),
            rhi::BindGroupEntry::TextureEntry(normal),
            rhi::BindGroupEntry::TextureEntry(material),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
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
        m_bindGroups.InsertOrAssign(scene, Entry{bg, depth, normal, material, generation});
        return bg;
    }

    rhi::BindGroup* SsrPass::EnsureResolveBindGroup(rhi::TextureView* refl,
                                                    rhi::TextureView* histPrev,
                                                    rhi::TextureView* velocity,
                                                    rhi::TextureView* hdr, u64 generation)
    {
        if (refl == nullptr || histPrev == nullptr || velocity == nullptr || hdr == nullptr)
        {
            return nullptr;
        }
        if (ResolveEntry* e = m_resolveBindGroups.Find(histPrev))
        {
            if (e->gen == generation && e->refl == refl && e->velocity == velocity &&
                e->hdr == hdr && e->bg != nullptr)
            {
                return e->bg;
            }
            if (e->bg != nullptr)
            {
                m_retired.PushBack(Retired{e->bg, kRetireFrames});
                e->bg = nullptr;
            }
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(refl),
            rhi::BindGroupEntry::TextureEntry(histPrev),
            rhi::BindGroupEntry::TextureEntry(velocity),
            rhi::BindGroupEntry::TextureEntry(hdr),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
            rhi::BindGroupEntry::SamplerEntry(m_linearSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_resolveLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 6};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_resolveBindGroups.InsertOrAssign(histPrev,
                                           ResolveEntry{bg, refl, velocity, hdr, generation});
        return bg;
    }

    void SsrPass::Shutdown()
    {
        for (auto& kv : m_bindGroups)
        {
            if (kv.value.bg != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_bindGroups.Clear();
        for (auto& kv : m_resolveBindGroups)
        {
            if (kv.value.bg != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_resolveBindGroups.Clear();
        for (auto& r : m_retired)
        {
            m_device->DestroyBindGroup(r.bg);
        }
        m_retired.Clear();
        for (u32 v = 0; v < kMaxViews; ++v)
        {
            DestroyHistory(m_views[v]);
        }
        if (m_pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_resolvePipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_resolvePipeline);
            m_resolvePipeline = nullptr;
        }
        if (m_sampler != nullptr)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
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
        if (m_resolvePipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_resolvePipelineLayout);
            m_resolvePipelineLayout = nullptr;
        }
        if (m_layout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
        if (m_resolveLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_resolveLayout);
            m_resolveLayout = nullptr;
        }
    }
}
