// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render.Api - the `foundation.render.api` module.
///
/// The renderer's SCENE-RENDERING INTERFACE, extracted into a light module (core + rhi + scene
/// only) so tools can drive scene rendering through `ISceneRenderer` without linking the
/// renderer (the editor shell's layering rule; Sedulous models the same split with its
/// Abstractions assemblies). `foundation.render` re-exports everything here, so renderer-side
/// code is unaffected; `RenderSubsystem` implements `ISceneRenderer`.
///
/// The lifecycle (one bracket per frame, N views inside):
///
///     BeginRendering(encoder, frameIndex);
///     RenderScene(sceneA, targetA, ...);
///     RenderScene(sceneB, targetB, ...);   // multiple scenes / views share frame state
///     EndRendering();
///
/// BeginRendering resets shared per-frame state (the view pool, the renderers' transient
/// buffers) once; each RenderScene extracts a scene into an immutable ExtractedScene and
/// collects a view over it; EndRendering composes every collected view into the frame. The
/// caller owns the encoder + targets + frame pacing. Begin/EndRendering self-guard when the
/// renderer isn't ready yet, so a frame bracket may be driven unconditionally.

module;
#include "Core/Prelude.h"

export module foundation.render.api;

import foundation.core;
import foundation.rhi;
import foundation.scene;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // The camera for a view: world→view and view→clip, plus the world-space eye (for depth
    // sorting / culling). The view matrix is the inverse of the camera entity's world matrix.
    struct ViewCamera
    {
        Float4x4 view = Float4x4::Identity();
        Float4x4 projection = Float4x4::Identity();
        Float3 position = Float3{0, 0, 0};
        f32 farZ = 1000.0f; // for depth-key normalization
        // How far behind `position` the view draws from, along its view: an orthographic editor
        // view pulled back to show what lies behind its eye's plane (the view matrix's origin is
        // there, `position` stays the eye). What reaches a distance from the eye, the shadows,
        // reaches this much further from the draw eye.
        f32 pullBack = 0.0f;

        [[nodiscard]] Float4x4 ViewProjection() const noexcept { return view * projection; }
        // What the projection says about itself, so no builder has to repeat it beside the matrix
        // (and none can disagree with it): its kind, its planes, and an orthographic view's height
        // in world units.
        [[nodiscard]] bool IsOrthographic() const noexcept
        {
            return projection::IsOrthographic(projection);
        }
        [[nodiscard]] f32 NearZ() const noexcept { return projection::NearPlane(projection); }
        [[nodiscard]] f32 FarZ() const noexcept { return projection::FarPlane(projection); }
        [[nodiscard]] f32 OrthoHeight() const noexcept
        {
            return (IsOrthographic() && projection.m[1][1] != 0.0f) ? 2.0f / projection.m[1][1]
                                                                     : 0.0f;
        }
    };

    // A viewport sub-rect within a render target, in pixels. Width 0 => the full target.
    struct ViewportRect
    {
        i32 x = 0, y = 0;
        u32 width = 0, height = 0;
    };

    // The size a view's scene draws at when it is not the viewport's own, and which part of that
    // image the viewport shows: all of it, or for a Crop fit the slice that fills the viewport.
    // Unset (nought) draws at the viewport's size.
    struct SceneSize
    {
        u32 width = 0;
        u32 height = 0;
        // The shown part of the scene image, as fractions of it.
        f32 sourceX = 0.0f;
        f32 sourceY = 0.0f;
        f32 sourceWidth = 1.0f;
        f32 sourceHeight = 1.0f;
        // How the finished image is turned as it is presented (a landscape game on a portrait
        // screen; Specs/present-transform.md).
        ContentRotation rotation = ContentRotation::None;

        SceneSize() = default;
        SceneSize(u32 w, u32 h) : width(w), height(h) {}

        [[nodiscard]] bool IsSet() const noexcept { return width > 0 && height > 0; }
        // The whole image is not what shows.
        [[nodiscard]] bool IsCropped() const noexcept
        {
            return sourceX != 0.0f || sourceY != 0.0f || sourceWidth != 1.0f || sourceHeight != 1.0f;
        }
        // The image is presented turned.
        [[nodiscard]] bool IsTurned() const noexcept { return rotation != ContentRotation::None; }

        // A fit of a scene into a region: the scene size, and the slice of it the fit shows (the
        // whole image, but for Crop).
        [[nodiscard]] static SceneSize FromFit(const ContentFit& fit)
        {
            SceneSize size(static_cast<u32>(fit.contentSize.x), static_cast<u32>(fit.contentSize.y));
            size.rotation = fit.rotation;
            if (fit.contentSize.x > 0.0f && fit.contentSize.y > 0.0f)
            {
                const Rectangle source = fit.SrcRect();
                size.sourceX = source.x / fit.contentSize.x;
                size.sourceY = source.y / fit.contentSize.y;
                size.sourceWidth = source.width / fit.contentSize.x;
                size.sourceHeight = source.height / fit.contentSize.y;
            }
            return size;
        }
    };

    // An explicit camera for a RenderScene call, bypassing the scene's primary CameraComponent.
    struct CameraOverride
    {
        ViewCamera camera;
        // The view's backdrop, LINEAR like all render data (an authored colour is decoded with
        // ToLinear before it lands here); cornflower blue's sRGB, decoded.
        Color clearColor = Color{0.127f, 0.300f, 0.846f, 1.0f};
    };

    // Ephemeral per-view post-processing overrides for a RenderScene call - the editor viewport's
    // "show flags". Applied ON TOP of the view's resolved
    // post config; NEVER touches the scene asset. Lets an editor viewport strip effects for editing
    // clarity (crisp unjittered image for pixel inspection, raw lit image without bloom/AO/SSR)
    // without changing the authored look the game ships.
    struct ViewPostOverride
    {
        bool disablePost =
            false; // master: drop bloom + AO + SSR + AA (exposure + tonemap stay, so it still displays)
        bool disableBloom = false;
        bool disableAo = false;
        bool disableSsr = false;
        bool disableAa = false; // TAA + FXAA off (crisp + unjittered)
        bool disableSsgi = false;
        // Draw EVERYTHING in the scene for this view, frustum test off: the A/B for what
        // view-frustum culling saves (on by default since 2026-09-24; the player culls too).
        bool disableCulling = false;
        // Scene-pass MSAA for this view (the editor viewport's off/2x/4x toggle). 0 = no override
        // (leave the resolved count as-is); 1/2/4 = force that sample count. The editor viewport is
        // the source of its own MSAA count (the scene authors none), so
        // it forces the count here. Capability-clamped downstream.
        u8 msaaOverride = 0;
    };

    // Per-view debug-view selection (the editor viewport's "show me this texture"): `resource`
    // names a render-graph texture from GetDebugResources ("ao.ao", "ssr.refl", "gbuffer.depth",
    // ...); empty = final image. The compose appends a blit pass that overwrites the view's
    // sub-rect with the visualized resource - overlays/gizmos still draw on top. Never touches
    // the scene asset; ephemeral per RenderScene call, like ViewPostOverride.
    // Semantic (shader-instrumented) debug modes: the forward shader outputs the named
    // term instead of the lit result (a uniform branch - free when off, never a cooked
    // variant). Values reach the screen RAW (the compose blits the scene color past
    // tonemap). Mesh/forward materials only - sky and bespoke renderers draw normally.
    enum class ViewDebugSemantic : u8
    {
        Off = 0,
        Albedo,      // base color after vertex color + textures, pre-lighting
        Normal,      // mapped world-space normal (0.5 remap)
        Roughness,   // final clamped roughness
        Metallic,    // final metallic
        Cascades,    // CSM cascade selection tinted over albedo luma
        ClusterHeat, // clustered-light count heatmap (blue -> red)
        Overbright,  // magenta where the lit result is non-finite / implausibly hot
    };

    struct ViewDebugView
    {
        String resource;            // graph resource name; empty = none
        ViewDebugSemantic semantic = ViewDebugSemantic::Off; // shader-term mode; resource wins if both set
        f32 rangeMin = 0.0f;        // display remap: out = saturate((v - min) / (max - min))
        f32 rangeMax = 1.0f;
        u8 channel = 0;             // 0 RGB, 1 R, 2 G, 3 B, 4 A, 5 luma
        bool linearizeDepth = true; // depth sources display as normalized view-space distance
        f32 nearZ = 0.1f;           // linearization planes (the viewport camera's)
        f32 farZ = 1000.0f;
    };

    // One row of the renderer's "what can I look at" table: the PREVIOUS frame's graph
    // texture inventory (the graph rebuilds per frame under stable names). Multisampled
    // sources are listed but not blittable - pick the resolved twin instead.
    struct DebugResourceInfo
    {
        String name;
        u32 width = 0;
        u32 height = 0;
        u8 samples = 1;
        bool isDepth = false;
    };

    // How the render target's resource state is handled. Default = the host-managed backbuffer (present).
    // For an offscreen target, give its `texture` (so the graph barriers it) + the state it's currently
    // in + the state to leave it in (ShaderRead to sample it next, CopySrc to blit it).
    struct TargetState
    {
        rhi::Texture* texture = nullptr;
        rhi::ResourceState currentState = rhi::ResourceState::RenderTarget;
        rhi::ResourceState finalState = rhi::ResourceState::RenderTarget;
    };

    // ---- overlay roles (the two-tier overlay coordination model, Sedulous-derived) ----
    //
    // Scene tier (`ISceneOverlay`): content scoped to a SCENE's rendered output - HUD canvases,
    // billboards. Sources register with the scene renderer ONCE; a shared per-view overlay pass
    // inside the compose (after post, before debug draw - gizmos/diagnostics stay on top) calls
    // every source with that view's real camera, so scene UI renders wherever the scene renders
    // (player, editor viewports, camera previews) and projects correctly per view.
    //
    // Screen tier (`IScreenOverlay` + `IScreenRenderer`): window-space chrome - screen UI,
    // profiler HUDs, editor overlays. Sources register with the `IScreenRenderer` (the render
    // subsystem); the HOST makes one `RenderOverlays` call per window target after the scene
    // composed, and every source records into one shared Load-op pass in `OverlayOrder`.
    //
    // Both signatures use only render-level types: the renderer stays unaware of higher-level
    // tech (UI, VG, fonts). Registries are non-owning; callers unregister before destruction.

    // Everything an overlay source needs to draw into one view's output.
    struct SceneOverlayView
    {
        const void* sceneKey = nullptr; // opaque scene identity (the driving subsystem sets it;
                                        // sources that registered per-scene state match on it)
        Float4x4 viewProjection = Float4x4::Identity(); // the view's UNJITTERED camera VP
        Float3 cameraPosition = Float3{0, 0, 0};
        i32 viewportX = 0, viewportY = 0; // the view's sub-rect within the target
        u32 viewportWidth = 0, viewportHeight = 0;
        u32 targetWidth = 0, targetHeight = 0; // full target extent (pixels)
        rhi::TextureFormat targetFormat = rhi::TextureFormat::BGRA8Unorm;
        // The pass's depth-stencil attachment format (Undefined = color-only pass).
        // Sources may record stencil-based draws only when this is set AND matches the
        // format their own pipelines were built against.
        rhi::TextureFormat depthStencilFormat = rhi::TextureFormat::Undefined;
        u32 frameIndex = 0;
    };

    // Everything a screen-overlay source needs to draw into one window target.
    struct ScreenOverlayView
    {
        u32 width = 0, height = 0; // target extent (pixels)
        rhi::TextureFormat targetFormat = rhi::TextureFormat::BGRA8Unorm;
        // Same contract as SceneOverlayView::depthStencilFormat.
        rhi::TextureFormat depthStencilFormat = rhi::TextureFormat::Undefined;
        u32 frameIndex = 0;
        // The pass draws through a UNORM view of an sRGB target (targetFormat is that view's):
        // overlays keep colours encoded and blend them so, as browsers composite UI (VG's
        // Encoded blend space). False: the target as it is, linear light.
        bool encodedTarget = false;
    };

    // Per-view overlay source (scene-attached UI). The pass and color target are already bound;
    // implementers only record draws (never open their own passes) and configure their own
    // pipeline state. All of a view's overlays share one Load-op pass on the final LDR output.
    class ISceneOverlay
    {
    public:
        virtual ~ISceneOverlay() = default;
        // Sort order: lower draws first (background), higher last (foreground).
        [[nodiscard]] virtual i32 OverlayOrder() const noexcept { return 0; }
        virtual void Render(rhi::RenderPassEncoder& encoder, const SceneOverlayView& view) = 0;
    };

    // Window-space overlay source. Same recording contract as ISceneOverlay.
    class IScreenOverlay
    {
    public:
        virtual ~IScreenOverlay() = default;
        [[nodiscard]] virtual i32 OverlayOrder() const noexcept { return 0; }
        virtual void Render(rhi::RenderPassEncoder& encoder, const ScreenOverlayView& view) = 0;
    };

    // A non-owning overlay registry, insertion-sorted by OverlayOrder (stable ties: registration
    // order). Shared by both tiers; pure logic (unit-tested without a device).
    template <typename TOverlay>
    class OverlayRegistry
    {
    public:
        // Idempotent: re-registering is a no-op.
        void Add(TOverlay* overlay)
        {
            if (overlay == nullptr || Contains(overlay))
            {
                return;
            }
            usize i = 0;
            while (i < m_items.Size() && m_items[i]->OverlayOrder() <= overlay->OverlayOrder())
            {
                ++i;
            }
            m_items.Insert(i, overlay);
        }
        void Remove(TOverlay* overlay)
        {
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                if (m_items[i] == overlay)
                {
                    m_items.RemoveAt(i);
                    return;
                }
            }
        }
        [[nodiscard]] bool Contains(const TOverlay* overlay) const noexcept
        {
            for (TOverlay* item : m_items)
            {
                if (item == overlay)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_items.IsEmpty(); }
        [[nodiscard]] const Array<TOverlay*>& Items() const noexcept { return m_items; }

    private:
        Array<TOverlay*> m_items;
    };

    // The scene-rendering coordinator. Implemented by RenderSubsystem; queried by the app via
    // the Context (a renderer-agnostic seam for tools/editor that render scenes themselves).
    class ISceneRenderer
    {
    public:
        virtual ~ISceneRenderer() = default;

        // Begin a frame. Resets shared per-frame state. `encoder` (caller-owned) receives all
        // the frame's GPU commands; `frameIndex` is the device ring index.
        virtual void BeginRendering(rhi::CommandEncoder& encoder, u32 frameIndex) = 0;

        // Collect `scene`, viewed from its primary camera (or `cameraOverride` if given), to be
        // drawn into `target`. The view's clear color comes from that camera. Must be called
        // between Begin/EndRendering.
        // `viewport` is the sub-rect of `target` to render into (default = full target); pass distinct
        // viewports + camera overrides across multiple RenderScene calls for split-screen.
        // `viewportKey` (default null) is a STABLE, per-view identity (e.g. the hosting ViewportView*).
        // When non-null, this view's scene gizmos come from DebugView(viewportKey) instead of the
        // shared per-scene DebugScene(scene) - so an editor viewport can draw grid/gizmos that appear
        // ONLY in it, not in a second view of the same scene (the camera-preview inset). Null keeps the
        // per-scene buffer (drawn in every view), which is what gameplay/player views want.
        // A `sceneSize` other than the viewport's draws the scene at THAT size and scales the
        // finished image (its shown part, for a crop) into the viewport, clearing the target around
        // it black: a game with a fixed render resolution fitted into a window of another shape.
        // The projection takes that size's aspect. Unset draws at the viewport's own size.
        virtual void RenderScene(scene::Scene& scene, rhi::TextureView* target,
                                 rhi::TextureFormat targetFormat, u32 width, u32 height,
                                 ViewportRect viewport = {},
                                 const CameraOverride* cameraOverride = nullptr,
                                 const TargetState& targetState = {},
                                 const ViewPostOverride* postOverride = nullptr,
                                 const void* viewportKey = nullptr,
                                 const ViewDebugView* debugView = nullptr,
                                 const SceneSize& sceneSize = {}) = 0;

        // Compose every collected view into the frame's encoder.
        virtual void EndRendering() = 0;

        // The previous frame's render-graph texture inventory for debug-view pickers.
        // Default-empty so light ISceneRenderer fakes need not implement it.
        virtual void GetDebugResources(Array<DebugResourceInfo>& out) { out.Clear(); }

        // Scene-tier overlay registry (idempotent; non-owning - unregister before destruction).
        virtual void RegisterOverlay(ISceneOverlay* overlay) = 0;
        virtual void UnregisterOverlay(ISceneOverlay* overlay) = 0;
    };

    // The window-space overlay coordinator. Implemented by RenderSubsystem. The pair
    // (ISceneRenderer for scenes + per-view overlays, IScreenRenderer for window overlays
    // after the scene blit) forms the engine's two-tier render coordination model.
    class IScreenRenderer
    {
    public:
        virtual ~IScreenRenderer() = default;

        // Screen-tier overlay registry (idempotent; non-owning - unregister before destruction).
        virtual void RegisterOverlay(IScreenOverlay* overlay) = 0;
        virtual void UnregisterOverlay(IScreenOverlay* overlay) = 0;

        // Open one Load-op render pass against `target` (must be in RenderTarget state; left
        // there), walk every registered overlay in order, and call each one's Render with the
        // active encoder. No-op when no overlays are registered or `target` is null.
        // `encodedTarget`, when given, is a UNORM view of the same (sRGB) texture: the pass draws
        // through it and the overlays blend encoded values (ScreenOverlayView::encodedTarget).
        virtual void RenderOverlays(rhi::CommandEncoder& encoder, rhi::TextureView* target,
                                    rhi::TextureFormat targetFormat, u32 width, u32 height,
                                    u32 frameIndex, rhi::TextureView* encodedTarget = nullptr) = 0;
    };

} // namespace foundation::render
