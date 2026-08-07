/// Draconic::Render - the `:ao` partition.
///
/// Ambient occlusion. Two interchangeable generators feed one shared pipeline:
///   - GTAO: Ground-Truth AO (Jimenez horizon integration over screen-space slices).
///   - SSAO: classic hemisphere-kernel occlusion (Crysis-style), ported from Sedulous.
/// Both consume the opaque depth + octahedral view-normal (G-buffer), write an R8 AO transient,
/// then share the SAME depth-aware bilateral blur, the SAME apply pass (multiply into the HDR before
/// TAA so the resolve stabilizes it), the SAME bind-group cache, and the SAME debug channels. The two
/// modes are mutually exclusive (AoMode). All targets are render-graph transients (sized per view).

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
    Status AoPass::Initialize()
    {

        // Shared bind-group layout: two sampled textures (t0, t1) + a sampler (s0).
        rhi::BindGroupLayoutEntry ge[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        // Every AO sub-pass samples with the POINT sampler only, and t0/t1 receive
        // depth (gtao/ssao/blur) as well as color views across passes - declare the
        // slots UnfilterableFloat + the sampler non-filtering so WebGPU accepts both
        // (Vulkan/DX12 ignore the annotations).
        ge[0].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        ge[1].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        ge[2].samplerNonFiltering = true;
        rhi::BindGroupLayoutDesc gld{};
        gld.entries = Span<const rhi::BindGroupLayoutEntry>{ge, 3};
        if (!m_device->CreateBindGroupLayout(gld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        m_gtaoLayout = MakePipelineLayout(sizeof(GtaoPushC));
        m_ssaoLayout = MakePipelineLayout(sizeof(SsaoPushC));
        m_blurLayout = MakePipelineLayout(sizeof(BlurPushC));
        m_applyLayout = MakePipelineLayout(sizeof(ApplyPushC));
        if (m_gtaoLayout == nullptr || m_ssaoLayout == nullptr || m_blurLayout == nullptr ||
            m_applyLayout == nullptr)
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Nearest;
        ss.magFilter = rhi::FilterMode::Nearest;
        ss.mipmapFilter = rhi::MipmapFilterMode::Nearest; // ALL-nearest: WebGPU counts
                                                          // a linear mip filter as
                                                          // "filtering" (default Linear)
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"ao.sampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        m_gtaoPipeline = MakePipeline(u8"ao_gtao", m_gtaoLayout);
        m_ssaoPipeline = MakePipeline(u8"ao_ssao", m_ssaoLayout);
        m_blurPipeline = MakePipeline(u8"ao_blur", m_blurLayout);
        m_applyPipeline = MakePipeline(u8"ao_apply", m_applyLayout, kHdrFormat);
        if (m_gtaoPipeline == nullptr || m_ssaoPipeline == nullptr || m_blurPipeline == nullptr ||
            m_applyPipeline == nullptr)
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    rendergraph::RGHandle AoPass::DeclareAo(rendergraph::RenderGraph& graph,
                                            rendergraph::RGHandle depth,
                                            rendergraph::RGHandle normal, u32 w, u32 h,
                                            const Float4x4& invProj, const Float4x4& proj,
                                            f32 radius, f32 intensity, u32 frameIndex, AoMode mode,
                                            i32 debugMode)
    {
        if (mode == AoMode::Off || w == 0 || h == 0)
        {
            return {};
        }
        // Hot reload: rebuild all four pipelines when any AO shader changed (GPU idled
        // on reload; the shared fullscreen/common .hlsli bumps every version at once).
        const u64 shaderVersion = m_shaders->Version(u8"ao_gtao") + m_shaders->Version(u8"ao_ssao") +
                                  m_shaders->Version(u8"ao_blur") + m_shaders->Version(u8"ao_apply");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            rhi::RenderPipeline* stale[] = {m_gtaoPipeline, m_ssaoPipeline, m_blurPipeline,
                                            m_applyPipeline};
            for (rhi::RenderPipeline* p : stale)
            {
                if (p != nullptr)
                {
                    m_device->DestroyRenderPipeline(p);
                }
            }
            m_gtaoPipeline = MakePipeline(u8"ao_gtao", m_gtaoLayout);
            m_ssaoPipeline = MakePipeline(u8"ao_ssao", m_ssaoLayout);
            m_blurPipeline = MakePipeline(u8"ao_blur", m_blurLayout);
            m_applyPipeline = MakePipeline(u8"ao_apply", m_applyLayout, kHdrFormat);
            m_pipelineShaderVersion = shaderVersion;
        }
        if (m_gtaoPipeline == nullptr || m_ssaoPipeline == nullptr || m_blurPipeline == nullptr ||
            m_applyPipeline == nullptr)
        {
            return {};
        }
        Tick(frameIndex);
        const Float2 texel{1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h)};
        const rendergraph::RGHandle aoRaw =
            graph.CreateTransient(u8"ao.raw", rendergraph::RGTextureDesc(kAoFormat, w, h));
        const rendergraph::RGHandle aoTmp =
            graph.CreateTransient(u8"ao.tmp", rendergraph::RGTextureDesc(kAoFormat, w, h));
        const rendergraph::RGHandle aoOut =
            graph.CreateTransient(u8"ao.ao", rendergraph::RGTextureDesc(kAoFormat, w, h));

        PushBuf push{};
        rhi::RenderPipeline* pipeline = nullptr;
        if (mode == AoMode::GTAO)
        {
            GtaoPushC gp{};
            gp.invProj = invProj;
            gp.texelSize = texel;
            gp.radius = radius;
            gp.intensity = intensity;
            gp.projScaleY = proj(1, 1);
            gp.frameMod = static_cast<i32>(frameIndex & 63u);
            gp.debugMode = debugMode;
            push = PushBuf::From(gp);
            pipeline = m_gtaoPipeline;
        }
        else
        {
            SsaoPushC sp{};
            sp.invProj = invProj;
            sp.texelSize = texel;
            sp.projXX = proj(0, 0);
            sp.projYY = proj(1, 1);
            sp.jitter = Float2{
                proj(2, 0),
                proj(2,
                     1)}; // NDC jitter (proj z-row) so back-projection matches the jittered depth
            sp.radius = radius;
            sp.intensity = intensity;
            sp.bias = 0.05f;
            sp.sampleCount = 16;
            sp.debugMode = debugMode;
            push = PushBuf::From(sp);
            pipeline = m_ssaoPipeline;
        }

        graph.AddRenderPass(
            u8"ao.gen",
            [this, &graph, depth, normal, aoRaw, w, h, pipeline, push](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, aoRaw, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::White());
                b.ReadTexture(depth);
                b.ReadTexture(normal);
                b.SetViewport(0, 0, w, h);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, depth, normal, pipeline, push](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(
                            graph.GetTextureView(depth), graph.GetTextureView(normal),
                            graph.GetTextureGeneration(depth) ^
                                (graph.GetTextureGeneration(normal) * 1099511628211ull));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, push.size, push.data);
                        rp.Draw(3, 1, 0, 0);
                    });
            });
        // Non-AO debug channels are raw per-pixel values - skip the bilateral blur that would smear them.
        if (debugMode >= 2)
        {
            return aoRaw;
        }
        DeclareBlur(graph, aoRaw, depth, aoTmp, w, h, texel, Float2{1.0f, 0.0f});
        DeclareBlur(graph, aoTmp, depth, aoOut, w, h, texel, Float2{0.0f, 1.0f});
        return aoOut;
    }

    rendergraph::RGHandle AoPass::DeclareApply(rendergraph::RenderGraph& graph,
                                               rendergraph::RGHandle hdr, rendergraph::RGHandle ao,
                                               u32 w, u32 h, f32 strength)
    {
        if (w == 0 || h == 0)
        {
            return hdr;
        }
        const rendergraph::RGHandle out =
            graph.CreateTransient(u8"ao.applied", rendergraph::RGTextureDesc(kHdrFormat, w, h));
        ApplyPushC ap{};
        ap.strength = strength;
        // The SSAO/GTAO fullscreen passes store the AO buffer vertically flipped vs the HDR scene on
        // Y-flip targets (WebGPU today, positive viewport); realign by sampling AO with flipped uv.y there.
        ap.flipAoY = m_device->NeedsClipSpaceYFlip() ? 1.0f : 0.0f;
        graph.AddRenderPass(
            u8"ao.apply",
            [this, &graph, hdr, ao, out, w, h, ap](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, out, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(hdr);
                b.ReadTexture(ao);
                b.SetViewport(0, 0, w, h);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, hdr, ao, ap](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(
                            graph.GetTextureView(hdr), graph.GetTextureView(ao),
                            graph.GetTextureGeneration(hdr) ^
                                (graph.GetTextureGeneration(ao) * 1099511628211ull));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_applyPipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(ApplyPushC), &ap);
                        rp.Draw(3, 1, 0, 0);
                    });
            });
        return out;
    }

    rhi::PipelineLayout* AoPass::MakePipelineLayout(usize pushSize)
    {
        rhi::BindGroupLayout* gl[] = {m_layout};
        rhi::PushConstantRange pc{};
        pc.stages = rhi::ShaderStage::Fragment;
        pc.offset = 0;
        pc.size = static_cast<u32>(pushSize);
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{gl, 1};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
        rhi::PipelineLayout* layout = nullptr;
        if (!m_device->CreatePipelineLayout(pld, layout).IsOk())
        {
            return nullptr;
        }
        return layout;
    }

    void AoPass::DeclareBlur(rendergraph::RenderGraph& graph, rendergraph::RGHandle ao,
                             rendergraph::RGHandle depth, rendergraph::RGHandle out, u32 w, u32 h,
                             Float2 texel, Float2 dir)
    {
        BlurPushC bp{};
        bp.dir = dir;
        bp.texelSize = texel;
        graph.AddRenderPass(
            u8"ao.blur",
            [this, &graph, ao, depth, out, w, h, bp](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, out, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::White());
                b.ReadTexture(ao);
                b.ReadTexture(depth);
                b.SetViewport(0, 0, w, h);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, ao, depth, bp](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(
                            graph.GetTextureView(ao), graph.GetTextureView(depth),
                            graph.GetTextureGeneration(ao) ^
                                (graph.GetTextureGeneration(depth) * 14695981039346656037ull));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_blurPipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(BlurPushC), &bp);
                        rp.Draw(3, 1, 0, 0);
                    });
            });
    }

    rhi::RenderPipeline* AoPass::MakePipeline(StringView name, rhi::PipelineLayout* layout,
                                              rhi::TextureFormat fmt)
    {
        rhi::ShaderModule* vs =
            m_shaders->GetVariant(name, shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        rhi::ShaderModule* ps =
            m_shaders->GetVariant(name, shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        rhi::ColorTargetState color{};
        color.format = fmt;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = layout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = name;
        rhi::RenderPipeline* p = nullptr;
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return nullptr;
        }
        return p;
    }

    void AoPass::Tick(u32 frameIndex)
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

    rhi::BindGroup* AoPass::EnsureBindGroup(rhi::TextureView* a, rhi::TextureView* bView,
                                            u64 generation)
    {
        if (a == nullptr || bView == nullptr)
        {
            return nullptr;
        }
        if (Entry* e = m_bindGroups.Find(a))
        {
            if (e->gen == generation && e->b == bView && e->bg != nullptr)
            {
                return e->bg;
            }
            if (e->bg != nullptr)
            {
                m_retired.PushBack(Retired{e->bg, kRetireFrames});
                e->bg = nullptr;
            } // defer-free (in-flight)
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(a),
            rhi::BindGroupEntry::TextureEntry(bView),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 3};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_bindGroups.InsertOrAssign(a, Entry{bg, bView, generation});
        return bg;
    }

    void AoPass::Shutdown()
    {
        for (auto& kv : m_bindGroups)
        {
            if (kv.value.bg != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_bindGroups.Clear();
        for (auto& r : m_retired)
        {
            m_device->DestroyBindGroup(r.bg);
        }
        m_retired.Clear();
        if (m_gtaoPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_gtaoPipeline);
            m_gtaoPipeline = nullptr;
        }
        if (m_ssaoPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_ssaoPipeline);
            m_ssaoPipeline = nullptr;
        }
        if (m_blurPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_blurPipeline);
            m_blurPipeline = nullptr;
        }
        if (m_applyPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_applyPipeline);
            m_applyPipeline = nullptr;
        }
        if (m_sampler != nullptr)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
        if (m_gtaoLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_gtaoLayout);
            m_gtaoLayout = nullptr;
        }
        if (m_ssaoLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_ssaoLayout);
            m_ssaoLayout = nullptr;
        }
        if (m_blurLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_blurLayout);
            m_blurLayout = nullptr;
        }
        if (m_applyLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_applyLayout);
            m_applyLayout = nullptr;
        }
        if (m_layout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
    }
}
