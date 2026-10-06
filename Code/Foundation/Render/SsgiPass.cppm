// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:ssgi` partition (GI tier 1).
///
/// Screen-space global illumination - SsrPass's DIFFUSE twin. A trace pass marches a few
/// cosine-weighted hemisphere rays per pixel against the depth buffer and gathers the lit HDR
/// at each hit as one-bounce radiance; a resolve pass temporally accumulates the (necessarily
/// noisy) trace against a per-view ping-pong history and composites into the HDR. The forward
/// already lit every surface with the sky's ambient as if nothing stood in the way; a ray that
/// hits geometry finds what really is in that direction, so the composite swaps the sky light
/// for the bounce there: out = hdr + albedo * (bounce - hitWeight * skyRadiance) * intensity,
/// skyRadiance being the pixel's own ambient (the scene's SH sky over its normal, plus the flat
/// fill). The bounce is tinted by the G-buffer's diffuse albedo, and occluded corners darken
/// instead of lifting. (A unit-albedo, purely additive v1 washed colours out and erased contact
/// shadows, counting the sky twice.)
///
/// Declared AFTER decals and BEFORE SSR (reflections then see the bounce), same slot rules as
/// :ssr: per-view history, generation-keyed bind-group caches with deferred frees, trace noise
/// rotated per frame so a still camera converges under the temporal blend.

module;
#include "Core/Prelude.h"
#include <cstddef> // offsetof

export module foundation.render:ssgi;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :bind_group_cache;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // Owns the SSGI pipelines. Produces a fresh HDR transient (scene + one screen-space bounce).
    class SsgiPass
    {
    public:
        SsgiPass(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }
        ~SsgiPass() { Shutdown(); }
        SsgiPass(const SsgiPass&) = delete;
        SsgiPass& operator=(const SsgiPass&) = delete;

        Status Initialize();

        // Tunables (per-view intensity comes from the authored PostProcessSettings).
        struct Params
        {
            f32 intensity = 1.0f;     // additive bounce strength
            f32 maxRadiance = 4.0f;   // per-hit gather clamp (firefly suppression)
            f32 radius = 3.0f;        // view-space gather radius (world units)
            f32 thickness = 0.6f;     // hit acceptance band (view-space linear depth)
            i32 maxSteps = 24;        // march budget PER RAY
            i32 rayCount = 2;         // hemisphere rays per pixel (1..4)
            bool temporal = true;     // temporal accumulate (reproject + variance-clip history)
            f32 historyBlend = 0.95f; // max history weight (GI is noisier than SSR - lean on it)
            f32 varianceGamma = 2.5f; // neighborhood clip half-width in stddevs (post-blur)
            f32 depthSigma = 0.05f;   // spatial-denoise depth tolerance (relative)
            f32 motionScale = 24.0f;  // how fast history drops with motion
            f32 ghostReject = 3.0f;   // history-vs-current luma-diff rejection strength
            i32 debug = 0;            // >0 = show the accumulated GI raw (no composite)
        };

        // The ambient the forward lit the view's scene with, which a hit replaces: the scene's SH
        // sky (when its IBL is active; else none) scaled by its diffuse dimmer, plus the flat fill,
        // and the camera's world matrix to take the G-buffer's view-space normal to world space.
        struct Sky
        {
            rhi::Buffer* shBuffer = nullptr;   // the scene's SH9 irradiance (null = none)
            rendergraph::RGHandle shHandle{};  // read by the composite (orders the SH bake first)
            u64 generation = 0;                // the IBL context's (bind group key)
            f32 iblDiffuse = 1.0f;             // EnvironmentSettings' sky-lighting dimmer
            Float3 ambient{0.0f, 0.0f, 0.0f};  // the flat fill (linear)
            Float4x4 viewToWorld = Float4x4::Identity();
        };

        // Declare trace + resolve for one view; returns the composited HDR handle
        // ("ssgi.scene"), or `hdr` unchanged when the pass cannot run.
        [[nodiscard]] rendergraph::RGHandle
        DeclareSsgi(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                    rendergraph::RGHandle depth, rendergraph::RGHandle normal,
                    rendergraph::RGHandle velocity, rendergraph::RGHandle albedo, const Sky& sky,
                    u32 w, u32 h, i32 vx, i32 vy, u32 vw, u32 vh, const Float4x4& invProj,
                    const Float4x4& proj, const Params& p, u32 viewIndex, u32 frameIndex);

    private:
        static constexpr rhi::TextureFormat kHdrFormat =
            rhi::TextureFormat::RGBA16Float; // matches the scene HDR

        // Byte-identical to the HLSL SsgiPush.
        struct SsgiPushC
        {
            Float4x4 invProj{};
            Float2 vpMin{0.0f, 0.0f};
            Float2 vpSize{1.0f, 1.0f};
            Float2 jitter{};
            f32 projXX = 1.0f;
            f32 projYY = 1.0f;
            f32 thickness = 0.6f;
            f32 radius = 3.0f;
            i32 maxSteps = 24;
            i32 rayCount = 2;
            f32 ySign = -1.0f;
            u32 frameIndex = 0;
            f32 maxRadiance = 4.0f;
            f32 pad0 = 0.0f;
        };
        static_assert(sizeof(SsgiPushC) <= 128,
                      "SSGI push exceeds the portable 128-byte push-constant limit");

        // Byte-identical to the HLSL SsgiDownPush.
        struct SsgiDownPushC
        {
            Float2 srcTexelSize{};
            Float2 pad0{};
        };
        static_assert(sizeof(SsgiDownPushC) <= 128,
                      "SSGI downsample push exceeds the portable 128-byte limit");

        // Byte-identical to the HLSL SsgiBlurPush.
        struct SsgiBlurPushC
        {
            Float2 texelSize{};
            f32 depthSigma = 0.05f;
            f32 pad0 = 0.0f;
            Float4x4 invProj{};
            Float2 vpMin{0.0f, 0.0f};
            Float2 vpSize{1.0f, 1.0f};
            f32 ySign = -1.0f;
            f32 pad1 = 0.0f;
        };
        static_assert(sizeof(SsgiBlurPushC) <= 128,
                      "SSGI blur push exceeds the portable 128-byte limit");

        // Byte-identical to the HLSL SsgiResolvePush.
        struct SsgiResolvePushC
        {
            Float2 vpMin{0.0f, 0.0f};
            Float2 vpSize{1.0f, 1.0f};
            Float2 texelSize{};
            f32 blendFactor = 0.92f;
            f32 historyValid = 0.0f;
            f32 varianceGamma = 1.5f;
            f32 motionScale = 24.0f;
            i32 temporalOn = 1;
            i32 debug = 0;
            f32 ghostReject = 3.0f;
            f32 intensity = 1.0f;
            // HLSL starts a float4 on a 16-byte boundary; Float4 here is only 4-byte aligned, so
            // the pad keeps the C++ offsets the shader's (a shifted read scrambled the sky term).
            f32 pad0 = 0.0f;
            f32 pad1 = 0.0f;
            Float4 viewToWorld[3]{};       // rows of the camera's world rotation (view -> world normal)
            Float4 skyAmbient{};           // rgb = flat fill, w = SH sky dimmer (0 = no SH sky)
        };
        static_assert(offsetof(SsgiResolvePushC, viewToWorld) == 64,
                      "SSGI resolve push: the float4 block must start where HLSL puts it");
        static_assert(sizeof(SsgiResolvePushC) <= 128,
                      "SSGI resolve push exceeds the portable 128-byte limit");

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
        bool CreateDownPipeline();
        bool CreateBlurPipeline();
        bool CreateResolvePipeline();

        static u64 Combine(rendergraph::RenderGraph& g, rendergraph::RGHandle a,
                           rendergraph::RGHandle b, rendergraph::RGHandle c);

        // Advance the deferred-free list once per frame (replaced sets must outlive
        // in-flight frames - the :ssr discipline).
        void Tick(u32 frameIndex);

        // Downsample bind group (full-res hdr + linear sampler), cached by (hdr view, gen).
        rhi::BindGroup* EnsureDownBindGroup(rhi::TextureView* hdr, u64 generation);

        // Trace bind group (scene, depth, normal + 2 samplers), cached by (scene view, gen).
        rhi::BindGroup* EnsureBindGroup(rhi::TextureView* scene, rhi::TextureView* depth,
                                        rhi::TextureView* normal, u64 generation);

        // Blur bind group (raw gi + depth + point sampler), cached by (gi view, gen).
        rhi::BindGroup* EnsureBlurBindGroup(rhi::TextureView* gi, rhi::TextureView* depth,
                                            u64 generation);

        // Resolve bind group (gi, history, velocity, hdr, albedo, normal, sky SH + 2 samplers), one
        // per history view, kept only while its transient inputs (gi, velocity, hdr, albedo, normal:
        // views and generations; the SH buffer and its context's generation) are the ones it was
        // built from (BindGroupCache).
        using ResolveInputs = BindGroupInputs<5>;
        rhi::BindGroup* EnsureResolveBindGroup(rhi::TextureView* histPrev, const ResolveInputs& inputs);

        void Shutdown();

        struct Entry
        {
            rhi::BindGroup* bg = nullptr;
            rhi::TextureView* depth = nullptr;
            rhi::TextureView* normal = nullptr;
            u64 gen = 0;
        };
        struct DownEntry
        {
            rhi::BindGroup* bg = nullptr;
            u64 gen = 0;
        };
        struct BlurEntry
        {
            rhi::BindGroup* bg = nullptr;
            rhi::TextureView* depth = nullptr;
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
        rhi::BindGroupLayout* m_downLayout = nullptr;
        rhi::PipelineLayout* m_downPipelineLayout = nullptr;
        rhi::RenderPipeline* m_downPipeline = nullptr; // radiance prefilter (quarter-res)
        rhi::BindGroupLayout* m_blurLayout = nullptr;
        rhi::PipelineLayout* m_blurPipelineLayout = nullptr;
        rhi::RenderPipeline* m_blurPipeline = nullptr; // spatial denoise (trace -> resolve)
        rhi::BindGroupLayout* m_resolveLayout = nullptr;
        rhi::PipelineLayout* m_resolvePipelineLayout = nullptr;
        rhi::RenderPipeline* m_resolvePipeline = nullptr; // temporal resolve + composite
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_sampler = nullptr;       // point: depth/reconstruction
        rhi::Sampler* m_linearSampler = nullptr; // linear: radiance gather
        ViewHistory m_views[kMaxViews];
        HashMap<rhi::TextureView*, Entry> m_bindGroups;
        HashMap<rhi::TextureView*, DownEntry> m_downBindGroups;
        HashMap<rhi::TextureView*, BlurEntry> m_blurBindGroups;
        BindGroupCache<5> m_resolveBindGroups;
        rhi::Buffer* m_dummyShBuffer = nullptr; // bound when the scene has no SH sky (never read)
        Array<Retired> m_retired;
        u32 m_lastFrame = 0xFFFFFFFFu;
    };

} // namespace foundation::render
