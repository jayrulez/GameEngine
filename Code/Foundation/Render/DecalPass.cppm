// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:decal_pass` partition.
///
/// Screen-space projected decals: reconstruct the world position under each pixel from the scene
/// depth, transform it into a decal's oriented unit box, clip to the box, and alpha-blend the decal
/// texture onto the lit HDR. The result is a "sprayed" sticker that lands on whatever surface is under
/// the box, including animated meshes, since it reads the depth buffer.
///
/// Runs after the forward and sky passes and before AO and TAA, blending into the HDR so decals are
/// TAA-resolved. The shader assumes row-major matrices, D3D-style [0,1] clip depth and a top-origin uv
/// under the negative viewport (the same reconstruction as AoPass).
///
/// Each decal draws a fullscreen triangle, which is robust across split-screen sub-rects and avoids box
/// winding and culling pitfalls; the box test lives in the fragment shader. The receiver normal for the
/// angle fade comes from the reconstructed world-position derivatives (ddx, ddy). A per-decal box mesh,
/// and sampling the normal G-buffer instead, would be cheaper.

module;
#include "Core/Prelude.h"
#include <cstddef> // offsetof

export module foundation.render:decal_pass;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data;
import :resources;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // The scene HDR format decals blend into (matches TonemapPass::HdrFormat and AoPass).
    inline constexpr rhi::TextureFormat kDecalHdrFormat = rhi::TextureFormat::RGBA16Float;

    // One decal, uploaded to the per-decal UBO (set 1, dynamic offset). 240 bytes.
    struct DecalUniforms
    {
        Float4x4 world;    // decal box world transform (scale = box size); projects along local +Z
        Float4x4 invWorld; // world to decal-local (the box clip)
        Float4x4
            invViewProj; // (ndc, depth) to world, for depth reconstruction (this view, jittered)
        Float4 color;    // rgba tint
        Float2 invTargetSize; // 1 / the full target's size
        f32 fadeStartCos = 1; // cos(the angle fade's start)
        f32 fadeEndCos = 1;   // cos(its end)
        f32 yFlip = 1;        // interpolant Y correction (+1 Vulkan, -1 Y-flip targets)
        f32 _pad0 = 0, _pad1 = 0, _pad2 = 0;
    };
    static_assert(sizeof(DecalUniforms) == 240);
    static_assert(offsetof(DecalUniforms, invTargetSize) == 208);
    static_assert(offsetof(DecalUniforms, fadeStartCos) == 216);
    static_assert(offsetof(DecalUniforms, fadeEndCos) == 220);
    static_assert(offsetof(DecalUniforms, yFlip) == 224);

    class DecalPass
    {
    public:
        DecalPass(rhi::Device& device, shaders::ShaderSystem& shaders, u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_decalRing(device, framesInFlight, sizeof(DecalUniforms) <= 256 ? 256u : 512u,
                          rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"decal.uniforms")
        {
        }
        ~DecalPass() { Shutdown(); }
        DecalPass(const DecalPass&) = delete;
        DecalPass& operator=(const DecalPass&) = delete;

        Status Initialize();

        // Bracket the frame once, before the per-view loop. The per-decal uniform ring must not be reset per
        // view: DeclareDecals is called once per view, and the GPU reads the ring at graph-execute time,
        // after all views have recorded. Per-view resets would make every view read the last view's slot
        // (its InvViewProj), so decals would reconstruct with the wrong matrix and swim across views.
        void BeginFrame(u32 frameIndex);
        void EndFrame() { m_decalRing.EndFrame(); }

        // Blend the scene's decals into `hdr` for one view. `viewProj` is the view's (jittered) view-proj,
        // whose inverse reconstructs world position from `depth`. Allocates its own ring slots (accumulating
        // across views within the frame). No-op if there are no decals. Bracket with BeginFrame and EndFrame.
        void DeclareDecals(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                           rendergraph::RGHandle depth, Span<const DecalInstance> decals,
                           const Float4x4& viewProj, u32 w, u32 h, i32 vpX, i32 vpY, u32 vpW,
                           u32 vpH, u32 samples = 1);

        /// Wire the frames-in-flight retire queue (web-safe ring grows). Null means drain.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept
        {
            m_decalRing.SetRetireQueue(retire);
        }

    private:
        struct Draw
        {
            u32 offset;
            rhi::TextureView* texture;
        };

        static constexpr u32 kRetireFrames = 3;
        static constexpr u32 kMaxDecalsPerFrame =
            256; // ring capacity (across all views), reserved once

        rhi::RenderPipeline* MakePipeline(u32 samples);

        // One bind group over the decal-uniform ring (dynamic offset per decal). Rebuilt only on ring
        // realloc (generation), which drains the GPU first, so an in-flight set is never freed.
        rhi::BindGroup* EnsureUboBindGroup();

        // Depth bind group (set 0), cached by (view, generation) with a deferred-free retire list. The depth
        // transient is aliased and recreated, so a raw-pointer cache would free a set an in-flight frame
        // still uses.
        rhi::BindGroup* EnsureDepthBindGroup(rhi::TextureView* depth, u64 generation);

        rhi::BindGroup* EnsureTextureBindGroup(rhi::TextureView* tex);

        void Tick(u32 frameIndex);

        void Shutdown();

        struct Retired
        {
            rhi::BindGroup* bg = nullptr;       // a superseded bind group, or...
            rhi::RenderPipeline* pipeline = nullptr; // ...a superseded pipeline (MSAA-count rebuild)
            u32 left = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        DynamicUniformRing m_decalRing;
        rhi::BindGroupLayout* m_depthLayout = nullptr;
        rhi::BindGroupLayout* m_uboLayout = nullptr;
        rhi::BindGroupLayout* m_texLayout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        u32 m_pipelineSampleCount = 1;   // scene-pass MSAA count the cached pipeline was built for
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_depthSampler = nullptr;
        rhi::Sampler* m_texSampler = nullptr;
        rhi::BindGroup* m_uboBg = nullptr;
        u32 m_uboBgGen = 0xFFFFFFFFu;
        rhi::BindGroup* m_depthBg = nullptr;
        rhi::TextureView* m_depthView = nullptr;
        u64 m_depthGen = 0;
        u32 m_lastFrame = 0xFFFFFFFFu;
        bool m_reserved = false;
        Array<Retired> m_retired;
        // Keyed by view pointer, validated by TextureView::uniqueId on every hit (a destroyed
        // dynamic texture's address can be reused; see SpriteRenderer::TexBindGroup).
        struct TexBindGroup
        {
            rhi::BindGroup* bindGroup = nullptr;
            u64 viewId = 0;
        };
        HashMap<rhi::TextureView*, TexBindGroup> m_texBindGroups;
    };

} // namespace foundation::render
