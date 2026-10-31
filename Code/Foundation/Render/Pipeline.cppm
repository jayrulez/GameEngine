// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:pipeline` partition.
///
/// The frame driver and the renderer extension point. A `Renderer` is a per-category drawer;
/// subsystems (meshes here, and likewise particles, sprites and world UI) register one with the
/// `RendererRegistry` and contribute draws without core changes. The core sorts a view's
/// `DrawItem`s by category and dispatches each run to its registered `Renderer`.
///
/// `RenderFrame` is the single per-frame driver, not a per-view object. Its lifecycle is Begin,
/// AddView for each view, then End, so multiple scenes and views share per-frame state (the view
/// pool, the renderers' transient buffers) without overwriting each other. Views are collected,
/// then composed together at End. Recording goes through foundation.rendergraph (MRT, automatic
/// barriers, transient aliasing).

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h"

export module foundation.render:pipeline;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.profiler;
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
import :ssgi;
import :picking;
import :msaa_resolve;
import :fxaa;
import :debug_blit;
import :exposure;
import :debug_draw;
import :debug_pass;
import :decal_pass;
import :sky;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // Halton(base) low-discrepancy sequence term (1-based index).
    [[nodiscard]] inline f32 HaltonSeq(u32 i, u32 base)
    {
        f32 f = 1.0f, r = 0.0f;
        u32 idx = i + 1u;
        while (idx > 0u)
        {
            f /= static_cast<f32>(base);
            r += f * static_cast<f32>(idx % base);
            idx /= base;
        }
        return r;
    }
    // TAA sub-pixel jitter for frame `index` (mod the sequence length), in clip space:
    // Halton(2,3) centered to [-0.5,0.5], scaled to a 1-texel clip offset (projection::ApplyJitter).
    [[nodiscard]] inline Float2 HaltonJitter(u32 index, u32 width, u32 height)
    {
        const f32 x = HaltonSeq(index, 2u) - 0.5f;
        const f32 y = HaltonSeq(index, 3u) - 0.5f;
        return Float2{x * 2.0f / static_cast<f32>(width > 0 ? width : 1u),
                      y * 2.0f / static_cast<f32>(height > 0 ? height : 1u)};
    }

    // The view-projection the sky draws its rays through, from an unjittered camera: the view's own
    // for a perspective camera; for an orthographic one, whose rays are parallel (the whole
    // background one colour), a 60 degree perspective with the same view and aspect, so the sky
    // still reads as sky and turns with the camera.
    [[nodiscard]] inline Float4x4 SkyViewProjection(const ViewCamera& camera)
    {
        if (!camera.IsOrthographic())
        {
            return camera.ViewProjection();
        }
        const Float4x4& p = camera.projection;
        const f32 aspect = (p.m[0][0] != 0.0f) ? p.m[1][1] / p.m[0][0] : 1.0f;
        const f32 farZ = Max(camera.FarZ(), 1.0f);
        return camera.view * Float4x4::PerspectiveFovRH(1.0471976f, aspect, Min(0.1f, farZ * 0.5f), farZ);
    }

    // What the IBL precompute exposes to the forward pass: this frame's graph handles for the products the
    // forward samples in set 0 (prefiltered specular cube, BRDF LUT, SH9 diffuse buffer). The forward reads
    // them with ReadTexture and ReadBuffer, so the graph orders any precompute writes first and transitions
    // them to shader-readable.
    struct IblBinding
    {
        // The actual per-scene products, bound per view into set 0 (a frame renders several
        // scenes, each with its own IBL context). `generation` is unique across contexts
        // (one system-wide counter), so bind-group caches can key on it alone.
        rhi::Buffer* shBuffer = nullptr;
        rhi::TextureView* prefilterView = nullptr;
        rhi::TextureView* brdfView = nullptr;
        f32 maxLod = 0.0f;
        u64 generation = 0;

        rendergraph::RGHandle prefilterHandle = {};
        rendergraph::RGHandle brdfHandle = {};
        rendergraph::RGHandle shHandle = {};
        bool valid = false;
        [[nodiscard]] bool Valid() const noexcept { return valid; }
    };

    // What a Renderer needs to record draws for one view. `pass` is a RenderCommandEncoder, the
    // shared draw-recording surface, so a renderer records identically whether it targets a live
    // render pass or an off-thread render bundle (the basis for parallel command recording).
    struct RenderRecordContext
    {
        const RenderView* view = nullptr;
        rhi::RenderCommandEncoder* pass = nullptr;
        Float4x4 viewProj = Float4x4::Identity();
        Float4x4 prevViewProj =
            Float4x4::Identity();         // last frame's view-proj (camera motion vectors)
        Float2 jitter = Float2{0, 0};     // this frame's NDC sub-pixel TAA jitter
        Float2 prevJitter = Float2{0, 0}; // last frame's jitter (unjitter the reprojection)
        Float4x4 viewMatrix = Float4x4::Identity(); // for view-space depth (clustered shading)
        Float3 cameraPos = Float3{0, 0, 0};
        Float3 ambient = Float3{0.03f, 0.03f, 0.03f}; // scene environment ambient
        ShadowCascades cascades = {};                 // this view's CSM cascades
        u32 cascadeLayerBase = 0;                     // this view's first shadow-array layer
        u32 localShadowEntryBase = 0; // this view's scene's first GpuLocalShadow entry
        u32 probeBase = 0;            // this view's scene's first probe record
        u32 probeCount = 0;           // ...and how many (0 for none)
        IblBinding ibl = {};          // this view's scene's IBL products (per-scene contexts)
        Span<const GpuLight> lights = {};
        ClusterBinding cluster = {}; // per-cluster light lists (empty when clustering is off)
        u32 frameIndex = 0;
        u32 viewIndex = 0; // this view's index in the frame (per-view buffer slots)
        rhi::TextureFormat colorFormat = rhi::TextureFormat::BGRA8Unorm;
        rhi::TextureFormat depthFormat = rhi::TextureFormat::Depth32Float;
        // Scene-pass MSAA sample count for the pass being recorded. The opaque MSAA passes (prepass
        // and forward opaque, and the mesh, sprite and particle renderers dispatched inside them)
        // set the view's count so their PSO and render-bundle sample state match the MSAA attachments;
        // the transparent pass and all post effects stay 1 (they run on the resolved 1x color). Every
        // renderer's PipelineConfig takes this so the count flows to whichever pass records it.
        u8 sampleCount = 1;
        // The opaque scene-depth as a sampleable view (transparent pass only; null otherwise). Already in
        // DepthStencilRead from the read-only depth target, so renderers can sample it (soft particles, say).
        rhi::TextureView* sceneDepthView = nullptr;
        bool depthPrepass =
            false; // camera depth-only prepass: no depth bias (match forward exactly)
        bool probesEnabled =
            true; // false during probe capture: reflect the sky IBL, not the probe being captured
        bool needsMotion =
            true; // false: no temporal effect consumes velocity, so skip the per-instance prev-world lookup
        bool fillInstanceCache =
            false; // camera depth prepass: build the full instance data and cache the per-group range for the forward to reuse
        u8 debugSemantic =
            0; // editor semantic debug view (ViewDebugSemantic): forward outputs the term, not the lit result
        f32 timeSeconds = 0.0f;     // the frame's time (RenderFrame::SetTime): the WIND sway phase
        f32 prevTimeSeconds = 0.0f; // last frame's time (the previous position sways with it)
        // Sky-lighting dimmers from the scene environment (SkySnapshot): scale the forward's
        // IBL diffuse and specular terms without touching the visible sky.
        f32 iblDiffuseIntensity = 1.0f;
        f32 iblSpecularIntensity = 1.0f;
    };

    // The part of a record context every pass drawing from `view`'s camera shares: the view, the
    // camera's matrix and position, and the wind clock (the view's scene clock, else the frame's).
    // One function so no camera pass misses a field: a pass without the camera position would
    // fade vegetation by its distance from the origin in depth but from the camera in colour.
    [[nodiscard]] RenderRecordContext CameraRecordContext(const RenderView& view, f32 frameTime,
                                                          f32 prevFrameTime) noexcept;

    // A fully resolved draw: all GPU state resolved (PSO built, bind groups and ring slots allocated,
    // buffers bound), ready to emit as pure commands with no shared mutation, so emission can run
    // in parallel across threads and bundles. Produced by Renderer::Resolve (single-threaded, where
    // allocation, upload and caching happen); replayed by EmitDraw. It holds only RHI handles, so the
    // emit phase is renderer-agnostic.
    // Bind groups are named by the engine's set-frequency convention (set 0 view and per frame,
    // set 1 per-draw object or instance, set 2 material), which every renderer follows.
    struct ResolvedDraw
    {
        rhi::RenderPipeline* pso = nullptr;
        rhi::BindGroup* viewSet = nullptr; // set 0 (view)
        u32 viewOffset = 0;
        bool viewDynamic = false;
        rhi::BindGroup* drawSet = nullptr; // set 1 (object UBO or instance storage)
        u32 drawOffset = 0;
        bool drawDynamic = false;
        rhi::BindGroup* materialSet = nullptr; // set 2 (material, inferred from properties)
        rhi::BindGroup* clusterSet = nullptr;  // set 3 (clustered light lists; dummy when off)
        rhi::Buffer* vertexBuffer0 = nullptr;
        u64 vertexOffset0 = 0;
        rhi::Buffer* vertexBuffer1 = nullptr;
        u64 vertexOffset1 = 0; // optional: skin stream (skinned) or instance offsets
        rhi::Buffer* vertexBuffer2 = nullptr;
        u64 vertexOffset2 = 0; // optional: instance offsets (skinned and instanced)
        rhi::Buffer* indexBuffer = nullptr;
        u64 indexOffset = 0;
        rhi::IndexFormat indexFormat = rhi::IndexFormat::UInt32;
        u32 indexCount = 0;
        u32 instanceCount = 1;
    };

    // What the directional shadow pass exposes to the forward pass: the depth map's sample view (bound
    // in set 0 by the mesh renderer), the graph handle (read with ReadTexture so the forward is ordered
    // after the depth write and the map transitions to a shader-readable state), and the light-space matrix.
    // Defined here (not in :shadows) because the forward pass consumes it and :shadows imports :pipeline.
    struct ShadowBinding
    {
        rhi::TextureView* sampleView = nullptr; // the cascade depth array (all views' layers)
        rendergraph::RGHandle handle = {}; // read with ReadTexture to order the cascade writes before forward
        ShadowCascades cascades;           // this view's cascade matrices and splits
        u32 layerBase = 0;                 // this view's first array layer (viewIndex * cascades)
        bool valid = false;
        // Local-light (spot and point) shadow atlas: one physical atlas shared by all views, with
        // tile space and the GpuLocalShadow entry buffer partitioned per scene (the editor renders
        // different scenes side by side in one frame; extraction assigns shadowIndex scene-relative).
        // Every forward pass reads it with ReadTexture, so the atlas depth pass is ordered and
        // transitioned ahead of it.
        rendergraph::RGHandle atlasHandle = {};
        bool atlasValid = false;
        u32 localShadowEntryBase = 0; // this view's scene's first entry in the flat buffer
        [[nodiscard]] bool Valid() const noexcept { return valid && sampleView != nullptr; }
        // True whenever the cascade map exists this frame, even for a view whose scene has no
        // directional caster (valid == false). The renderers' set-0 group binds the real map
        // frame-globally and the forward shader samples it statically, so every pass must
        // declare the read (barrier and ordering) or it executes against attachment-layout layers
        // that another view's cascade writes just produced.
        [[nodiscard]] bool MapBound() const noexcept { return sampleView != nullptr; }
    };

    // Replay one resolved draw into any command sink (a live pass or an off-thread bundle). Pure
    // command emission: it touches no shared state, so it is safe to run concurrently.
    inline void EmitDraw(rhi::RenderCommandEncoder& enc, const ResolvedDraw& d)
    {
        if (d.pso == nullptr || d.indexBuffer == nullptr)
        {
            return;
        }
        enc.SetPipeline(d.pso);
        if (d.viewSet != nullptr)
        {
            if (d.viewDynamic)
            {
                enc.SetBindGroup(0, d.viewSet, Span<const u32>{&d.viewOffset, 1});
            }
            else
            {
                enc.SetBindGroup(0, d.viewSet, Span<const u32>{});
            }
        }
        if (d.drawSet != nullptr)
        {
            if (d.drawDynamic)
            {
                enc.SetBindGroup(1, d.drawSet, Span<const u32>{&d.drawOffset, 1});
            }
            else
            {
                enc.SetBindGroup(1, d.drawSet, Span<const u32>{});
            }
        }
        if (d.materialSet != nullptr)
        {
            enc.SetBindGroup(2, d.materialSet, Span<const u32>{});
        } // material
        if (d.clusterSet != nullptr)
        {
            enc.SetBindGroup(3, d.clusterSet, Span<const u32>{});
        } // cluster lists
        if (d.vertexBuffer0 != nullptr)
        {
            enc.SetVertexBuffer(0, d.vertexBuffer0, d.vertexOffset0);
        }
        if (d.vertexBuffer1 != nullptr)
        {
            enc.SetVertexBuffer(1, d.vertexBuffer1, d.vertexOffset1);
        }
        if (d.vertexBuffer2 != nullptr)
        {
            enc.SetVertexBuffer(2, d.vertexBuffer2, d.vertexOffset2);
        }
        enc.SetIndexBuffer(d.indexBuffer, d.indexFormat, d.indexOffset);
        enc.DrawIndexed(d.indexCount, d.instanceCount);
    }

    // A per-category drawer. Implemented by the mesh, sprite, particle and other subsystems and
    // registered with the RendererRegistry. Two phases: Resolve turns a sorted run of DrawItems into
    // ResolvedDraws (single-threaded; this is where mesh upload, PSO build and ring allocation happen);
    // the ForwardPass then emits the resolved draws (serially or in parallel) with no shared mutation.
    // PrepareFrame/FinishFrame bracket the whole frame so a renderer sizes its transient buffers once.
    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        // The categories this renderer draws (its registration keys).
        [[nodiscard]] virtual Span<const RenderCategory> SupportedCategories() const = 0;

        // Bracket the frame: `maxDraws` is an upper bound on DrawItems this renderer may receive
        // across all views, so per-object transient storage (the object-UBO ring) is sized once and
        // never reallocated mid-frame (which would invalidate already-resolved draws). `frameIndex`
        // is the device ring slot, selecting this frame's region of any frames-in-flight ring.
        virtual void PrepareFrame(u32 maxDraws, u32 frameIndex);

        // Resolve a sorted run of this renderer's DrawItems into `out` (append). Single-threaded:
        // all GPU-state mutation (mesh upload, PSO build, ring allocation and writes) happens here.
        virtual void Resolve(const RenderRecordContext& ctx, Span<const DrawItem> items,
                             Array<ResolvedDraw>& out) = 0;

        // Resolve the same draws as depth-only casters for a shadow pass: `ctx.viewProj` is the light's
        // world-to-clip matrix and `ctx.depthFormat` the shadow map's format. Produces depth-only
        // ResolvedDraws (set 0 the light view, set 1 object or instance; no material or cluster). Default
        // no-op so a renderer opts in to casting shadows (the mesh renderer does; sprites need not).
        virtual void ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                      Array<ResolvedDraw>& out);

        // Resolve the same draws as pick-id writers: `ctx.viewProj` is the pick view's (cropped)
        // world-to-clip, `ctx.colorFormat` the RG32Uint id target and `ctx.depthFormat` its depth.
        // Each draw's fragment writes (EntityTag index + 1, generation) of its RenderData
        // entityId; nearest wins through the pass's own depth. Default no-op, so a renderer
        // whose draws are not pickable (sprites, particles) costs nothing; the editor's CPU
        // pick covers their entities.
        virtual void ResolvePickIds(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                    Array<ResolvedDraw>& out);

        // Hand this frame's directional shadow map (null for none) to a renderer that samples it in set 0,
        // with the ShadowSystem's generation (bumped on texture recreation) so the renderer's bind-group
        // cache invalidates on a reused-address view. Called once per frame before PrepareFrame. Default
        // no-op (a renderer that doesn't shade ignores it).
        virtual void SetShadowMap(rhi::TextureView* shadowMap, u64 generation);

        // Hand this frame's local-light (spot and point) shadow atlas (null for none) and its generation,
        // with the same contract and timing as SetShadowMap. `passCount` is how many atlas depth passes
        // (one per caster tile) will re-emit this renderer's casters, so it can size its per-object
        // rings. Default no-op.
        virtual void SetShadowAtlas(rhi::TextureView* atlas, u64 generation, u32 passCount);

        // How many reflection-probe capture faces will re-emit this renderer's draws this frame (one forward
        // pass per face). Lets it size its per-object rings for the extra draw-resolving passes. Called once
        // per frame before PrepareFrame. Default no-op.
        virtual void SetCaptureFacePasses(u32 passes) { (void)passes; }

        // How many pick passes re-emit draws this frame (per-object ring sizing, like the
        // capture faces). Called before PrepareFrame.
        virtual void SetPickPasses(u32 passes) { (void)passes; }

        // The shadow passes' draws this frame: for each view, its scene's caster count (every
        // cascade and local tile of that view draws from it). Casters are camera-independent, so
        // they can far outnumber the view's own draws; per-object rings must hold them too.
        // Called before PrepareFrame.
        virtual void SetShadowCasterDraws(u32 draws) { (void)draws; }

        // This frame's reflection probes: the prefiltered cube array (set-0 t8), the probe-metadata SRV (t9)
        // and the active probe count (the forward loops Probes[0..count]). Null means no probes. Called once per
        // frame before PrepareFrame. Default no-op.
        virtual void SetProbes(rhi::TextureView* cubeArray, rhi::Buffer* probeBuffer, u32 count);

        // Upload this frame's local-shadow entries (the atlas's per-light matrices and rects) for a renderer
        // that binds them in set 0. Called once per frame after PrepareFrame. Default no-op.
        virtual void UploadLocalShadows(Span<const GpuLocalShadow> shadows, u32 frameIndex);

        // Pre-pass: write this frame's skinning matrices into the renderer's persistent bone pool once
        // (current and previous slab per distinct skeleton instance) and copy staging to device on `encoder`.
        // Called once per frame after PrepareFrame and before the render graph executes, so the forward
        // and shadow passes share one device-local bone buffer (no per-pass re-upload). `scenes` is every
        // distinct scene the frame draws (editor pages side by side), given at once: the pool is sized
        // and filled for all of them together, and what one scene uploaded is never lost to the next.
        // Default no-op.
        virtual void UploadSkinning(Span<const ExtractedScene* const> scenes, rhi::CommandEncoder& encoder);

        virtual void FinishFrame() {}

        // This renderer's dispatch id: its index in the RendererRegistry, assigned at Register. Producers
        // stamp it onto their RenderData::rendererId so emission routes each draw back to its owner (so
        // several renderers can share a category and still be dispatched correctly). Set by the registry.
        [[nodiscard]] u16 RendererId() const noexcept { return m_rendererId; }
        void SetRendererId(u16 id) noexcept { m_rendererId = id; }

    private:
        u16 m_rendererId = 0;
    };

    // Holds the registered renderers and dispatches a draw to its owner by RenderData::rendererId.
    // Dispatch is per-item (not per-category) so several renderers can share a category and still be
    // routed correctly. The id is the renderer's registration index; the first renderer
    // registered gets id 0, which is the RenderData::rendererId default (so plain mesh data needs no tag).
    class RendererRegistry
    {
    public:
        // Register `renderer` (borrowed; the caller owns its lifetime); assigns its dispatch id.
        void Register(Renderer* renderer);

        [[nodiscard]] Renderer* ById(u16 id) const noexcept;

        [[nodiscard]] Span<Renderer* const> Unique() const noexcept;

    private:
        Array<Renderer*> m_unique;
    };

    // The forward pass: declares a view's opaque, transparent and post-tonemap UI passes into the
    // frame graph and dispatches the view's sorted draw list to the registered Renderers, one
    // contiguous category run at a time.
    class ForwardPass
    {
    public:
        ForwardPass(rhi::Device& device, u32 framesInFlight) noexcept
            : m_device(&device), m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight)
        {
        }
        ~ForwardPass() { ReleaseWorkerPools(); }

        ForwardPass(const ForwardPass&) = delete;
        ForwardPass& operator=(const ForwardPass&) = delete;

        // Whether any temporal effect (TAA, temporal SSR) consumes motion vectors this frame. When false,
        // the forward resolve skips the per-instance prev-world lookup (velocity written as 0). Set per frame.
        void SetMotionNeeded(bool needed) noexcept { m_motionNeeded = needed; }
        // The frame clock (this frame's and last frame's seconds): the WIND sway phase. Set per frame.
        void SetTime(f32 seconds, f32 prevSeconds) noexcept
        {
            m_timeSeconds = seconds;
            m_prevTimeSeconds = prevSeconds;
        }

        // Once per frame, before composing views: provision and reset this frame's per-worker command
        // pools (used for parallel emit). The reset happens once per frame because a worker bundle's
        // secondary command buffer must outlive the main submission that executes it, so it can't be
        // freed between views. No-op when the job system is absent; emit then runs serially.
        void BeginFrame(u32 frameIndex);

        // DeclarePass, below, declares this view's forward pass into the frame graph against `depth`
        // (loaded by default, `depthLoad`) and `colorH`, the target the forward writes (an HDR transient
        // when tonemapping, else the imported LDR target); `colorFormat` is its format, so the PSO
        // matches. `clearColor` is true for the first view that writes a given target (it clears the
        // whole target) and false for later views into the same target, which load so they don't wipe
        // earlier views' regions. The pass body is a render bundle the graph executes (secondary
        // contents); resolve and emit run in the bundle callback at graph Execute time.
        [[nodiscard]] rhi::TextureFormat DepthFormat() const noexcept { return m_depthFormat; }

        void DeclarePass(const RenderView& view, const RendererRegistry& registry,
                         rendergraph::RenderGraph& graph, u32 frameIndex, u32 viewIndex,
                         rendergraph::RGHandle colorH, rendergraph::RGHandle depth, bool clearColor,
                         rhi::TextureFormat colorFormat, rendergraph::RGHandle normalH,
                         rendergraph::RGHandle velocityH, rendergraph::RGHandle materialH,
                         rendergraph::RGHandle albedoH, const Float4x4& prevViewProj, Float2 jitter, Float2 prevJitter,
                         const ClusterBinding& cluster = {}, const ShadowBinding& shadow = {},
                         const IblBinding& ibl = {}, rhi::LoadOp depthLoad = rhi::LoadOp::Load,
                         rendergraph::RGSubresourceRange colorSub = {},
                         rendergraph::RGHandle probeHandle = {}, bool probeValid = false,
                         u32 probeBase = 0, u32 probeCount = 0);

        // The transparent pass: blended geometry into the already-lit color target only, with no G-buffer
        // and no depth write. Runs after opaque and sky, depth-tested read-only against the opaque depth,
        // back to front (the draw list is pre-sorted). Transparent is lit, so it still binds the cluster,
        // shadow and IBL set-0 resources. colorH is loaded (keeping opaque and sky); the PSO is the
        // color-only forward permutation.
        void DeclareTransparent(const RenderView& view, const RendererRegistry& registry,
                                rendergraph::RenderGraph& graph, u32 frameIndex, u32 viewIndex,
                                rendergraph::RGHandle colorH, rendergraph::RGHandle depth,
                                rhi::TextureFormat colorFormat, const Float4x4& drawViewProj,
                                const Float4x4& prevViewProj, Float2 jitter, Float2 prevJitter,
                                const ClusterBinding& cluster = {},
                                const ShadowBinding& shadow = {}, const IblBinding& ibl = {},
                                u32 probeBase = 0, u32 probeCount = 0);

        // World-space UI (the WorldUI category): drawn after tonemap into the final LDR,
        // depth-tested read-only against the opaque depth, so panels keep their authored
        // colors (identical to the screen tier's) yet still occlude behind scene geometry.
        // Unlit: no cluster, shadow or IBL bindings.
        void DeclarePostTonemapUI(const RenderView& view, const RendererRegistry& registry,
                                  rendergraph::RenderGraph& graph, u32 frameIndex, u32 viewIndex,
                                  rendergraph::RGHandle colorH, rendergraph::RGHandle depth,
                                  rhi::TextureFormat colorFormat, const Float4x4& drawViewProj,
                                  const Float4x4& prevViewProj);

    private:
        // The bundle-pass body: resolve the view's draws (single-threaded) then emit them into render
        // bundles appended to `out`, serially below the threshold, otherwise fanned out across the job
        // system (per-worker bundles). The graph replays `out` via ExecuteBundles.
        void ResolveAndEmit(const RenderView& view, const RendererRegistry& registry,
                            rhi::CommandEncoder& encoder, u32 frameIndex, u32 viewIndex,
                            rhi::TextureFormat colorFormat, const Float4x4& drawViewProj,
                            const Float4x4& prevViewProj, Float2 jitter, Float2 prevJitter,
                            PassAffinity passAffinity, const ClusterBinding& cluster,
                            const ShadowBinding& shadow, const IblBinding& ibl,
                            Array<rhi::RenderBundle*>& out,
                            rhi::TextureView* sceneDepthView = nullptr, bool probesEnabled = true,
                            u32 probeBase = 0, u32 probeCount = 0);

        // Split the resolved draws into <= SlotCount contiguous chunks; record each into its own
        // bundle on a JobSystem worker, using that chunk's own command pool (so no two threads touch
        // a pool concurrently; pools are indexed by chunk, not worker slot). Bundles are kept in
        // draw order in m_bundles. Pools were reset for this frame by BeginFrame.
        void EmitParallel(const rhi::RenderBundleDesc& bd, u32 frameIndex);

        // (Re)provision the per-(frameIndex, slot) command-pool grid. Bundles are minted straight
        // from each pool (CommandPool::CreateRenderBundleEncoder) with no per-pool encoder, so the
        // pools never hold an open primary command list (a DX12 requirement for their per-frame
        // Reset). Grows only. Returns false on failure (parallel emit then skips).
        bool EnsureWorkerPools(u32 slotCount);

        void ReleaseWorkerPools();

        // Above this many resolved draws, emission fans out across the job system; below it, one
        // bundle on the calling thread. Parallel recording pays off only with many distinct draws;
        // instanced batches collapse to one resolved draw each.
        static constexpr u32 kParallelEmitThreshold = 256;

        rhi::Device* m_device;
        u32 m_framesInFlight = 2;
        rhi::TextureFormat m_depthFormat =
            rhi::TextureFormat::Depth32Float; // depth texture is a graph transient
        bool m_motionNeeded = true;  // per-frame: does any temporal effect read velocity this frame
        f32 m_timeSeconds = 0.0f;     // the WIND sway clock (this frame)
        f32 m_prevTimeSeconds = 0.0f; // ...and last frame's
        Array<ResolvedDraw> m_resolved;      // reused resolve buffer (drained each pass)
        Array<rhi::RenderBundle*> m_bundles; // per-chunk bundles (draw order)
        // Per-(frameIndex, slot) worker command pools for parallel bundle emit.
        Array<rhi::CommandPool*> m_workerPools;
        u32 m_workerSlots = 0;
    };

    // A 90-degree FOV camera looking along one cube face (+X,-X,+Y,-Y,+Z,-Z) from a probe center, for reflection-
    // probe capture. Uses LookAtRH (right-handed, so triangle winding is correct and back-face culling works).
    // A right-handed LookAt produces faces mirrored horizontally relative to the cube sampler; that mirror is
    // corrected in image space by a horizontal-flip blit (captured to prefiltered), not in the camera, since
    // negating a camera axis would flip winding and break culling.
    [[nodiscard]] inline ViewCamera ProbeFaceCamera(Float3 center, u32 face, f32 nearZ, f32 farZ)
    {
        static const Float3 dirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        static const Float3 ups[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1},
                                      {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
        ViewCamera vc;
        vc.view = Float4x4::LookAtRH(center, center + dirs[face], ups[face]);
        vc.projection = Float4x4::PerspectiveFovRH(1.57079633f, 1.0f, nearZ, farZ); // 90° square
        vc.position = center;
        vc.farZ = farZ;
        return vc;
    }

    // Probe-capture faces resolve with their own view indices, far above any real view count, so
    // their per-view bind-group cache slots never collide with the main views'.
    inline constexpr u32 kCaptureViewIndexBase = 64;

    // The single per-frame driver. Begin resets shared per-frame state; AddView collects a view
    // (extracting its draw list from a scene snapshot); End sizes the renderers' transient once
    // for the whole frame and composes every view. One driver for all views; there is no per-view object.
    class RenderFrame
    {
    public:
        RenderFrame(IAllocator& allocator, rhi::Device& device, RendererRegistry& registry,
                    u32 framesInFlight,
                    ClusterSystem* clusters = nullptr, TonemapPass* tonemap = nullptr,
                    ShadowSystem* shadows = nullptr, IBLSystem* ibl = nullptr,
                    SkyPass* sky = nullptr, BloomPass* bloom = nullptr, TaaPass* taa = nullptr,
                    AoPass* ao = nullptr, FxaaPass* fxaa = nullptr,
                    ExposurePass* exposure = nullptr, DebugBlitPass* debugBlit = nullptr) noexcept
            : m_registry(&registry), m_pass(device, framesInFlight), m_graph(allocator, &device),
              m_clusters(clusters), m_tonemap(tonemap), m_shadows(shadows), m_ibl(ibl), m_sky(sky),
              m_bloom(bloom), m_taa(taa), m_ao(ao), m_fxaa(fxaa), m_exposurePass(exposure),
              m_debugBlit(debugBlit), m_views(allocator)
        {
            m_device = &device;
        }

        /// Frame delta for the auto-exposure adaptation (seconds); set by the host each frame.
        void SetDeltaSeconds(f32 dt) noexcept { m_deltaSeconds = dt; }

    private:
        // (Declared before the methods below, whose signatures use it.)
        struct LocalShadowTile
        {
            Float4x4 viewProj;
            u32 x = 0, y = 0, w = 0, h = 0;
            Float3 cullCenter;
            f32 cullRadius = 0.0f;
        };
        // All shadow inputs sourced from one scene. A frame can render several distinct scenes
        // (editor pages side by side); each view reads its own scene's context, never another's.
        // Pooled behind UniquePtrs: graph-execute lambdas capture the pointers, so addresses must
        // outlive the frame and never move when the pool grows.
        struct SceneShadowCtx
        {
            const ExtractedScene* scene = nullptr;
            Array<DrawItem> casters;            // camera-independent caster list (this scene)
            Array<Float4> casterBounds;         // aligned to casters: xyz worldCenter, w radius
            Array<Float4> animatedSpheres;      // moving casters' spheres, xyz centre w radius (static-tile routing)
            Array<Float4> prevAnimatedSpheres;  // last frame's (a caster leaving a tile redraws it too)
            usize movingCasters = 0;            // how many casters moved this frame (diagnostics)
            // Each caster's bounds by entity, this frame's and last frame's alternating, so a caster that
            // moved (a door swinging), appeared or went redraws the cached tiles it touches. Kept only
            // while the scene has cached (Static) local shadows.
            HashMap<u64, Float4> casterBoundsById[2];
            u32 boundsFrame = 0;
            Array<LocalShadowTile> staticTiles; // this scene's static-layer tiles (this frame)
            Array<LocalShadowTile> staticRenderTiles; // subset dirty this frame (rendered)
            Array<u32> staticTileDirty;               // per-static-tile refresh countdown
            u64 staticSig = 0; // static caster-set signature (cache invalidation)
            const ExtractedScene* lastStaticScene =
                nullptr;                       // pool-slot reuse detection (dirty-all)
            u32 staticTileBase = 0;            // scene's first static tile (layout-shift detection)
            u32 entryBase = 0;                 // scene's first entry in m_localShadows
            IBLSystem::Context* ibl = nullptr; // this scene's IBL products (per-scene sky)
        };
        // One atlas-tile draw: the tile and the scene whose casters render into it.
        struct AtlasDraw
        {
            LocalShadowTile tile;
            SceneShadowCtx* ctx = nullptr;
        };

    public:
        // Test and debug introspection of the last End()'s shadow composition: which views had a
        // directional shadow and each view's scene entry base into the local-shadow buffer.
        struct ViewShadowDebug
        {
            bool directional = false;
            bool mapBound = false;
            u32 localEntryBase = 0;
        };
        [[nodiscard]] Span<const ViewShadowDebug> ViewShadowInfo() const noexcept;
        [[nodiscard]] Span<const GpuLocalShadow> LocalShadowEntries() const noexcept;

        // Begin a frame against the caller's encoder (the caller owns the encoder and targets).
        void Begin(rhi::CommandEncoder& encoder, u32 frameIndex);

        // Collect a view over `scene`. Builds its sorted draw list now (parallelizable);
        // the GPU recording is deferred to End so transient buffers are sized once per frame.
        // debugScene is the per-scene list (drawn in every view of the scene); debugView is the
        // per-view list (drawn only in this view: editor chrome that must not leak into a
        // second view of the same scene, such as the camera preview).
        RenderView* AddView(const ExtractedScene& scene, const ViewCamera& camera,
                            const ViewSettings& settings, rhi::TextureView* target,
                            rhi::TextureFormat targetFormat, u32 width, u32 height,
                            const void* debugScene = nullptr, const void* sceneKey = nullptr,
                            const void* debugView = nullptr);

        // Per-frame debug draw: the pass, the global list (drawn in every view) and the screen list (drawn
        // once, whole window). Per-scene lists ride on each RenderView.
        void SetDebug(DebugDrawPass* pass, const debug::DebugDraw* global,
                      const debug::DebugDraw* screen) noexcept;

        // Scene-tier overlay sources (borrowed registry, owned by the subsystem; null or empty means no
        // overlay pass). Each view gets one shared Load-op pass on its final LDR output, after
        // post, before debug draw. Set once per frame before End.
        void SetSceneOverlays(const Array<ISceneOverlay*>* overlays) noexcept;

        // Per-frame decal pass (borrowed); null means no decals. Declared per view after sky, before AO.
        void SetDecal(DecalPass* pass) noexcept { m_decalPass = pass; }

        // Screen-space reflections (borrowed pass); null means no SSR. Declared per view after sky and decals,
        // before AO and TAA (so TAA stabilizes the march). Set once per frame before End.
        void SetSsr(SsrPass* pass) noexcept { m_ssr = pass; }
        void SetSsgi(SsgiPass* pass) noexcept { m_ssgi = pass; }

        // The history slot a view keeps its between-frame state in (previous camera and jitter, the
        // TAA, SSR, SSGI and exposure histories), from its ViewSettings::historyKey (0 means the
        // view's place in this frame's list, `viewIndex`). The same key gets the same slot every
        // frame, wherever the view falls in the list; a key new to the frame takes a free slot or
        // the one seen longest ago, and `fresh` says so (its state is the last owner's: reset it).
        static constexpr u32 kHistorySlots = 8; // the passes' per-view history arrays
        [[nodiscard]] u32 HistorySlotFor(u64 historyKey, u32 viewIndex, bool& fresh) noexcept;
        // Scene-pass MSAA first-sample resolve (borrowed pass); null means no MSAA support. Used only when
        // a view's post.msaaSamples > 1, to resolve the MSAA depth and aux to 1x for the post consumers.
        void SetMsaaResolve(MsaaResolvePass* pass) noexcept { m_msaaResolve = pass; }
        // GPU picking (null means no pick passes). Begin() retires its readbacks; a view whose
        // ViewSettings::viewportKey has pending requests declares its pick passes.
        void SetPick(PickSystem* pick) noexcept { m_pick = pick; }
        // SSR enable and tunables (enabled=false leaves the scene HDR untouched).
        void SetSsrParams(bool enabled, const SsrPass::Params& params) noexcept;

        // Reflection-probe system (borrowed); null means no probes. Dirty probes are captured (6 faces each,
        // lit forward and sky into their cube-array slices) before the main views, so the forward can sample
        // the prefiltered result. Set once per frame before End.
        void SetProbes(ReflectionProbeSystem* probes) noexcept { m_probeSystem = probes; }

        // Linear exposure multiplier applied in the tonemap pass (scene or camera setting).
        void SetExposure(f32 exposure) noexcept { m_exposure = exposure; }
        // The frame's time in seconds (the app's run clock): the WIND vertex sway phase. Set once
        // per frame before Begin; the previous frame's value rides along for motion vectors.
        void SetTime(f32 seconds) noexcept
        {
            m_prevTimeSeconds = m_timeSeconds;
            m_timeSeconds = seconds;
        }
        [[nodiscard]] f32 TimeSeconds() const noexcept { return m_timeSeconds; }
        // Bloom composite strength and soft-knee prefilter (intensity 0 is off).
        void SetBloom(f32 intensity, f32 threshold, f32 knee) noexcept;
        // Temporal AA: jitters the projection and resolves against per-view history (off: no jitter, no
        // resolve). blend is the max history weight (stability), gamma the variance-clip box half-width,
        // motionScale how fast history drops with motion, per pixel a frame (it halves at 1 / motionScale
        // pixels; 0 never).
        void SetTaa(bool on, f32 blend, f32 gamma, f32 motionScale) noexcept;
        // Ambient occlusion: mode (Off, GTAO, SSAO) and knobs. AO is applied to the HDR before TAA (strength 0
        // or Off means no AO). debugMode != 0 forces the AO on and shows the debug channel straight to screen.
        void SetAo(AoMode mode, f32 strength, f32 radius, f32 intensity,
                   i32 debugMode = 0) noexcept;
        // FXAA (TAA-off fallback): run FXAA after tonemap when TAA is off. Never stacked with TAA.
        void SetFxaa(bool on, f32 subpixelQuality) noexcept;
        // Instance-data sharing between the camera depth prepass and the forward (build once rather than
        // twice). On by default; off fills again in the forward (for A/B and regression checks).
        void SetInstanceSharing(bool on) noexcept { m_instanceSharing = on; }
        [[nodiscard]] bool InstanceSharing() const noexcept { return m_instanceSharing; }
        // View-frustum culling: reject renderables outside the camera frustum when building each view's draw
        // list. On by default (a no-op for benchmarks that frame everything; a win for scenes with much off
        // screen). Directional shadows source casters independently, so culling never drops shadow casters.
        void SetViewCulling(bool on) noexcept { m_viewCulling = on; }
        [[nodiscard]] bool ViewCulling() const noexcept { return m_viewCulling; }
        // Last frame's view-frustum cull totals, summed over active views (0/0 when culling was off). Read
        // before Begin() rewinds the view pool (from a sample's OnUpdate, say) to see the previous frame.
        void CullStats(u32& culled, u32& total) const noexcept;

        // Depth prepass body: emit the view's opaque draws as depth-only from the camera POV (no bias, so
        // the depth equals the forward pass's exactly for LessEqual early-Z). Masked isn't prepassed (the
        // depth-only shader can't alpha-discard); transparent doesn't write depth. Runs at graph execute.
        void RecordDepthPrepass(rhi::RenderPassEncoder& rp, const RenderView& view,
                                const RendererRegistry& registry, u32 viewIndex);
        // Re-emit `view`'s opaque, masked and blended draws as pick-id writers with the cropped
        // `viewProj` (the PickSystem's record callback).
        void RecordPickIds(rhi::RenderPassEncoder& rp, const RenderView& view,
                           const RendererRegistry& registry, u32 viewIndex,
                           const Float4x4& viewProj);

        // Resolve and emit a caster list as depth-only draws from a light's POV (the shadow depth pass
        // body); lightViewProj is the depth shader's "camera". Runs at graph execute time, before the
        // forward pass (which reads the shadow map), so it fills the rings ahead of forward. `casters`
        // is camera-independent for local lights (the scene-global list) or the view's draw list for
        // cascades. When cullRadius > 0, casters whose world bounding sphere doesn't intersect the
        // light sphere (cullCenter, cullRadius) are skipped.
        // `lodView` is the camera view whose LOD selection the casters follow: cascades pass their
        // owning view so shadow geometry matches what that view draws; the camera-independent
        // local-shadow tiles pass null, which selects each chain's coarsest level, since shadows
        // never render finer than any view shows.
        void RecordShadowCasters(rhi::RenderPassEncoder& rp, Span<const DrawItem> casters,
                                 const RendererRegistry& registry, const Float4x4& lightViewProj,
                                 Float3 cullCenter = {}, f32 cullRadius = 0.0f,
                                 bool frustumCull = false, Span<const Float4> cullBounds = {},
                                 const RenderView* lodView = nullptr,
                                 const ExtractedScene* scene = nullptr);

        // Build the camera-independent shadow-caster list from a scene (opaque and masked meshes), grouped
        // by (mesh, material) so the depth pass batches them. Used by both the directional cascades and the
        // local-light atlas tiles, and built per scene into that scene's context (multi-scene frames).
        void BuildShadowCasterList(const ExtractedScene& scene, SceneShadowCtx& ctx);

        // A signature over the static shadow-casting local lights (type, position, direction, range and
        // cone, quantized). When it changes, the cached static atlas layer is re-rendered for one
        // frames-in-flight cycle. The Static-mode contract is that caster geometry doesn't move, so only
        // the lights themselves feed the signature.
        [[nodiscard]] u64 StaticCasterSignature(const ExtractedScene* scene) const;

        // Compose all collected views into the frame's encoder.
        void End();

        // The frame's render-graph texture inventory (deduped by name), for debug-view
        // pickers. Valid after End() until the next Begin() rebuilds the graph.
        void CollectDebugResources(Array<DebugResourceInfo>& out) const;

        // Read-only view of the frame's graph (pass and resource introspection for debug tooling
        // and tests). Same validity window as CollectDebugResources.
        [[nodiscard]] const rendergraph::RenderGraph& Graph() const noexcept { return m_graph; }

        [[nodiscard]] usize ViewCount() const noexcept { return m_views.ActiveCount(); }
        /// Diagnostics and tests: this scene's shadow caster count and how many of them moved (skinned, or
        /// moved, appeared or gone since the last frame while the scene has cached shadows), from the
        /// last frame that built the scene's shadow context.
        [[nodiscard]] usize ShadowCasterCount(const ExtractedScene* scene) const noexcept;
        [[nodiscard]] usize MovingShadowCasterCount(const ExtractedScene* scene) const noexcept;

    private:
        RendererRegistry* m_registry;
        ForwardPass m_pass;
        rendergraph::RenderGraph m_graph;    // one graph per frame, composes all views
        ClusterSystem* m_clusters = nullptr; // borrowed; declares the per-view cluster build pass
        TonemapPass* m_tonemap =
            nullptr; // borrowed; HDR-resolve pass (null: the forward writes LDR directly)
        ShadowSystem* m_shadows = nullptr; // borrowed; owns the directional shadow depth texture
        IBLSystem* m_ibl =
            nullptr; // borrowed; owns the IBL precompute products (env, SH, prefilter, BRDF)
        SkyPass* m_sky = nullptr; // borrowed; draws the visible environment background
        BloomPass* m_bloom =
            nullptr;              // borrowed; builds the HDR bloom pyramid (composited at tonemap)
        TaaPass* m_taa = nullptr; // borrowed; temporal AA resolve (per-view history)
        AoPass* m_ao = nullptr;   // borrowed; ambient occlusion (GTAO or SSAO) from the G-buffer
        FxaaPass* m_fxaa = nullptr;       // borrowed; TAA-off fallback AA (after tonemap)
        DecalPass* m_decalPass = nullptr; // borrowed; per-view screen-space decal pass
        SsrPass* m_ssr = nullptr; // borrowed; screen-space reflections (after sky and decals, before TAA)
        SsgiPass* m_ssgi = nullptr; // borrowed; screen-space GI bounce (before SSR, so reflections see it)
        MsaaResolvePass* m_msaaResolve =
            nullptr; // borrowed; MSAA depth and aux to 1x resolve (only when a view is MSAA)
        bool m_ssrEnabled = false;
        SsrPass::Params m_ssrParams{};
        ReflectionProbeSystem* m_probeSystem =
            nullptr;                  // borrowed; dirty probes captured before the main views
        RenderView m_captureViews[6]; // persistent 6-face capture views (outlive graph execute)
        u32 m_probeCaptureCursor = 0; // round-robin: which dirty probe to capture this frame
        DebugDrawPass* m_debugPass = nullptr;            // borrowed; per-view debug gizmo and text pass
        const debug::DebugDraw* m_debugGlobal = nullptr; // borrowed; global (all-views) debug list
        const debug::DebugDraw* m_debugScreen =
            nullptr; // borrowed; whole-window screen HUD (drawn once)
        rhi::Device* m_device = nullptr; // borrowed; outlives the frame
        const Array<ISceneOverlay*>* m_sceneOverlays =
            nullptr;           // borrowed; scene-tier overlay sources
        // Stencil-capable DS format for the scene-overlay pass (probed lazily; Undefined means the
        // device has none, so the pass stays color-only and overlay UI tessellates fills).
        rhi::TextureFormat m_overlayStencilFormat = rhi::TextureFormat::Undefined;
        bool m_overlayStencilProbed = false;
        f32 m_exposure = 1.0f; // linear exposure multiplier (tonemap input)
        f32 m_timeSeconds = 0.0f;     // SetTime: this frame's clock (WIND sway)
        f32 m_prevTimeSeconds = 0.0f; // last frame's clock
        bool m_fxaaEnabled = false;
        f32 m_fxaaSubpixel = 0.75f;
        bool m_instanceSharing = true; // share prepass instance data with the forward (A/B toggle)
        bool m_viewCulling = true;     // view-frustum cull camera draw lists
        AoMode m_aoMode = AoMode::Off;
        i32 m_aoDebug = 0;
        f32 m_aoStrength = 0.6f;
        f32 m_aoRadius = 0.5f;
        f32 m_aoIntensity = 1.0f;
        f32 m_bloomIntensity = 0.05f; // 0 turns bloom off
        f32 m_bloomThreshold = 1.0f;
        f32 m_bloomKnee = 0.6f;
        bool m_taaEnabled = false;
        f32 m_taaBlend = 0.97f;
        f32 m_taaGamma = 1.25f;
        f32 m_taaMotionScale = 1.0f / 32.0f;
        ExposurePass* m_exposurePass = nullptr;
        DebugBlitPass* m_debugBlit = nullptr; // borrowed; editor debug-view blit (after compose)
        f32 m_deltaSeconds = 1.0f / 60.0f;
        u32 m_jitterIndex = 0; // Halton phase, advances once per frame (mod 8)
        // Monotonic frame counter for stochastic-pass noise rotation. The frameIndex the
        // host passes to Begin() is the frames-in-flight ring index (0..2); fed to the SSGI
        // trace, it would give the rotating noise only 3 patterns, and the temporal
        // accumulator would converge to a 3-frame average that stays speckled.
        u32 m_noiseFrame = 0;
        bool m_anyViewTaa =
            false; // set per frame when any view enables TAA (drives the jitter advance)
        // Motion vectors and TAA: last frame's view-proj and jitter per view index (this frame's collected into
        // m_curViewProj and m_curJitter, swapped in at End). Camera motion is the previous against the current
        // (jittered) view-proj.
        Array<Float4x4> m_prevViewProj; // indexed by history slot (HistorySlotFor)
        Array<Float4x4> m_curViewProj;
        Array<Float2> m_prevJitter;
        Array<Float2> m_curJitter;
        // The sky's view-projection by history slot (the view's, or an orthographic view's
        // perspective stand-in: SkyViewProjection), for its motion vectors.
        Array<Float4x4> m_prevSkyViewProj;
        Array<Float4x4> m_curSkyViewProj;
        struct HistorySlot
        {
            u64 key = 0;
            u32 lastFrame = 0; // m_noiseFrame when a view last took it
            bool used = false;
        };
        HistorySlot m_historySlots[kHistorySlots];
        Array<ResolvedDraw>
            m_prepassResolved; // reused depth-draw buffer for the camera depth prepass
        Array<ResolvedDraw> m_shadowResolved; // reused depth-draw buffer for the shadow pass
        Array<ResolvedDraw> m_pickResolved;   // reused id-draw buffer for the pick passes
        PickSystem* m_pick = nullptr;         // borrowed (the subsystem owns it)
        // The PickSystem's record entry (a plain function: `context` is this frame).
        static void RecordPickIdsThunk(void* context, rhi::RenderPassEncoder& rp,
                                       const Float4x4& viewProj, const PickRect& rect,
                                       const void* viewContext);
        // Local-light (spot and point) shadows: per-frame caster matrices and their atlas tiles. Members
        // (not locals) so the atlas pass's execute lambda can reference the tiles for the frame's lifetime.
        Array<UniquePtr<SceneShadowCtx>>
            m_sceneShadowPool;       // slot k = k-th distinct scene this frame
        Array<u32> m_viewSceneIndex; // per view: index into the pool (~0u for no scene)
        Array<const ExtractedScene*> m_frameScenes; // the pool's scenes, in order: what the skinning upload is given
        Array<GpuLocalShadow>
            m_localShadows; // scenes' entries concatenated, caster order per scene
        Array<AtlasDraw>
            m_rtAtlasDraws; // realtime atlas layer (all scenes; re-rendered every frame)
        Array<AtlasDraw>
            m_staticAtlasDraws; // static atlas layer, all tiles (all scenes; cache source)
        Array<AtlasDraw> m_staticRenderDraws; // static layer tiles dirty this frame (all scenes)
        Array<DrawItem> m_shadowCullScratch;  // per-tile sphere-culled subset (reused)
        Array<ViewShadowDebug> m_viewShadowDebug; // tests and debug: last End()'s per-view composition
        RenderViewPool m_views;
        Array<DrawItem> m_sortScratch; // reused radix-sort ping-pong buffer
        rhi::CommandEncoder* m_encoder = nullptr;
        u32 m_frameIndex = 0;
    };

} // namespace foundation::render
