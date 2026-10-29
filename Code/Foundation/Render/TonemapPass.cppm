// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:tonemap` partition.
///
/// The HDR resolve: the forward pass renders linear HDR into a transient (RGBA16F); this fullscreen
/// pass reads it, applies exposure + a tonemap operator + the display OETF, and writes the LDR
/// target. Keeps the renderer in a strict linear working space that the IBL/post stack is
/// designed against. The operator is selectable: a trivial clamp or AgX.

module;
#include "Core/Prelude.h"

export module foundation.render:tonemap;

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

    /// Auto-exposure inputs for the tonemap: the exposure pass's adapted 1x1 (imported handle
    /// + raw view + generation) and the key/clamp window. A null view (or enabled=false)
    /// disables.
    struct TonemapAutoExposure
    {
        bool enabled = false;
        rendergraph::RGHandle handle{};
        rhi::TextureView* view = nullptr;
        u64 generation = 0;
        f32 key = 0.18f;
        f32 minExposure = 0.0625f;
        f32 maxExposure = 16.0f;
    };
    /// Display-referred grading strip LUT for the tonemap (a static cooked texture; uid keys
    /// the bind-group cache, never the pointer). lutSize < 2 (or a null view) disables.
    struct TonemapGrading
    {
        rhi::TextureView* view = nullptr;
        u64 uid = 0;
        f32 lutSize = 0.0f;
        f32 intensity = 1.0f;
    };

    // The tonemap's push block (TonemapPush, tonemap.ps.hlsl): twenty scalars, each a float in the
    // shader (the flags as 0/1, the operator as 0 clamp, 1 AgX, 2 the present copy). The pads are
    // scalars, never a float3: a WGSL uniform wants a vec3 on a 16-byte boundary.
    // Integration.ShaderBlocks checks it against the compiled shader, member by member.
    struct GpuTonemapPush
    {
        f32 exposure = 1.0f;
        f32 bloomIntensity = 0.0f;
        Float2 uvScale{1.0f, 1.0f}; // fullscreen uv -> the view's sub-rect
        Float2 uvOffset{0.0f, 0.0f};
        f32 aoStrength = 0.0f;
        f32 debugShowAo = 0.0f;
        f32 toneOperator = 0.0f; // the shader's Operator
        f32 flipSceneY = 0.0f;
        f32 autoExposure = 0.0f;
        f32 autoKey = 0.18f;
        f32 autoMin = 0.0f;
        f32 autoMax = 0.0f;
        f32 gradeIntensity = 0.0f;
        f32 lutSize = 0.0f;
        f32 encodeOutput = 0.0f; // 1 = the target stores the display encoding as written (plain UNORM)
        f32 rotation = 0.0f;     // the ContentRotation, as a float
        f32 _pad0 = 0.0f;
        f32 _pad1 = 0.0f;
    };
    static_assert(sizeof(GpuTonemapPush) == 80, "push TonemapPush layout drift");

    // Tonemaps an HDR transient into an LDR target. Owns the fullscreen pipeline + per-(view,frame)
    // bind groups over the (transient) HDR view. Declared once per view, after that view's forward pass.
    class TonemapPass
    {
    public:
        TonemapPass(rhi::Device& device, shaders::ShaderSystem& shaders,
                    u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight)
        {
        }

        ~TonemapPass() { Shutdown(); }
        TonemapPass(const TonemapPass&) = delete;
        TonemapPass& operator=(const TonemapPass&) = delete;

        Status Initialize();

        [[nodiscard]] rhi::TextureFormat HdrFormat() const noexcept { return kHdrFormat; }

        // Declare the tonemap pass: read `hdr`, write `ldr` (clearColor decides clear vs load), into the
        // view's viewport sub-rect. The execute builds/binds the HDR bind group (the view is a transient,
        // resolved at execute time) and draws a fullscreen triangle.
        void DeclareTonemap(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                            rendergraph::RGHandle bloom, rendergraph::RGHandle ao,
                            rendergraph::RGHandle ldr, bool clearColor,
                            const rhi::ClearColor& clear, rhi::TextureFormat ldrFormat, i32 vpX,
                            i32 vpY, u32 vpW, u32 vpH, u32 frameIndex, u32 viewIndex,
                            f32 exposure = 1.0f, f32 bloomIntensity = 0.0f,
                            Float2 uvScale = Float2{1, 1}, Float2 uvOffset = Float2{0, 0},
                            f32 aoStrength = 0.0f, bool debugShowAo = false, bool agx = true,
                            bool sceneYFlipped = false,
                            const TonemapAutoExposure& autoExposure = {},
                            const TonemapGrading& grading = {}, bool copyOnly = false,
                            ContentRotation turn = ContentRotation::None);

    private:
        static constexpr rhi::TextureFormat kHdrFormat = rhi::TextureFormat::RGBA16Float;
        static constexpr u32 kMaxFramesInFlight = 8;
        static constexpr u32 kMaxViews = 8;
        // Two uses per view a frame: the scene's tone map, and a scaled view's present copy.
        static constexpr u32 kMaxSlots = kMaxViews * kMaxFramesInFlight * 2;

        // Build the fullscreen pipeline for `fmt` (rebuilt if the LDR target format changes - usually one).
        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat fmt);

        // One bind group per (view, frame) slot, kept while EVERY input is the same view of the
        // same texture (BindGroupInputs: the HDR, bloom and AO transients, the adapted exposure,
        // the grading LUT, each with its generation). A combined generation alone collided: the
        // exposure's two ping-pong textures folded into one number with the transients' could
        // land on last frame's value, and the group kept the other exposure texture, so every
        // other frame showed a stale exposure (PaperKid's Block 1 flickered, 2026-10-29).
        using BindInputs = BindGroupInputs<5>;
        rhi::BindGroup* EnsureBindGroup(u32 slot, const BindInputs& inputs);

        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;

        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        // Per-FORMAT pipeline cache: one frame can hold views with different target formats
        // (the HDR viewports + the thumbnail stage's readback target), and a destroy-on-format-
        // mismatch single slot destroyed a pipeline the same encoder's earlier view still
        // referenced (a validation error, then a crash). Small fixed set; entries rebuild only
        // on shader hot reload.
        static constexpr usize kMaxPipelineFormats = 4;
        struct PipelineEntry
        {
            rhi::RenderPipeline* pipeline = nullptr;
            rhi::TextureFormat format = rhi::TextureFormat::Undefined;
            u64 shaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        };
        PipelineEntry m_pipelines[kMaxPipelineFormats] = {};

        rhi::Sampler* m_sampler = nullptr; // linear-clamp, for the bloom composite
        rhi::BindGroup* m_bindGroups[kMaxSlots] = {};
        BindInputs m_bgInputs[kMaxSlots] = {}; // what each slot's group was built from
    };

} // namespace foundation::render
