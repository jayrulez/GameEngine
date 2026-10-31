// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:ao` partition.
///
/// Ambient occlusion. Two interchangeable generators feed one shared pipeline:
///   - GTAO: Ground-Truth AO (Jimenez horizon integration over screen-space slices).
///   - SSAO: classic hemisphere-kernel occlusion (Crysis-style).
/// Both read the opaque depth and the octahedral view normal (G-buffer) and write an R8 AO
/// transient. They share one depth-aware bilateral blur, one apply pass (a multiply into the HDR
/// before TAA, so the resolve stabilizes it), one bind-group cache and one set of debug channels.
/// The two modes are mutually exclusive (AoMode). All targets are render-graph transients, sized
/// per view.

module;
#include "Core/Prelude.h"

export module foundation.render:ao;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // The AO passes' push blocks (ao_gtao, ao_ssao, ao_blur and ao_apply .ps.hlsl: GtaoPush,
    // SsaoPush, BlurPush, ApplyPush); Integration.ShaderBlocks checks each against the compiled
    // shader, member by member.
    struct GpuGtaoPush
    {
        Float4x4 invProj{};
        Float2 texelSize{};
        f32 radius = 0.5f;
        f32 intensity = 1.0f;
        f32 projScaleY = 1.0f;
        i32 frameMod = 0;
        i32 debugMode = 0;
        i32 orthographic = 0; // 1: view rays are parallel, no divide by depth
    };
    static_assert(sizeof(GpuGtaoPush) == 96, "push GtaoPush layout drift");
    struct GpuSsaoPush
    {
        Float4x4 invProj{};
        Float2 texelSize{};
        Float2 jitter{};
        f32 projXX = 1.0f;
        f32 projYY = 1.0f;
        f32 radius = 0.5f;
        f32 intensity = 1.0f;
        f32 bias = 0.05f;
        i32 sampleCount = 16;
        i32 debugMode = 0;
        i32 orthographic = 0; // 1: view rays are parallel, no divide by depth
        i32 pad = 0;
    };
    static_assert(sizeof(GpuSsaoPush) == 116, "push SsaoPush layout drift");
    struct GpuAoBlurPush
    {
        Float2 dir{};
        Float2 texelSize{};
        f32 depthSigma = 120.0f;
        f32 p0 = 0, p1 = 0, p2 = 0;
    };
    static_assert(sizeof(GpuAoBlurPush) == 32, "push BlurPush layout drift");
    struct GpuAoApplyPush
    {
        f32 strength = 1.0f;
        f32 flipAoY = 0; // 1 = sample AO with flipped uv.y (on Y-flip targets such as WebGPU)
        f32 p1 = 0, p2 = 0;
    };
    static_assert(sizeof(GpuAoApplyPush) == 16, "push ApplyPush layout drift");

    // Which AO generator to run. The modes are exclusive: both write the same AO buffer.
    enum class AoMode : u32
    {
        Off = 0,
        GTAO = 1,
        SSAO = 2
    };

    // Owns both AO generators and the shared blur and apply pipelines. Produces an AO transient (R8)
    // applied to the HDR before TAA.
    class AoPass
    {
    public:
        static constexpr rhi::TextureFormat kAoFormat = rhi::TextureFormat::R8Unorm;

        AoPass(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }
        ~AoPass() { Shutdown(); }
        AoPass(const AoPass&) = delete;
        AoPass& operator=(const AoPass&) = delete;

        Status Initialize();

        [[nodiscard]] rhi::TextureFormat AoFormat() const noexcept { return kAoFormat; }

        // Declare AO for one view; returns the blurred AO handle, or invalid if the mode is Off. invProj is
        // the inverse camera projection and proj the camera projection (GTAO uses proj(1,1); SSAO uses
        // proj(0,0) and proj(1,1)).
        [[nodiscard]] rendergraph::RGHandle DeclareAo(rendergraph::RenderGraph& graph,
                                                      rendergraph::RGHandle depth,
                                                      rendergraph::RGHandle normal, u32 w, u32 h,
                                                      const Float4x4& invProj, const Float4x4& proj,
                                                      f32 radius, f32 intensity, u32 frameIndex,
                                                      AoMode mode, i32 debugMode = 0);

        // Multiply the AO into `hdr` (out = hdr * lerp(1, ao, strength)), writing and returning a fresh HDR
        // transient. Run it before the TAA resolve so TAA stabilizes the AO; applied after TAA, the AO
        // wobbles under jitter.
        [[nodiscard]] rendergraph::RGHandle DeclareApply(rendergraph::RenderGraph& graph,
                                                         rendergraph::RGHandle hdr,
                                                         rendergraph::RGHandle ao, u32 w, u32 h,
                                                         f32 strength);

    private:
        static constexpr rhi::TextureFormat kHdrFormat =
            rhi::TextureFormat::RGBA16Float; // matches the scene HDR
        // A fixed byte buffer so a generate push (GTAO or SSAO) can be captured by value into the pass lambda.
        struct PushBuf
        {
            u8 data[128] = {};
            u32 size = 0;
            template <class T>
            static PushBuf From(const T& v)
            {
                static_assert(sizeof(T) <= 128,
                              "AO push exceeds the portable 128-byte push-constant limit");
                PushBuf b;
                b.size = sizeof(T);
                const u8* src = reinterpret_cast<const u8*>(&v);
                for (u32 i = 0; i < b.size; ++i)
                {
                    b.data[i] = src[i];
                }
                return b;
            }
        };

        rhi::PipelineLayout* MakePipelineLayout(usize pushSize);

        void DeclareBlur(rendergraph::RenderGraph& graph, rendergraph::RGHandle ao,
                         rendergraph::RGHandle depth, rendergraph::RGHandle out, u32 w, u32 h,
                         Float2 texel, Float2 dir);

        rhi::RenderPipeline* MakePipeline(StringView name, rhi::PipelineLayout* layout,
                                          rhi::TextureFormat fmt = kAoFormat);

        // Advance the deferred-free list once per frame, freeing bind groups retired long enough ago to be
        // idle. The graph aliases transients, so cached sets are replaced mid-frame; a replaced set goes to
        // the retire list and is freed after kRetireFrames rather than while it may still be in flight.
        void Tick(u32 frameIndex);

        // Two-texture bind group (t0, t1) and sampler, cached by the t0 view and the combined generation.
        // Every AO pass shares the layout: generation binds depth and normal, blur binds AO and depth, apply
        // binds HDR and AO.
        rhi::BindGroup* EnsureBindGroup(rhi::TextureView* a, rhi::TextureView* bView,
                                        u64 generation);

        void Shutdown();

        struct Entry
        {
            rhi::BindGroup* bg = nullptr;
            rhi::TextureView* b = nullptr;
            u64 gen = 0;
        };
        struct Retired
        {
            rhi::BindGroup* bg = nullptr;
            u32 left = 0;
        };
        static constexpr u32 kRetireFrames =
            4; // frames-in-flight headroom before a replaced set is idle

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_gtaoLayout = nullptr;
        rhi::PipelineLayout* m_ssaoLayout = nullptr;
        rhi::PipelineLayout* m_blurLayout = nullptr;
        rhi::PipelineLayout* m_applyLayout = nullptr;
        rhi::RenderPipeline* m_gtaoPipeline = nullptr;
        rhi::RenderPipeline* m_ssaoPipeline = nullptr;
        rhi::RenderPipeline* m_blurPipeline = nullptr;
        rhi::RenderPipeline* m_applyPipeline = nullptr;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_sampler = nullptr;
        HashMap<rhi::TextureView*, Entry> m_bindGroups;
        Array<Retired> m_retired;
        u32 m_lastFrame = 0xFFFFFFFFu;
    };

} // namespace foundation::render
