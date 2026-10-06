// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:msaa_resolve` partition.
///
/// Scene-pass MSAA first-sample (sample 0) resolve.
/// Resolves the MSAA depth + G-buffer aux (normal / velocity / material) into single-sample 1x
/// targets the 1x post consumers (GTAO / SSR / TAA / motion reprojection) read UNCHANGED. Depth is
/// written via SV_Depth into a real depth-format target, so no consumer's binding
/// changes. The scene COLOR is resolved separately by the hardware resolve attachment (averaged - the
/// real edge AA); this pass never touches color. Sample-0 (not averaged) keeps AO/SSR/TAA identical to
/// the single-sample quality.

module;
#include "Core/Prelude.h"

export module foundation.render:msaa_resolve;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data; // kGNormalFormat / kGVelocityFormat / kGMaterialFormat
import :bind_group_cache;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // The 1x resolved depth + aux handles the post stack consumes when a view is MSAA.
    struct MsaaResolveOutputs
    {
        rendergraph::RGHandle depth{};
        rendergraph::RGHandle normal{};
        rendergraph::RGHandle velocity{};
        rendergraph::RGHandle material{};
        rendergraph::RGHandle albedo{};
    };

    // Owns the fullscreen sample-0 resolve pipeline. Produces 1x depth + aux transients from the MSAA
    // opaque G-buffer. One instance shared across views (the pipeline is view-independent; the
    // bind group is cached per input-generation).
    class MsaaResolvePass
    {
    public:
        MsaaResolvePass(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }
        ~MsaaResolvePass() { Shutdown(); }
        MsaaResolvePass(const MsaaResolvePass&) = delete;
        MsaaResolvePass& operator=(const MsaaResolvePass&) = delete;

        Status Initialize();

        // Declare the first-sample resolve of the MSAA depth + aux into fresh 1x transients.
        // `depthFormat` is the scene depth format (the resolved depth matches it, written via SV_Depth).
        // Returns invalid handles (no pass emitted) if the pipeline is unavailable.
        [[nodiscard]] MsaaResolveOutputs DeclareResolve(rendergraph::RenderGraph& graph,
                                                        rendergraph::RGHandle msaaDepth,
                                                        rendergraph::RGHandle msaaNormal,
                                                        rendergraph::RGHandle msaaVelocity,
                                                        rendergraph::RGHandle msaaMaterial,
                                                        rendergraph::RGHandle msaaAlbedo,
                                                        rhi::TextureFormat depthFormat, u32 w, u32 h);

    private:
        rhi::RenderPipeline* MakePipeline(rhi::TextureFormat depthFormat);
        // The multisampled inputs: normal, velocity, material, depth, albedo.
        using BindInputs = BindGroupInputs<5>;
        rhi::BindGroup* EnsureBindGroup(const BindInputs& inputs);
        void Shutdown();

        rhi::Device* m_device = nullptr;
        shaders::ShaderSystem* m_shaders = nullptr;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::TextureFormat m_pipelineDepthFormat = rhi::TextureFormat::Undefined;
        u64 m_pipelineShaderVersion = 0;

        // Bind groups keyed by the depth view, each kept only while all four inputs (views and
        // their textures' generations) are the ones it was built from: transients are pooled per
        // frame, and checking depth alone kept other frames' normal, velocity or material bound.
        BindGroupCache<5> m_bindGroups;
    };
}
