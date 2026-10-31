// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:bloom` partition.
///
/// HDR bloom via a downsample/upsample pyramid (Jimenez, "Next Generation Post Processing in Call of
/// Duty": a 13-tap downsample and a 9-tap tent upsample, additive). The first downsample applies a
/// soft-knee threshold and a Karis average to keep fireflies out. The pyramid is built from
/// render-graph transients (sized per view, so no resize handling); the tonemap pass composites the
/// result additively. Runs on the linear HDR scene, before tonemap.

module;
#include "Core/Prelude.h"

export module foundation.render:bloom;

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

    // The bloom down/up passes' push block (BloomPush, bloom_common.hlsli); Integration.ShaderBlocks
    // checks it against the compiled shader, member by member.
    struct GpuBloomPush
    {
        Float2 srcTexel{};
        f32 threshold = 1.0f;
        f32 knee = 0.5f;
        i32 firstPass = 0;
        f32 pad0 = 0, pad1 = 0, pad2 = 0;
    };
    static_assert(sizeof(GpuBloomPush) == 32, "push BloomPush layout drift");

    // Builds the bloom pyramid from an HDR input and returns the mip-0 (half-res) accumulated bloom
    // handle, which the tonemap composites. Owns the down and up pipelines and the sampler; the pyramid
    // is graph transients.
    class BloomPass
    {
    public:
        static constexpr rhi::TextureFormat kBloomFormat = rhi::TextureFormat::RGBA16Float;
        static constexpr u32 kMaxLevels = 7;

        BloomPass(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }
        ~BloomPass() { Shutdown(); }
        BloomPass(const BloomPass&) = delete;
        BloomPass& operator=(const BloomPass&) = delete;

        Status Initialize();

        [[nodiscard]] rhi::TextureFormat Format() const noexcept { return kBloomFormat; }

        // Declare the pyramid for one view's HDR input. Returns the accumulated bloom (mip 0, half-res), or
        // an invalid handle if the view is too small. threshold and knee gate what blooms (soft-knee
        // prefilter).
        [[nodiscard]] rendergraph::RGHandle DeclareBloom(rendergraph::RenderGraph& graph,
                                                         rendergraph::RGHandle hdr, u32 vpW,
                                                         u32 vpH, f32 threshold, f32 knee);

    private:
        rhi::RenderPipeline* MakePipeline(StringView name, bool additive);

        // Bind group over a transient source view, cached by view pointer and generation. Transients are
        // pooled with stable generations across frames, so the cache settles; a resize bumps the generation.
        rhi::BindGroup* EnsureBindGroup(rhi::TextureView* view, u64 generation);

        void Shutdown();

        struct Entry
        {
            rhi::BindGroup* bg = nullptr;
            u64 gen = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_downPipeline = nullptr;
        rhi::RenderPipeline* m_upPipeline = nullptr;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_sampler = nullptr;
        HashMap<rhi::TextureView*, Entry> m_bindGroups;
    };

} // namespace foundation::render
