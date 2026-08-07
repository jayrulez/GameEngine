/// Draconic::Render - the `:probes` partition.
///
/// Reflection probes: local, parallax-corrected, cluster-assigned cubemap reflections
/// (docs/design/reflection-probes.md). This system owns the per-probe GPU resources and (in later
/// sub-phases) drives capture + prefilter + froxel assignment:
///   - captured cube-ARRAY (RGBA16F, [maxProbes×6] layers)   : the raw 6-face scene capture per probe.
///   - prefiltered specular cube-ARRAY (RGBA16F, mip chain)  : GGX split-sum per probe, sampled by the
///                                                             forward (set 0, t8) with parallax.
///   - probe-metadata StructuredBuffer (GpuProbe[])          : center/box/blend/slice, sampled per froxel.
///
/// A stable ProbeKey (entity id) → slot map keeps a probe's array slice persistent across frames, so only
/// dirty probes re-capture (static caching). This sub-phase (P1a) allocates the resources + the slot
/// assignment; capture, prefilter, and the debug view land in P1b/P1c.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data; // ReflectionProbe / kMaxReflectionProbes / ProbeUpdateMode

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    Status ReflectionProbeSystem::Initialize()
    {
        if (!CreateResources())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreateBlitPipeline() || !CreatePrefilterPipeline())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void ReflectionProbeSystem::BeginFrame()
    {
        // Hot reload: rebuild the blit/prefilter pipelines when their shaders changed
        // (GPU idled on reload; layouts and cached bind groups survive).
        const u64 shaderVersion = m_shaders->Version(u8"probe_blit_vs") +
                                  m_shaders->Version(u8"probe_blit_ps") +
                                  m_shaders->Version(u8"probe_prefilter_ps");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            if (m_blitPipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_blitPipeline);
                m_blitPipeline = nullptr;
            }
            if (m_prefilterPipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_prefilterPipeline);
                m_prefilterPipeline = nullptr;
            }
            (void)CreateBlitPipeline();
            (void)CreatePrefilterPipeline();
            m_pipelineShaderVersion = shaderVersion;
        }

        m_active = 0;
        m_captures.Clear();
        m_sceneRanges.Clear();
        if (m_captureWarmup > 0)
        {
            --m_captureWarmup; // startup re-capture window (survives dropped-submit startup frames)
        }
    }

    u32 ReflectionProbeSystem::Assign(const ExtractedScene* scene,
                                      Span<const ReflectionProbe> probes)
    {
        for (const SceneRange& r : m_sceneRanges)
        {
            if (r.scene == scene)
            {
                return m_active;
            } // this frame's data is per-scene-identical
        }
        SceneRange range;
        range.scene = scene;
        range.base = m_active;
        for (const ReflectionProbe& p : probes)
        {
            if (m_active >= kMaxProbes)
            {
                break;
            }
            const u32 slot = SlotFor(p.key);
            if (slot == kInvalidSlot)
            {
                continue;
            }

            const Float3 boxMin = p.center - p.halfExtents;
            const Float3 boxMax = p.center + p.halfExtents;

            GpuProbe g{};
            g.center = Float4{p.center.x, p.center.y, p.center.z, p.intensity};
            g.boxMin = Float4{boxMin.x, boxMin.y, boxMin.z, p.blendDistance};
            g.boxMax = Float4{boxMax.x, boxMax.y, boxMax.z, static_cast<f32>(slot)};
            g.params = Float4{static_cast<f32>(kPrefilterMips), static_cast<f32>(p.priority),
                              p.parallax ? 1.0f : 0.0f, 0.0f};
            m_cpuProbes[m_active] = g;

            // Dirty tracking for static caching: recapture on a new slot or a moved probe.
            const u64 sig = TransformSignature(p);
            SlotState& st = m_slotState[slot];
            // The warmup keeps re-capturing for the first frames: on web a startup submit can
            // be dropped (expired canvas texture), which would silently lose this one-shot bake
            // while `captured` says done - the same window IBLSystem uses for the env bake.
            if (!st.captured || st.signature != sig || p.update == ProbeUpdateMode::Realtime ||
                m_captureWarmup > 0)
            {
                st.dirty = true;
            }
            st.signature = sig;
            st.probeKey = p.key;
            if (st.dirty)
            {
                // Dedupe by slot (a scene assigned twice, or key reuse, must not double a task).
                bool queued = false;
                for (const CaptureTask& t : m_captures)
                {
                    if (t.slot == slot)
                    {
                        queued = true;
                        break;
                    }
                }
                // The capture renders THIS probe's owning scene (never another scene's geometry).
                if (!queued)
                {
                    m_captures.PushBack(CaptureTask{slot, p.center, scene});
                }
            }
            ++m_active;
        }
        range.count = m_active - range.base;
        m_sceneRanges.PushBack(range);
        return m_active;
    }

    ReflectionProbeSystem::ProbeRange
    ReflectionProbeSystem::RangeFor(const ExtractedScene* scene) const noexcept
    {
        for (const SceneRange& r : m_sceneRanges)
        {
            if (r.scene == scene)
            {
                return ProbeRange{r.base, r.count};
            }
        }
        return ProbeRange{};
    }

    Span<const GpuProbe> ReflectionProbeSystem::CpuProbes() const noexcept
    {
        return Span<const GpuProbe>{m_cpuProbes, m_active};
    }

    void ReflectionProbeSystem::Upload()
    {
        if (m_probeBuffer == nullptr || m_active == 0)
        {
            return;
        }
        if (void* p = m_probeBuffer->Map())
        {
            MemCopy(p, m_cpuProbes, static_cast<usize>(m_active) * sizeof(GpuProbe));
            m_probeBuffer->Unmap();
        }
    }

    Span<const ReflectionProbeSystem::CaptureTask> ReflectionProbeSystem::Captures() const noexcept
    {
        return Span<const CaptureTask>{m_captures.Data(), m_captures.Size()};
    }

    void ReflectionProbeSystem::MarkCaptured(u32 slot) noexcept
    {
        if (slot < kMaxProbes)
        {
            m_slotState[slot].captured = true;
            m_slotState[slot].dirty = false;
        }
    }

    void ReflectionProbeSystem::InitLayouts(rhi::CommandEncoder& encoder)
    {
        if (m_layoutsInit)
        {
            return;
        }
        encoder.TransitionTexture(m_capturedCube, rhi::ResourceState::Undefined,
                                  rhi::ResourceState::ShaderRead);
        encoder.TransitionTexture(m_prefilterCube, rhi::ResourceState::Undefined,
                                  rhi::ResourceState::ShaderRead);
        m_capturedState = rhi::ResourceState::ShaderRead;
        m_prefilterState = rhi::ResourceState::ShaderRead;
        m_layoutsInit = true;
    }

    rendergraph::RGHandle ReflectionProbeSystem::ImportCaptured(rendergraph::RenderGraph& graph)
    {
        const rendergraph::RGHandle h =
            graph.ImportTarget(u8"probes.captured", m_capturedCube, m_capturedArrayView,
                               rhi::ResourceState::ShaderRead, m_capturedState);
        m_capturedState = rhi::ResourceState::ShaderRead;
        return h;
    }

    rendergraph::RGHandle ReflectionProbeSystem::ImportPrefiltered(rendergraph::RenderGraph& graph)
    {
        const rendergraph::RGHandle h =
            graph.ImportTarget(u8"probes.prefilter", m_prefilterCube, m_prefilterArrayView,
                               rhi::ResourceState::ShaderRead, m_prefilterState);
        m_prefilterState = rhi::ResourceState::ShaderRead;
        return h;
    }

    void ReflectionProbeSystem::DeclareBlit(rendergraph::RenderGraph& graph,
                                            rendergraph::RGHandle capturedH,
                                            rendergraph::RGHandle prefilteredH, u32 slot)
    {
        if (m_blitPipeline == nullptr) // broken shader mid-reload: skip until it compiles
        {
            return;
        }
        for (u32 face = 0; face < 6; ++face)
        {
            rhi::BindGroup* faceBG = EnsureFaceBlit(slot, face);
            if (faceBG == nullptr)
            {
                continue;
            }
            graph.AddRenderPass(
                u8"probes.blit",
                [this, capturedH, prefilteredH, slot, face, faceBG](rendergraph::PassBuilder& b)
                {
                    rendergraph::RGSubresourceRange sub{};
                    sub.baseArrayLayer = slot * 6u + face;
                    sub.arrayLayerCount = 1;
                    b.SetColorTarget(0, prefilteredH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                     rhi::ClearColor::Black(), sub);
                    b.ReadTexture(capturedH);
                    b.SetViewport(0, 0, kPrefilterRes, kPrefilterRes);
                    b.NeverCull();
                    b.SetExecute(
                        [this, faceBG](rhi::RenderPassEncoder& rp)
                        {
                            rp.SetPipeline(m_blitPipeline);
                            rp.SetBindGroup(0, faceBG, Span<const u32>{});
                            rp.Draw(3, 1, 0, 0);
                        });
                });
        }
    }

    void ReflectionProbeSystem::DeclarePrefilter(rendergraph::RenderGraph& graph,
                                                 rendergraph::RGHandle prefilteredH, u32 slot)
    {
        if (m_prefilterPipeline == nullptr) // broken shader mid-reload: skip until it compiles
        {
            return;
        }
        rhi::BindGroup* srcBG = EnsurePrefilterSource(slot);
        if (srcBG == nullptr)
        {
            return;
        }
        for (u32 mip = 1; mip < kPrefilterMips; ++mip)
        {
            const f32 roughness = static_cast<f32>(mip) / static_cast<f32>(kPrefilterMips - 1);
            const u32 mipRes = kPrefilterRes >> mip;
            for (u32 face = 0; face < 6; ++face)
            {
                graph.AddRenderPass(
                    u8"probes.prefilter",
                    [this, prefilteredH, slot, mip, face, roughness, mipRes,
                     srcBG](rendergraph::PassBuilder& b)
                    {
                        rendergraph::RGSubresourceRange src{};
                        src.baseMipLevel = 0;
                        src.mipLevelCount = 1;
                        src.baseArrayLayer = slot * 6u;
                        src.arrayLayerCount = 6;
                        rendergraph::RGSubresourceRange dst{};
                        dst.baseMipLevel = mip;
                        dst.mipLevelCount = 1;
                        dst.baseArrayLayer = slot * 6u + face;
                        dst.arrayLayerCount = 1;
                        b.ReadTexture(prefilteredH, src);
                        b.SetColorTarget(0, prefilteredH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                         rhi::ClearColor::Black(), dst);
                        b.SetViewport(0, 0, mipRes, mipRes);
                        b.NeverCull();
                        b.SetExecute(
                            [this, face, roughness, srcBG](rhi::RenderPassEncoder& rp)
                            {
                                PrefilterPush push{};
                                push.faceIndex = static_cast<i32>(face);
                                push.roughness = roughness;
                                rp.SetPipeline(m_prefilterPipeline);
                                rp.SetBindGroup(0, srcBG, Span<const u32>{});
                                rp.SetPushConstants(rhi::ShaderStage::Fragment, 0,
                                                    sizeof(PrefilterPush), &push);
                                rp.Draw(3, 1, 0, 0);
                            });
                    });
            }
        }
    }

    rhi::TextureView* ReflectionProbeSystem::CapturedSampleView() const noexcept
    {
        return m_capturedArrayView;
    }

    rhi::TextureView* ReflectionProbeSystem::PrefilterArrayView() const noexcept
    {
        return m_prefilterArrayView;
    }

    u32 ReflectionProbeSystem::SlotFor(u64 key)
    {
        if (const u32* found = m_slots.Find(key))
        {
            return *found;
        }
        if (m_nextSlot >= kMaxProbes)
        {
            return kInvalidSlot;
        }
        const u32 slot = m_nextSlot++;
        m_slots.InsertOrAssign(key, slot);
        return slot;
    }

    u64 ReflectionProbeSystem::TransformSignature(const ReflectionProbe& p)
    {
        u64 h = 1469598103934665603ull; // FNV-1a
        auto mix = [&](f32 v)
        {
            h ^= static_cast<u64>(__builtin_bit_cast(u32, v));
            h *= 1099511628211ull;
        };
        mix(p.center.x);
        mix(p.center.y);
        mix(p.center.z);
        mix(p.halfExtents.x);
        mix(p.halfExtents.y);
        mix(p.halfExtents.z);
        h ^= static_cast<u64>(p.resolution);
        return h;
    }

    bool ReflectionProbeSystem::CreateResources()
    {
        const u32 layers = kMaxProbes * 6u;

        // Captured cube-array: the raw 6-face scene render per probe (RenderTarget), sampled by prefilter.
        rhi::TextureDesc cd{};
        cd.format = kCubeFormat;
        cd.width = kCaptureRes;
        cd.height = kCaptureRes;
        cd.arrayLayerCount = layers;
        cd.mipLevelCount = 1;
        cd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
                   rhi::TextureUsage::CopySrc;
        cd.label = u8"probes.captured";
        if (!m_device->CreateTexture(cd, m_capturedCube).IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc cv{};
        cv.format = kCubeFormat;
        cv.dimension = rhi::TextureViewDimension::TextureCubeArray;
        cv.arrayLayerCount = layers;
        cv.mipLevelCount = 1;
        if (!m_device->CreateTextureView(m_capturedCube, cv, m_capturedArrayView).IsOk())
        {
            return false;
        }

        // Prefiltered specular cube-array (mip chain): GGX split-sum per probe, sampled by the forward.
        rhi::TextureDesc pd{};
        pd.format = kCubeFormat;
        pd.width = kPrefilterRes;
        pd.height = kPrefilterRes;
        pd.arrayLayerCount = layers;
        pd.mipLevelCount = kPrefilterMips;
        pd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
                   rhi::TextureUsage::CopyDst;
        pd.label = u8"probes.prefilter";
        if (!m_device->CreateTexture(pd, m_prefilterCube).IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc pv{};
        pv.format = kCubeFormat;
        pv.dimension = rhi::TextureViewDimension::TextureCubeArray;
        pv.arrayLayerCount = layers;
        pv.mipLevelCount = kPrefilterMips;
        if (!m_device->CreateTextureView(m_prefilterCube, pv, m_prefilterArrayView).IsOk())
        {
            return false;
        }

        // Probe-metadata buffer (set-0 t9, StructuredBuffer<GpuProbe>). Host-visible so the forward reads
        // this frame's probes directly (small: kMaxProbes*64B); uploaded each frame in Upload().
        rhi::BufferDesc bd{};
        bd.size = sizeof(GpuProbe) * kMaxProbes;
        bd.usage = rhi::BufferUsage::StorageRead;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        bd.label = u8"probes.meta";
        if (!m_device->CreateBuffer(bd, m_probeBuffer).IsOk())
        {
            return false;
        }

        // Linear-clamp sampler.
        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Linear;
        ss.magFilter = rhi::FilterMode::Linear;
        ss.mipmapFilter = rhi::MipmapFilterMode::Linear;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"probes.sampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return false;
        }

        return true;
    }

    bool ReflectionProbeSystem::CreateBlitPipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(
            u8"probe_blit_vs", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(
            u8"probe_blit_ps", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return false;
        }

        // Texture2DArray, not Texture2D: the source is ONE LAYER of the captured cube-array,
        // and a DX12 TEXTURE2D SRV cannot address a non-zero array slice (always layer 0).
        // The per-face view carries the slice; the shader samples slice 0 of it.
        if (m_blitLayout == nullptr) // layouts survive a shader hot reload
        {
            rhi::BindGroupLayoutEntry srcTex = rhi::BindGroupLayoutEntry::SampledTexture(
                0, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray);
            rhi::BindGroupLayoutEntry srcSamp =
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry entries[] = {srcTex, srcSamp};
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{entries, 2};
            if (!m_device->CreateBindGroupLayout(ld, m_blitLayout).IsOk())
            {
                return false;
            }

            rhi::BindGroupLayout* layouts[] = {m_blitLayout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
            if (!m_device->CreatePipelineLayout(pld, m_blitPipeLayout).IsOk())
            {
                return false;
            }
        }

        rhi::ColorTargetState color{};
        color.format = kCubeFormat;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_blitPipeLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"probes.blit";
        if (!m_device->CreateRenderPipeline(pd, m_blitPipeline).IsOk())
        {
            return false;
        }
        return true;
    }

    rhi::BindGroup* ReflectionProbeSystem::EnsureFaceBlit(u32 slot, u32 face)
    {
        const u32 idx = slot * 6u + face;
        if (m_blitFaceBG[idx] != nullptr)
        {
            return m_blitFaceBG[idx];
        }
        rhi::TextureViewDesc vd{};
        vd.format = kCubeFormat;
        // Single-slice ARRAY view (see the blit layout comment - DX12 TEXTURE2D SRVs
        // cannot select an array slice).
        vd.dimension = rhi::TextureViewDimension::Texture2DArray;
        vd.baseArrayLayer = idx;
        vd.arrayLayerCount = 1;
        vd.mipLevelCount = 1;
        if (!m_device->CreateTextureView(m_capturedCube, vd, m_capturedFaceView[idx]).IsOk())
        {
            return nullptr;
        }
        rhi::BindGroupEntry be[] = {rhi::BindGroupEntry::TextureEntry(m_capturedFaceView[idx]),
                                    rhi::BindGroupEntry::SamplerEntry(m_sampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_blitLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
        if (!m_device->CreateBindGroup(bgd, m_blitFaceBG[idx]).IsOk())
        {
            m_blitFaceBG[idx] = nullptr;
            return nullptr;
        }
        return m_blitFaceBG[idx];
    }

    bool ReflectionProbeSystem::CreatePrefilterPipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(
            u8"probe_blit_vs", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(
            u8"probe_prefilter_ps", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return false;
        }

        // TextureCubeArray, not TextureCube: the source is ONE CUBE of the prefiltered
        // cube-array, and a DX12 TEXTURECUBE SRV cannot address a non-zero first face
        // (always cube 0). The per-probe view carries the base; the shader samples cube 0.
        if (m_prefilterLayout == nullptr) // layouts survive a shader hot reload
        {
            rhi::BindGroupLayoutEntry srcTex = rhi::BindGroupLayoutEntry::SampledTexture(
                0, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::TextureCubeArray);
            rhi::BindGroupLayoutEntry srcSamp =
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry entries[] = {srcTex, srcSamp};
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{entries, 2};
            if (!m_device->CreateBindGroupLayout(ld, m_prefilterLayout).IsOk())
            {
                return false;
            }

            rhi::PushConstantRange pcRange{};
            pcRange.stages = rhi::ShaderStage::Fragment;
            pcRange.offset = 0;
            pcRange.size = sizeof(PrefilterPush);
            rhi::BindGroupLayout* layouts[] = {m_prefilterLayout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
            pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pcRange, 1};
            if (!m_device->CreatePipelineLayout(pld, m_prefilterPipeLayout).IsOk())
            {
                return false;
            }
        }

        rhi::ColorTargetState color{};
        color.format = kCubeFormat;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_prefilterPipeLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"probes.prefilter";
        if (!m_device->CreateRenderPipeline(pd, m_prefilterPipeline).IsOk())
        {
            return false;
        }
        return true;
    }

    rhi::BindGroup* ReflectionProbeSystem::EnsurePrefilterSource(u32 slot)
    {
        if (m_prefilterSrcBG[slot] != nullptr)
        {
            return m_prefilterSrcBG[slot];
        }
        rhi::TextureViewDesc vd{};
        vd.format = kCubeFormat;
        // Single-cube ARRAY view (see the prefilter layout comment - DX12 TEXTURECUBE
        // SRVs cannot select a first face).
        vd.dimension = rhi::TextureViewDimension::TextureCubeArray;
        vd.baseMipLevel = 0;
        vd.mipLevelCount = 1;
        vd.baseArrayLayer = slot * 6u;
        vd.arrayLayerCount = 6;
        if (!m_device->CreateTextureView(m_prefilterCube, vd, m_prefilterSrcView[slot]).IsOk())
        {
            return nullptr;
        }
        rhi::BindGroupEntry be[] = {rhi::BindGroupEntry::TextureEntry(m_prefilterSrcView[slot]),
                                    rhi::BindGroupEntry::SamplerEntry(m_sampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_prefilterLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
        if (!m_device->CreateBindGroup(bgd, m_prefilterSrcBG[slot]).IsOk())
        {
            m_prefilterSrcBG[slot] = nullptr;
            return nullptr;
        }
        return m_prefilterSrcBG[slot];
    }

    void ReflectionProbeSystem::DestroyResources()
    {
        if (m_device == nullptr)
        {
            return;
        }
        for (u32 i = 0; i < kMaxProbes * 6; ++i)
        {
            if (m_blitFaceBG[i] != nullptr)
            {
                m_device->DestroyBindGroup(m_blitFaceBG[i]);
                m_blitFaceBG[i] = nullptr;
            }
            if (m_capturedFaceView[i] != nullptr)
            {
                m_device->DestroyTextureView(m_capturedFaceView[i]);
                m_capturedFaceView[i] = nullptr;
            }
        }
        for (u32 i = 0; i < kMaxProbes; ++i)
        {
            if (m_prefilterSrcBG[i] != nullptr)
            {
                m_device->DestroyBindGroup(m_prefilterSrcBG[i]);
                m_prefilterSrcBG[i] = nullptr;
            }
            if (m_prefilterSrcView[i] != nullptr)
            {
                m_device->DestroyTextureView(m_prefilterSrcView[i]);
                m_prefilterSrcView[i] = nullptr;
            }
        }
        if (m_prefilterPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_prefilterPipeline);
            m_prefilterPipeline = nullptr;
        }
        if (m_prefilterPipeLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_prefilterPipeLayout);
            m_prefilterPipeLayout = nullptr;
        }
        if (m_prefilterLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_prefilterLayout);
            m_prefilterLayout = nullptr;
        }
        if (m_blitPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_blitPipeline);
            m_blitPipeline = nullptr;
        }
        if (m_blitPipeLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_blitPipeLayout);
            m_blitPipeLayout = nullptr;
        }
        if (m_blitLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_blitLayout);
            m_blitLayout = nullptr;
        }
        if (m_prefilterArrayView != nullptr)
        {
            m_device->DestroyTextureView(m_prefilterArrayView);
            m_prefilterArrayView = nullptr;
        }
        if (m_prefilterCube != nullptr)
        {
            m_device->DestroyTexture(m_prefilterCube);
            m_prefilterCube = nullptr;
        }
        if (m_capturedArrayView != nullptr)
        {
            m_device->DestroyTextureView(m_capturedArrayView);
            m_capturedArrayView = nullptr;
        }
        if (m_capturedCube != nullptr)
        {
            m_device->DestroyTexture(m_capturedCube);
            m_capturedCube = nullptr;
        }
        if (m_probeBuffer != nullptr)
        {
            m_device->DestroyBuffer(m_probeBuffer);
            m_probeBuffer = nullptr;
        }
        if (m_sampler != nullptr)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
    }
}
