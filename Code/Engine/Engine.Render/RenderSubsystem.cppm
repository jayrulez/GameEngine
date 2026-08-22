/// Engine::Render - the `:subsystem` partition.
///
/// RenderSubsystem: the Context-level driver that connects scenes to the (scene-agnostic)
/// renderer. It owns the GPU systems - the DXC compiler, ShaderSystem, PipelineStateCache,
/// the MeshRenderer + RendererRegistry, and the per-frame RenderFrame driver - and, as an
/// As an ISceneObserver it reacts to scene teardown; assembly happens via the scene composition.
///
/// It implements ISceneRenderer (Begin/RenderScene×N/End): the app's render callback brackets
/// the frame with BeginRendering/EndRendering and calls RenderScene per active scene. Each
/// RenderScene extracts the scene into an ExtractedScene snapshot and collects a RenderView;
/// EndRendering composes all views. (No MaterialSystem yet - the built-in forward shader binds
/// no material set; that lands with material binding in phase 3.)

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h"

export module engine.render:subsystem;

import foundation.core;
import foundation.rhi;
import foundation.profiler;
import foundation.runtime;         // Subsystem, Context
import foundation.scene; // Scene
import engine.scene; // SceneSubsystem (to register as scene-aware)
import foundation.shaders.system;  // ShaderSystem, ShaderSystemHost
import foundation.materials;       // MaterialSystem
import foundation.materials.pipelinecache;   // PipelineStateCache
import foundation.render;          // MeshRenderer, RendererRegistry, RenderFrame, ExtractedScene
import :components;
import :extract;
import :scene_renderer;

using namespace foundation::core;
using namespace foundation::render;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace engine::render
{
    // Foundation aliases: inside engine::render, the sibling engine::scene would
    // otherwise shadow the foundation foundation::scene these reference. (The engine SceneSubsystem is
    // spelled engine::scene::SceneSubsystem where needed.)
    namespace scene = foundation::scene;

    // THE render manager set for a scene - injected by the subsystem at runtime AND by headless
    // scene consumers (Engine.SceneSurface -> export/MCP transcode scratch), so a manager added
    // there reaches both automatically. Defined in RenderSubsystemImpl.cpp next to OnSceneCreated.
    // Add a manager => bump the SceneSurface tripwire (engine::kSceneSystemCount).
    void AddRenderSceneManagers(scene::Scene& scene);

    // The canonical, ordered scene-pass MSAA levels - the SINGLE source of truth for the UI list and
    // the index<->sample-count mapping. Add a level HERE (e.g. {8u, "8x"}, plus raising the device
    // ceiling + SupportsSampleCount) and every combo/menu + mapping picks it up; nothing else hardcodes
    // the list. The device still capability-clamps at runtime (SupportsMsaaSamples), so an unsupported
    // level is simply not offered / degrades.
    struct MsaaLevel
    {
        u32 samples;
        StringView label;
    };
    inline constexpr MsaaLevel kMsaaLevels[] = {
        {1u, StringView(u8"Off")},
        {2u, StringView(u8"2x")},
        {4u, StringView(u8"4x")},
    };
    [[nodiscard]] constexpr u32 MsaaLevelCount() noexcept
    {
        return static_cast<u32>(sizeof(kMsaaLevels) / sizeof(kMsaaLevels[0]));
    }
    // The highest level whose sample count is <= `samples` (a stored 4 selects "4x", 3 selects "2x").
    [[nodiscard]] constexpr i32 MsaaIndexForSamples(u32 samples) noexcept
    {
        i32 index = 0;
        for (u32 i = 0; i < MsaaLevelCount(); ++i)
        {
            if (samples >= kMsaaLevels[i].samples)
            {
                index = static_cast<i32>(i);
            }
        }
        return index;
    }
    [[nodiscard]] constexpr u32 MsaaSamplesForIndex(i32 index) noexcept
    {
        return (index >= 0 && index < static_cast<i32>(MsaaLevelCount()))
                   ? kMsaaLevels[static_cast<u32>(index)].samples
                   : 1u;
    }

    class RenderSubsystem final : public foundation::runtime::Subsystem,
                                  public ISceneRenderer,
                                  public IScreenRenderer,
                                  public scene::ISceneObserver
    {
    public:
        RenderSubsystem(rhi::Device& device, u32 framesInFlight) noexcept
            : m_device(&device), m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight)
        {
        }

        // --- Extension seam for downstream subsystems (particles, world-UI, ...) --------------------
        // Register an external Renderer (borrowed - caller owns it); returns its dispatch id to stamp on
        // the subsystem's render-data. The pipeline drives PrepareFrame/Resolve/FinishFrame registry-wide.
        u16 RegisterRenderer(Renderer& renderer);
        // Register a render-data provider FOR a scene (borrowed). Invoked during that scene's extraction.
        // The provider interface is scene-free; the scene binding lives here. Auto-cleared on scene destroy.
        void RegisterProvider(scene::Scene& scene, IRenderDataProvider& provider);
        // GPU handles a subsystem needs to build its renderer's pipeline (valid once RenderSubsystem is ready).
        [[nodiscard]] rhi::Device* Device() const noexcept { return m_device; }
        [[nodiscard]] shaders::ShaderSystem* Shaders() const noexcept { return m_shaders; }
        [[nodiscard]] u32 FramesInFlight() const noexcept { return m_framesInFlight; }

        [[nodiscard]] i32 UpdateOrder() const noexcept override;

        // Drop any render-data providers registered for a scene that's going away (borrowed pointers).
        void OnDestroying(scene::Scene& scene) override;

        [[nodiscard]] bool IsReady() const noexcept { return m_frame.Get() != nullptr; }

        // Set the scene's HDR equirectangular environment (RGBA32F, w*h*4 floats). The IBL env rebuilds
        // from it when EnvironmentSettings.skyMode is HDREquirect. Owns a copy of the pixels (uploads next
        // frame). No-op if IBL is unavailable.
        void SetSkyEquirect(u32 w, u32 h, Span<const f32> rgba);

        // Set the scene's cubemap environment: 6 RGBA8 faces (+X,-X,+Y,-Y,+Z,-Z) concatenated, each
        // faceSize*faceSize*4 bytes. Used when EnvironmentSettings.skyMode is Cubemap.
        void SetSkyCubemap(u32 faceSize, Span<const u8> sixFaces);

        // Exposure/bloom/AO are now AUTHORED per scene (PostProcessSettings) and resolved per view.
        // These programmatic setters remain as a GLOBAL OVERRIDE (samples' ImGui debug panels): calling
        // any of them latches m_globalPostActive, and the resolve in RenderScene then prefers the global
        // values over the scene's authored ones for exposure/bloom/AO. Untouched (the editor path), the
        // scene's authored settings drive. (AO debug below stays a pure frame-global renderer toggle.)

        // Linear exposure multiplier applied in the tonemap (default 1.0).
        void SetExposure(f32 exposure) noexcept;
        [[nodiscard]] f32 Exposure() const noexcept { return m_exposure; }

        // Bloom on/off (skips the whole pyramid when off) + composite strength + soft-knee prefilter.
        void SetBloomEnabled(bool on) noexcept;
        [[nodiscard]] bool BloomEnabled() const noexcept { return m_bloomEnabled; }
        void SetBloomIntensity(f32 v) noexcept;
        [[nodiscard]] f32 BloomIntensity() const noexcept { return m_bloomIntensity; }
        void SetBloomThreshold(f32 v) noexcept;
        [[nodiscard]] f32 BloomThreshold() const noexcept { return m_bloomThreshold; }

        // Ambient occlusion: mode (Off/GTAO/SSAO, mutually exclusive) + tunables (strength = composite amount,
        // radius = world AO radius, intensity = power). Both modes share the whole apply/blur/debug pipeline.
        void SetAoMode(AoMode m) noexcept;
        [[nodiscard]] AoMode GetAoMode() const noexcept { return m_aoMode; }
        void SetAoStrength(f32 v) noexcept;
        [[nodiscard]] f32 AoStrength() const noexcept { return m_aoStrength; }
        void SetAoRadius(f32 v) noexcept;
        [[nodiscard]] f32 AoRadius() const noexcept { return m_aoRadius; }
        void SetAoIntensity(f32 v) noexcept;
        [[nodiscard]] f32 AoIntensity() const noexcept { return m_aoIntensity; }
        void SetAoDebug(i32 mode) noexcept;
        [[nodiscard]] i32 AoDebug() const noexcept { return m_aoDebug; }

        // Screen-space reflections: on/off + tunables (intensity, max view-space ray length, hit thickness,
        // screen-edge fade, roughness cutoff, march steps). Reflects the lit HDR before AO/TAA.
        void SetSsrEnabled(bool on) noexcept;
        [[nodiscard]] bool SsrEnabled() const noexcept { return m_ssrEnabled; }
        // Scene-pass MSAA sample count (1/2/4) for the global post path - capability-clamped per view
        // (msaa.md). Samples/tools drive it directly (the editor uses the per-view override instead).
        void SetMsaaSamples(u32 count) noexcept;
        [[nodiscard]] u32 MsaaSamples() const noexcept { return m_globalMsaaSamples; }
        // Whether this exact scene-pass MSAA count is usable on the active device (the valid set is not
        // contiguous - WebGPU supports only {1, 4}). UIs should offer only counts that return true.
        [[nodiscard]] bool SupportsMsaaSamples(u32 count) const noexcept;
        [[nodiscard]] SsrPass::Params& SsrParams() noexcept { return m_ssrParams; }

        // Share instance data between the camera depth-prepass and the forward (build once). A/B toggle.
        void SetInstanceSharing(bool on) noexcept { m_instanceSharing = on; }
        [[nodiscard]] bool InstanceSharing() const noexcept { return m_instanceSharing; }

        // View-frustum culling: skip renderables outside the camera frustum per view. Off by default (a no-op
        // for benchmarks that frame everything; a win for real scenes with lots off-screen). Shadow casters are
        // sourced independently, so culling the camera view never drops a shadow.
        void SetViewCulling(bool on) noexcept { m_viewCulling = on; }
        [[nodiscard]] bool ViewCulling() const noexcept { return m_viewCulling; }
        // Last frame's cull totals (culled / considered, summed over views). 0/0 when culling was off.
        void ViewCullStats(u32& culled, u32& total) const noexcept;

        // FXAA on/off (TAA-off fallback AA - ignored while TAA is on) + sub-pixel quality (0..1).
        void SetFxaaEnabled(bool on) noexcept;
        [[nodiscard]] bool FxaaEnabled() const noexcept { return m_fxaaEnabled; }
        void SetFxaaSubpixel(f32 v) noexcept;
        [[nodiscard]] f32 FxaaSubpixel() const noexcept { return m_fxaaSubpixel; }

        // Debug draw (immediate-mode, cleared each frame after rendering). Destinations by WHERE the
        // draw lands: DebugGlobal() = drawn in EVERY view (world gizmos + per-view HUD, replicated per view);
        // DebugScene(scene) = every view OF THAT SCENE (no side-by-side bleed across scenes); DebugView(key)
        // = only the ONE view that renders with a matching `viewportKey` (so an editor viewport's grid/
        // gizmos stay out of a second view of the same scene - the camera-preview inset); DebugScreen() =
        // ONCE over the whole window (screen-space HUD - text/rects; 3D calls have no camera here, ignored).
        [[nodiscard]] debug::DebugDraw& DebugGlobal() noexcept { return m_debugGlobal; }
        [[nodiscard]] debug::DebugDraw& DebugScene(scene::Scene& s);
        [[nodiscard]] debug::DebugDraw& DebugView(const void* viewportKey);
        [[nodiscard]] debug::DebugDraw& DebugScreen() noexcept { return m_debugScreen; }

        // Temporal AA on/off (projection jitter + history resolve) + resolve tunables.
        void SetTaaEnabled(bool on) noexcept;
        [[nodiscard]] bool TaaEnabled() const noexcept { return m_taaEnabled; }
        void SetTaaBlend(f32 v) noexcept;
        [[nodiscard]] f32 TaaBlend() const noexcept { return m_taaBlend; }
        void SetTaaGamma(f32 v) noexcept;
        [[nodiscard]] f32 TaaGamma() const noexcept { return m_taaGamma; }
        void SetTaaMotionScale(f32 v) noexcept { m_taaMotionScale = v; }
        [[nodiscard]] f32 TaaMotionScale() const noexcept { return m_taaMotionScale; }

        // Directional (CSM) shadow reach + far-fade. Both in world units: distance is the reach (clamped to
        // camera farZ); farFade is the fixed WIDTH of the soft edge over which shadows dissolve to fully-lit
        // at the boundary (kills the diagonal coverage-boundary pop on a tilted, rotating camera).
        void SetShadowDistance(f32 v) noexcept { m_shadowDistance = v; }
        [[nodiscard]] f32 ShadowDistance() const noexcept { return m_shadowDistance; }
        void SetShadowFarFade(f32 v) noexcept { m_shadowFarFade = v; }
        [[nodiscard]] f32 ShadowFarFade() const noexcept { return m_shadowFarFade; }

        // Append a per-pass GPU timing report. STALLS (waits for the GPU to finish) so the timestamps
        // are valid - intended for an on-demand dump (the P-key), not per-frame use.
        void BuildGpuProfileReport(String& out);

        // ---- ISceneRenderer ----

        void BeginRendering(rhi::CommandEncoder& encoder, u32 frameIndex) override;

        void RenderScene(scene::Scene& scene, rhi::TextureView* target,
                         rhi::TextureFormat targetFormat, u32 width, u32 height,
                         ViewportRect viewport = {}, const CameraOverride* cameraOverride = nullptr,
                         const TargetState& targetState = {},
                         const ViewPostOverride* postOverride = nullptr,
                         const void* viewportKey = nullptr) override;

        void EndRendering() override;

        // Scene-tier overlay sources: drawn per view inside the compose (after post, before
        // debug draw), matched to views by SceneKey. Idempotent, non-owning.
        void RegisterOverlay(ISceneOverlay* overlay) override { m_sceneOverlays.Add(overlay); }
        void UnregisterOverlay(ISceneOverlay* overlay) override { m_sceneOverlays.Remove(overlay); }

        // ---- IScreenRenderer ----

        void RegisterOverlay(IScreenOverlay* overlay) override { m_screenOverlays.Add(overlay); }
        void UnregisterOverlay(IScreenOverlay* overlay) override;

        // Window-space overlays: one shared Load-op pass against `target` (in RenderTarget
        // state; left there), every registered source in OverlayOrder. The HOST calls this once
        // per window target after the scene composed (post-EndRendering).
        void RenderOverlays(rhi::CommandEncoder& encoder, rhi::TextureView* target,
                            rhi::TextureFormat targetFormat, u32 width, u32 height,
                            u32 frameIndex) override;

    protected:
        void OnInit() override;

        void OnReady() override;

        void OnShutdown() override;

    private:
        // A per-frame snapshot pool: one ExtractedScene per RenderScene call, kept alive (and its
        // arena chunks reused) until the next BeginRendering. (Phase 8 shares one snapshot across
        // multiple cameras of the same scene; phase 1 takes one per call.)
        [[nodiscard]] ExtractedScene* AcquireScene();

        rhi::Device* m_device;
        u32 m_framesInFlight = 2;
        u32 m_maxMsaaSamples = 1; // device-supported scene-pass MSAA ceiling (queried at init; msaa.md)
        // Owns the pack-vs-dev ShaderSystem (cooked blobs in a dist/web build, DXC + file provider
        // with hot reload otherwise). m_shaders caches its ShaderSystem for the passes to borrow.
        shaders::ShaderSystemHost m_shaderHost;
        shaders::ShaderSystem* m_shaders = nullptr;
        UniquePtr<materials::PipelineStateCache> m_psoCache;
        UniquePtr<materials::MaterialSystem> m_materialSystem;
        UniquePtr<MeshRenderer> m_meshRenderer;
        UniquePtr<SpriteRenderer> m_spriteRenderer;
        UniquePtr<ClusterSystem> m_clusterSystem;
        UniquePtr<TonemapPass> m_tonemapPass;
        UniquePtr<ShadowSystem> m_shadowSystem;

    public:
        /// The frames-in-flight retire queue external renderers (particles, ...) wire
        /// their DynamicUniformRings into - grow-path replacements must retire, never
        /// WaitIdle mid-frame (the web dropped-submit class).
        [[nodiscard]] GpuRetireQueue* RetireQueue() noexcept { return &m_retireQueue; }

    private:
        GpuRetireQueue m_retireQueue; // frames-in-flight deferred GPU destruction (web-safe grows)
        UniquePtr<IBLSystem> m_iblSystem;
        UniquePtr<ReflectionProbeSystem> m_probeSystem;
        UniquePtr<SkyPass> m_skyPass;
        UniquePtr<BloomPass> m_bloomPass;
        UniquePtr<TaaPass> m_taaPass;
        UniquePtr<AoPass> m_aoPass;
        UniquePtr<SsrPass> m_ssrPass;
        UniquePtr<MsaaResolvePass> m_msaaResolvePass; // scene-pass MSAA depth+aux resolve (msaa.md)
        UniquePtr<FxaaPass> m_fxaaPass;
        UniquePtr<DecalPass> m_decalPass;
        UniquePtr<DebugDrawPass> m_debugPass;
        debug::DebugDraw m_debugGlobal;                         // global gizmos (all views)
        debug::DebugDraw m_debugScreen;                         // whole-window HUD (drawn once)
        HashMap<scene::Scene*, debug::DebugDraw> m_debugScenes; // per-scene gizmos (every view of a scene)
        HashMap<const void*, debug::DebugDraw> m_debugViews;    // per-view gizmos (one keyed viewport)
        bool m_globalPostActive = false; // a post setter was called => global override wins
        f32 m_exposure = 1.0f;
        bool m_bloomEnabled = true;
        AoMode m_aoMode = AoMode::Off; // AO off by default (UI combo)
        i32 m_aoDebug = 0;             // AO debug view (0=off)
        f32 m_aoStrength = 0.6f;       // partial by default (full darkens curved surfaces too much)
        f32 m_aoRadius = 0.5f;
        f32 m_aoIntensity = 1.0f;
        bool m_ssrEnabled = false;  // SSR off by default (UI toggle)
        u32 m_globalMsaaSamples = 1; // scene-pass MSAA count for the global post path (off by default)
        SsrPass::Params m_ssrParams{};
        bool m_instanceSharing = true; // prepass->forward instance-data sharing (A/B toggle)
        bool m_viewCulling = false;    // view-frustum cull camera draw lists (default off)
        bool m_fxaaEnabled = false;    // FXAA off by default (TAA-off fallback)
        f32 m_fxaaSubpixel = 0.75f;
        bool m_taaEnabled = false;     // TAA off by default (UI toggle)
        f32 m_taaBlend = 0.97f;        // history weight (stability)
        f32 m_taaGamma = 1.25f;        // variance-clip box half-width
        f32 m_taaMotionScale = 32.0f;  // history drop-off with motion
        f32 m_shadowDistance = 300.0f; // directional-shadow reach (world units)
        f32 m_shadowFarFade = 40.0f;   // far-fade width (world units)
        f32 m_bloomIntensity = 0.05f;  // 0 = bloom off
        f32 m_bloomThreshold = 1.0f;
        f32 m_bloomKnee = 0.6f;
        RendererRegistry m_registry;
        struct SceneProvider
        {
            scene::Scene* scene;
            IRenderDataProvider* provider;
        };
        Array<SceneProvider> m_providers; // per-scene render-data contributors (borrowed)
        OverlayRegistry<ISceneOverlay> m_sceneOverlays;   // scene-tier overlay sources (borrowed)
        OverlayRegistry<IScreenOverlay> m_screenOverlays; // window-space overlay sources (borrowed)
        // Screen-overlay stencil attachment (stencil-then-cover UI fills): one cached DS
        // sized to the last window target (single-window runtime; a size change retires
        // the old texture through the queue and recreates). Undefined format = no stencil.
        rhi::Texture* m_overlayDsTexture = nullptr;
        rhi::TextureView* m_overlayDsView = nullptr;
        u32 m_overlayDsWidth = 0;
        u32 m_overlayDsHeight = 0;
        rhi::TextureFormat m_overlayDsFormat = rhi::TextureFormat::Undefined;
        bool m_overlayDsProbed = false;
        UniquePtr<RenderFrame> m_frame;

        Array<UniquePtr<ExtractedScene>> m_scenes; // snapshot pool
        usize m_sceneCount = 0;
        Array<scene::Scene*> m_snapshotOwners; // [i] = the scene m_scenes[i] holds this frame
        RenderContext m_renderCtx;             // per-worker extraction arenas
    };

} // namespace foundation::render
