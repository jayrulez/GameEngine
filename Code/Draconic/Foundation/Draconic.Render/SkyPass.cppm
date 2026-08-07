/// Draconic::Render - the `:sky` partition.
///
/// Draws the environment as the visible background: a fullscreen triangle at the far plane, depth-
/// tested (LessEqual, no write) against the forward depth so it only fills pixels no geometry covered,
/// reconstructing a world-space view ray per pixel (inverse view-proj) and sampling the env cubemap.
/// Runs after the forward pass, into the same (HDR) color target, before tonemap - so the sky is in
/// the linear working space and gets tonemapped with the scene.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:sky;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data;        // kGVelocityFormat (sky writes camera-motion velocity for TAA)

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

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
                        u64 envUid, rendergraph::RGSubresourceRange colorSub = {});

    private:
        static constexpr u32 kMaxFIF = 4;
        static constexpr u32 kMaxViews = 16; // mains 0..9, probe-capture faces 10..15
        static constexpr u32 kMaxSlots = kMaxViews * kMaxFIF;
        struct SkyUniform
        {
            Float4x4 invViewProj;
            Float4x4 prevViewProj;
            Float4 camPosIntensity;
            Float4 sunDir;
            Float4 sunColor;
            Float4 jitter;
            Float4 skyFlags; // x = sky-ray Y sign (-1 flips the sampled ray on Y-flip targets)
        };

        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat colorFmt,
                                            rhi::TextureFormat depthFmt);

        // Env identity = (view pointer, context uid): the uid catches pointer REUSE after an IBL
        // context eviction, per the versioned-cache rule.
        rhi::BindGroup* EnsureBindGroup(u32 slot, rhi::TextureView* envView, u64 envUid,
                                        const SkyUniform& u);

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
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_sampler = nullptr;
        Slot m_slots[kMaxSlots] = {};
    };

} // namespace draconic::render
