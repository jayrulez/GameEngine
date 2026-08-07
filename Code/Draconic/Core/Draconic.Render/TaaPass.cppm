/// Draconic::Render - the `:taa` partition.
///
/// Temporal anti-aliasing resolve (ported from Sedulous taa.frag.hlsl). Blends the current jittered
/// HDR frame with the reprojected history: closest-depth motion selection, a YCoCg variance clip, a
/// Catmull-Rom history sample, a luma/motion-adaptive blend, and a depth-disocclusion reject. Runs in
/// linear HDR after the scene is composed (opaque+sky+transparent) and before bloom/tonemap. Per-view
/// color-history ping-pong (persistent), managed here; jitter is applied to the projection by the caller.
///
/// Depth-disocclusion (ported from Sedulous, hardens ghost-on-reveal): compares this frame's linearized
/// depth against the previous frame's depth at the reprojected historyUV, rejecting history on a large
/// relative mismatch (a surface revealed/occluded). We avoid a separate prev-depth ping-pong by carrying
/// the previous frame's LINEAR depth in the color-history texture's alpha channel (unused downstream);
/// linear depth in half-float keeps ~0.05% relative precision everywhere vs the 10% reject threshold.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:taa;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // Owns the TAA resolve pipeline + per-view color-history ping-pong. One per renderer.
    class TaaPass
    {
    public:
        static constexpr rhi::TextureFormat kHistoryFormat = rhi::TextureFormat::RGBA16Float;
        static constexpr u32 kMaxViews = 8;

        TaaPass(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }
        ~TaaPass() { Shutdown(); }
        TaaPass(const TaaPass&) = delete;
        TaaPass& operator=(const TaaPass&) = delete;

        Status Initialize();

        [[nodiscard]] rhi::TextureFormat HistoryFormat() const noexcept { return kHistoryFormat; }

        // Resolve TAA for one view: reads the jittered HDR (`current`), the previous history, the motion +
        // depth targets; writes the resolved HDR (a new transient, returned) and the next-frame history.
        // `blendFactor` ~0.95. Returns the resolved handle, or `current` unchanged if the view is invalid.
        [[nodiscard]] rendergraph::RGHandle
        DeclareTaa(rendergraph::RenderGraph& graph, rendergraph::RGHandle current,
                   rendergraph::RGHandle motion, rendergraph::RGHandle depth, u32 viewIndex, u32 w,
                   u32 h, f32 blendFactor, f32 varianceGamma, f32 motionScale, f32 nearPlane,
                   f32 farPlane);

    private:
        struct TaaPush
        {
            Float2 texelSize{};
            f32 blendFactor = 0.97f;
            f32 historyValid = 0.0f;
            f32 varianceGamma = 1.25f;
            f32 motionScale = 32.0f;
            f32 nearPlane = 0.1f;
            f32 farPlane = 1000.0f;
        };

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

        rhi::RenderPipeline* MakePipeline();

        // Bind group over the 4 inputs, cached by (current view, generation) - the transients are pooled with
        // stable generations, the history view is stable per size, so this rebuilds only on resize.
        rhi::BindGroup* EnsureBindGroup(rhi::TextureView* cur, rhi::TextureView* histPrev,
                                        rhi::TextureView* motion, rhi::TextureView* depth,
                                        u64 generation);

        void Shutdown();

        struct Entry
        {
            rhi::BindGroup* bg = nullptr;
            rhi::TextureView* cur = nullptr;
            u64 gen = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_pointSampler = nullptr;
        rhi::Sampler* m_linearSampler = nullptr;
        ViewHistory m_views[kMaxViews];
        HashMap<rhi::TextureView*, Entry> m_bindGroups;
    };

} // namespace draconic::render
