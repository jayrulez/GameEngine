/// Draconic::Render - the `:pipeline` partition.
///
/// The frame driver + extension seam. A `Renderer` is a per-category drawer; subsystems
/// (meshes here, particles/sprites/world-UI later) register one with the `RendererRegistry`
/// and contribute draws with ZERO core changes - the keeper architecture validated by
/// Sedulous's particles being a separate library. The core sorts a view's `DrawItem`s by
/// category and dispatches each run to its registered `Renderer`. (§6.)
///
/// `RenderFrame` is the SINGLE per-frame driver (not a per-view god object - the explicit
/// replacement for Sedulous's Pipeline/ShadowPipeline/ProbePipeline). Its lifecycle mirrors
/// Sedulous's useful `ISceneRenderer` shape - Begin / AddView×N / End - so multiple scenes
/// and views share per-frame state (the view pool, the renderers' transient buffers) without
/// clobbering each other. Views are collected, then composed together at End. (§9.) Phase 1
/// records directly into the command encoder via `ForwardPass`; phase 3 routes the same
/// pass-group through draconic.rendergraph (MRT + automatic barriers + transient aliasing).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Profiler/Profiler.h"

export module draconic.render:pipeline;

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

export namespace draconic::render
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
    // TAA sub-pixel jitter for frame `index` (mod the sequence length), in clip space (matches Sedulous):
    // Halton(2,3) centered to [-0.5,0.5], scaled to a 1-texel clip offset. Add to projection (2,0)/(2,1).
    [[nodiscard]] inline Float2 HaltonJitter(u32 index, u32 width, u32 height)
    {
        const f32 x = HaltonSeq(index, 2u) - 0.5f;
        const f32 y = HaltonSeq(index, 3u) - 0.5f;
        return Float2{x * 2.0f / static_cast<f32>(width > 0 ? width : 1u),
                      y * 2.0f / static_cast<f32>(height > 0 ? height : 1u)};
    }

    // What the IBL precompute exposes to the forward pass: this frame's graph handles for the products the
    // forward samples in set 0 (prefiltered specular cube, BRDF LUT, SH9 diffuse buffer). ReadTexture'd /
    // ReadBuffer'd so the graph orders any precompute writes -> forward and barriers them shader-readable.
    struct IblBinding
    {
        // The ACTUAL per-scene products, bound per view into set 0 (a frame renders several
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

    // What a Renderer needs to record draws for one view. `pass` is a RenderCommandEncoder - the
    // shared draw-recording surface - so a renderer records identically whether it targets a live
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
        ShadowCascades cascades = {};                 // this view's CSM cascades (phase 5.2)
        u32 cascadeLayerBase = 0;                     // this view's first shadow-array layer
        u32 localShadowEntryBase = 0; // this view's scene's first GpuLocalShadow entry
        u32 probeBase = 0;            // this view's scene's first probe record
        u32 probeCount = 0;           // ...and how many (0 = none)
        IblBinding ibl = {};          // this view's SCENE's IBL products (per-scene contexts)
        Span<const GpuLight> lights = {};
        ClusterBinding cluster = {}; // per-cluster light lists (empty = clustering off)
        u32 frameIndex = 0;
        u32 viewIndex = 0; // this view's index in the frame (per-view buffer slots)
        rhi::TextureFormat colorFormat = rhi::TextureFormat::BGRA8Unorm;
        rhi::TextureFormat depthFormat = rhi::TextureFormat::Depth32Float;
        // The opaque scene-depth as a sampleable view (transparent pass only; null otherwise). Already in
        // DepthStencilRead from the read-only depth target, so renderers can sample it (e.g. soft particles).
        rhi::TextureView* sceneDepthView = nullptr;
        bool depthPrepass =
            false; // camera depth-only prepass: no depth bias (match forward exactly)
        bool probesEnabled =
            true; // false during probe capture: reflect the sky IBL, not the probe (no feedback/self-black)
        bool needsMotion =
            true; // false = no temporal effect consumes velocity -> skip the per-instance prev-world lookup
        bool fillInstanceCache =
            false; // camera depth prepass: build the FULL instance data + cache the per-group range so the forward reuses it (build once, not twice)
        f32 shadowFarFade =
            40.0f; // CSM far-fade width in world units (SampleCSM dissolves shadows over the last cascade's far edge)
    };

    // A fully-resolved draw: all GPU state resolved (PSO built, bind groups + ring slots allocated,
    // buffers bound), ready to EMIT as pure commands with NO shared mutation - so emission can run
    // in parallel across threads/bundles. Produced by Renderer::Resolve (single-threaded, where the
    // allocation/upload/caching happens); replayed by EmitDraw. Backend-agnostic (all RHI handles),
    // so the emit phase is renderer-agnostic.
    // Bind groups are named by the engine's set-frequency convention (set 0 = view/per-frame,
    // set 1 = per-draw object/instance, set 2 = material), which every renderer follows.
    struct ResolvedDraw
    {
        rhi::RenderPipeline* pso = nullptr;
        rhi::BindGroup* viewSet = nullptr; // set 0 (view)
        u32 viewOffset = 0;
        bool viewDynamic = false;
        rhi::BindGroup* drawSet = nullptr; // set 1 (object UBO / instance storage)
        u32 drawOffset = 0;
        bool drawDynamic = false;
        rhi::BindGroup* materialSet = nullptr; // set 2 (material - inferred from properties)
        rhi::BindGroup* clusterSet = nullptr;  // set 3 (clustered light lists; dummy when off)
        rhi::Buffer* vertexBuffer0 = nullptr;
        u64 vertexOffset0 = 0;
        rhi::Buffer* vertexBuffer1 = nullptr;
        u64 vertexOffset1 = 0; // optional: skin stream (skinned) or instance offsets
        rhi::Buffer* vertexBuffer2 = nullptr;
        u64 vertexOffset2 = 0; // optional: instance offsets (skinned+instanced)
        rhi::Buffer* indexBuffer = nullptr;
        u64 indexOffset = 0;
        rhi::IndexFormat indexFormat = rhi::IndexFormat::UInt32;
        u32 indexCount = 0;
        u32 instanceCount = 1;
    };

    // What the directional shadow pass exposes to the forward pass: the depth map's sample view (bound
    // in set 0 by the mesh renderer), the graph handle (ReadTexture'd so the forward is ordered after
    // the depth write + the map barriers to a shader-readable state), and the light-space matrix.
    // Defined here (not in :shadows) because the forward pass consumes it and :shadows imports :pipeline.
    struct ShadowBinding
    {
        rhi::TextureView* sampleView = nullptr; // the cascade depth ARRAY (all views' layers)
        rendergraph::RGHandle handle = {}; // ReadTexture'd to order the cascade writes -> forward
        ShadowCascades cascades;           // THIS view's cascade matrices/splits
        u32 layerBase = 0;                 // this view's first array layer (viewIndex * cascades)
        bool valid = false;
        // Local-light (spot/point) shadow atlas (5.3) - ONE physical atlas shared by all views, with
        // tile space + the GpuLocalShadow entry buffer PARTITIONED PER SCENE (the editor renders
        // different scenes side-by-side in one frame; extraction assigns shadowIndex scene-relative).
        // ReadTexture'd by every forward pass so the atlas depth pass is ordered + barriered ahead of it.
        rendergraph::RGHandle atlasHandle = {};
        bool atlasValid = false;
        u32 localShadowEntryBase = 0; // this view's scene's first entry in the flat buffer
        [[nodiscard]] bool Valid() const noexcept { return valid && sampleView != nullptr; }
        // True whenever the cascade MAP exists this frame - even for a view whose scene has no
        // directional caster (valid == false). The renderers' set-0 group binds the real map
        // frame-globally and the forward shader samples it STATICALLY, so EVERY pass must
        // declare the read (barrier + ordering) or it executes against ATTACHMENT-layout layers
        // another view's cascade writes just produced (the caster-less-scene hole).
        [[nodiscard]] bool MapBound() const noexcept { return sampleView != nullptr; }
    };

    // Replay one resolved draw into any command sink (a live pass or an off-thread bundle). Pure
    // command emission - touches no shared state, so it is safe to run concurrently.
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

    // A per-category drawer. Implemented by mesh/sprite/particle/etc. subsystems and registered with
    // the RendererRegistry. Two phases: RESOLVE turns a sorted run of DrawItems into ResolvedDraws
    // (single-threaded - this is where mesh upload, PSO build, and ring allocation happen); the
    // ForwardPass then EMITs the resolved draws (serially or in parallel) with no shared mutation.
    // PrepareFrame/FinishFrame bracket the whole frame so a renderer sizes its transient buffers once.
    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        // The categories this renderer draws (its registration keys).
        [[nodiscard]] virtual Span<const RenderCategory> SupportedCategories() const = 0;

        // Bracket the frame: `maxDraws` is an upper bound on DrawItems this renderer may receive
        // across all views, so per-object transient (e.g. the object-UBO ring) is sized once and
        // never reallocated mid-frame (which would invalidate already-resolved draws). `frameIndex`
        // is the device ring slot, selecting this frame's region of any frames-in-flight ring.
        virtual void PrepareFrame(u32 maxDraws, u32 frameIndex);

        // Resolve a sorted run of this renderer's DrawItems into `out` (append). Single-threaded:
        // all GPU-state mutation (mesh upload, PSO build, ring allocation + writes) happens here.
        virtual void Resolve(const RenderRecordContext& ctx, Span<const DrawItem> items,
                             Array<ResolvedDraw>& out) = 0;

        // Resolve the same draws as DEPTH-ONLY casters for a shadow pass: `ctx.viewProj` is the light's
        // world->clip matrix and `ctx.depthFormat` the shadow map's format. Produces depth-only
        // ResolvedDraws (set 0 = light view, set 1 = object/instance; no material/cluster). Default
        // no-op so a renderer opts in to casting shadows (the mesh renderer does; sprites need not).
        virtual void ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                      Array<ResolvedDraw>& out);

        // Hand this frame's directional shadow map (null = none) to a renderer that samples it in set 0,
        // with the ShadowSystem's generation (bumped on texture recreation) so the renderer's bind-group
        // cache invalidates on a reused-address view. Called once per frame before PrepareFrame. Default
        // no-op (a renderer that doesn't shade ignores it).
        virtual void SetShadowMap(rhi::TextureView* shadowMap, u64 generation);

        // Hand this frame's local-light (spot/point) shadow atlas (null = none) + its generation, same
        // contract/timing as SetShadowMap. `passCount` is how many atlas depth passes (one per caster
        // tile) will re-emit this renderer's casters, so it can size its per-object rings. Default no-op.
        virtual void SetShadowAtlas(rhi::TextureView* atlas, u64 generation, u32 passCount);

        // How many reflection-probe capture faces will re-emit this renderer's draws this frame (one forward
        // pass per face). Lets it size its per-object rings for the extra draw-resolving passes. Called once
        // per frame before PrepareFrame. Default no-op.
        virtual void SetCaptureFacePasses(u32 passes) { (void)passes; }

        // This frame's reflection probes: the prefiltered cube-ARRAY (set-0 t8) + the probe-metadata SRV (t9)
        // + the active probe count (the forward loops Probes[0..count]). null => no probes. Called once per
        // frame before PrepareFrame. Default no-op.
        virtual void SetProbes(rhi::TextureView* cubeArray, rhi::Buffer* probeBuffer, u32 count);

        // Upload this frame's local-shadow entries (the atlas's per-light matrices/rects) for a renderer
        // that binds them in set 0. Called once per frame after PrepareFrame. Default no-op.
        virtual void UploadLocalShadows(Span<const GpuLocalShadow> shadows, u32 frameIndex);

        // Pre-pass: write this frame's skinning matrices into the renderer's persistent bone pool ONCE
        // (current + previous slab per distinct skeleton instance) and copy staging->device on `encoder`.
        // Called once per frame after PrepareFrame and BEFORE the render graph executes, so the forward
        // and shadow passes share one device-local bone buffer (no per-pass re-upload). Default no-op.
        virtual void UploadSkinning(const ExtractedScene& scene, rhi::CommandEncoder& encoder);

        virtual void FinishFrame() {}

        // This renderer's dispatch id - its index in the RendererRegistry, assigned at Register. Producers
        // stamp it onto their RenderData::rendererId so emission routes each draw back to its owner (so
        // several renderers can share a category and still be dispatched correctly). Set by the registry.
        [[nodiscard]] u16 RendererId() const noexcept { return m_rendererId; }
        void SetRendererId(u16 id) noexcept { m_rendererId = id; }

    private:
        u16 m_rendererId = 0;
    };

    // Holds the registered renderers and dispatches a draw to its owner by RenderData::rendererId.
    // Dispatch is per-item (not per-category) so several renderers can share a category and still be
    // routed correctly (ezEngine-style). The id is the renderer's registration index; the FIRST renderer
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

    // The phase-1 forward pass: opens one render pass against a view's color target + an owned
    // depth buffer, then dispatches the view's sorted draw list to the registered Renderers,
    // one contiguous category-run at a time. (Phase 3 replaces this with graph-scheduled passes:
    // depth prepass + MRT forward + post, with automatic barriers and transient aliasing.)
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

        // Whether any temporal effect (TAA / SSR-temporal) consumes motion vectors this frame. When false,
        // the forward resolve skips the per-instance prev-world lookup (velocity written as 0). Set per frame.
        void SetMotionNeeded(bool needed) noexcept { m_motionNeeded = needed; }
        void SetShadowFarFade(f32 v) noexcept { m_shadowFarFade = v; }

        // Once per frame, before composing views: provision + reset this frame's per-worker command
        // pools (used for parallel emit). Reset happens ONCE per frame - a worker bundle's secondary
        // command buffer must outlive the main submission that executes it, so it can't be freed
        // between views. (No-op when the job system is absent - emit then runs serially.)
        void BeginFrame(u32 frameIndex);

        // Declare this view's forward pass into the frame graph: a TRANSIENT depth target (the graph
        // allocates it + inserts the depth barrier automatically - retiring the hand-rolled depth
        // transition) + the IMPORTED color target (left in RenderTarget for the host to present). The
        // pass body is a render bundle the graph executes (secondary contents). Resolve + emit run in
        // the bundle callback at graph Execute time.
        // `colorH` is the (shared) imported target handle; `clearColor` is true for the first view that
        // writes a given target (it clears the whole target), false for later views into the same target
        // (they Load so they don't wipe earlier views' regions). Depth is a per-view transient (each clears).
        // `colorH` is the target the forward writes (an HDR transient when tonemapping, else the imported
        // LDR target); `colorFormat` is its format (so the PSO matches). `clearColor` clears vs loads.
        [[nodiscard]] rhi::TextureFormat DepthFormat() const noexcept { return m_depthFormat; }

        void DeclarePass(const RenderView& view, const RendererRegistry& registry,
                         rendergraph::RenderGraph& graph, u32 frameIndex, u32 viewIndex,
                         rendergraph::RGHandle colorH, rendergraph::RGHandle depth, bool clearColor,
                         rhi::TextureFormat colorFormat, rendergraph::RGHandle normalH,
                         rendergraph::RGHandle velocityH, rendergraph::RGHandle materialH,
                         const Float4x4& prevViewProj, Float2 jitter, Float2 prevJitter,
                         const ClusterBinding& cluster = {}, const ShadowBinding& shadow = {},
                         const IblBinding& ibl = {}, rhi::LoadOp depthLoad = rhi::LoadOp::Load,
                         rendergraph::RGSubresourceRange colorSub = {},
                         rendergraph::RGHandle probeHandle = {}, bool probeValid = false,
                         u32 probeBase = 0, u32 probeCount = 0);

        // The transparent pass: blended geometry into the (already-lit) color target only - no G-buffer, no
        // depth write. Runs after opaque + sky, depth-tested read-only against the opaque depth, back-to-front
        // (the draw list is pre-sorted). Transparent is lit, so it still binds the cluster/shadow/IBL set-0
        // resources. colorH is loaded (preserves opaque + sky); the PSO is the color-only forward permutation.
        void DeclareTransparent(const RenderView& view, const RendererRegistry& registry,
                                rendergraph::RenderGraph& graph, u32 frameIndex, u32 viewIndex,
                                rendergraph::RGHandle colorH, rendergraph::RGHandle depth,
                                rhi::TextureFormat colorFormat, const Float4x4& drawViewProj,
                                const Float4x4& prevViewProj, Float2 jitter, Float2 prevJitter,
                                const ClusterBinding& cluster = {},
                                const ShadowBinding& shadow = {}, const IblBinding& ibl = {},
                                u32 probeBase = 0, u32 probeCount = 0);

        // World-space UI (the WorldUI category): drawn AFTER tonemap into the final LDR,
        // depth-tested read-only against the opaque depth - panels keep their authored
        // colors (identical to the screen tier's) yet still occlude behind scene geometry.
        // Unlit by design: no cluster/shadow/IBL bindings.
        void DeclarePostTonemapUI(const RenderView& view, const RendererRegistry& registry,
                                  rendergraph::RenderGraph& graph, u32 frameIndex, u32 viewIndex,
                                  rendergraph::RGHandle colorH, rendergraph::RGHandle depth,
                                  rhi::TextureFormat colorFormat, const Float4x4& drawViewProj,
                                  const Float4x4& prevViewProj);

    private:
        // The bundle-pass body: resolve the view's draws (single-threaded) then emit them into render
        // bundle(s) appended to `out` - serially below the threshold, else fanned out across the job
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
        // bundle on a JobSystem worker, using that chunk's OWN command pool (so no two threads touch
        // a pool concurrently - pools are indexed by chunk, not worker slot). Bundles are kept in
        // draw order in m_bundles. Pools were reset for this frame by BeginFrame.
        void EmitParallel(const rhi::RenderBundleDesc& bd, u32 frameIndex);

        // (Re)provision the per-(frameIndex, slot) command-pool grid. Bundles are minted straight
        // from each pool (CommandPool::CreateRenderBundleEncoder) - no per-pool encoder, so the
        // pools never hold an open primary command list (a DX12 requirement for their per-frame
        // Reset). Grows only. Returns false on failure (parallel emit then skips).
        bool EnsureWorkerPools(u32 slotCount);

        void ReleaseWorkerPools();

        // Above this many resolved draws, emission fans out across the job system; below it, one
        // bundle on the calling thread. (Parallel recording pays off only with many distinct draws -
        // instanced batches collapse to one resolved draw each.)
        static constexpr u32 kParallelEmitThreshold = 256;

        rhi::Device* m_device;
        u32 m_framesInFlight = 2;
        rhi::TextureFormat m_depthFormat =
            rhi::TextureFormat::Depth32Float; // depth texture is a graph transient
        bool m_motionNeeded = true;  // per-frame: does any temporal effect read velocity this frame
        f32 m_shadowFarFade = 40.0f; // CSM far-fade width (world units)
        Array<ResolvedDraw> m_resolved;      // reused resolve buffer (drained each pass)
        Array<rhi::RenderBundle*> m_bundles; // per-chunk bundles (draw order)
        // Per-(frameIndex, slot) worker command pools for parallel bundle emit.
        Array<rhi::CommandPool*> m_workerPools;
        u32 m_workerSlots = 0;
    };

    // A 90 degrees-FOV camera looking along one cube face (+X,-X,+Y,-Y,+Z,-Z) from a probe center, for reflection-
    // probe capture. Uses LookAtRH (right-handed => correct triangle winding, so back-face culling works).
    // RH LookAt produces HORIZONTALLY-MIRRORED faces vs the cube sampler; that mirror is corrected in image
    // space by a horizontal-flip blit (captured -> prefiltered), NOT in the camera - negating a camera axis
    // would flip winding and break culling. Forwards/ups match Sedulous's probe + point-shadow convention.
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

    // Probe-capture faces resolve with their own view indices, far above any real view count -
    // their per-view bind-group cache slots never collide with (and thrash) the main views'.
    inline constexpr u32 kCaptureViewIndexBase = 64;

    // The single per-frame driver. Begin resets shared per-frame state; AddView collects a view
    // (extracting its draw list from a scene snapshot); End sizes the renderers' transient once
    // for the whole frame and composes every view. One driver, all views - no per-view object.
    class RenderFrame
    {
    public:
        RenderFrame(rhi::Device& device, RendererRegistry& registry, u32 framesInFlight,
                    ClusterSystem* clusters = nullptr, TonemapPass* tonemap = nullptr,
                    ShadowSystem* shadows = nullptr, IBLSystem* ibl = nullptr,
                    SkyPass* sky = nullptr, BloomPass* bloom = nullptr, TaaPass* taa = nullptr,
                    AoPass* ao = nullptr, FxaaPass* fxaa = nullptr) noexcept
            : m_registry(&registry), m_pass(device, framesInFlight), m_graph(&device),
              m_clusters(clusters), m_tonemap(tonemap), m_shadows(shadows), m_ibl(ibl), m_sky(sky),
              m_bloom(bloom), m_taa(taa), m_ao(ao), m_fxaa(fxaa)
        {
            m_device = &device;
        }

    private:
        // (Declared before the methods below - they appear in member-function SIGNATURES.)
        struct LocalShadowTile
        {
            Float4x4 viewProj;
            u32 x = 0, y = 0, w = 0, h = 0;
            Float3 cullCenter;
            f32 cullRadius = 0.0f;
        };
        struct Sphere
        {
            Float3 center;
            f32 radius = 0.0f;
        }; // a caster's world bounding sphere

        // All shadow inputs sourced from ONE scene. A frame can render several DISTINCT scenes
        // (editor pages side-by-side); each view reads its scene's context - never another's.
        // Pooled behind UniquePtrs: graph-execute lambdas capture the pointers, so addresses must
        // outlive the frame and never move when the pool grows.
        struct SceneShadowCtx
        {
            const ExtractedScene* scene = nullptr;
            Array<DrawItem> casters;            // camera-independent caster list (this scene)
            Array<Float4> casterBounds;         // aligned to casters: xyz=worldCenter, w=radius
            Array<Sphere> animatedSpheres;      // skinned-caster spheres (static-tile routing)
            Array<LocalShadowTile> staticTiles; // this scene's static-layer tiles (this frame)
            Array<LocalShadowTile> staticRenderTiles; // subset dirty THIS frame (rendered)
            Array<u32> staticTileDirty;               // per-static-tile refresh countdown
            u64 staticSig = 0; // static caster-set signature (cache-invalidation)
            const ExtractedScene* lastStaticScene =
                nullptr;                       // pool-slot reuse detection (dirty-all)
            u32 staticTileBase = 0;            // scene's first static tile (layout-shift detection)
            u32 entryBase = 0;                 // scene's first entry in m_localShadows
            IBLSystem::Context* ibl = nullptr; // this scene's IBL products (per-scene sky)
        };
        // One atlas-tile draw: the tile + the scene whose casters render into it.
        struct AtlasDraw
        {
            LocalShadowTile tile;
            SceneShadowCtx* ctx = nullptr;
        };

    public:
        // Test/debug introspection of the LAST End()'s shadow composition: which views had a
        // directional shadow and each view's scene entry base into the local-shadow buffer.
        struct ViewShadowDebug
        {
            bool directional = false;
            bool mapBound = false;
            u32 localEntryBase = 0;
        };
        [[nodiscard]] Span<const ViewShadowDebug> ViewShadowInfo() const noexcept;
        [[nodiscard]] Span<const GpuLocalShadow> LocalShadowEntries() const noexcept;

        // Begin a frame against the caller's encoder (the caller owns the encoder + targets).
        void Begin(rhi::CommandEncoder& encoder, u32 frameIndex);

        // Collect a view over `scene`. Builds its sorted draw list now (parallelizable later);
        // the GPU recording is deferred to End so transient buffers are sized once per frame.
        RenderView* AddView(const ExtractedScene& scene, const ViewCamera& camera,
                            const ViewSettings& settings, rhi::TextureView* target,
                            rhi::TextureFormat targetFormat, u32 width, u32 height,
                            const void* debugScene = nullptr, const void* sceneKey = nullptr);

        // Per-frame debug draw: the pass + the GLOBAL list (drawn in every view) + the SCREEN list (drawn
        // once, whole-window). Per-scene lists ride on each RenderView.
        void SetDebug(DebugDrawPass* pass, const debug::DebugDraw* global,
                      const debug::DebugDraw* screen) noexcept;

        // Scene-tier overlay sources (borrowed registry, owned by the subsystem; null/empty = no
        // overlay pass). Each view gets one shared Load-op pass on its final LDR output, after
        // post, before debug draw. Set once per frame before End.
        void SetSceneOverlays(const Array<ISceneOverlay*>* overlays) noexcept;

        // Per-frame decal pass (borrowed); null = no decals. Declared per view after sky, before AO.
        void SetDecal(DecalPass* pass) noexcept { m_decalPass = pass; }

        // Screen-space reflections (borrowed pass); null = no SSR. Declared per view after sky+decals, before
        // AO/TAA (so TAA stabilizes the march). Set once per frame before End.
        void SetSsr(SsrPass* pass) noexcept { m_ssr = pass; }
        // SSR enable + tunables (enabled=false leaves the scene HDR untouched).
        void SetSsrParams(bool enabled, const SsrPass::Params& params) noexcept;

        // Reflection-probe system (borrowed); null = no probes. Dirty probes are captured (6 faces each,
        // lit forward + sky into their cube-array slices) before the main views, so the forward can sample
        // the prefiltered result. Set once per frame before End.
        void SetProbes(ReflectionProbeSystem* probes) noexcept { m_probeSystem = probes; }

        // Turn on per-pass GPU timestamp profiling for the frame graph (idempotent).
        void EnableGpuProfiling() { m_graph.EnableGpuProfiling(); }

        // Linear exposure multiplier applied in the tonemap pass (scene/camera setting).
        void SetExposure(f32 exposure) noexcept { m_exposure = exposure; }
        // Bloom composite strength + soft-knee prefilter (intensity 0 = off).
        void SetBloom(f32 intensity, f32 threshold, f32 knee) noexcept;
        // Temporal AA: jitters the projection + resolves against per-view history (off = no jitter, no resolve).
        // blend = max history weight (stability), gamma = variance-clip box half-width, motionScale = how fast
        // history drops with motion.
        void SetTaa(bool on, f32 blend, f32 gamma, f32 motionScale) noexcept;
        // Ambient occlusion: mode (Off/GTAO/SSAO) + knobs. AO is applied to the HDR before TAA (strength 0
        // or Off = no AO). debugMode != 0 forces the AO on and shows the debug channel straight to screen.
        void SetAo(AoMode mode, f32 strength, f32 radius, f32 intensity,
                   i32 debugMode = 0) noexcept;
        // FXAA (TAA-off fallback): run FXAA after tonemap when TAA is off. Never stacked with TAA.
        void SetFxaa(bool on, f32 subpixelQuality) noexcept;
        // Instance-data sharing between the camera depth-prepass and the forward (build once vs twice). On by
        // default; off re-fills in the forward (for A/B / regression checks).
        void SetInstanceSharing(bool on) noexcept { m_instanceSharing = on; }
        [[nodiscard]] bool InstanceSharing() const noexcept { return m_instanceSharing; }
        // View-frustum culling: reject renderables outside the camera frustum when building each view's draw
        // list. Off by default (no-op for benchmarks that frame everything; a win for real off-screen-heavy
        // scenes). Directional shadows source casters independently so culling never drops shadow casters.
        void SetViewCulling(bool on) noexcept { m_viewCulling = on; }
        [[nodiscard]] bool ViewCulling() const noexcept { return m_viewCulling; }
        // Last frame's view-frustum cull totals, summed over active views (0/0 when culling was off). Read
        // before Begin() rewinds the view pool (e.g. from the sample's OnUpdate) to see the previous frame.
        void CullStats(u32& culled, u32& total) const noexcept;
        // Directional-shadow reach (world units, clamped to the camera far plane) + the far-fade width (also
        // world units - a fixed-thickness soft edge, distance-independent). Larger distance covers more ground
        // but spreads cascade texel density; the fade dissolves the coverage boundary so it doesn't pop along
        // a diagonal on a tilted camera.
        void SetShadowParams(f32 distance, f32 farFade) noexcept;
        // Append a per-pass GPU timing report + the per-pass CPU record cost (call only after device idle).
        void ReadGpuProfile(String& out);

        // Depth prepass body: emit the view's OPAQUE draws as depth-only from the camera POV (no bias, so
        // the depth equals the forward pass's exactly -> LessEqual early-Z). Masked isn't prepassed (the
        // depth-only shader can't alpha-discard); transparent doesn't write depth. Runs at graph execute.
        void RecordDepthPrepass(rhi::RenderPassEncoder& rp, const RenderView& view,
                                const RendererRegistry& registry, u32 viewIndex);

        // Resolve + emit the scene's casters as depth-only draws from the light's POV (the shadow depth
        // pass body). lightViewProj is the depth shader's "camera". Runs at graph execute time, before
        // the forward pass (which ReadTextures the shadow map), so it fills the rings ahead of forward.
        // Re-emit a caster list as depth-only draws from a light's POV. `casters` is camera-independent for
        // local lights (the scene-global list) or the view's draw list for cascades. When cullRadius > 0,
        // casters whose world bounding sphere doesn't intersect the light sphere (cullCenter, cullRadius)
        // are skipped - per-light shadow-caster culling (phase 5.4).
        void RecordShadowCasters(rhi::RenderPassEncoder& rp, Span<const DrawItem> casters,
                                 const RendererRegistry& registry, const Float4x4& lightViewProj,
                                 Float3 cullCenter = {}, f32 cullRadius = 0.0f,
                                 bool frustumCull = false, Span<const Float4> cullBounds = {});

        // Build the camera-independent shadow-caster list from a scene (opaque + masked meshes), grouped by
        // (mesh, material) so the depth pass batches them. Used by BOTH the directional cascades and the
        // local-light atlas tiles - built PER SCENE into that scene's context (multi-scene frames).
        void BuildShadowCasterList(const ExtractedScene& scene, SceneShadowCtx& ctx);

        // A signature over the STATIC local casters' transforms (quantized) + count. When it changes, the
        // cached static atlas layer is re-rendered for one frames-in-flight cycle. The Static-mode contract
        // is that caster GEOMETRY doesn't move, so only the lights themselves feed the signature.
        [[nodiscard]] u64 StaticCasterSignature(const ExtractedScene* scene) const;

        // Compose all collected views into the frame's encoder.
        void End();

        [[nodiscard]] usize ViewCount() const noexcept { return m_views.ActiveCount(); }

    private:
        RendererRegistry* m_registry;
        ForwardPass m_pass;
        rendergraph::RenderGraph m_graph;    // one graph per frame, composes all views
        ClusterSystem* m_clusters = nullptr; // borrowed; declares the per-view cluster build pass
        TonemapPass* m_tonemap =
            nullptr; // borrowed; HDR-resolve pass (null => forward writes LDR direct)
        ShadowSystem* m_shadows = nullptr; // borrowed; owns the directional shadow depth texture
        IBLSystem* m_ibl =
            nullptr; // borrowed; owns the IBL precompute products (env/SH/prefilter/BRDF)
        SkyPass* m_sky = nullptr; // borrowed; draws the visible environment background
        BloomPass* m_bloom =
            nullptr;              // borrowed; builds the HDR bloom pyramid (composited at tonemap)
        TaaPass* m_taa = nullptr; // borrowed; temporal AA resolve (per-view history)
        AoPass* m_ao = nullptr;   // borrowed; ambient occlusion (GTAO/SSAO) from the G-buffer
        FxaaPass* m_fxaa = nullptr;       // borrowed; TAA-off fallback AA (after tonemap)
        DecalPass* m_decalPass = nullptr; // borrowed; per-view screen-space decal pass
        SsrPass* m_ssr = nullptr; // borrowed; screen-space reflections (after sky/decals, pre-TAA)
        bool m_ssrEnabled = false;
        SsrPass::Params m_ssrParams{};
        ReflectionProbeSystem* m_probeSystem =
            nullptr;                  // borrowed; dirty probes captured before the main views
        RenderView m_captureViews[6]; // persistent 6-face capture views (outlive graph execute)
        u32 m_probeCaptureCursor = 0; // round-robin: which dirty probe to capture this frame
        DebugDrawPass* m_debugPass = nullptr;            // borrowed; per-view debug gizmo/text pass
        const debug::DebugDraw* m_debugGlobal = nullptr; // borrowed; global (all-views) debug list
        const debug::DebugDraw* m_debugScreen =
            nullptr; // borrowed; whole-window screen HUD (drawn once)
        rhi::Device* m_device = nullptr; // borrowed; outlives the frame
        const Array<ISceneOverlay*>* m_sceneOverlays =
            nullptr;           // borrowed; scene-tier overlay sources
        // Stencil-capable DS format for the scene-overlay pass (lazy probe; Undefined =
        // device has none, the pass stays color-only and overlay UI tessellates fills).
        rhi::TextureFormat m_overlayStencilFormat = rhi::TextureFormat::Undefined;
        bool m_overlayStencilProbed = false;
        f32 m_exposure = 1.0f; // linear exposure multiplier (tonemap input)
        bool m_fxaaEnabled = false;
        f32 m_fxaaSubpixel = 0.75f;
        bool m_instanceSharing = true; // share prepass->forward instance data (A/B toggle)
        bool m_viewCulling = false;    // view-frustum cull camera draw lists (default off)
        f32 m_shadowDistance = 300.0f; // directional-shadow reach (clamped to camera farZ)
        f32 m_shadowFarFade = 40.0f;   // far-fade width in world units
        AoMode m_aoMode = AoMode::Off;
        i32 m_aoDebug = 0;
        f32 m_aoStrength = 0.6f;
        f32 m_aoRadius = 0.5f;
        f32 m_aoIntensity = 1.0f;
        f32 m_bloomIntensity = 0.05f; // 0 = bloom off
        f32 m_bloomThreshold = 1.0f;
        f32 m_bloomKnee = 0.6f;
        bool m_taaEnabled = false;
        f32 m_taaBlend = 0.97f;
        f32 m_taaGamma = 1.25f;
        f32 m_taaMotionScale = 32.0f;
        u32 m_jitterIndex = 0; // Halton phase, advances once per frame (mod 8)
        bool m_anyViewTaa =
            false; // set per frame when any view enables TAA (drives the jitter advance)
        // Motion vectors + TAA: last frame's view-proj + jitter per view index (this frame's collected into
        // m_curViewProj/m_curJitter, swapped in at End). Camera motion = prev vs current (jittered) view-proj.
        Array<Float4x4> m_prevViewProj;
        Array<Float4x4> m_curViewProj;
        Array<Float2> m_prevJitter;
        Array<Float2> m_curJitter;
        Array<ResolvedDraw>
            m_prepassResolved; // reused depth-draw buffer for the camera depth prepass
        Array<ResolvedDraw> m_shadowResolved; // reused depth-draw buffer for the shadow pass
        // Local-light (spot/point) shadows (5.3): per-frame caster matrices + their atlas tiles. Members
        // (not locals) so the atlas pass's execute lambda can reference the tiles for the frame's lifetime.
        Array<UniquePtr<SceneShadowCtx>>
            m_sceneShadowPool;       // slot k = k-th distinct scene this frame
        Array<u32> m_viewSceneIndex; // per view: index into the pool (~0u = no scene)
        Array<GpuLocalShadow>
            m_localShadows; // scenes' entries CONCATENATED, caster order per scene
        Array<AtlasDraw>
            m_rtAtlasDraws; // realtime atlas layer (all scenes; re-rendered every frame)
        Array<AtlasDraw>
            m_staticAtlasDraws; // static atlas layer, ALL tiles (all scenes; cache source)
        Array<AtlasDraw> m_staticRenderDraws; // static layer tiles dirty THIS frame (all scenes)
        Array<DrawItem> m_shadowCullScratch;  // per-tile sphere-culled subset (reused)
        Array<ViewShadowDebug> m_viewShadowDebug; // test/debug: last End()'s per-view composition
        RenderViewPool m_views;
        Array<DrawItem> m_sortScratch; // reused radix-sort ping-pong buffer
        rhi::CommandEncoder* m_encoder = nullptr;
        u32 m_frameIndex = 0;
    };

} // namespace draconic::render
