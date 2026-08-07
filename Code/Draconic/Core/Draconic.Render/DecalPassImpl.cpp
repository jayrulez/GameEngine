/// Draconic::Render - the `:decal_pass` partition.
///
/// Screen-space projected decals, ported from SedulousEngine's DecalPass/decal.frag: reconstruct the
/// world position under each pixel from the scene depth, transform it into a decal's oriented unit box,
/// clip to the box, and alpha-blend the decal texture onto the lit HDR - a "sprayed" sticker that lands
/// on whatever surface is under the box (including animated meshes, since it reads the depth buffer).
///
/// Runs after the forward+sky pass and BEFORE AO/TAA, blending into the HDR (so decals get TAA-resolved).
/// Draconic matches Sedulous's shader assumptions (row-major, D3D-style [0,1] clip depth, top-origin uv
/// under the negative viewport - same reconstruction as AoPass), so the projection math ports verbatim.
///
/// v1 draws a FULLSCREEN triangle per decal (robust across split-screen sub-rects + no box winding/cull
/// pitfalls); the box test lives in the fragment shader. Receiver normal for angle-fade comes from the
/// reconstructed world-position derivatives (ddx/ddy), faithful to the port. (A per-decal box mesh +
/// sampling the normalT G-buffer are later optimizations.)

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data;
import :resources;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    Status DecalPass::Initialize()
    {

        // set 0: scene depth (t0) + sampler (s0). set 1: per-decal UBO (dynamic). set 2: decal texture.
        rhi::BindGroupLayoutEntry depthEntries[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        // WebGPU annotations (Vulkan/DX12 ignore): t0 IS the scene depth, read as
        // data through the point sampler.
        depthEntries[0].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        depthEntries[1].samplerNonFiltering = true;
        rhi::BindGroupLayoutDesc dld{};
        dld.entries = Span<const rhi::BindGroupLayoutEntry>{depthEntries, 2};
        if (!m_device->CreateBindGroupLayout(dld, m_depthLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayoutEntry uboEntry =
            rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Fragment);
        uboEntry.hasDynamicOffset = true;
        rhi::BindGroupLayoutDesc uld{};
        uld.entries = Span<const rhi::BindGroupLayoutEntry>{&uboEntry, 1};
        if (!m_device->CreateBindGroupLayout(uld, m_uboLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayoutEntry texEntries[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        rhi::BindGroupLayoutDesc tld{};
        tld.entries = Span<const rhi::BindGroupLayoutEntry>{texEntries, 2};
        if (!m_device->CreateBindGroupLayout(tld, m_texLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayout* layouts[] = {m_depthLayout, m_uboLayout, m_texLayout};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 3};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        auto mkSampler = [this](rhi::FilterMode f, rhi::Sampler*& out)
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
        if (!mkSampler(rhi::FilterMode::Nearest, m_depthSampler) ||
            !mkSampler(rhi::FilterMode::Linear, m_texSampler))
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

    void DecalPass::BeginFrame(u32 frameIndex)
    {
        Tick(frameIndex);
        if (!m_reserved)
        {
            m_reserved = m_decalRing.Reserve(kMaxDecalsPerFrame);
        }
        m_decalRing.BeginFrame(frameIndex);
    }

    void DecalPass::DeclareDecals(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                                  rendergraph::RGHandle depth, Span<const DecalInstance> decals,
                                  const Float4x4& viewProj, u32 w, u32 h, i32 vpX, i32 vpY, u32 vpW,
                                  u32 vpH)
    {
        if (decals.IsEmpty() || w == 0 || h == 0 || m_decalRing.Buffer() == nullptr)
        {
            return;
        }
        // Hot reload: rebuild the pipeline when the shader changed (GPU idled on reload).
        const u64 shaderVersion = m_shaders->Version(u8"decal");
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
            return;
        }

        const Float4x4 invViewProj = Inverse(viewProj);
        const Float2 invSize{1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h)};

        // This view's decals -> fresh ring slots (its own InvViewProj). Local list, captured by value into
        // the pass so it survives to execute time (a shared member would be clobbered by the next view).
        Array<Draw> viewDraws;
        for (const DecalInstance& d : decals)
        {
            if (d.texture == nullptr)
            {
                continue;
            }
            const DynamicUniformRing::Range r = m_decalRing.Allocate();
            if (!r.ok)
            {
                break;
            }
            DecalUniforms u{};
            u.world = d.world;
            u.invWorld = Inverse(d.world);
            u.invViewProj = invViewProj;
            u.color = Float4{d.color.r, d.color.g, d.color.b, d.color.a};
            u.params = Float4{invSize.x, invSize.y, Cos(d.fadeStart), Cos(d.fadeEnd)};
            // Interpolant Y correction (the SkyFlags.x convention): Vulkan's negative viewport
            // makes the emitted NDC land on the right pixel already (+1 = verbatim, bit-identical
            // to the pre-flip shader); Y-flip targets mirror the interpolant, so -1 un-mirrors.
            u.flip = Float4{m_device->NeedsClipSpaceYFlip() ? -1.0f : 1.0f, 0.0f, 0.0f, 0.0f};
            MemCopy(r.ptr, &u, sizeof(u));
            viewDraws.PushBack(Draw{r.byteOffset, d.texture});
        }
        if (viewDraws.IsEmpty())
        {
            return;
        }

        rhi::BindGroup* uboBg = EnsureUboBindGroup();
        graph.AddRenderPass(
            u8"decal",
            [this, &graph, hdr, depth, uboBg, vpX, vpY, vpW, vpH,
             draws = viewDraws](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, hdr, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(depth);
                // The view's sub-rect (matches the forward/sky), so the fullscreen tri's emitted NDC lines up
                // with the scene geometry's NDC at each pixel.
                b.SetViewport(vpX, vpY, vpW, vpH);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, depth, uboBg, draws](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* depthBg = EnsureDepthBindGroup(
                            graph.GetTextureView(depth), graph.GetTextureGeneration(depth));
                        if (depthBg == nullptr || uboBg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_pipeline);
                        rp.SetBindGroup(0, depthBg, Span<const u32>{});
                        for (const Draw& d : draws)
                        {
                            rhi::BindGroup* texBg = EnsureTextureBindGroup(d.texture);
                            if (texBg == nullptr)
                            {
                                continue;
                            }
                            rp.SetBindGroup(1, uboBg, Span<const u32>{&d.offset, 1});
                            rp.SetBindGroup(2, texBg, Span<const u32>{});
                            rp.Draw(3, 1, 0, 0);
                        }
                    });
            });
    }

    rhi::RenderPipeline* DecalPass::MakePipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"decal", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"decal", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        rhi::ColorTargetState color{};
        color.format = kDecalHdrFormat;
        color.blend = rhi::BlendState::AlphaBlend();
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None; // fullscreen tri, no depth attachment
        pd.label = u8"decal";
        rhi::RenderPipeline* p = nullptr;
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return nullptr;
        }
        return p;
    }

    rhi::BindGroup* DecalPass::EnsureUboBindGroup()
    {
        const u32 gen = m_decalRing.Generation();
        if (m_uboBg != nullptr && m_uboBgGen == gen)
        {
            return m_uboBg;
        }
        if (m_uboBg != nullptr)
        {
            m_device->DestroyBindGroup(m_uboBg);
            m_uboBg = nullptr;
        }
        if (m_decalRing.Buffer() == nullptr)
        {
            return nullptr;
        }
        rhi::BindGroupEntry e =
            rhi::BindGroupEntry::BufferEntry(m_decalRing.Buffer(), 0, m_decalRing.SlotSize());
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_uboLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
        if (!m_device->CreateBindGroup(bgd, m_uboBg).IsOk())
        {
            m_uboBg = nullptr;
            return nullptr;
        }
        m_uboBgGen = gen;
        return m_uboBg;
    }

    rhi::BindGroup* DecalPass::EnsureDepthBindGroup(rhi::TextureView* depth, u64 generation)
    {
        if (depth == nullptr)
        {
            return nullptr;
        }
        if (m_depthBg != nullptr && m_depthView == depth && m_depthGen == generation)
        {
            return m_depthBg;
        }
        if (m_depthBg != nullptr)
        {
            m_retired.PushBack(Retired{m_depthBg, kRetireFrames});
            m_depthBg = nullptr;
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(depth),
            rhi::BindGroupEntry::SamplerEntry(m_depthSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_depthLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
        if (!m_device->CreateBindGroup(bgd, m_depthBg).IsOk())
        {
            m_depthBg = nullptr;
            return nullptr;
        }
        m_depthView = depth;
        m_depthGen = generation;
        return m_depthBg;
    }

    rhi::BindGroup* DecalPass::EnsureTextureBindGroup(rhi::TextureView* tex)
    {
        if (tex == nullptr)
        {
            return nullptr;
        }
        if (TexBindGroup* found = m_texBindGroups.Find(tex))
        {
            if (found->viewId == tex->uniqueId)
            {
                return found->bindGroup;
            }
            // Address reuse: the cached group references a DESTROYED view - rebuild.
            if (found->bindGroup != nullptr)
            {
                m_device->DestroyBindGroup(found->bindGroup);
            }
            m_texBindGroups.Remove(tex);
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(tex),
            rhi::BindGroupEntry::SamplerEntry(m_texSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_texLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_texBindGroups.InsertOrAssign(tex, TexBindGroup{bg, tex->uniqueId});
        return bg;
    }

    void DecalPass::Tick(u32 frameIndex)
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

    void DecalPass::Shutdown()
    {
        for (auto& kv : m_texBindGroups)
        {
            if (kv.value.bindGroup != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bindGroup);
            }
        }
        m_texBindGroups.Clear();
        for (Retired& r : m_retired)
        {
            if (r.bg != nullptr)
            {
                m_device->DestroyBindGroup(r.bg);
            }
        }
        m_retired.Clear();
        if (m_depthBg != nullptr)
        {
            m_device->DestroyBindGroup(m_depthBg);
            m_depthBg = nullptr;
        }
        if (m_uboBg != nullptr)
        {
            m_device->DestroyBindGroup(m_uboBg);
            m_uboBg = nullptr;
        }
        if (m_pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_texSampler != nullptr)
        {
            m_device->DestroySampler(m_texSampler);
            m_texSampler = nullptr;
        }
        if (m_depthSampler != nullptr)
        {
            m_device->DestroySampler(m_depthSampler);
            m_depthSampler = nullptr;
        }
        if (m_pipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = nullptr;
        }
        if (m_texLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_texLayout);
            m_texLayout = nullptr;
        }
        if (m_uboLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_uboLayout);
            m_uboLayout = nullptr;
        }
        if (m_depthLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_depthLayout);
            m_depthLayout = nullptr;
        }
    }
}
