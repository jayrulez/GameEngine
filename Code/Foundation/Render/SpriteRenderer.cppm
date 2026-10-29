// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:sprite_renderer` partition.
///
/// A Renderer that draws textured billboard quads (sprites). Ported from SedulousEngine's
/// SpriteRenderer/sprite.hlsl: a 6-vertex quad is generated in the vertex shader from SV_VertexID and
/// hardware-instanced, with per-sprite data (position/size/uv-rect/tint/orientation) fed as instance-
/// stepped vertex attributes. Sprites register for the Transparent category and interleave with
/// transparent meshes by depth (per-item renderer dispatch), sharing the blended forward pass:
/// alpha/additive blend, depth-test LessEqual against the scene depth, no depth write, cull none.
///
/// Batching: within a resolved run, consecutive sprites sharing (texture, blend mode) fuse into one
/// instanced draw. Non-indexed geometry isn't expressible through ResolvedDraw/EmitDraw (it always
/// DrawIndexed), so a static 6-index buffer [0..5] drives the SV_VertexID quad.

module;
#include "Core/Prelude.h"
#include <cstddef> // offsetof

export module foundation.render:sprite_renderer;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import foundation.shaders.system;
import :data;
import :pipeline;
import :resources;

using namespace foundation::core;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // The sprite pass's view (cbuffer SpriteView, sprite.vs.hlsl); Integration.ShaderBlocks checks it
    // against the compiled shader, member by member.
    struct GpuSpriteView
    {
        Float4x4 viewProj;
        Float4x4 view;
    };
    static_assert(sizeof(GpuSpriteView) == 128, "cbuffer SpriteView layout drift");

    // One instance record uploaded to the instance stream (96 bytes). The vertex layout reads it as
    // six float4s at locations 0 to 5; sprite.vs.hlsl names each attribute for the fields it
    // carries, so the offsets below are the shader's contract.
    struct SpriteInstance
    {
        Float3 position; // world centre
        f32 width;
        f32 height;
        f32 orientation; // the orientation mode's ordinal, as a float (the shader rounds it)
        f32 _pad0, _pad1;
        Float4 tint;
        Float4 uvRect;    // xy uv min, zw uv size
        Float3 axisRight; // the entity's right (EntityOriented)
        f32 _pad2;
        Float3 axisUp; // the entity's up (EntityOriented)
        f32 _pad3;
    };
    static_assert(sizeof(SpriteInstance) == 96);
    static_assert(offsetof(SpriteInstance, position) == 0);
    static_assert(offsetof(SpriteInstance, width) == 12);
    static_assert(offsetof(SpriteInstance, height) == 16);
    static_assert(offsetof(SpriteInstance, orientation) == 20);
    static_assert(offsetof(SpriteInstance, tint) == 32);
    static_assert(offsetof(SpriteInstance, uvRect) == 48);
    static_assert(offsetof(SpriteInstance, axisRight) == 64);
    static_assert(offsetof(SpriteInstance, axisUp) == 80);

    class SpriteRenderer final : public Renderer
    {
    public:
        SpriteRenderer(rhi::Device& device, shaders::ShaderSystem& shaders,
                       u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_instanceRing(device, framesInFlight, sizeof(SpriteInstance),
                             rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
                             u8"sprite.instances"),
              m_viewRing(device, framesInFlight, kViewSlotSize,
                         rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"sprite.view")
        {
        }
        ~SpriteRenderer() override { Shutdown(); }
        SpriteRenderer(const SpriteRenderer&) = delete;
        SpriteRenderer& operator=(const SpriteRenderer&) = delete;

        Status Initialize();

        // ---- Renderer ----
        [[nodiscard]] Span<const RenderCategory> SupportedCategories() const override;

        void PrepareFrame(u32 maxDraws, u32 frameIndex) override;

        void Resolve(const RenderRecordContext& ctx, Span<const DrawItem> items,
                     Array<ResolvedDraw>& out) override;

        void FinishFrame() override;

        /// Wire the frames-in-flight retire queue (web-safe ring grows). Null = drain.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept
        {
            m_instanceRing.SetRetireQueue(retire);
            m_viewRing.SetRetireQueue(retire);
        }

    private:
        static constexpr u32 kMaxViews = 8;
        static constexpr u64 kViewSlotSize = 256; // GpuSpriteView padded to the dynamic-uniform alignment

        // One bind group over the whole view ring (per-draw dynamic offset selects the slot). Rebuilt ONLY
        // when the ring reallocates (generation bump) - which drains the GPU first (Reserve's WaitIdle) - so
        // we never free a descriptor set an in-flight frame still references.
        rhi::BindGroup* EnsureViewBindGroup();

        rhi::BindGroup* EnsureTextureBindGroup(rhi::TextureView* tex);

        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat colorFormat, bool additive);

        void Shutdown();

        // One cached pipeline per (target format, blend mode): in-scene sprites render into
        // the HDR pass while post-tonemap sprites render into the LDR target IN THE SAME
        // frame - a single slot per blend mode thrashed destroy/create mid-recording
        // (destroying a pipeline the frame's command buffer already bound).
        struct Pipelines
        {
            rhi::RenderPipeline* pso = nullptr;
            rhi::TextureFormat format = rhi::TextureFormat::Undefined;
            bool additive = false;
            u64 shaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        DynamicUniformRing m_instanceRing;
        DynamicUniformRing m_viewRing;
        rhi::BindGroupLayout* m_viewLayout = nullptr;
        rhi::BindGroupLayout* m_texLayout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::Sampler* m_sampler = nullptr;
        rhi::Buffer* m_indexBuffer = nullptr;
        rhi::BindGroup* m_viewBg = nullptr;
        u32 m_viewBgGen = 0xFFFFFFFFu; // generation the view bind group was built for
        Array<Pipelines> m_pipelines; // one entry per (format, additive) seen
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Depth32Float;
        // Cached per-texture bind groups. Keyed by view POINTER for lookup speed, but every hit
        // validates the view's uniqueId - dynamic textures (UI render targets) are destroyed and
        // their address reused, and a stale descriptor samples a dead image view (device lost).
        struct TexBindGroup
        {
            rhi::BindGroup* bindGroup = nullptr;
            u64 viewId = 0; // TextureView::uniqueId the group was built for
        };
        HashMap<rhi::TextureView*, TexBindGroup> m_texBindGroups;
    };

} // namespace foundation::render
