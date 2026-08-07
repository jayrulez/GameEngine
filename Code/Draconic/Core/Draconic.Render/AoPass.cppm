/// Draconic::Render - the `:ao` partition.
///
/// Ambient occlusion. Two interchangeable generators feed one shared pipeline:
///   - GTAO: Ground-Truth AO (Jimenez horizon integration over screen-space slices).
///   - SSAO: classic hemisphere-kernel occlusion (Crysis-style), ported from Sedulous.
/// Both consume the opaque depth + octahedral view-normal (G-buffer), write an R8 AO transient,
/// then share the SAME depth-aware bilateral blur, the SAME apply pass (multiply into the HDR before
/// TAA so the resolve stabilizes it), the SAME bind-group cache, and the SAME debug channels. The two
/// modes are mutually exclusive (AoMode). All targets are render-graph transients (sized per view).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:ao;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // Which AO generator to run (mutually exclusive - both write the same AO buffer).
    enum class AoMode : u32
    {
        Off = 0,
        GTAO = 1,
        SSAO = 2
    };

    // Owns both AO generators + the shared blur/apply pipelines. Produces an AO transient (R8) applied to
    // the HDR before TAA.
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

        // Declare AO for one view; returns the (blurred) AO handle, or invalid if mode is Off. invProj =
        // inverse camera projection, proj = camera projection (GTAO uses proj(1,1); SSAO uses proj(0,0)/(1,1)).
        [[nodiscard]] rendergraph::RGHandle DeclareAo(rendergraph::RenderGraph& graph,
                                                      rendergraph::RGHandle depth,
                                                      rendergraph::RGHandle normal, u32 w, u32 h,
                                                      const Float4x4& invProj, const Float4x4& proj,
                                                      f32 radius, f32 intensity, u32 frameIndex,
                                                      AoMode mode, i32 debugMode = 0);

        // Multiply the AO into `hdr` (out = hdr * lerp(1, ao, strength)) into a fresh HDR transient, returned.
        // Run BEFORE the TAA resolve so TAA stabilizes the AO (post-TAA application wobbles under jitter).
        [[nodiscard]] rendergraph::RGHandle DeclareApply(rendergraph::RenderGraph& graph,
                                                         rendergraph::RGHandle hdr,
                                                         rendergraph::RGHandle ao, u32 w, u32 h,
                                                         f32 strength);

    private:
        static constexpr rhi::TextureFormat kHdrFormat =
            rhi::TextureFormat::RGBA16Float; // matches the scene HDR
        struct GtaoPushC
        {
            Float4x4 invProj{};
            Float2 texelSize{};
            f32 radius = 0.5f;
            f32 intensity = 1.0f;
            f32 projScaleY = 1.0f;
            i32 frameMod = 0;
            i32 debugMode = 0;
            i32 pad = 0;
        };
        struct SsaoPushC
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
        };
        struct BlurPushC
        {
            Float2 dir{};
            Float2 texelSize{};
            f32 depthSigma = 120.0f;
            f32 p0 = 0, p1 = 0, p2 = 0;
        };
        struct ApplyPushC
        {
            f32 strength = 1.0f;
            f32 flipAoY = 0; // 1 = sample AO with flipped uv.y (Y-flip targets: WebGPU today)
            f32 p1 = 0, p2 = 0;
        };

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

        // Advance the deferred-free list once per frame (frees bind groups retired long enough ago to be
        // idle). The graph aliases transients, so a raw-pointer cache thrashes mid-frame - replaced sets go
        // to the retire list (freed after kRetireFrames) instead of being freed while still in-flight.
        void Tick(u32 frameIndex);

        // Two-texture bind group (t0, t1) + sampler, cached by (t0 view, combined generation). Shared by every
        // AO pass (gen depth+normal, blur ao+depth, apply hdr+ao) - same layout.
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

} // namespace draconic::render
