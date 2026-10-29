// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:sky` partition.
///
/// Draws the environment as the visible background: a fullscreen triangle at the far plane, depth-
/// tested (LessEqual, no write) against the forward depth so it only fills pixels no geometry covered,
/// reconstructing a world-space view ray per pixel (inverse view-proj) and sampling the env cubemap.
/// Runs after the forward pass, into the same (HDR) color target, before tonemap - so the sky is in
/// the linear working space and gets tonemapped with the scene.

module;
#include "Core/Prelude.h"

export module foundation.render:sky;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data;        // kGVelocityFormat (sky writes camera-motion velocity for TAA)

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // The sky's uniform (cbuffer Sky, sky_common.hlsli); Integration.ShaderBlocks checks it against
    // the compiled shader, member by member.
    struct GpuSky
    {
        Float4x4 invViewProj;  // inverse of the UNJITTERED view-proj (a stable sky ray under TAA)
        Float4x4 prevViewProj; // last frame's view-proj (motion vectors)
        Float4 camPosIntensity; // xyz = camera world position, w = the sky background's display multiplier
        Float4 sunDir;          // xyz = light direction, w = the sun's angular size (degrees)
        Float4 sunColor;        // rgb = sun colour, w = sun intensity
        Float4 jitter;          // xy = this frame's NDC jitter, zw = last frame's
        Float4 skyFlags; // x = sky-ray Y sign (-1 flips the sampled ray on Y-flip targets); yzw unused
    };
    static_assert(sizeof(GpuSky) == 208, "cbuffer Sky layout drift");

    class SkyPass
    {
    public:
        SkyPass(rhi::Device& device, shaders::ShaderSystem& shaders, u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders), m_fif(framesInFlight < 1 ? 1 : framesInFlight)
        {
        }
        ~SkyPass() { Shutdown(); }
        SkyPass(const SkyPass&) = delete;
        SkyPass& operator=(const SkyPass&) = delete;

        Status Initialize();

        // Declare the sky pass: load `color`, depth-test (read-only) against `depth`, read `envH`, draw a
        // fullscreen triangle sampling the env cube along the per-pixel world ray. `invViewProj` = inverse
        // of this view's view*proj; `camPos`/`intensity` scale the result.
        void DeclareSky(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                        rendergraph::RGHandle velocity, rendergraph::RGHandle depth,
                        rendergraph::RGHandle envH, rhi::TextureView* envView,
                        rhi::TextureFormat colorFormat, rhi::TextureFormat depthFormat,
                        const Float4x4& invViewProj, const Float4x4& prevViewProj, Float2 jitter,
                        Float2 prevJitter, const Float3& camPos, f32 backgroundIntensity,
                        const Float3& sunDir, f32 sunSize, const Float3& sunColor, f32 sunIntensity,
                        i32 vpX, i32 vpY, u32 vpW, u32 vpH, u32 frameIndex, u32 viewIndex,
                        u64 envUid, u32 samples = 1,
                        rendergraph::RGSubresourceRange colorSub = {});

    private:
        static constexpr u32 kMaxFIF = 4;
        static constexpr u32 kMaxViews = 16; // mains 0..9, probe-capture faces 10..15
        static constexpr u32 kMaxSlots = kMaxViews * kMaxFIF;
        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat colorFmt,
                                            rhi::TextureFormat depthFmt, u32 samples);

        // Env identity = (view pointer, context uid): the uid catches pointer REUSE after an IBL
        // context eviction, per the versioned-cache rule.
        rhi::BindGroup* EnsureBindGroup(u32 slot, rhi::TextureView* envView, u64 envUid,
                                        const GpuSky& u);

        void Shutdown();

        struct Slot
        {
            rhi::Buffer* ubo = nullptr;
            rhi::BindGroup* bindGroup = nullptr;
            rhi::TextureView* env = nullptr;
            u64 envUid = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_fif = 2;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::TextureFormat m_colorFormat = rhi::TextureFormat::Undefined;
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Undefined;
        u32 m_pipelineSampleCount = 1; // scene-pass MSAA count the cached pipeline was built for
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        // Deferred pipeline free: a rebuild (shader reload / MSAA-count toggle) retires the old pipeline
        // here and DeclareSky drains it after kRetireFrames, never freeing one still in-flight.
        struct RetiredPipe
        {
            rhi::RenderPipeline* p = nullptr;
            u32 left = 0;
        };
        static constexpr u32 kRetireFrames = 3;
        Array<RetiredPipe> m_retiredPipes;
        u32 m_lastRetireFrame = 0xFFFFFFFFu;
        rhi::Sampler* m_sampler = nullptr;
        Slot m_slots[kMaxSlots] = {};
    };

} // namespace foundation::render
