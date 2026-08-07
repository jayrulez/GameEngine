// Draconic Render - draconic.render:pipeline implementation unit (sec 3.2 / sec 10.6).
module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Profiler/Profiler.h"

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.profiler;
import :data;
import :views;
import :cluster_system;
import :tonemap;
import :shadows;
import :ibl;
import :probes;
import :bloom;
import :taa;
import :ao;
import :ssr;
import :fxaa;
import :debug_draw;
import :debug_pass;
import :decal_pass;
import :sky;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    void Renderer::PrepareFrame(u32 maxDraws, u32 frameIndex)
    {
        (void)maxDraws;
        (void)frameIndex;
    }

    void Renderer::ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                    Array<ResolvedDraw>& out)
    {
        (void)ctx;
        (void)items;
        (void)out;
    }

    void Renderer::SetShadowMap(rhi::TextureView* shadowMap, u64 generation)
    {
        (void)shadowMap;
        (void)generation;
    }

    void Renderer::SetShadowAtlas(rhi::TextureView* atlas, u64 generation, u32 passCount)
    {
        (void)atlas;
        (void)generation;
        (void)passCount;
    }

    void Renderer::SetProbes(rhi::TextureView* cubeArray, rhi::Buffer* probeBuffer, u32 count)
    {
        (void)cubeArray;
        (void)probeBuffer;
        (void)count;
    }

    void Renderer::UploadLocalShadows(Span<const GpuLocalShadow> shadows, u32 frameIndex)
    {
        (void)shadows;
        (void)frameIndex;
    }

    void Renderer::UploadSkinning(const ExtractedScene& scene, rhi::CommandEncoder& encoder)
    {
        (void)scene;
        (void)encoder;
    }
    void RendererRegistry::Register(Renderer* renderer)
    {
        if (renderer == nullptr)
        {
            return;
        }
        renderer->SetRendererId(static_cast<u16>(m_unique.Size()));
        m_unique.PushBack(renderer);
    }

    Renderer* RendererRegistry::ById(u16 id) const noexcept
    {
        return (id < m_unique.Size()) ? m_unique[id] : nullptr;
    }

    Span<Renderer* const> RendererRegistry::Unique() const noexcept
    {
        return Span<Renderer* const>{m_unique.Data(), m_unique.Size()};
    }
    void ForwardPass::BeginFrame(u32 frameIndex)
    {
        if (!HasGlobalJobSystem())
        {
            return;
        }
        if (!EnsureWorkerPools(GlobalJobs().SlotCount()))
        {
            return;
        }
        const u32 base = frameIndex * m_workerSlots;
        for (u32 s = 0; s < m_workerSlots; ++s)
        {
            if (m_workerPools[base + s] != nullptr)
            {
                m_workerPools[base + s]->Reset();
            }
        }
    }

    void ForwardPass::DeclarePass(const RenderView& view, const RendererRegistry& registry,
                                  rendergraph::RenderGraph& graph, u32 frameIndex, u32 viewIndex,
                                  rendergraph::RGHandle colorH, rendergraph::RGHandle depth,
                                  bool clearColor, rhi::TextureFormat colorFormat,
                                  rendergraph::RGHandle normalH, rendergraph::RGHandle velocityH,
                                  rendergraph::RGHandle materialH, const Float4x4& prevViewProj,
                                  Float2 jitter, Float2 prevJitter, const ClusterBinding& cluster,
                                  const ShadowBinding& shadow, const IblBinding& ibl,
                                  rhi::LoadOp depthLoad, rendergraph::RGSubresourceRange colorSub,
                                  rendergraph::RGHandle probeHandle, bool probeValid, u32 probeBase,
                                  u32 probeCount)
    {
        if (view.Width() == 0 || view.Height() == 0)
        {
            return;
        }

        const rhi::LoadOp colorLoad = clearColor ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
        graph.AddRenderPass(
            u8"forward",
            [this, &view, &registry, depth, colorH, normalH, velocityH, materialH, colorLoad,
             colorFormat, frameIndex, viewIndex, prevViewProj, jitter, prevJitter, cluster, shadow,
             ibl, depthLoad, colorSub, probeHandle, probeValid, probeBase,
             probeCount](rendergraph::PassBuilder& b)
            {
                // colorSub targets a single layer when capturing into a cube-array face (default {} = whole target).
                b.SetColorTarget(0, colorH, colorLoad, rhi::StoreOp::Store, view.Settings().clear,
                                 colorSub);
                // MRT G-buffer aux (cleared each view): view-space normal + motion vector + roughness/metallic (SSR).
                b.SetColorTarget(1, normalH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.SetColorTarget(2, velocityH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.SetColorTarget(3, materialH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                // Depth: loaded after the prepass for early-Z; capture (no prepass) passes Clear.
                b.SetDepthTarget(depth, depthLoad, rhi::StoreOp::Store);
                // Render into this view's viewport sub-rect of the target (split-screen).
                b.SetViewport(view.ViewportX(), view.ViewportY(), view.ViewportWidth(),
                              view.ViewportHeight());
                // Read the cluster lists the build compute pass wrote (orders compute -> this pass).
                if (cluster.Valid())
                {
                    b.ReadBuffer(cluster.offsetsHandle);
                    b.ReadBuffer(cluster.indicesHandle);
                }
                // Read the WHOLE cascade array (orders all cascade passes -> this pass + barriers every
                // layer readable). The forward shader binds the full-array sample view, so the descriptor
                // spans all layers - every one must be in DepthStencilRead when this pass's secondary CB
                // samples it, even layers belonging to other views (VUID-vkCmdExecuteCommands depth-layout).
                // All cascade passes are declared up front, so depending on the whole array is correctly ordered.
                if (shadow.MapBound())
                {
                    b.SampleDepth(shadow.handle);
                }
                // Read the whole local-shadow atlas (orders the atlas depth pass -> this pass + barriers
                // it readable). One atlas shared by all views, so the whole texture is the dependency.
                if (shadow.atlasValid)
                {
                    b.SampleDepth(shadow.atlasHandle);
                }
                // Read the IBL products (orders any precompute writes -> this pass + barriers them readable).
                if (ibl.Valid())
                {
                    b.ReadTexture(ibl.prefilterHandle);
                    b.ReadTexture(ibl.brdfHandle);
                    b.ReadBuffer(ibl.shHandle);
                }
                // Read the probe captured cube-array (orders probe capture -> this forward + barriers the whole
                // array to ShaderRead, incl. uncaptured slices) - the forward samples it (t8) for local reflections.
                if (probeValid)
                {
                    b.ReadTexture(probeHandle);
                }
                b.NeverCull();
                b.SetBundleExecute(
                    [this, &view, &registry, colorFormat, frameIndex, viewIndex, prevViewProj,
                     jitter, prevJitter, cluster, shadow, ibl, probeValid, probeBase,
                     probeCount](rhi::CommandEncoder& enc, Array<rhi::RenderBundle*>& out)
                    {
                        ResolveAndEmit(view, registry, enc, frameIndex, viewIndex, colorFormat,
                                       view.Camera().ViewProjection(), prevViewProj, jitter,
                                       prevJitter, PassAffinity::Opaque, cluster, shadow, ibl, out,
                                       /*sceneDepth*/ nullptr,
                                       /*probesEnabled*/ probeValid, probeBase, probeCount);
                    });
            });
    }

    void ForwardPass::DeclareTransparent(
        const RenderView& view, const RendererRegistry& registry, rendergraph::RenderGraph& graph,
        u32 frameIndex, u32 viewIndex, rendergraph::RGHandle colorH, rendergraph::RGHandle depth,
        rhi::TextureFormat colorFormat, const Float4x4& drawViewProj, const Float4x4& prevViewProj,
        Float2 jitter, Float2 prevJitter, const ClusterBinding& cluster,
        const ShadowBinding& shadow, const IblBinding& ibl, u32 probeBase, u32 probeCount)
    {
        if (view.Width() == 0 || view.Height() == 0)
        {
            return;
        }
        graph.AddRenderPass(
            u8"transparent",
            [this, &view, &registry, &graph, depth, colorH, colorFormat, frameIndex, viewIndex,
             drawViewProj, prevViewProj, jitter, prevJitter, cluster, shadow, ibl, probeBase,
             probeCount](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, colorH, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                 view.Settings().clear);
                b.SetReadOnlyDepthTarget(depth); // test against opaque depth, no write
                b.SetViewport(view.ViewportX(), view.ViewportY(), view.ViewportWidth(),
                              view.ViewportHeight());
                if (cluster.Valid())
                {
                    b.ReadBuffer(cluster.offsetsHandle);
                    b.ReadBuffer(cluster.indicesHandle);
                }
                if (shadow.MapBound())
                {
                    b.SampleDepth(shadow.handle);
                }
                if (shadow.atlasValid)
                {
                    b.SampleDepth(shadow.atlasHandle);
                }
                if (ibl.Valid())
                {
                    b.ReadTexture(ibl.prefilterHandle);
                    b.ReadTexture(ibl.brdfHandle);
                    b.ReadBuffer(ibl.shHandle);
                }
                b.NeverCull();
                b.SetBundleExecute(
                    [this, &view, &registry, &graph, depth, colorFormat, frameIndex, viewIndex,
                     drawViewProj, prevViewProj, jitter, prevJitter, cluster, shadow, ibl,
                     probeBase,
                     probeCount](rhi::CommandEncoder& enc, Array<rhi::RenderBundle*>& out)
                    {
                        // The opaque depth is now DepthStencilRead (read-only depth target) - resolve its sampleable
                        // view and hand it to the renderers for soft particles. No render-graph change; already in state.
                        rhi::TextureView* sceneDepth = graph.GetTextureView(depth);
                        ResolveAndEmit(view, registry, enc, frameIndex, viewIndex, colorFormat,
                                       drawViewProj, prevViewProj, jitter, prevJitter,
                                       PassAffinity::Blended, cluster, shadow, ibl, out, sceneDepth,
                                       /*probesEnabled*/ true, probeBase, probeCount);
                    });
            });
    }

    void ForwardPass::DeclarePostTonemapUI(
        const RenderView& view, const RendererRegistry& registry, rendergraph::RenderGraph& graph,
        u32 frameIndex, u32 viewIndex, rendergraph::RGHandle colorH, rendergraph::RGHandle depth,
        rhi::TextureFormat colorFormat, const Float4x4& drawViewProj, const Float4x4& prevViewProj)
    {
        if (view.Width() == 0 || view.Height() == 0)
        {
            return;
        }
        bool any = false;
        for (const DrawItem& it : view.DrawList())
        {
            if (Categories().Affinity(it.data->category) == PassAffinity::PostTonemap)
            {
                any = true;
                break;
            }
        }
        if (!any)
        {
            return;
        }
        graph.AddRenderPass(
            u8"worldui",
            [this, &view, &registry, depth, colorH, colorFormat, frameIndex, viewIndex,
             drawViewProj, prevViewProj](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, colorH, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                 view.Settings().clear);
                b.SetReadOnlyDepthTarget(depth);
                b.SetViewport(view.ViewportX(), view.ViewportY(), view.ViewportWidth(),
                              view.ViewportHeight());
                b.NeverCull();
                b.SetBundleExecute(
                    [this, &view, &registry, colorFormat, frameIndex, viewIndex, drawViewProj,
                     prevViewProj](rhi::CommandEncoder& enc, Array<rhi::RenderBundle*>& out)
                    {
                        ResolveAndEmit(view, registry, enc, frameIndex, viewIndex, colorFormat,
                                       drawViewProj, prevViewProj, Float2{0.0f, 0.0f},
                                       Float2{0.0f, 0.0f}, PassAffinity::PostTonemap,
                                       ClusterBinding{}, ShadowBinding{}, IblBinding{}, out,
                                       nullptr,
                                       /*probesEnabled*/ false, 0, 0);
                    });
            });
    }

    void ForwardPass::ResolveAndEmit(const RenderView& view, const RendererRegistry& registry,
                                     rhi::CommandEncoder& encoder, u32 frameIndex, u32 viewIndex,
                                     rhi::TextureFormat colorFormat, const Float4x4& drawViewProj,
                                     const Float4x4& prevViewProj, Float2 jitter, Float2 prevJitter,
                                     PassAffinity passAffinity, const ClusterBinding& cluster,
                                     const ShadowBinding& shadow, const IblBinding& ibl,
                                     Array<rhi::RenderBundle*>& out,
                                     rhi::TextureView* sceneDepthView, bool probesEnabled,
                                     u32 probeBase, u32 probeCount)
    {
        RenderRecordContext ctx{};
        ctx.view = &view;
        ctx.viewProj =
            drawViewProj; // opaque = jittered (TAA), transparent = unjittered (drawn post-TAA)
        ctx.prevViewProj = prevViewProj;
        ctx.jitter = jitter;
        ctx.prevJitter = prevJitter;
        ctx.viewMatrix = view.Camera().view;
        ctx.cameraPos = view.Camera().position;
        ctx.ambient =
            (view.Scene() != nullptr) ? view.Scene()->Ambient() : Float3{0.03f, 0.03f, 0.03f};
        ctx.cascades = shadow.cascades;          // this view's CSM cascades
        ctx.cascadeLayerBase = shadow.layerBase; // this view's first shadow-array layer
        ctx.localShadowEntryBase = shadow.localShadowEntryBase;
        ctx.lights = (view.Scene() != nullptr) ? view.Scene()->Lights() : Span<const GpuLight>{};
        ctx.cluster = cluster;
        ctx.frameIndex = frameIndex;
        ctx.viewIndex = viewIndex;
        ctx.colorFormat = colorFormat;
        ctx.depthFormat = m_depthFormat;
        ctx.sceneDepthView =
            sceneDepthView; // opaque depth (transparent pass only) for soft particles
        ctx.probesEnabled = probesEnabled;
        ctx.probeBase = probeBase; // this view's scene's record range (multi-scene frames)
        ctx.probeCount = probeCount;
        ctx.ibl = ibl; // this view's SCENE's IBL products (per-scene contexts)
        ctx.needsMotion =
            view.Settings()
                .post
                .needsMotion; // per-view: skip prev-world when no temporal effect reads velocity
        ctx.shadowFarFade = m_shadowFarFade;

        // RESOLVE (single-threaded): sorted draw list -> ResolvedDraws (PSO build, mesh upload,
        // ring allocation). Split by the category's pass affinity (which pass draws it), then walk
        // runs of the SAME renderer within this pass and hand each to its owner (dispatched by
        // rendererId, not category - so blended meshes + sprites interleave by depth yet each run
        // still batches within one renderer). The list is category-sorted, so this-pass items are
        // contiguous; within the blended span, depth order mixes renderers as needed.
        const auto inThisPass = [&](const DrawItem& it) noexcept
        { return Categories().Affinity(it.data->category) == passAffinity; };
        m_resolved.Clear();
        const Span<const DrawItem> items = view.DrawList();
        usize i = 0;
        while (i < items.Size())
        {
            if (!inThisPass(items[i]))
            {
                ++i;
                continue;
            }
            const u16 rid = items[i].data->rendererId;
            usize j = i + 1;
            while (j < items.Size() && inThisPass(items[j]) && items[j].data->rendererId == rid)
            {
                ++j;
            }
            if (Renderer* r = registry.ById(rid))
            {
                r->Resolve(ctx, Span<const DrawItem>{items.Data() + i, j - i}, m_resolved);
            }
            i = j;
        }

        // EMIT into bundles. The bundle records its viewport up front (Vulkan secondaries / DX12
        // bundles can't inherit it) - this view's sub-rect of the target, not the full target.
        rhi::RenderBundleDesc bd{};
        bd.colorFormats[0] = colorFormat;
        if (passAffinity != PassAffinity::Opaque)
        {
            bd.colorFormatCount = 1; // color-only pass (blended / post-tonemap)
        }
        else
        {
            bd.colorFormats[1] = kGNormalFormat;   // MRT: view-space normal
            bd.colorFormats[2] = kGVelocityFormat; // MRT: motion vector
            bd.colorFormats[3] = kGMaterialFormat; // MRT: roughness/metallic (SSR)
            bd.colorFormatCount = 4;
        }
        bd.depthStencilFormat = m_depthFormat;
        // A bundle's read-only flags MUST match the render pass that executes it - the browser's
        // WebGPU (Dawn) rejects a mismatch (native wgpu is lenient). Depth: only the opaque pass
        // writes depth (blended/overlay passes run over the prepass depth read-only), which matches
        // the passes' SetDepthTarget vs SetReadOnlyDepthTarget. Stencil: the engine depth format is
        // depth-only (no stencil aspect), so the render graph never marks stencil read-only - the
        // pass ships stencilReadOnly=false, so the bundle must too (a hardcoded `true` here is what
        // tripped Dawn's execute-bundle validation on WebScene).
        bd.depthReadOnly = passAffinity != PassAffinity::Opaque;
        bd.stencilReadOnly = false;
        bd.sampleCount = 1;
        bd.viewportX = view.ViewportX();
        bd.viewportY = view.ViewportY();
        bd.width = view.ViewportWidth();
        bd.height = view.ViewportHeight();
        bd.label = u8"forward.bundle";

        m_bundles.Clear();
        const u32 total = static_cast<u32>(m_resolved.Size());
        if (HasGlobalJobSystem() && total >= kParallelEmitThreshold)
        {
            EmitParallel(bd, frameIndex);
        }
        else if (rhi::RenderBundleEncoder* be = encoder.CreateRenderBundleEncoder(bd))
        {
            for (const ResolvedDraw& d : m_resolved)
            {
                EmitDraw(*be, d);
            }
            m_bundles.PushBack(be->Finish());
        }
        for (rhi::RenderBundle* b : m_bundles)
        {
            if (b != nullptr)
            {
                out.PushBack(b);
            }
        }
    }

    void ForwardPass::EmitParallel(const rhi::RenderBundleDesc& bd, u32 frameIndex)
    {
        JobSystem& jobs = GlobalJobs();
        const u32 slots = jobs.SlotCount();
        if (!EnsureWorkerPools(slots) || m_workerSlots == 0)
        {
            return;
        }

        const u32 total = static_cast<u32>(m_resolved.Size());
        const u32 grain = (total + slots - 1u) / slots;                       // ~slots chunks
        const u32 chunks = (grain > 0) ? ((total + grain - 1u) / grain) : 1u; // <= slots

        m_bundles.Resize(chunks);
        const u32 base = frameIndex * m_workerSlots;
        const ResolvedDraw* draws = m_resolved.Data();
        jobs.ParallelFor(chunks,
                         [&, draws, total, grain, base](u32 c)
                         {
                             m_bundles[c] = nullptr;
                             rhi::CommandPool* pool =
                                 m_workerPools[base + c]; // unique pool per chunk c
                             if (pool == nullptr)
                             {
                                 return;
                             }
                             // Bundles are minted straight from the pool - no command
                             // encoder needed, so the pool never has an open primary
                             // list and its per-frame Reset is legal on DX12.
                             rhi::RenderBundleEncoder* be = pool->CreateRenderBundleEncoder(bd);
                             if (be == nullptr)
                             {
                                 return;
                             }
                             const u32 begin = c * grain;
                             const u32 end = Min((c + 1u) * grain, total);
                             for (u32 k = begin; k < end; ++k)
                             {
                                 EmitDraw(*be, draws[k]);
                             }
                             m_bundles[c] = be->Finish();
                         });
    }

    bool ForwardPass::EnsureWorkerPools(u32 slotCount)
    {
        if (slotCount <= m_workerSlots)
        {
            return m_workerSlots > 0;
        }
        ReleaseWorkerPools();
        const usize n = static_cast<usize>(m_framesInFlight) * slotCount;
        m_workerPools.Resize(n, nullptr);
        for (usize i = 0; i < n; ++i)
        {
            rhi::CommandPool* pool = nullptr;
            if (!m_device->CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk() ||
                pool == nullptr)
            {
                ReleaseWorkerPools();
                m_workerSlots = 0;
                return false;
            }
            m_workerPools[i] = pool;
        }
        m_workerSlots = slotCount;
        return true;
    }

    void ForwardPass::ReleaseWorkerPools()
    {
        for (usize i = 0; i < m_workerPools.Size(); ++i)
        {
            if (m_workerPools[i] != nullptr)
            {
                m_device->DestroyCommandPool(m_workerPools[i]);
            }
        }
        m_workerPools.Clear();
        m_workerSlots = 0;
    }
    Span<const RenderFrame::ViewShadowDebug> RenderFrame::ViewShadowInfo() const noexcept
    {
        return Span<const ViewShadowDebug>{m_viewShadowDebug.Data(), m_viewShadowDebug.Size()};
    }

    Span<const GpuLocalShadow> RenderFrame::LocalShadowEntries() const noexcept
    {
        return Span<const GpuLocalShadow>{m_localShadows.Data(), m_localShadows.Size()};
    }

    void RenderFrame::Begin(rhi::CommandEncoder& encoder, u32 frameIndex)
    {
        m_encoder = &encoder;
        m_frameIndex = frameIndex;
        m_views.Begin();
        // Motion vectors are only consumed by TAA + SSR-temporal; when neither is active, the forward skips
        // the per-instance prev-world lookup (a full-scene hashmap rebuild/frame at stress scale).
        m_pass.SetMotionNeeded(m_taaEnabled ||
                               (m_ssr != nullptr && m_ssrEnabled && m_ssrParams.temporal));
        m_pass.SetShadowFarFade(m_shadowFarFade);
        m_graph.BeginFrame(
            static_cast<i32>(frameIndex)); // one graph composes all this frame's views
    }

    RenderView* RenderFrame::AddView(const ExtractedScene& scene, const ViewCamera& camera,
                                     const ViewSettings& settings, rhi::TextureView* target,
                                     rhi::TextureFormat targetFormat, u32 width, u32 height,
                                     const void* debugScene, const void* sceneKey)
    {
        RenderView* view = m_views.Acquire();
        view->Bind(scene, camera, settings, target, targetFormat, width, height);
        view->SetDebugScene(debugScene);
        view->SetSceneKey(sceneKey);
        view->BuildDrawList(m_sortScratch, m_viewCulling);
        return view;
    }

    void RenderFrame::SetDebug(DebugDrawPass* pass, const debug::DebugDraw* global,
                               const debug::DebugDraw* screen) noexcept
    {
        m_debugPass = pass;
        m_debugGlobal = global;
        m_debugScreen = screen;
    }

    void RenderFrame::SetSceneOverlays(const Array<ISceneOverlay*>* overlays) noexcept
    {
        m_sceneOverlays = overlays;
    }

    void RenderFrame::SetSsrParams(bool enabled, const SsrPass::Params& params) noexcept
    {
        m_ssrEnabled = enabled;
        m_ssrParams = params;
    }

    void RenderFrame::SetBloom(f32 intensity, f32 threshold, f32 knee) noexcept
    {
        m_bloomIntensity = intensity;
        m_bloomThreshold = threshold;
        m_bloomKnee = knee;
    }

    void RenderFrame::SetTaa(bool on, f32 blend, f32 gamma, f32 motionScale) noexcept
    {
        m_taaEnabled = on;
        m_taaBlend = blend;
        m_taaGamma = gamma;
        m_taaMotionScale = motionScale;
    }

    void RenderFrame::SetAo(AoMode mode, f32 strength, f32 radius, f32 intensity,
                            i32 debugMode) noexcept
    {
        m_aoMode = mode;
        m_aoStrength = strength;
        m_aoRadius = radius;
        m_aoIntensity = intensity;
        m_aoDebug = debugMode;
    }

    void RenderFrame::SetFxaa(bool on, f32 subpixelQuality) noexcept
    {
        m_fxaaEnabled = on;
        m_fxaaSubpixel = subpixelQuality;
    }

    void RenderFrame::CullStats(u32& culled, u32& total) const noexcept
    {
        culled = 0;
        total = 0;
        for (usize i = 0; i < m_views.ActiveCount(); ++i)
        {
            culled += m_views.At(i)->CulledCount();
            total += m_views.At(i)->SceneItemCount();
        }
    }

    void RenderFrame::SetShadowParams(f32 distance, f32 farFade) noexcept
    {
        m_shadowDistance = distance;
        m_shadowFarFade = farFade;
    }

    void RenderFrame::ReadGpuProfile(String& out)
    {
        if (auto* p = m_graph.GpuProfiler())
        {
            p->ReadResults(m_graph.LastProfiledPassCount(), out);
        }
        m_graph.AppendCpuPassReport(out);
    }

    void RenderFrame::RecordDepthPrepass(rhi::RenderPassEncoder& rp, const RenderView& view,
                                         const RendererRegistry& registry, u32 viewIndex)
    {
        RenderRecordContext ctx{};
        ctx.view =
            &view; // instance-share cache is keyed by view pointer (prepass fills, forward reuses)
        ctx.viewProj = view.Camera().ViewProjection();
        ctx.depthFormat = m_pass.DepthFormat();
        ctx.depthPrepass = true;
        ctx.frameIndex = m_frameIndex;
        ctx.viewIndex = viewIndex;
        // Build the FULL per-instance data here (once) and cache each opaque group's range so the forward
        // reuses it instead of re-filling (matches Sedulous's build-instance-offsets-once). Needs the same
        // prevWorld the forward would use, so mirror the motion-needed condition.
        ctx.fillInstanceCache =
            m_instanceSharing; // toggleable: off => forward re-fills (old double-build), for A/B
        ctx.needsMotion =
            view.Settings().post.needsMotion; // per-view (resolved: TAA || SSR-temporal)

        m_prepassResolved.Clear();
        const Span<const DrawItem> items = view.DrawList();
        usize i = 0;
        while (i < items.Size())
        {
            const RenderCategory cat = items[i].data->category;
            usize j = i + 1;
            while (j < items.Size() && items[j].data->category == cat)
            {
                ++j;
            }
            if (cat == RenderCategories::Opaque)
            {
                if (Renderer* r = registry.ById(items[i].data->rendererId))
                {
                    r->ResolveDepthOnly(ctx, Span<const DrawItem>{items.Data() + i, j - i},
                                        m_prepassResolved);
                }
            }
            i = j;
        }
        for (const ResolvedDraw& d : m_prepassResolved)
        {
            EmitDraw(rp, d);
        }
    }

    void RenderFrame::RecordShadowCasters(rhi::RenderPassEncoder& rp, Span<const DrawItem> casters,
                                          const RendererRegistry& registry,
                                          const Float4x4& lightViewProj, Float3 cullCenter,
                                          f32 cullRadius, bool frustumCull,
                                          Span<const Float4> cullBounds)
    {
        RenderRecordContext ctx{};
        ctx.viewProj = lightViewProj;
        ctx.depthFormat =
            (m_shadows != nullptr) ? m_shadows->Format() : rhi::TextureFormat::Depth32Float;
        ctx.frameIndex = m_frameIndex;
        ctx.viewIndex = 0;

        // Optional cull into a scratch list (keeps the category-run batching below intact).
        // - frustum cull: CSM cascades - reject casters whose world sphere doesn't intersect THIS
        //   cascade's ortho frustum. The cascade VP already extends toward the light (see
        //   ComputeCascades), so its frustum is the correct assignment volume - each caster lands in
        //   ~1 cascade instead of all 4 (was Sedulous's compiled-out !FRUSTUM_CULL_SHADOWS fallback).
        // - sphere cull (cullRadius > 0): local point/spot lights vs the light's reach.
        Span<const DrawItem> items = casters;
        if (frustumCull)
        {
            DRACONIC_PROFILE_SCOPE("shadow.cull"); // per-cascade frustum scan
            const BoundingFrustum frustum{lightViewProj};
            m_shadowCullScratch.Clear();
            // Prefer the compact bounds SoA (linear, cache-friendly) over chasing it.data->worldCenter.
            if (cullBounds.Size() == casters.Size())
            {
                for (usize k = 0; k < casters.Size(); ++k)
                {
                    const Float4 b = cullBounds[k];
                    const Float3 c{b.x, b.y, b.z};
                    // Inline sphere-vs-frustum with early-out: reject if the sphere is fully outside any
                    // plane (outward normals). Avoids BoundingSphere construction + the enum/switch
                    // Intersects() runs per plane - this is the hot path (~448k tests/frame at 112k casters).
                    bool inside = true;
                    for (i32 p = 0; p < BoundingFrustum::kPlaneCount; ++p)
                    {
                        if (Dot(frustum.planes[p].normal, c) + frustum.planes[p].d > b.w)
                        {
                            inside = false;
                            break;
                        }
                    }
                    if (inside)
                    {
                        m_shadowCullScratch.PushBack(casters[k]);
                    }
                }
            }
            else
            {
                for (const DrawItem& it : casters)
                {
                    const auto* md = static_cast<const MeshRenderData*>(it.data);
                    if (Intersects(frustum, BoundingSphere{md->worldCenter, md->worldRadius}))
                    {
                        m_shadowCullScratch.PushBack(it);
                    }
                }
            }
            items = Span<const DrawItem>{m_shadowCullScratch.Data(), m_shadowCullScratch.Size()};
        }
        else if (cullRadius > 0.0f)
        {
            m_shadowCullScratch.Clear();
            for (const DrawItem& it : casters)
            {
                const auto* md = static_cast<const MeshRenderData*>(it.data);
                const Float3 d = md->worldCenter - cullCenter;
                const f32 r = cullRadius + md->worldRadius;
                if (Dot(d, d) <= r * r)
                {
                    m_shadowCullScratch.PushBack(it);
                }
            }
            items = Span<const DrawItem>{m_shadowCullScratch.Data(), m_shadowCullScratch.Size()};
        }

        // Group by RENDERER (not category): the caster list is the view's draw list, whose blended span
        // now mixes transparent meshes (id 0) and sprites (id 1) interleaved by depth. Dispatching a
        // whole category run to the first item's renderer would hand sprite data to the mesh renderer
        // (read as MeshRenderData -> garbage/UAF). Same-renderer runs route correctly; sprites' depth-only
        // resolve is a no-op (they don't cast shadows).
        DRACONIC_PROFILE_SCOPE("shadow.resolve"); // per-survivor instance resolve + emit
        m_shadowResolved.Clear();
        usize i = 0;
        while (i < items.Size())
        {
            const u16 rid = items[i].data->rendererId;
            usize j = i + 1;
            while (j < items.Size() && items[j].data->rendererId == rid)
            {
                ++j;
            }
            if (Renderer* r = registry.ById(rid))
            {
                r->ResolveDepthOnly(ctx, Span<const DrawItem>{items.Data() + i, j - i},
                                    m_shadowResolved);
            }
            i = j;
        }
        for (const ResolvedDraw& d : m_shadowResolved)
        {
            EmitDraw(rp, d);
        }
    }

    void RenderFrame::BuildShadowCasterList(const ExtractedScene& scene, SceneShadowCtx& ctx)
    {
        ctx.casters.Clear();
        ctx.animatedSpheres.Clear();
        for (RenderData* data : scene.Items())
        {
            if (data == nullptr)
            {
                continue;
            }
            if (data->category != RenderCategories::Opaque &&
                data->category != RenderCategories::Masked)
            {
                continue;
            }
            const auto* md = static_cast<const MeshRenderData*>(data);
            // Animated (skinned) casters deform every frame: remember each one's world bounding sphere so
            // only the static atlas tiles whose light volume it overlaps get re-rendered (per-tile routing).
            if (md->boneMatrices != nullptr && md->boneCount > 0)
            {
                ctx.animatedSpheres.PushBack(Sphere{md->worldCenter, md->worldRadius});
            }
            const usize m = reinterpret_cast<usize>(md->mesh),
                        n = reinterpret_cast<usize>(md->material);
            const u32 stateBits = static_cast<u32>(
                (((m >> 4) * 1099511628211ull + (n >> 4)) & ((1u << kSortStateBits) - 1)));
            ctx.casters.PushBack(DrawItem{MakeSortKey(data->category, stateBits, 0u), data});
        }
        RadixSortDrawItems(ctx.casters, m_sortScratch);
        // Compact bounds SoA (xyz = worldCenter, w = worldRadius) aligned to the SORTED caster order, so the
        // per-cascade frustum cull streams 16B/item linearly (4 per cache line) instead of chasing
        // it.data->worldCenter into scattered ~200B MeshRenderData - the dominant shadow-record cost at scale.
        ctx.casterBounds.Clear();
        ctx.casterBounds.Reserve(ctx.casters.Size());
        for (const DrawItem& it : ctx.casters)
        {
            const auto* md = static_cast<const MeshRenderData*>(it.data);
            ctx.casterBounds.PushBack(
                Float4{md->worldCenter.x, md->worldCenter.y, md->worldCenter.z, md->worldRadius});
        }
    }

    u64 RenderFrame::StaticCasterSignature(const ExtractedScene* scene) const
    {
        if (scene == nullptr)
        {
            return 0;
        }
        u64 sig = 1469598103934665603ull; // FNV-1a offset basis
        const auto mix = [&sig](f32 v)
        {
            const u64 q =
                static_cast<u64>(static_cast<i64>(v * 1000.0f)); // ~1mm / 0.001 quantization
            sig = (sig ^ q) * 1099511628211ull;
        };
        for (const LocalShadowCaster& c : scene->LocalShadowCasters())
        {
            if (!c.isStatic)
            {
                continue;
            }
            mix(static_cast<f32>(c.type));
            mix(c.positionWS.x);
            mix(c.positionWS.y);
            mix(c.positionWS.z);
            mix(c.directionWS.x);
            mix(c.directionWS.y);
            mix(c.directionWS.z);
            mix(c.range);
            mix(c.outerAngle);
        }
        return sig;
    }

    void RenderFrame::End()
    {
        if (m_encoder == nullptr)
        {
            return;
        }

        u32 totalDraws = 0;
        for (usize i = 0; i < m_views.ActiveCount(); ++i)
        {
            totalDraws += static_cast<u32>(m_views.At(i)->DrawList().Size());
        }

        // ---- Per-scene shadow composition -------------------------------------------------------
        // Views can show DIFFERENT scenes in one frame (editor pages side-by-side; the Sandbox's
        // multi-view-of-one-scene shape never exercised this). Every shadow input - caster list,
        // light direction, atlas tiles, static-cache signature - is sourced from the VIEW'S OWN
        // scene via these contexts; the old primary-scene sourcing bled scene A's shadows into
        // scene B and never rendered B's own.
        const u32 viewCount = static_cast<u32>(m_views.ActiveCount());

        // Distinct scenes in order of first appearance + each view's index into them. Contexts are
        // pooled UniquePtrs: stable addresses (graph-execute lambdas capture them), reused arrays.
        m_viewSceneIndex.Resize(m_views.ActiveCount());
        usize sceneCount = 0;
        for (usize i = 0; i < m_views.ActiveCount(); ++i)
        {
            const ExtractedScene* scene = m_views.At(i)->Scene();
            m_viewSceneIndex[i] = static_cast<u32>(~0u);
            if (scene == nullptr)
            {
                continue;
            }
            usize slot = sceneCount;
            for (usize k = 0; k < sceneCount; ++k)
            {
                if (m_sceneShadowPool[k]->scene == scene)
                {
                    slot = k;
                    break;
                }
            }
            if (slot == sceneCount)
            {
                if (m_sceneShadowPool.Size() <= slot)
                {
                    m_sceneShadowPool.PushBack(MakeUnique<SceneShadowCtx>(DefaultAllocator()));
                }
                SceneShadowCtx& ctx = *m_sceneShadowPool[slot];
                ctx.scene = scene;
                ctx.casters.Clear();
                ctx.casterBounds.Clear();
                ctx.animatedSpheres.Clear();
                ctx.staticTiles.Clear();
                ctx.staticRenderTiles.Clear();
                ctx.entryBase = 0;
                ctx.ibl = nullptr;
                ++sceneCount;
            }
            m_viewSceneIndex[i] = static_cast<u32>(slot);
        }
        // Stale pool slots must not alias a fresh frame's scene set.
        for (usize k = sceneCount; k < m_sceneShadowPool.Size(); ++k)
        {
            m_sceneShadowPool[k]->scene = nullptr;
        }

        // One shadow ARRAY shared by all views, sized for per-view cascades (viewCount * cascades
        // layers). Each view fits + renders its OWN cascades into its layer range, and samples them.
        bool anyDirectional = false, anyLocalCasters = false;
        for (usize k = 0; k < sceneCount; ++k)
        {
            const SceneShadowCtx& ctx = *m_sceneShadowPool[k];
            anyDirectional = anyDirectional || ctx.scene->DirectionalShadowData().valid;
            anyLocalCasters = anyLocalCasters || !ctx.scene->LocalShadowCasters().IsEmpty();
        }
        const bool hasShadow = m_shadows != nullptr && anyDirectional;
        rhi::TextureView* shadowMap =
            hasShadow ? m_shadows->PrepareFrame(m_frameIndex, viewCount) : nullptr;
        const u64 shadowGen = (m_shadows != nullptr) ? m_shadows->Generation() : 0;

        // Camera-INDEPENDENT caster lists (opaque+masked, off-camera casters included), one per scene.
        // Shared by BOTH that scene's directional cascades and its local-light atlas tiles: sourcing
        // shadows from these - not the per-view camera draw lists - means view-frustum culling the
        // camera never drops a shadow caster (Sedulous: transparent + sprites don't cast).
        for (usize k = 0; k < sceneCount; ++k)
        {
            SceneShadowCtx& ctx = *m_sceneShadowPool[k];
            if (ctx.scene->DirectionalShadowData().valid ||
                !ctx.scene->LocalShadowCasters().IsEmpty())
            {
                BuildShadowCasterList(*ctx.scene, ctx);
            }
        }

        // Local-light (spot/point) shadows (5.3): build the per-caster perspective matrices + atlas
        // tiles up front. ONE physical atlas; its per-layer tile space is handed out across scenes by
        // GLOBAL counters, and the flat GpuLocalShadow buffer is the scenes' entries CONCATENATED -
        // each view offsets its scene-relative shadowIndex by its scene's entryBase (ShadowBinding).
        // Doing it here lets the renderers size their per-object rings (SetShadowAtlas passCount) and
        // upload the data before the forward.
        m_localShadows.Clear();
        m_rtAtlasDraws.Clear();
        m_staticAtlasDraws.Clear();
        rhi::TextureView* atlasView = nullptr;
        u32 staticTileCount = 0;
        if (m_shadows != nullptr && anyLocalCasters)
        {
            atlasView = m_shadows->PrepareAtlas(m_frameIndex);
        }
        if (atlasView != nullptr)
        {
            const u32 capacity =
                m_shadows->AtlasTileCapacity(); // per layer, WHOLE frame (all scenes)
            const u32 atlasRes = m_shadows->AtlasResolution();
            const u32 tileRes = m_shadows->AtlasTileResolution();
            const u32 fif = m_shadows->FramesInFlight();
            u32 rtTile = 0, stTile = 0; // global per-layer tile counters, running across scenes
            for (usize k = 0; k < sceneCount; ++k)
            {
                SceneShadowCtx& ctx = *m_sceneShadowPool[k];
                ctx.entryBase = static_cast<u32>(m_localShadows.Size());
                const u32 sceneStaticTileBase = stTile;
                for (const LocalShadowCaster& c : ctx.scene->LocalShadowCasters())
                {
                    const u32 need = (c.type == 1u /*point*/) ? 6u : 1u;
                    u32& tileCtr = c.isStatic ? stTile : rtTile;
                    // Extraction assigned this caster a shadowIndex (scene-relative, per-scene caps);
                    // whether it fits HERE depends on the whole frame's tile budget. A caster that
                    // doesn't fit still consumes its entry slots - as DEGENERATE entries the shader
                    // treats as unshadowed - so every later caster's shadowIndex stays aligned.
                    const bool fits = tileCtr + need <= capacity &&
                                      m_localShadows.Size() + need <= kMaxLocalShadowEntries;
                    if (!fits)
                    {
                        for (u32 f = 0; f < need && m_localShadows.Size() < kMaxLocalShadowEntries;
                             ++f)
                        {
                            GpuLocalShadow dead;
                            dead.atlasScaleBias = Float4{0, 0, 0, 0}; // shader guard -> unshadowed
                            m_localShadows.PushBack(dead);
                        }
                        continue;
                    }
                    Array<AtlasDraw>& dst = c.isStatic ? m_staticAtlasDraws : m_rtAtlasDraws;
                    for (u32 f = 0; f < need; ++f)
                    {
                        const u32 ti = tileCtr + f; // tile index WITHIN the layer
                        GpuLocalShadow entry =
                            (need == 6u) ? BuildPointShadowFace(c, f, ti, atlasRes, tileRes)
                                         : BuildSpotShadow(c, ti, atlasRes, tileRes);
                        entry.atlasSelect = c.isStatic ? 1.0f : 0.0f; // sampled atlas array layer
                        const AtlasTile t = AtlasTileRect(ti, atlasRes, tileRes);
                        // Cull casters to the light's bounding sphere (point/spot share pos + range).
                        dst.PushBack(AtlasDraw{LocalShadowTile{entry.viewProj, t.x, t.y, t.w, t.h,
                                                               c.positionWS, Max(0.1f, c.range)},
                                               &ctx});
                        m_localShadows.PushBack(entry);
                    }
                    tileCtr += need;
                }

                // Static atlas layer, PER SCENE: each tile is cached and re-rendered only when needed,
                // tracked by a per-tile dirty COUNTDOWN (a tile re-renders for FramesInFlight frames to
                // refresh every in-flight slot's copy). Dirtied by: the scene's static caster set
                // changing (signature), the scene landing on a different pool slot / tile range (atlas
                // layout shifted under it), or an animated caster's sphere overlapping the tile.
                ctx.staticTiles.Clear();
                for (const AtlasDraw& d : m_staticAtlasDraws)
                {
                    if (d.ctx == &ctx)
                    {
                        ctx.staticTiles.PushBack(d.tile);
                    }
                }
                ctx.staticTileDirty.Resize(ctx.staticTiles.Size());
                const u64 sig = StaticCasterSignature(ctx.scene);
                if (sig != ctx.staticSig || ctx.scene != ctx.lastStaticScene ||
                    sceneStaticTileBase != ctx.staticTileBase)
                {
                    ctx.staticSig = sig;
                    ctx.lastStaticScene = ctx.scene;
                    ctx.staticTileBase = sceneStaticTileBase;
                    for (u32& d : ctx.staticTileDirty)
                    {
                        d = fif;
                    }
                }
                for (usize ti = 0; ti < ctx.staticTiles.Size(); ++ti)
                {
                    const LocalShadowTile& t = ctx.staticTiles[ti];
                    for (const Sphere& sp : ctx.animatedSpheres)
                    {
                        if (Length(sp.center - t.cullCenter) <= t.cullRadius + sp.radius)
                        {
                            ctx.staticTileDirty[ti] = fif;
                            break;
                        }
                    }
                }
                ctx.staticRenderTiles.Clear();
                for (usize ti = 0; ti < ctx.staticTiles.Size(); ++ti)
                {
                    if (ctx.staticTileDirty[ti] > 0)
                    {
                        ctx.staticRenderTiles.PushBack(ctx.staticTiles[ti]);
                        --ctx.staticTileDirty[ti];
                    }
                }
                staticTileCount += static_cast<u32>(ctx.staticTiles.Size());
            }
        }
        const u64 atlasGen = (m_shadows != nullptr) ? m_shadows->Generation() : 0;

        // This frame's static tiles to render, across scenes (each knows its scene's casters).
        m_staticRenderDraws.Clear();
        for (usize k = 0; k < sceneCount; ++k)
        {
            SceneShadowCtx& ctx = *m_sceneShadowPool[k];
            for (const LocalShadowTile& t : ctx.staticRenderTiles)
            {
                m_staticRenderDraws.PushBack(AtlasDraw{t, &ctx});
            }
        }
        const bool renderStatic = !m_staticRenderDraws.IsEmpty();
        // Per-renderer ring sizing: count only the atlas passes that actually re-emit casters this frame.
        const u32 localPassCount =
            static_cast<u32>(m_rtAtlasDraws.Size()) + static_cast<u32>(m_staticRenderDraws.Size());

        {
            DRACONIC_PROFILE_SCOPE("Compose.Prepare"); // per-frame GPU buffer sizing + pool resets
            for (Renderer* r : m_registry->Unique())
            {
                r->SetShadowMap(shadowMap, shadowGen);
            }
            for (Renderer* r : m_registry->Unique())
            {
                r->SetShadowAtlas(atlasView, atlasGen, localPassCount);
            }
            // Probe capture re-emits the draws once per face (P1b caps at 1 probe/frame = 6 forward passes);
            // count them into the per-object rings or the extra passes would starve the forward (silent drops).
            const u32 captureFaces =
                (m_probeSystem != nullptr && !m_probeSystem->Captures().IsEmpty()) ? 6u : 0u;
            for (Renderer* r : m_registry->Unique())
            {
                r->SetCaptureFacePasses(captureFaces);
            }
            // Reflection probes (P4, multi-probe): upload this frame's records + bind the prefiltered cube-
            // array (t8) + the probe-metadata SRV (t9) + count. The forward loops + blends them. 0 -> no probe.
            if (m_probeSystem != nullptr && m_probeSystem->ActiveCount() > 0)
            {
                m_probeSystem->Upload();
                for (Renderer* r : m_registry->Unique())
                {
                    r->SetProbes(m_probeSystem->PrefilterArrayView(), m_probeSystem->ProbeBuffer(),
                                 m_probeSystem->ActiveCount());
                }
            }
            else
            {
                for (Renderer* r : m_registry->Unique())
                {
                    r->SetProbes(nullptr, nullptr, 0);
                }
            }
            if (m_ibl != nullptr && m_ibl->Ready())
            {
                m_ibl->Upload(
                    *m_encoder); // pending equirect/cubemap uploads, before the graph executes
            }
            // (IBL products are PER SCENE now - each view's binding rides ResolveAndEmit's
            // RenderRecordContext instead of a frame-global renderer setting.)
            for (Renderer* r : m_registry->Unique())
            {
                r->PrepareFrame(totalDraws, m_frameIndex);
            }
            for (Renderer* r : m_registry->Unique())
            {
                r->UploadLocalShadows(
                    Span<const GpuLocalShadow>{m_localShadows.Data(), m_localShadows.Size()},
                    m_frameIndex);
            }
            // Skinning bone upload: write each distinct skeleton instance's matrices ONCE into the bone
            // pool + copy staging->device, before any pass reads them. Once per DISTINCT scene (a scene's
            // instances cover every view of it; multi-scene frames upload each scene's skeletons).
            if (m_encoder != nullptr)
            {
                for (usize k = 0; k < sceneCount; ++k)
                {
                    for (Renderer* r : m_registry->Unique())
                    {
                        r->UploadSkinning(*m_sceneShadowPool[k]->scene, *m_encoder);
                    }
                }
            }
            if (m_clusters != nullptr)
            {
                m_clusters->PrepareFrame(m_frameIndex);
            } // size the cluster build's per-frame buffers
            m_pass.BeginFrame(m_frameIndex); // reset per-worker pools once (before any view)
        }

        // Declare every view's forward pass into the one frame graph, then let the graph compile
        // (barriers + transient depth allocation/aliasing) + execute. (§9: one graph, all views.)
        {
            DRACONIC_PROFILE_SCOPE(
                "Compose.Declare"); // build the frame graph (pass/resource declarations)
            if (m_views.ActiveCount() > 0)
            {
                m_graph.SetOutputSize(m_views.At(0)->Width(), m_views.At(0)->Height());
            }

            // Per-view CSM: import the shared cascade array once; each view fits its own cascades to its
            // camera and renders them into its layer range (so split-screen views don't share a fit).
            rendergraph::RGHandle shadowH;
            const bool shadowActive = hasShadow && shadowMap != nullptr;
            const u32 cascadeCount = (m_shadows != nullptr) ? m_shadows->CascadeCount() : 4u;
            const u32 shadowRes = (m_shadows != nullptr) ? m_shadows->Resolution() : 1024u;

            // IBL precompute, PER SCENE: each scene's authored sky builds into ITS context's persistent
            // products (env/SH/prefilter; BRDF LUT shared) when dirty; each view samples its own scene's
            // set below. The procedural sky tracks the scene's key light (sun disc + ambient match).
            if (m_ibl != nullptr && m_ibl->Ready())
            {
                m_ibl->BeginFrame(m_graph);
                for (usize k = 0; k < sceneCount; ++k)
                {
                    SceneShadowCtx& sctx = *m_sceneShadowPool[k];
                    const DirectionalShadow& ds = sctx.scene->DirectionalShadowData();
                    const Float3 sceneSun = ds.valid ? ds.direction : Float3{0.0f, -1.0f, 0.0f};
                    sctx.ibl = m_ibl->Prepare(sctx.scene, sctx.scene->Sky(), sceneSun, m_graph);
                }
            }

            // Local-light shadow atlas (5.3/5.4): a 2-layer array. Layer 0 (realtime) re-renders every
            // frame; layer 1 (static) only when the static set changed (renderStatic). Each pass targets
            // its layer (subresource), clears it, and renders its tiles (per-tile viewport+scissor). Every
            // forward pass ReadTextures the array, ordering both passes ahead + barriering it readable.
            rendergraph::RGHandle atlasH;
            const bool atlasActive =
                atlasView != nullptr && (!m_rtAtlasDraws.IsEmpty() || staticTileCount > 0);
            if (atlasActive)
            {
                atlasH = m_shadows->ImportAtlas(m_graph, m_frameIndex);
                RendererRegistry* reg = m_registry;
                const u32 atlasRes = m_shadows->AtlasResolution();
                // Declare one layer's depth pass over a draw list (tiles across ALL scenes; each draw
                // records ITS scene's casters). (Lambda-per-pass; the graph runs them at execute time,
                // ordered before the forward by its ReadTexture of atlasH.)
                const auto declareLayer = [&](u32 layer, Array<AtlasDraw>* draws)
                {
                    if (draws->IsEmpty())
                    {
                        return;
                    }
                    m_graph.AddRenderPass(
                        u8"shadow.atlas",
                        [this, atlasH, reg, draws, atlasRes, layer](rendergraph::PassBuilder& b)
                        {
                            rendergraph::RGSubresourceRange sub{};
                            sub.baseArrayLayer = layer;
                            sub.arrayLayerCount = 1;
                            b.SetDepthTarget(atlasH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                             /*clearDepth*/ 1.0f, sub);
                            b.SetViewport(0, 0, atlasRes,
                                          atlasRes); // pass default; each tile sets its own below
                            b.SetExecute(
                                [this, reg, draws](rhi::RenderPassEncoder& rp)
                                {
                                    for (const AtlasDraw& d : *draws)
                                    {
                                        const LocalShadowTile& t = d.tile;
                                        const Span<const DrawItem> casters{d.ctx->casters.Data(),
                                                                           d.ctx->casters.Size()};
                                        rp.SetViewport(static_cast<f32>(t.x), static_cast<f32>(t.y),
                                                       static_cast<f32>(t.w),
                                                       static_cast<f32>(t.h));
                                        rp.SetScissor(static_cast<i32>(t.x), static_cast<i32>(t.y),
                                                      t.w, t.h);
                                        RecordShadowCasters(rp, casters, *reg, t.viewProj,
                                                            t.cullCenter, t.cullRadius);
                                    }
                                });
                        });
                };
                declareLayer(0u, &m_rtAtlasDraws); // realtime layer - every frame
                if (renderStatic)
                {
                    declareLayer(1u, &m_staticRenderDraws);
                } // static layer - only dirty tiles
            }
            if (shadowActive)
            {
                shadowH = m_shadows->ImportTarget(m_graph, m_frameIndex);
            }

            // Declare EVERY view's cascade depth passes up front - before any forward pass. The cascade
            // array is one imported resource: if a forward pass sampling the whole array were declared
            // before a later view's cascade writes, those layers would still be in DepthStencilAttachment
            // (not Read) when sampled (VUID-vkCmdExecuteCommands depth-layout). Fitting all cascades first
            // means every layer is written + barriered to Read before the first forward sample.
            Array<ShadowBinding> viewShadows;
            viewShadows.Resize(m_views.ActiveCount());
            m_viewShadowDebug.Clear();
            m_viewShadowDebug.Resize(m_views.ActiveCount());
            for (usize i = 0; i < m_views.ActiveCount(); ++i)
            {
                // Every view (shadowed or not) carries its scene's local-shadow entry base - its lights'
                // scene-relative shadowIndex values offset into the frame's concatenated entry buffer.
                SceneShadowCtx* sctx = (m_viewSceneIndex[i] != ~0u)
                                           ? m_sceneShadowPool[m_viewSceneIndex[i]].Get()
                                           : nullptr;
                viewShadows[i].localShadowEntryBase = (sctx != nullptr) ? sctx->entryBase : 0;
                m_viewShadowDebug[i].localEntryBase = viewShadows[i].localShadowEntryBase;

                if (!shadowActive)
                {
                    continue;
                }
                // The map exists this frame: EVERY view (caster-less scenes + views beyond the
                // cascade budget included) must bind + DECLARE it - see ShadowBinding::MapBound.
                viewShadows[i].sampleView = shadowMap;
                viewShadows[i].handle = shadowH;
                m_viewShadowDebug[i].mapBound = true;
                if (i >= ShadowSystem::kMaxShadowViews)
                {
                    continue;
                }
                // Per-view directional: THIS view's scene must have a caster (another scene's key light
                // must never shadow - or light-leak into - this one).
                if (sctx == nullptr || !sctx->scene->DirectionalShadowData().valid)
                {
                    continue;
                }
                RenderView* v = m_views.At(i);
                const Float3 viewLightDir = sctx->scene->DirectionalShadowData().direction;
                // Shadow distance: how far directional shadows reach. Per-cascade frustum culling keeps
                // this affordable (casters only touch the one cascade they fall in), and SampleCSM
                // far-fades over the last cascade so the boundary dissolves instead of popping. Larger
                // reach covers more ground but spreads cascade texel density (softer near shadows).
                const f32 shadowDistance = Min(v->Camera().farZ, m_shadowDistance);
                const ShadowCascades cascades =
                    ComputeCascades(v->Camera(), viewLightDir, shadowDistance, shadowRes);
                const u32 layerBase = static_cast<u32>(i) * cascadeCount;
                RendererRegistry* reg = m_registry;
                for (u32 c = 0; c < cascadeCount; ++c)
                {
                    const Float4x4 cascadeVP = cascades.viewProj[c];
                    const u32 layer = layerBase + c;
                    // Casters come from the view's SCENE's camera-independent list (built above), not this
                    // view's culled draw list - so view-frustum culling can't drop an off-camera caster
                    // whose shadow is visible. Per-cascade frustum cull then keeps each caster to ~1 cascade.
                    m_graph.AddRenderPass(
                        u8"shadow.cascade",
                        [this, shadowH, cascadeVP, shadowRes, layer, reg,
                         sctx](rendergraph::PassBuilder& b)
                        {
                            rendergraph::RGSubresourceRange sub{};
                            sub.baseArrayLayer = layer;
                            sub.arrayLayerCount = 1;
                            b.SetDepthTarget(shadowH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                             /*clearDepth*/ 1.0f, sub);
                            b.SetViewport(0, 0, shadowRes, shadowRes);
                            b.SetExecute(
                                [this, cascadeVP, reg, sctx](rhi::RenderPassEncoder& rp)
                                {
                                    const Span<const DrawItem> casters{sctx->casters.Data(),
                                                                       sctx->casters.Size()};
                                    const Span<const Float4> bounds{sctx->casterBounds.Data(),
                                                                    sctx->casterBounds.Size()};
                                    RecordShadowCasters(rp, casters, *reg, cascadeVP, {}, 0.0f,
                                                        /*frustumCull*/ true, bounds);
                                });
                        });
                }
                ShadowBinding& sb = viewShadows[i];
                sb.cascades = cascades;
                sb.layerBase = layerBase;
                sb.valid = true;
                m_viewShadowDebug[i].directional = true;
            }

            // Reflection probe (P2): init the captured-cube layout once (so uncaptured slices are ShaderRead, not
            // UNDEFINED, under the whole-array SRV) + import it ONCE so both the capture passes AND the main
            // forward's ReadTexture share one imported resource (which orders capture->forward + barriers it).
            rendergraph::RGHandle probeCapturedH, probePrefilteredH;
            bool probeActive = false;
            if (m_probeSystem != nullptr && m_probeSystem->ActiveCount() > 0 &&
                m_encoder != nullptr)
            {
                m_probeSystem->InitLayouts(*m_encoder);
                probeCapturedH = m_probeSystem->ImportCaptured(m_graph);
                probePrefilteredH = m_probeSystem->ImportPrefiltered(
                    m_graph); // the SEPARATE texture the forward samples
                probeActive = true;
            }

            // ---- Reflection probe capture (P1b) --------------------------------------------------------
            // Render each dirty probe's 6 faces (lit forward + sky, HDR) into its captured-cube slices, BEFORE
            // the main views (which sample the result). Feedback-safe: the capture forward samples its
            // scene's IBL only, never the probe array. Capped at one probe/frame for P1b.
            if (probeActive && m_ibl != nullptr && m_ibl->Ready() &&
                !m_probeSystem->Captures().IsEmpty())
            {
                const rendergraph::RGHandle capturedH = probeCapturedH;
                const u32 res = ReflectionProbeSystem::kCaptureRes;
                const f32 nearZ = ReflectionProbeSystem::kCaptureNear;
                const f32 farZ = ReflectionProbeSystem::kCaptureFar;
                // Round-robin: capture ONE dirty probe per frame, cycling through them. Keeps the capture to 6
                // faces/frame (fits the sky-uniform slots 2..7) and amortizes multi-probe / Realtime re-capture.
                const Span<const ReflectionProbeSystem::CaptureTask> captures =
                    m_probeSystem->Captures();
                const ReflectionProbeSystem::CaptureTask task =
                    captures[m_probeCaptureCursor % captures.Size()];
                ++m_probeCaptureCursor;
                // The capture renders the probe's OWNING scene (a probe in scene B must never bake
                // scene A's geometry) and lights with ITS scene's IBL context - never another sky's.
                const ExtractedScene* captureScene = task.scene;
                IBLSystem::Context* capCtx = nullptr;
                for (usize k = 0; k < sceneCount; ++k)
                {
                    if (m_sceneShadowPool[k]->scene == captureScene)
                    {
                        capCtx = m_sceneShadowPool[k]->ibl;
                        break;
                    }
                }
                if (captureScene != nullptr && capCtx != nullptr)
                {
                    IblBinding capIbl;
                    capIbl.prefilterHandle = capCtx->PrefilterHandle();
                    capIbl.brdfHandle = m_ibl->BrdfHandle();
                    capIbl.shHandle = capCtx->ShHandle();
                    capIbl.shBuffer = capCtx->ShBuffer();
                    capIbl.prefilterView = capCtx->PrefilterView();
                    capIbl.brdfView = m_ibl->BrdfView();
                    capIbl.maxLod = m_ibl->MaxLod();
                    capIbl.generation = capCtx->Generation();
                    capIbl.valid = true;
                    // Reuse the primary view's CSM binding (handle + cascades) + the local atlas. The capture
                    // forward's shader statically samples both depth textures, so the binding MUST be valid or the
                    // graph won't barrier them to DEPTH_STENCIL_READ_ONLY (they'd still be in ATTACHMENT layout).
                    // The cascades are geometrically the primary camera's (approximate for a face) - fine for P1b.
                    ShadowBinding capShadow =
                        viewShadows.IsEmpty() ? ShadowBinding{} : viewShadows[0];
                    capShadow.atlasHandle = atlasH;
                    capShadow.atlasValid = atlasActive;
                    const u32 layerBase = ReflectionProbeSystem::LayerBase(task.slot);
                    const f32 sunInt = capCtx->HasSunDisc() ? capCtx->SunIntensity() : 0.0f;
                    for (u32 face = 0; face < 6; ++face)
                    {
                        const ViewCamera fc = ProbeFaceCamera(task.center, face, nearZ, farZ);
                        RenderView& cv = m_captureViews[face];
                        cv.Bind(*captureScene, fc, ViewSettings{}, nullptr,
                                ReflectionProbeSystem::kCubeFormat, res, res);
                        cv.BuildDrawList(m_sortScratch);

                        const rendergraph::RGHandle capDepth = m_graph.CreateTransient(
                            u8"probe.depth",
                            rendergraph::RGTextureDesc(m_pass.DepthFormat(), res, res));
                        const rendergraph::RGHandle capNormal = m_graph.CreateTransient(
                            u8"probe.normal", rendergraph::RGTextureDesc(kGNormalFormat, res, res));
                        const rendergraph::RGHandle capVel = m_graph.CreateTransient(
                            u8"probe.velocity",
                            rendergraph::RGTextureDesc(kGVelocityFormat, res, res));
                        const rendergraph::RGHandle capMaterial = m_graph.CreateTransient(
                            u8"probe.material",
                            rendergraph::RGTextureDesc(kGMaterialFormat, res, res));

                        rendergraph::RGSubresourceRange sub{};
                        sub.baseArrayLayer = layerBase + face;
                        sub.arrayLayerCount = 1;

                        const Float4x4 faceVP = fc.ViewProjection();
                        // Lit forward: cluster {} -> dummy cluster -> all-lights fallback (no per-face build);
                        // clear depth (no prepass); write only color slot 0 into this cube face.
                        m_pass.DeclarePass(
                            cv, *m_registry, m_graph, m_frameIndex,
                            /*viewIndex*/ kCaptureViewIndexBase + face, capturedH, capDepth,
                            /*clearColor*/ true, ReflectionProbeSystem::kCubeFormat, capNormal,
                            capVel, capMaterial, faceVP, Float2{0, 0}, Float2{0, 0},
                            ClusterBinding{}, capShadow, capIbl, rhi::LoadOp::Clear, sub);
                        // Sky into the same face, after the forward (loads the captured depth).
                        // Distinct sky uniform slot per capture face (2..7), so the capture never shares SkyPass's
                        // per-view slot with a main view (0,1) or with another face - otherwise the last recorder
                        // wins the shared slot and the captured sky reads a main view's camera (cross-view leak).
                        m_sky->DeclareSky(m_graph, capturedH, capVel, capDepth, capCtx->EnvHandle(),
                                          capCtx->EnvView(), ReflectionProbeSystem::kCubeFormat,
                                          m_pass.DepthFormat(), Inverse(faceVP), faceVP,
                                          Float2{0, 0}, Float2{0, 0}, task.center,
                                          capCtx->SkyBackgroundIntensity(), capCtx->SunDir(),
                                          capCtx->SunAngularSize(), Float3{1.0f, 0.98f, 0.92f},
                                          sunInt, 0, 0, res, res, m_frameIndex,
                                          /*viewIndex*/ 10u + face, capCtx->Uid(), sub);
                    }
                    // Bridge captured -> prefiltered mip 0 (flip blit - corrects the RH-LookAt mirror) so the forward
                    // samples a SEPARATE texture, never the captured cube it just wrote; then GGX-convolve mip 0 into
                    // the rougher mips (roughness reflections).
                    m_probeSystem->DeclareBlit(m_graph, capturedH, probePrefilteredH, task.slot);
                    m_probeSystem->DeclarePrefilter(m_graph, probePrefilteredH, task.slot);
                    m_probeSystem->MarkCaptured(task.slot);
                } // captureScene + capCtx valid
            }

            // Import each distinct target ONCE (so the graph orders/barriers all views writing it as one
            // resource). The first view to a target clears it; later views into the same target Load,
            // preserving earlier views' regions (split-screen). Targets are few - a linear scan is fine.
            struct TargetImport
            {
                rhi::TextureView* target;
                rendergraph::RGHandle handle;
                u32 w;
                u32 h;
                rhi::TextureFormat fmt;
            };
            Array<TargetImport> imported;
            // Bracket the decal ring ONCE for the whole per-view loop (each view accumulates its own slots).
            if (m_decalPass != nullptr)
            {
                m_decalPass->BeginFrame(m_frameIndex);
            }
            for (usize i = 0; i < m_views.ActiveCount(); ++i)
            {
                RenderView* v = m_views.At(i);
                rhi::TextureView* tgt = v->Target();
                if (tgt == nullptr)
                {
                    continue;
                }

                rendergraph::RGHandle colorH;
                bool found = false;
                for (const TargetImport& ti : imported)
                {
                    if (ti.target == tgt)
                    {
                        colorH = ti.handle;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    // Backbuffer (targetTexture null): current==final==RenderTarget - the host did
                    // Undefined->RenderTarget and will Present, so the graph touches no barrier. Offscreen
                    // (targetTexture set): the graph barriers it current -> RenderTarget -> final (e.g.
                    // ShaderRead/CopySrc so the caller can sample/blit the result).
                    const ViewSettings& s = v->Settings();
                    colorH = m_graph.ImportTarget(u8"forward.color", s.targetTexture, tgt,
                                                  s.targetFinalState, s.targetCurrentState);
                    imported.PushBack(
                        TargetImport{tgt, colorH, v->Width(), v->Height(), v->TargetFormat()});
                }
                const bool clearColor =
                    !found; // first view to a target clears it; later views Load

                // Cluster build (compute) declared before the forward pass so the graph orders the
                // light-binning write ahead of the shading read. viewIndex isolates per-view buffers.
                const u32 viewIndex = static_cast<u32>(i);
                ClusterBinding cluster;
                if (m_clusters != nullptr)
                {
                    cluster = m_clusters->DeclareBuild(m_graph, *v, m_frameIndex, viewIndex);
                }

                // This view's CSM cascades (fit + declared up front, above). Cascades stay on the view's
                // camera-culled draw list - they're already camera-coupled (refit per frame).
                ShadowBinding shadow = viewShadows[i];
                // The local-light atlas is scene-global (one pass for all views) - every view depends on it.
                shadow.atlasHandle = atlasH;
                shadow.atlasValid = atlasActive;

                // This view's scene context (shadow/probe/IBL composition all key off it).
                SceneShadowCtx* viewScene = (m_viewSceneIndex[i] != ~0u)
                                                ? m_sceneShadowPool[m_viewSceneIndex[i]].Get()
                                                : nullptr;

                // This view's scene's probe-record range (the records buffer is the scenes' ranges
                // concatenated in extraction order; the shader offsets its probe loop by the base).
                ReflectionProbeSystem::ProbeRange probeRange;
                if (m_probeSystem != nullptr && probeActive)
                {
                    probeRange = m_probeSystem->RangeFor(v->Scene());
                }

                // This view's SCENE's IBL products for the forward to sample + barrier-order this
                // frame (per-scene contexts; the BRDF LUT is the shared piece).
                IblBinding ibl;
                IBLSystem::Context* viewIblCtx = (viewScene != nullptr) ? viewScene->ibl : nullptr;
                if (viewIblCtx != nullptr)
                {
                    ibl.prefilterHandle = viewIblCtx->PrefilterHandle();
                    ibl.brdfHandle = m_ibl->BrdfHandle();
                    ibl.shHandle = viewIblCtx->ShHandle();
                    ibl.shBuffer = viewIblCtx->ShBuffer();
                    ibl.prefilterView = viewIblCtx->PrefilterView();
                    ibl.brdfView = m_ibl->BrdfView();
                    ibl.maxLod = m_ibl->MaxLod();
                    ibl.generation = viewIblCtx->Generation();
                    ibl.valid = true;
                }

                // Per-view depth, shared by the forward pass + the sky pass (sky depth-tests against it).
                const rendergraph::RGHandle depth = m_graph.CreateTransient(
                    u8"forward.depth",
                    rendergraph::RGTextureDesc(m_pass.DepthFormat(), v->Width(), v->Height()));
                // Per-view MRT G-buffer aux targets (view-space normal + motion vector) - written by the
                // forward pass, consumed by the post stack (TAA/GTAO). Unused this phase; transients free after.
                const rendergraph::RGHandle normalT = m_graph.CreateTransient(
                    u8"forward.normal",
                    rendergraph::RGTextureDesc(kGNormalFormat, v->Width(), v->Height()));
                const rendergraph::RGHandle velocityT = m_graph.CreateTransient(
                    u8"forward.velocity",
                    rendergraph::RGTextureDesc(kGVelocityFormat, v->Width(), v->Height()));
                // Roughness/metallic G-buffer - consumed by the SSR pass (roughness gates/fades reflections).
                const rendergraph::RGHandle materialT = m_graph.CreateTransient(
                    u8"forward.material",
                    rendergraph::RGTextureDesc(kGMaterialFormat, v->Width(), v->Height()));

                // Per-view authored post (exposure/tonemap/bloom/AO/AA/SSR), resolved by the RenderSubsystem
                // from the scene's PostProcessSettings (or the legacy global override). Read per view here.
                const ViewPostConfig& post = v->Settings().post;

                // TAA jitter: sub-pixel-offset the projection so the resolve accumulates supersamples. Applied
                // BEFORE reading the view-proj, so the prepass + forward + sky all use the SAME jittered matrix
                // (mismatched depth would break the prepass early-Z). Off when TAA is disabled (per view).
                const Float4x4 unjitteredVP =
                    v->Camera()
                        .ViewProjection(); // captured BEFORE jitter (transparent draws with this, post-TAA)
                Float2 jitter{0.0f, 0.0f};
                if (post.taaEnabled && m_taa != nullptr)
                {
                    jitter = HaltonJitter(m_jitterIndex, v->Width(), v->Height());
                    v->ApplyProjectionJitter(jitter.x, jitter.y);
                    m_anyViewTaa =
                        true; // advance the Halton phase once this frame (any view used TAA)
                }
                // Motion vectors: this view's previous-frame (jittered) view-proj + jitter (no motion on first
                // sight). Record this frame's for next frame.
                const Float4x4 curViewProj = v->Camera().ViewProjection(); // jittered when TAA on
                const Float4x4 prevViewProj =
                    (viewIndex < m_prevViewProj.Size()) ? m_prevViewProj[viewIndex] : curViewProj;
                if (m_curViewProj.Size() <= viewIndex)
                {
                    m_curViewProj.Resize(viewIndex + 1u, curViewProj);
                }
                m_curViewProj[viewIndex] = curViewProj;
                const Float2 prevJitter = (viewIndex < m_prevJitter.Size())
                                              ? m_prevJitter[viewIndex]
                                              : Float2{0.0f, 0.0f};
                if (m_curJitter.Size() <= viewIndex)
                {
                    m_curJitter.Resize(viewIndex + 1u, jitter);
                }
                m_curJitter[viewIndex] = jitter;

                // Depth prepass: opaque-only, clears + writes the camera depth so the forward pass shades
                // each opaque pixel once (early-Z via LessEqual). Declared before the forward, which Loads it.
                {
                    RenderView* pv = v;
                    RendererRegistry* reg = m_registry;
                    const u32 vi = viewIndex;
                    m_graph.AddRenderPass(
                        u8"depth.prepass",
                        [this, depth, pv, reg, vi](rendergraph::PassBuilder& b)
                        {
                            b.SetDepthTarget(depth, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.SetViewport(pv->ViewportX(), pv->ViewportY(), pv->ViewportWidth(),
                                          pv->ViewportHeight());
                            b.NeverCull();
                            b.SetExecute([this, pv, reg, vi](rhi::RenderPassEncoder& rp)
                                         { RecordDepthPrepass(rp, *pv, *reg, vi); });
                        });
                }

                // Declare the visible sky into `colorTarget` after the forward pass (if IBL + sky active).
                const auto declareSky = [&](rendergraph::RGHandle colorTarget,
                                            rendergraph::RGHandle velocityTarget,
                                            rhi::TextureFormat colorFmt)
                {
                    if (m_sky == nullptr || viewIblCtx == nullptr)
                    {
                        return;
                    }
                    // Reconstruct the sky ray from the UNJITTERED view-proj: the background is at infinity, so
                    // jittering its sampling buys ~no AA but makes it oscillate sub-pixel each frame - which TAA
                    // can only partly cancel, i.e. the wobble. Unjittered => temporally invariant sky under a
                    // static camera; the sky pass still writes a geometric motion vector so rotation reprojects.
                    const Float4x4 invVP = Inverse(unjitteredVP);
                    // The crisp analytic sun disc is for untextured skies; textured envs carry their own sun.
                    const f32 sunInt = viewIblCtx->HasSunDisc() ? viewIblCtx->SunIntensity() : 0.0f;
                    m_sky->DeclareSky(m_graph, colorTarget, velocityTarget, depth,
                                      viewIblCtx->EnvHandle(), viewIblCtx->EnvView(), colorFmt,
                                      m_pass.DepthFormat(), invVP, prevViewProj, jitter, prevJitter,
                                      v->Camera().position, viewIblCtx->SkyBackgroundIntensity(),
                                      viewIblCtx->SunDir(), viewIblCtx->SunAngularSize(),
                                      Float3{1.0f, 0.98f, 0.92f}, sunInt, v->ViewportX(),
                                      v->ViewportY(), v->ViewportWidth(), v->ViewportHeight(),
                                      m_frameIndex, viewIndex, viewIblCtx->Uid());
                };

                if (m_tonemap != nullptr)
                {
                    // HDR path: forward renders linear HDR into a transient, then the tonemap pass
                    // resolves it (exposure + tonemap + OETF) into the LDR target.
                    const rendergraph::RGHandle hdr = m_graph.CreateTransient(
                        u8"forward.hdr", rendergraph::RGTextureDesc(m_tonemap->HdrFormat(),
                                                                    v->Width(), v->Height()));
                    m_pass.DeclarePass(*v, *m_registry, m_graph, m_frameIndex, viewIndex, hdr,
                                       depth, /*clear*/ true, m_tonemap->HdrFormat(), normalT,
                                       velocityT, materialT, prevViewProj, jitter, prevJitter,
                                       cluster, shadow, ibl, rhi::LoadOp::Load,
                                       rendergraph::RGSubresourceRange{}, probePrefilteredH,
                                       probeActive, probeRange.base, probeRange.count);
                    declareSky(
                        hdr, velocityT,
                        m_tonemap
                            ->HdrFormat()); // sky into HDR (+ camera-motion velocity), before TAA
                    // Screen-space decals: project onto the opaque depth + blend into the lit HDR, AFTER sky
                    // and BEFORE AO/TAA (so decals get TAA-resolved). Uses the jittered view-proj (matches the
                    // depth). Per-scene decal list rides on the view's snapshot.
                    if (m_decalPass != nullptr && v->Scene() != nullptr)
                    {
                        m_decalPass->DeclareDecals(m_graph, hdr, depth, v->Scene()->Decals(),
                                                   curViewProj, v->Width(), v->Height(),
                                                   v->ViewportX(), v->ViewportY(),
                                                   v->ViewportWidth(), v->ViewportHeight());
                    }
                    // Screen-space reflections: reflect the lit HDR (sky + opaque + decals) into itself, AFTER
                    // decals and BEFORE AO/TAA (pre-TAA so the resolve stabilizes the march). Reads the roughness
                    // G-buffer to gate/fade; LERP-replaces the IBL specular where it hits. Produces a fresh HDR.
                    rendergraph::RGHandle sceneHdr = hdr;
                    if (m_ssr != nullptr && post.ssrEnabled)
                    {
                        // Per-view enable + intensity over the frame-global SSR config.
                        SsrPass::Params ssrParams = m_ssrParams;
                        ssrParams.intensity = post.ssrIntensity;
                        sceneHdr = m_ssr->DeclareSsr(
                            m_graph, hdr, depth, normalT, materialT, velocityT, v->Width(),
                            v->Height(), v->ViewportX(), v->ViewportY(), v->ViewportWidth(),
                            v->ViewportHeight(), Inverse(v->Camera().projection),
                            v->Camera().projection, ssrParams, viewIndex, m_frameIndex);
                    }
                    // AO (GTAO or SSAO) from the opaque depth+normal G-buffer, computed BEFORE the TAA resolve
                    // and multiplied into the HDR pre-TAA, so TAA stabilizes it (applying AO post-TAA wobbles,
                    // since the AO is computed from the jittered G-buffer and shifts sub-pixel each frame).
                    // AO debug stays a frame-global renderer toggle (not authored content).
                    const AoMode viewAoMode = static_cast<AoMode>(post.aoMode);
                    const bool aoActive = (viewAoMode != AoMode::Off) || m_aoDebug != 0;
                    const AoMode aoMode = (viewAoMode != AoMode::Off)
                                              ? viewAoMode
                                              : AoMode::GTAO; // debug needs a generator
                    rendergraph::RGHandle aoH{};
                    if (m_ao != nullptr && aoActive)
                    {
                        aoH = m_ao->DeclareAo(m_graph, depth, normalT, v->Width(), v->Height(),
                                              Inverse(v->Camera().projection),
                                              v->Camera().projection, post.aoRadius,
                                              post.aoIntensity, m_frameIndex, aoMode, m_aoDebug);
                    }
                    const bool showAo =
                        aoH.IsValid() && m_aoDebug != 0; // debug: AO/channel straight to screen
                    rendergraph::RGHandle litHdr = sceneHdr;
                    if (aoH.IsValid() && viewAoMode != AoMode::Off && m_aoDebug == 0)
                    {
                        litHdr = m_ao->DeclareApply(m_graph, sceneHdr, aoH, v->Width(), v->Height(),
                                                    post.aoStrength);
                    }
                    // TAA resolve on the opaque+sky+AO HDR (jittered) -> stable HDR. Then transparent composites
                    // on the RESOLVED image (see below), so it's never temporally accumulated (no ghost) or
                    // jittered (no wobble). Bloom + tonemap run on the resolved color.
                    rendergraph::RGHandle sceneColor = litHdr;
                    if (post.taaEnabled && m_taa != nullptr)
                    {
                        const f32 taaFar = (v->Camera().farZ > 0.0f) ? v->Camera().farZ : 1000.0f;
                        sceneColor = m_taa->DeclareTaa(
                            m_graph, litHdr, velocityT, depth, viewIndex, v->Width(), v->Height(),
                            post.taaBlend, post.taaGamma, m_taaMotionScale, /*near*/ 0.1f, taaFar);
                    }
                    // Transparent (blended) AFTER TAA, into the resolved image, with the UNJITTERED projection:
                    // color-only, depth read-only against the opaque depth, back-to-front.
                    m_pass.DeclareTransparent(
                        *v, *m_registry, m_graph, m_frameIndex, viewIndex, sceneColor, depth,
                        m_tonemap->HdrFormat(), unjitteredVP, prevViewProj, jitter, prevJitter,
                        cluster, shadow, ibl, probeRange.base, probeRange.count);
                    // Bloom pyramid over the resolved scene, composited by the tonemap.
                    rendergraph::RGHandle bloomH{};
                    if (m_bloom != nullptr && post.bloomEnabled && post.bloomIntensity > 0.0f)
                    {
                        bloomH = m_bloom->DeclareBloom(m_graph, sceneColor, v->Width(), v->Height(),
                                                       post.bloomThreshold, post.bloomKnee);
                    }
                    const f32 bloomStrength = bloomH.IsValid() ? post.bloomIntensity : 0.0f;
                    const rendergraph::RGHandle bloomTex =
                        bloomH.IsValid() ? bloomH : sceneColor; // valid binding even when off
                    // AO already applied pre-TAA; tonemap only needs the AO handle for the debug view.
                    const f32 aoStrength = 0.0f;
                    const rendergraph::RGHandle aoTex =
                        aoH.IsValid() ? aoH : sceneColor; // valid binding when off
                    // Map the tonemap's fullscreen uv to this view's sub-rect of the (full-size) HDR/bloom,
                    // so split-screen views resolve their own region (the forward renders into the sub-rect).
                    const f32 fullW = static_cast<f32>(v->Width()),
                              fullH = static_cast<f32>(v->Height());
                    const Float2 uvScale{static_cast<f32>(v->ViewportWidth()) / fullW,
                                         static_cast<f32>(v->ViewportHeight()) / fullH};
                    const Float2 uvOffset{static_cast<f32>(v->ViewportX()) / fullW,
                                          static_cast<f32>(v->ViewportY()) / fullH};
                    // FXAA (TAA-off fallback) runs AFTER tonemap: tonemap -> LDR intermediate, FXAA -> final.
                    // Never stacked with TAA (TAA already resolves aliasing). Off/TAA-on -> tonemap writes final.
                    const bool fxaa = m_fxaa != nullptr && post.fxaaEnabled && !post.taaEnabled;
                    const rendergraph::RGHandle tonemapOut =
                        fxaa ? m_graph.CreateTransient(
                                   u8"post.ldr", rendergraph::RGTextureDesc(
                                                     v->TargetFormat(), v->Width(), v->Height()))
                             : colorH;
                    m_tonemap->DeclareTonemap(
                        m_graph, sceneColor, bloomTex, aoTex, tonemapOut,
                        /*clearColor*/ fxaa || clearColor, v->Settings().clear, v->TargetFormat(),
                        v->ViewportX(), v->ViewportY(), v->ViewportWidth(), v->ViewportHeight(),
                        m_frameIndex, viewIndex, post.exposure, bloomStrength, uvScale, uvOffset,
                        aoStrength, showAo, post.agxTonemap,
                        /*sceneYFlipped*/ !post.taaEnabled);
                    // World-space UI draws BETWEEN tonemap and FXAA: authored colors survive
                    // (FXAA doesn't grade) and the quad silhouettes get antialiased. With
                    // FXAA off the pass lands directly on the final LDR (TAA never touched
                    // transparents, so edge AA there matches the old transparent-pass look).
                    m_pass.DeclarePostTonemapUI(*v, *m_registry, m_graph, m_frameIndex, viewIndex,
                                                fxaa ? tonemapOut : colorH, depth,
                                                v->TargetFormat(), unjitteredVP, prevViewProj);
                    if (fxaa)
                    {
                        const Float2 texel{1.0f / fullW, 1.0f / fullH};
                        m_fxaa->DeclareFxaa(m_graph, tonemapOut, colorH, clearColor,
                                            v->Settings().clear, v->TargetFormat(), v->ViewportX(),
                                            v->ViewportY(), v->ViewportWidth(), v->ViewportHeight(),
                                            m_frameIndex, viewIndex, texel, uvScale, uvOffset,
                                            post.fxaaSubpixel);
                    }
                }
                else
                {
                    // No tonemap: forward writes the LDR target directly.
                    m_pass.DeclarePass(*v, *m_registry, m_graph, m_frameIndex, viewIndex, colorH,
                                       depth, clearColor, v->TargetFormat(), normalT, velocityT,
                                       materialT, prevViewProj, jitter, prevJitter, cluster, shadow,
                                       ibl, rhi::LoadOp::Load, rendergraph::RGSubresourceRange{},
                                       probePrefilteredH, probeActive, probeRange.base,
                                       probeRange.count);
                    declareSky(colorH, velocityT, v->TargetFormat());
                    m_pass.DeclareTransparent(*v, *m_registry, m_graph, m_frameIndex, viewIndex,
                                              colorH, depth, v->TargetFormat(), unjitteredVP,
                                              prevViewProj, jitter, prevJitter, cluster, shadow,
                                              ibl, probeRange.base, probeRange.count);
                    // No-tonemap path: world UI straight onto the LDR target.
                    m_pass.DeclarePostTonemapUI(*v, *m_registry, m_graph, m_frameIndex, viewIndex,
                                                colorH, depth, v->TargetFormat(), unjitteredVP,
                                                prevViewProj);
                }

                // Scene-tier overlays (game UI: HUD canvases, billboards): one shared Load-op pass on the
                // view's final LDR output, after post (never TAA-smeared / tonemapped over), BEFORE debug
                // draw so gizmos and diagnostic text stay on top (the Sedulous OverlayPass ordering).
                // Sources match views by SceneKey and draw with the view's REAL camera.
                if (m_sceneOverlays != nullptr && !m_sceneOverlays->IsEmpty())
                {
                    // Stencil attachment for overlay UI (stencil-then-cover fills): a
                    // transient DS cleared to 0, in a device-probed format the sources
                    // check against their own pipelines before recording stencil draws.
                    if (!m_overlayStencilProbed)
                    {
                        m_overlayStencilFormat = PickStencilFormat(*m_device);
                        m_overlayStencilProbed = true;
                    }
                    SceneOverlayView overlayView;
                    overlayView.sceneKey = v->SceneKey();
                    overlayView.viewProjection = unjitteredVP;
                    overlayView.cameraPosition = v->Camera().position;
                    overlayView.viewportX = v->ViewportX();
                    overlayView.viewportY = v->ViewportY();
                    overlayView.viewportWidth = v->ViewportWidth();
                    overlayView.viewportHeight = v->ViewportHeight();
                    overlayView.targetWidth = v->Width();
                    overlayView.targetHeight = v->Height();
                    overlayView.targetFormat = v->TargetFormat();
                    overlayView.depthStencilFormat = m_overlayStencilFormat;
                    overlayView.frameIndex = m_frameIndex;
                    rendergraph::RGHandle overlayDs = rendergraph::RGHandle::Invalid();
                    if (m_overlayStencilFormat != rhi::TextureFormat::Undefined)
                    {
                        overlayDs = m_graph.CreateTransient(
                            u8"scene.overlay.ds",
                            rendergraph::RGTextureDesc(m_overlayStencilFormat, v->Width(),
                                                       v->Height()));
                    }
                    const Array<ISceneOverlay*>* overlays = m_sceneOverlays;
                    m_graph.AddRenderPass(
                        u8"scene.overlay",
                        [colorH, overlayDs, overlays, overlayView](rendergraph::PassBuilder& b)
                        {
                            b.SetColorTarget(0, colorH, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                             rhi::ClearColor::Black());
                            if (overlayDs.IsValid())
                            {
                                // Depth unused; stencil cleared to 0 for stencil-then-cover.
                                b.SetDepthTarget(overlayDs, rhi::LoadOp::Clear,
                                                 rhi::StoreOp::DontCare, 1.0f, {},
                                                 rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 0);
                            }
                            b.NeverCull();
                            b.SetExecute(
                                [overlays, overlayView](rhi::RenderPassEncoder& rp)
                                {
                                    for (ISceneOverlay* overlay : *overlays)
                                    {
                                        overlay->Render(rp, overlayView);
                                    }
                                });
                        });
                }

                // Debug draw (per view): global + this view's scene gizmos, projected by the UNJITTERED VP,
                // into the final LDR (geometry depth-tested against the scene depth; screen text on top).
                // Keyed per-scene (+ global) so side-by-side scenes/views don't bleed.
                if (m_debugPass != nullptr)
                {
                    const debug::DebugDraw* sceneDbg =
                        static_cast<const debug::DebugDraw*>(v->DebugScene());
                    if (m_debugGlobal != nullptr || sceneDbg != nullptr)
                    {
                        m_debugPass->DeclareGeometry(
                            m_graph, colorH, depth, unjitteredVP, m_debugGlobal, sceneDbg,
                            v->TargetFormat(), m_pass.DepthFormat(), v->ViewportX(), v->ViewportY(),
                            v->ViewportWidth(), v->ViewportHeight(), m_frameIndex, viewIndex);
                        m_debugPass->DeclareScreen(m_graph, colorH, unjitteredVP, m_debugGlobal,
                                                   sceneDbg, v->TargetFormat(), v->ViewportX(),
                                                   v->ViewportY(), v->ViewportWidth(),
                                                   v->ViewportHeight(), m_frameIndex, viewIndex);
                    }
                }
            }

            // View-independent screen HUD: drawn ONCE per distinct target at that target's full extent
            // (not per viewport), so a whole-window overlay isn't duplicated across split-screen views.
            // Declared after every view's passes so it composites on top. Screen-space only (identity VP;
            // 3D calls need a camera and are ignored here). Each target gets its own buffer slot.
            if (m_debugPass != nullptr && m_debugScreen != nullptr)
            {
                u32 overlayIndex = static_cast<u32>(m_views.ActiveCount());
                for (const TargetImport& ti : imported)
                {
                    m_debugPass->DeclareScreen(m_graph, ti.handle, Float4x4::Identity(),
                                               m_debugScreen, nullptr, ti.fmt, 0, 0, ti.w, ti.h,
                                               m_frameIndex, overlayIndex);
                    ++overlayIndex;
                }
            }
        } // end Compose.Declare
        if (m_decalPass != nullptr)
        {
            m_decalPass->EndFrame();
        } // unmap the decal ring before execute
        {
            DRACONIC_PROFILE_SCOPE(
                "Compose.Execute"); // graph compile (barriers/transients) + record all passes
            (void)m_graph.Execute(m_encoder);
            // Age out the transient texture pool. Execute() only RETURNS transients to the pool; EndFrame()
            // is the sole caller of TransientTexturePool::EndFrame(), which destroys entries unused for
            // maxUnusedFrames. Without this, the pool keeps every size ever seen alive forever - invisible
            // in steady state (one size recurs) but an unbounded VkDeviceMemory leak under viewport-resize
            // churn (each new size adds a never-evicted set), exhausting VRAM -> clear-color viewports.
            m_graph.EndFrame();
        }

        for (Renderer* r : m_registry->Unique())
        {
            r->FinishFrame();
        }
        m_prevViewProj = m_curViewProj; // this frame's view-projs become next frame's "previous"
        m_prevJitter = m_curJitter;     // ...and jitters (for the motion-vector unjitter)
        if (m_anyViewTaa)
        {
            m_jitterIndex = (m_jitterIndex + 1u) % 8u;
        } // Halton phase advances per frame (any view used TAA)
        m_anyViewTaa = false; // reset for next frame (re-accumulated during the view loop)
        m_encoder = nullptr;
    }
}
