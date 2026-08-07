/// Draconic::Render - the `:ibl` partition.
///
/// Image-Based Lighting: the split-sum environment pipeline (ported from Sedulous.Renderer/IBL with
/// improvements), PER SCENE. A frame can render several scenes side-by-side (editor pages), each
/// with its own authored sky - so the products live in per-scene CONTEXTS pooled by scene identity,
/// and every view binds ITS scene's products (the set-0 bind group is per-view downstream):
///   - env cubemap (256², RGBA16F)        : the source radiance, written from the scene's sky source
///                                          (procedural gradient / analytic / HDR equirect / cubemap).
///   - SH9 diffuse irradiance (buffer)    : 9 RGB spherical-harmonic coeffs projected from the env
///                                          cube (REPLACES Sedulous's 32² irradiance cube - cheaper,
///                                          smoother, seamless). Improvement over Sedulous.
///   - GGX prefiltered specular (cube+mips): Karis split-sum, importance-sampled per roughness mip.
/// Shared across scenes: the BRDF integration LUT (sky-independent), all pipelines/layouts/samplers,
/// and the PROGRAMMATIC equirect/cubemap pixel sources (SetEquirect/SetCubemap - tools/samples).
///
/// Precompute runs only when a context's source is dirty; products are persistent, imported every
/// frame so the forward pass orders after + samples them (set 0). Context generations come from ONE
/// system-wide counter, so a generation value never collides across contexts - downstream bind-group
/// caches can key on it alone ([[bind-group-cache-versioning]]).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:ibl;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data;        // SkySnapshot / SkyMode / ExtractedScene (context identity)

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // Owns the per-scene IBL contexts + the passes that build their products. One per renderer.
    class IBLSystem
    {
    public:
        static constexpr u32 kEnvResolution = 256;
        static constexpr u32 kEnvMips = 5; // env mip pyramid (256..16) for prefilter PDF sampling
        static constexpr u32 kPrefilterRes = 256;
        static constexpr u32 kPrefilterMips = 5; // roughness = mip / (kPrefilterMips - 1)
        static constexpr u32 kBrdfResolution = 256;
        static constexpr u32 kShCoeffCount = 9;
        // A context unused for this many frames is evicted (its scene's page closed). Long enough
        // that nothing in flight can still reference the products.
        static constexpr u64 kEvictAfterFrames = 600;

        // ---- Per-scene context: products + sky state for ONE scene ---------------------------------
        class Context
        {
        public:
            // Products bound into the forward set 0 (per-view downstream).
            [[nodiscard]] rhi::TextureView* PrefilterView() const noexcept
            {
                return m_prefilterView;
            }
            [[nodiscard]] rhi::Buffer* ShBuffer() const noexcept { return m_shBuffer; }
            // Unique ACROSS contexts (one system-wide counter) - safe as a sole cache key.
            [[nodiscard]] u64 Generation() const noexcept { return m_generation; }
            // Stable per-context identity (creation-stamped, never reused) - downstream caches
            // that key on the env VIEW pair it with this (pointer reuse after eviction).
            [[nodiscard]] u64 Uid() const noexcept { return m_uid; }

            // This frame's graph handles (valid after Prepare). The forward ReadTexture/ReadBuffer's
            // these so the graph orders any precompute writes -> forward and barriers the products.
            [[nodiscard]] rendergraph::RGHandle PrefilterHandle() const noexcept
            {
                return m_prefilterH;
            }
            [[nodiscard]] rendergraph::RGHandle ShHandle() const noexcept { return m_shH; }
            // The full-radiance environment cube - sampled by the sky pass (background) at full detail.
            [[nodiscard]] rendergraph::RGHandle EnvHandle() const noexcept { return m_envH; }
            [[nodiscard]] rhi::TextureView* EnvView() const noexcept { return m_envSampleView; }
            [[nodiscard]] f32 SkyIntensity() const noexcept { return m_sky.intensity; }
            // Display-only backdrop dimmer for the visible sky (not baked into the cube).
            [[nodiscard]] f32 SkyBackgroundIntensity() const noexcept
            {
                return m_sky.backgroundIntensity;
            }
            // Sun (from the scene's directional light) for the sky pass's crisp analytic disc.
            [[nodiscard]] Float3 SunDir() const noexcept { return m_sunDir; }
            [[nodiscard]] f32 SunIntensity() const noexcept { return m_sky.sunIntensity; }
            [[nodiscard]] f32 SunAngularSize() const noexcept { return m_sky.sunAngularSize; }
            // The sky pass draws a crisp analytic sun disc for the untextured skies (procedural +
            // Preetham); textured envs (HDR/cubemap) carry their own sun, so it's suppressed there.
            [[nodiscard]] bool HasSunDisc() const noexcept
            {
                return m_sky.mode != SkyMode::HDREquirect && m_sky.mode != SkyMode::Cubemap;
            }

        private:
            friend class IBLSystem;
            const void* m_scene = nullptr; // identity key (the scene's ExtractedScene)
            u64 m_uid = 0;
            u64 m_lastUsedFrame = 0;

            rhi::Texture* m_envCube = nullptr;
            rhi::TextureView* m_envSampleView = nullptr;
            rhi::TextureView* m_envMipView[kEnvMips] = {};
            rhi::Texture* m_prefilterCube = nullptr;
            rhi::TextureView* m_prefilterView = nullptr;
            rhi::Buffer* m_shBuffer = nullptr;
            rhi::BindGroup* m_envBindGroup = nullptr;  // env full-chain sample (prefilter input)
            rhi::BindGroup* m_envMipBG[kEnvMips] = {}; // mip m as the downsample source
            rhi::BindGroup* m_shBindGroup = nullptr;   // env + SH output buffer (compute)
            // Asset-driven sky texture (external product view - NOT owned): bind groups only.
            rhi::BindGroup* m_externalEquirectBG = nullptr;
            rhi::BindGroup* m_externalCubeBG = nullptr;
            u64 m_externalUid = 0;

            rhi::ResourceState m_envState = rhi::ResourceState::Undefined;
            rhi::ResourceState m_prefilterState = rhi::ResourceState::Undefined;
            rendergraph::RGHandle m_prefilterH = {};
            rendergraph::RGHandle m_shH = {};
            rendergraph::RGHandle m_envH = {};

            SkySnapshot m_sky{};
            Float3 m_sunDir = Float3{0.0f, -1.0f, 0.0f};
            bool m_dirty = true;
            // Startup re-bake window. The env cube is a one-shot bake, but a frame's submit can be
            // dropped during startup (web: the swapchain surface texture expires before the browser
            // processes the submit). Re-baking for the first frames guarantees the cube lands on a
            // frame whose submit survives, instead of being lost forever to a single bad frame.
            u32 m_bakeWarmup = 20;
            u64 m_generation = 0;
            u64 m_sourceStamp = 0; // programmatic SetEquirect/SetCubemap change tick
        };

        IBLSystem(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }
        ~IBLSystem() { Shutdown(); }
        IBLSystem(const IBLSystem&) = delete;
        IBLSystem& operator=(const IBLSystem&) = delete;

        Status Initialize();

        // Shared products (sky-independent).
        [[nodiscard]] rhi::TextureView* BrdfView() const noexcept { return m_brdfView; }
        [[nodiscard]] rendergraph::RGHandle BrdfHandle() const noexcept { return m_brdfH; }
        [[nodiscard]] u64 ShBytes() const noexcept { return sizeof(f32) * 4 * kShCoeffCount; }
        [[nodiscard]] f32 MaxLod() const noexcept { return static_cast<f32>(kPrefilterMips - 1); }
        [[nodiscard]] bool Ready() const noexcept { return m_ready; }
        [[nodiscard]] usize ContextCount() const noexcept { return m_contexts.Size(); }

        // Frame tick: import the shared BRDF into this frame's graph (declare its one-time build),
        // advance the LRU clock, and evict contexts whose scene hasn't rendered in a long time.
        void BeginFrame(rendergraph::RenderGraph& graph);

        // Get-or-create the SCENE's context, apply its authored sky + sun, import its products into
        // this frame's graph, and declare the precompute passes when dirty. Null when unavailable.
        Context* Prepare(const void* scene, const SkySnapshot& sky, const Float3& sunDir,
                         rendergraph::RenderGraph& graph);

        // Set the shared PROGRAMMATIC HDR equirectangular source (RGBA32F, w*h*4 floats) - the
        // tools/samples pixel path; a scene's ASSET sky texture takes precedence per context.
        void SetEquirect(u32 w, u32 h, Span<const f32> rgba);

        // Set the shared PROGRAMMATIC cubemap source: 6 RGBA8 faces (+X,-X,+Y,-Y,+Z,-Z) concatenated,
        // each faceSize*faceSize*4 bytes. Same precedence note as SetEquirect.
        void SetCubemap(u32 faceSize, Span<const u8> sixFaces);

        // Pending texture uploads (equirect/cubemap staging -> texture) on the frame's encoder, BEFORE the
        // graph executes - so the env-build passes sample an already-uploaded, shader-readable source.
        void Upload(rhi::CommandEncoder& enc);

    private:
        // Declare one context's precompute into this frame's graph. Products are imported EVERY frame
        // (so the forward can read this frame's handles); the write passes only run when dirty.
        void ProcessContext(Context& ctx, rendergraph::RenderGraph& graph);

        struct IblPush
        {
            i32 faceIndex = 0;
            i32 mode = 0;
            f32 roughness = 0.0f;
            f32 skyIntensity = 1.0f;
            Float4 sun{};     // xyz = direction, w = sun angular size (deg)
            Float4 horizon{}; // rgb, a = sun intensity
            Float4 zenith{};  // rgb, a = rotation (radians)
            Float4 ground{};  // rgb
        };

        // Build the procedural-env push for one cube face from the context's sky + sun direction.
        [[nodiscard]] static IblPush MakeSkyPush(const Context& ctx, i32 face);

        void DestroyExternalBindGroups(Context& ctx);

        // Equal w.r.t. the fields baked into the env cube (drives the precompute-rebuild decision).
        // sunAngularSize is EXCLUDED - it only affects the analytic sky-pass sun, not the cube.
        [[nodiscard]] static bool PrecomputeEqual(const SkySnapshot& a, const SkySnapshot& b);

        void DeclareShProjection(Context& ctx, rendergraph::RenderGraph& graph,
                                 rendergraph::RGHandle envH, rendergraph::RGHandle shH);

        // Box-downsample the env cube's mip pyramid: mip m from mip m-1 (per face). Each pass reads only the
        // finer mip (a single-mip source view/bind-group) and renders the coarser one, so read + write never
        // touch the same subresource; the graph's per-subresource barriers serialize the chain by mip.
        void DeclareEnvMips(Context& ctx, rendergraph::RenderGraph& graph,
                            rendergraph::RGHandle envH);

        void DeclarePrefilter(Context& ctx, rendergraph::RenderGraph& graph,
                              rendergraph::RGHandle envH, rendergraph::RGHandle preH);

        void DeclareBrdf(rendergraph::RenderGraph& graph, rendergraph::RGHandle brdfH);

        static constexpr rhi::TextureFormat kCubeFormat = rhi::TextureFormat::RGBA16Float;
        static constexpr rhi::TextureFormat kBrdfFormat = rhi::TextureFormat::RG16Float;

        // Shared, sky-independent: the BRDF LUT + the env/cube sampler.
        bool CreateSharedResources();

        // One scene's products + the bind groups referencing them.
        bool CreateContextResources(Context& ctx);

        void DestroyContext(Context& ctx);

        bool CreatePipelines();
        // Hot reload: destroy + recreate every pipeline against the reloaded shaders
        // (layouts survive). False when a shader no longer compiles.
        bool RebuildPipelinesForReload();

        rhi::RenderPipeline* MakeFullscreenPipeline(rhi::ShaderModule* vs, StringView psName,
                                                    rhi::PipelineLayout* layout,
                                                    rhi::TextureFormat fmt);

        // Lazily create the equirect->cube pipeline (2D source tex + sampler + push) - only when an HDR
        // equirect is first set, since most scenes are procedural.
        bool EnsureEquirectPipeline();

        // Lazily create the cubemap->cube pipeline (samples the source cube; reuses the prefilter's cube
        // bind-group + pipeline layout - cube tex + sampler + push).
        bool EnsureCubemapPipeline();

        void DestroyCubemap();

        // Free the per-source equirect texture/staging/view/bind-group (the pipeline + layout + sampler
        // persist, recreated lazily once).
        void DestroyEquirect();

        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;

        // Per-scene contexts (UniquePtr = stable addresses; frame lambdas capture their bind groups).
        Array<UniquePtr<Context>> m_contexts;
        u64 m_frame = 0;
        u64 m_nextGeneration = 0; // system-wide: context generations never collide
        u64 m_nextContextUid = 0; // stable context identities (never reused)
        u64 m_sourceStamp =
            1; // programmatic SetEquirect/SetCubemap change tick (contexts start at 0)

        // Shared PROGRAMMATIC HDR equirect source: uploaded 2D texture sampled by the equirect->cube pass.
        rhi::Texture* m_equirectTex = nullptr;
        rhi::TextureView* m_equirectView = nullptr;
        rhi::Buffer* m_equirectStaging = nullptr;
        rhi::Sampler* m_equirectSampler = nullptr;
        rhi::BindGroupLayout* m_equirectLayout = nullptr;
        rhi::PipelineLayout* m_equirectPipelineLayout = nullptr;
        rhi::RenderPipeline* m_equirectPipeline = nullptr;
        rhi::BindGroup* m_equirectBindGroup = nullptr;
        u32 m_equirectW = 0, m_equirectH = 0;
        bool m_equirectPending = false;

        // Shared PROGRAMMATIC cubemap source.
        rhi::Texture* m_srcCube = nullptr;
        rhi::TextureView* m_srcCubeView = nullptr;
        rhi::Buffer* m_cubemapStaging = nullptr;
        rhi::RenderPipeline* m_cubemapPipeline = nullptr; // reuses m_prefilterLayout + m_envLayout
        rhi::BindGroup* m_cubemapBindGroup = nullptr;
        u32 m_cubemapFaceSize = 0;
        bool m_cubemapPending = false;

        // Shared products + machinery.
        rhi::Texture* m_brdfLut = nullptr;
        rhi::TextureView* m_brdfView = nullptr;
        rhi::Sampler* m_sampler = nullptr;

        rhi::BindGroupLayout* m_envLayout = nullptr;
        // A 1x1 cube + sampler bound when a pipeline's layout declares the env group
        // but the mode has no env source (procedural/analytic): WebGPU requires every
        // declared group bound; Vulkan/DX12 simply never sample it.
        rhi::Texture* m_dummyEnvCube = nullptr;
        rhi::TextureView* m_dummyEnvView = nullptr;
        rhi::BindGroup* m_dummyEnvBindGroup = nullptr;
        rhi::BindGroupLayout* m_shLayout = nullptr;
        rhi::PipelineLayout* m_envOnlyLayout = nullptr;
        rhi::PipelineLayout* m_prefilterLayout = nullptr;
        rhi::PipelineLayout* m_brdfPipelineLayout = nullptr;
        rhi::PipelineLayout* m_shPipelineLayout = nullptr;
        rhi::RenderPipeline* m_envPipeline = nullptr;
        rhi::RenderPipeline* m_analyticPipeline =
            nullptr; // Preetham; reuses m_envOnlyLayout (push only)
        rhi::RenderPipeline* m_downsamplePipeline =
            nullptr; // env mip pyramid; reuses m_prefilterLayout
        rhi::RenderPipeline* m_prefilterPipeline = nullptr;
        rhi::RenderPipeline* m_brdfPipeline = nullptr;
        rhi::ComputePipeline* m_shPipeline = nullptr;

        rhi::ResourceState m_brdfState = rhi::ResourceState::Undefined;
        rendergraph::RGHandle m_brdfH = {};

        bool m_ready = false;
        bool m_brdfDone = false; // the BRDF LUT is constant - generated once, not per sky change
        u64 m_pipelineShaderVersion = 0; // summed ShaderSystem::Version at build (hot reload)
    };

} // namespace draconic::render
