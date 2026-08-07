/// Draconic::Render - the `:tonemap` partition.
///
/// The HDR resolve: the forward pass renders linear HDR into a transient (RGBA16F); this fullscreen
/// pass reads it, applies exposure + a tonemap operator + the display OETF, and writes the LDR
/// target. Keeps the renderer in a strict linear working space (docs/design/renderer.md §12) - the
/// foundation IBL/post are designed against. CM1a uses a trivial clamp; CM1b swaps in AgX.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:tonemap;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

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
                            bool sceneYFlipped = false);

    private:
        static constexpr rhi::TextureFormat kHdrFormat = rhi::TextureFormat::RGBA16Float;
        static constexpr u32 kMaxFramesInFlight = 8;
        static constexpr u32 kMaxViews = 8;
        static constexpr u32 kMaxSlots = kMaxViews * kMaxFramesInFlight;

        // Build the fullscreen pipeline for `fmt` (rebuilt if the LDR target format changes - usually one).
        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat fmt);

        // One bind group per (view, frame) slot over its HDR transient view. Rebuilt when the transient's
        // GENERATION changes (the graph stamps a fresh id whenever a different physical texture backs the
        // transient - e.g. on resize). Pointer identity alone is unsafe: a freed view address can be reused
        // by the new allocation, leaving the cached bind group pointing at a destroyed texture.
        rhi::BindGroup* EnsureBindGroup(u32 slot, rhi::TextureView* hdrView,
                                        rhi::TextureView* bloomView, rhi::TextureView* aoView,
                                        u64 generation);

        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;

        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::TextureFormat m_pipelineFormat = rhi::TextureFormat::Undefined;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)

        rhi::Sampler* m_sampler = nullptr; // linear-clamp, for the bloom composite
        rhi::BindGroup* m_bindGroups[kMaxSlots] = {};
        rhi::TextureView* m_bgViews[kMaxSlots] = {};
        rhi::TextureView* m_bgBloom[kMaxSlots] = {};
        rhi::TextureView* m_bgAo[kMaxSlots] = {};
        u64 m_bgGen[kMaxSlots] = {}; // transient generation the cached BG was built for
    };

} // namespace draconic::render
