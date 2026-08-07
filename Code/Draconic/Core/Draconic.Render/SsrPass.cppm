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

export module draconic.render:ssr;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // Owns the SSR pipeline. Produces a fresh HDR transient (scene with reflections composited in).
    class SsrPass
    {
    public:
        SsrPass(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }
        ~SsrPass() { Shutdown(); }
        SsrPass(const SsrPass&) = delete;
        SsrPass& operator=(const SsrPass&) = delete;

        Status Initialize();

        // Tunables (driven from the render subsystem / UI).
        struct Params
        {
            f32 intensity = 1.0f;
            f32 thickness = 0.5f; // view-space linear-depth hit-acceptance band
            f32 edgeFade = 0.1f;  // uv fraction faded at each screen border
            f32 roughnessCutoff =
                0.8f; // roughness at/above which SSR is off (rougher = blurred cone-gather)
            f32 glossy = 1.0f;        // glossy blur scale (0 = sharp mirror, higher = blurrier)
            i32 maxSteps = 96;        // ray-march samples along the segment
            i32 debug = 0;            // 0=off, 1=raw refl, 2=hit uv, 3=weight, 4=reflect dir
            bool temporal = true;     // temporal accumulate (reproject + variance-clip history)
            f32 historyBlend = 0.88f; // max history weight on stable pixels
            f32 varianceGamma = 1.0f; // neighborhood clip half-width (stddevs)
            f32 motionScale = 24.0f;  // how fast history drops with motion
            f32 ghostReject =
                6.0f; // history-vs-current luma-diff rejection (higher = less ghosting)
        };

        // Reflect the scene into a fresh HDR transient (returned). Two passes: trace -> reflection buffer,
        // then resolve (temporal accumulate + composite). w,h = full target size; vx/vy/vw/vh = this view's
        // sub-rect (split-screen views reconstruct/project in local uv, sample the full texture). velocity =
        // the G-buffer motion vectors (for reprojection). invProj/proj = camera inverse-proj / proj.
        [[nodiscard]] rendergraph::RGHandle
        DeclareSsr(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                   rendergraph::RGHandle depth, rendergraph::RGHandle normal,
                   rendergraph::RGHandle material, rendergraph::RGHandle velocity, u32 w, u32 h,
                   i32 vx, i32 vy, u32 vw, u32 vh, const Float4x4& invProj, const Float4x4& proj,
                   const Params& p, u32 viewIndex, u32 frameIndex);

    private:
        static constexpr rhi::TextureFormat kHdrFormat =
            rhi::TextureFormat::RGBA16Float; // matches the scene HDR
        // Byte-identical to the HLSL SsrPush (124 bytes, under the portable 128-byte push limit).
        struct SsrPushC
        {
            Float4x4 invProj{};
            Float2 vpMin{0.0f, 0.0f};
            Float2 vpSize{1.0f, 1.0f};
            Float2 jitter{};
            f32 projXX = 1.0f;
            f32 projYY = 1.0f;
            f32 thickness = 0.5f;
            f32 intensity = 1.0f;
            f32 edgeFade = 0.1f;
            f32 roughCutoff = 0.6f;
            i32 maxSteps = 96;
            // Scene-NDC Y sign for the shader's uv<->ndc conversions: -1 on Vulkan (negative
            // viewport), +1 on Y-flip targets (NeedsClipSpaceYFlip). Mirrors ShadowParams.y.
            // (Occupies the old frameMod slot - the dither is static by design, frameMod was 0.)
            f32 ySign = -1.0f;
            i32 debug = 0;
            f32 glossy = 1.0f;
        };
        static_assert(sizeof(SsrPushC) <= 128,
                      "SSR push exceeds the portable 128-byte push-constant limit");

        // Byte-identical to the HLSL SsrResolvePush.
        struct SsrResolvePushC
        {
            Float2 vpMin{0.0f, 0.0f};
            Float2 vpSize{1.0f, 1.0f};
            Float2 texelSize{};
            f32 blendFactor = 0.88f;
            f32 historyValid = 0.0f;
            f32 varianceGamma = 1.0f;
            f32 motionScale = 24.0f;
            i32 temporalOn = 1;
            i32 debug = 0;
            f32 ghostReject = 6.0f;
        };
        static_assert(sizeof(SsrResolvePushC) <= 128,
                      "SSR resolve push exceeds the portable 128-byte limit");

        static constexpr u32 kMaxViews = 8;
        struct ViewHistory
        {
            rhi::Texture* tex[2] = {};
            rhi::TextureView* view[2] = {};
            rhi::ResourceState state[2] = {rhi::ResourceState::Undefined,
                                           rhi::ResourceState::Undefined};
            u32 w = 0, h = 0, cur = 0;
            bool valid = false;
        };

        bool EnsureHistory(ViewHistory& hist, u32 w, u32 h);
        void DestroyHistory(ViewHistory& hist);
        bool CreateTracePipeline();
        bool CreateResolvePipeline();

        // Combine four transient generations into one cache key (same FNV-ish mixing as :ao).
        static u64 Combine(rendergraph::RenderGraph& g, rendergraph::RGHandle a,
                           rendergraph::RGHandle b, rendergraph::RGHandle c,
                           rendergraph::RGHandle d);

        // Advance the deferred-free list once per frame (the graph aliases transients, so replaced sets must
        // outlive in-flight frames before being freed - a raw-pointer cache would thrash mid-frame).
        void Tick(u32 frameIndex);

        // Four-texture bind group (t0..t3) + sampler, cached by (scene view, combined generation).
        rhi::BindGroup* EnsureBindGroup(rhi::TextureView* scene, rhi::TextureView* depth,
                                        rhi::TextureView* normal, rhi::TextureView* material,
                                        u64 generation);

        // Resolve bind group (reflection, history, velocity, hdr + 2 samplers), cached by (history view, gen).
        rhi::BindGroup* EnsureResolveBindGroup(rhi::TextureView* refl, rhi::TextureView* histPrev,
                                               rhi::TextureView* velocity, rhi::TextureView* hdr,
                                               u64 generation);

        void Shutdown();

        struct Entry
        {
            rhi::BindGroup* bg = nullptr;
            rhi::TextureView* depth = nullptr;
            rhi::TextureView* normal = nullptr;
            rhi::TextureView* material = nullptr;
            u64 gen = 0;
        };
        struct ResolveEntry
        {
            rhi::BindGroup* bg = nullptr;
            rhi::TextureView* refl = nullptr;
            rhi::TextureView* velocity = nullptr;
            rhi::TextureView* hdr = nullptr;
            u64 gen = 0;
        };
        struct Retired
        {
            rhi::BindGroup* bg = nullptr;
            u32 left = 0;
        };
        static constexpr u32 kRetireFrames = 4;

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr; // trace
        rhi::BindGroupLayout* m_resolveLayout = nullptr;
        rhi::PipelineLayout* m_resolvePipelineLayout = nullptr;
        rhi::RenderPipeline* m_resolvePipeline = nullptr; // temporal resolve + composite
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_sampler = nullptr;                // point: depth/reconstruction
        rhi::Sampler* m_linearSampler = nullptr;          // linear: glossy color gather
        ViewHistory m_views[kMaxViews];
        HashMap<rhi::TextureView*, Entry> m_bindGroups;
        HashMap<rhi::TextureView*, ResolveEntry> m_resolveBindGroups;
        Array<Retired> m_retired;
        u32 m_lastFrame = 0xFFFFFFFFu;
    };

} // namespace draconic::render
