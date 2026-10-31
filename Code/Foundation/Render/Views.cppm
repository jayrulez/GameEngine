// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:views` partition.
///
/// `RenderView` is the renderer's unit of work *and* its isolation boundary: render one
/// scene's extracted data, from one camera, into one target. A frame renders a set of
/// views (primary cameras, plus derived shadow and probe views), and because each
/// `RenderView` owns its per-frame mutable state (the culled and sorted draw list, the
/// view UBO, cluster grid, shadow slots, HDR and depth targets), views don't overwrite
/// each other. The only state shared across views is the immutable `ExtractedScene` and
/// shared GPU resources.
///
/// Views are pooled per frame from a `RenderViewPool` (reset, not freed, each frame).

module;
#include "Core/Prelude.h"

export module foundation.render:views;

// ViewCamera and ViewportRect live in the light `foundation.render.api` module (with
// ISceneRenderer); re-exported here so foundation.render importers see them.
export import foundation.render.api;

import foundation.core;
import foundation.rhi;
import :data;

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // Per-view settings (post config, layer mask and the rest).
    struct ViewSettings
    {
        rhi::ClearColor clear = rhi::ClearColor::CornflowerBlue();
        // Viewport sub-rect within the target, in pixels (split-screen). Width 0 means the full target.
        i32 viewportX = 0, viewportY = 0;
        u32 viewportWidth = 0, viewportHeight = 0;
        // The size the scene draws at, when it is not the viewport's: the whole scene chain runs
        // at this size, and one last pass scales the finished image (its shown part) into the
        // viewport. Unset draws at the viewport's own size.
        SceneSize scene{};
        // Target resource-state handling for the imported color target. `targetTexture` is the backing
        // texture the graph transitions (null for a host-managed backbuffer; the graph adds no barrier).
        // `targetFinalState` is where the graph leaves it: RenderTarget for present, or ShaderRead or
        // CopySrc for an offscreen target the caller then samples or blits.
        rhi::Texture* targetTexture = nullptr;
        rhi::ResourceState targetCurrentState = rhi::ResourceState::RenderTarget;
        rhi::ResourceState targetFinalState = rhi::ResourceState::RenderTarget;
        // Resolved per-view post-processing (exposure, bloom, AO). The RenderSubsystem fills this from
        // the scene's authored PostProcessSettings; the compose passes read it per view.
        ViewPostConfig post{};
        // Debug-view selection (editor "show me this texture"); an empty resource is off.
        ViewDebugView debug{};
        // The opaque per-viewport key the RenderScene call carried (null when unkeyed). Pick
        // requests bind to it: only the view rendered with a matching key answers them.
        const void* viewportKey = nullptr;
        // Which view this is from frame to frame (0 means by its place in the frame's list). The state
        // a view carries between frames (its previous camera for motion vectors, the TAA, SSR, SSGI
        // and exposure histories) follows this key, not the view's position: a render texture drawn
        // on some frames before the main view shifts the main view's position, which would then read
        // the other view's history.
        u64 historyKey = 0;
        // This view may frustum-cull its draw list (the frame's global switch still gates it).
        // A per-view override (ViewPostOverride::disableCulling) clears it for an A/B.
        bool frustumCull = true;
        // This view draws the scene-tier overlays (HUD canvases, billboards). A camera rendering
        // into a texture (a minimap) shows the world without the HUD it is shown in.
        bool sceneOverlays = true;
    };

    // A single view: what to draw (a shared ExtractedScene), from where (camera), into what
    // (target). Owns the per-view draw list. Pooled: `Reset` rebinds it for a new use without
    // freeing the draw-list storage.
    class RenderView
    {
    public:
        // Bind this view to a scene snapshot, camera and target for the current frame.
        void Bind(const ExtractedScene& scene, const ViewCamera& camera,
                  const ViewSettings& settings, rhi::TextureView* target,
                  rhi::TextureFormat targetFormat, u32 width, u32 height) noexcept
        {
            m_scene = &scene;
            m_camera = camera;
            m_settings = settings;
            m_target = target;
            m_targetFormat = targetFormat;
            m_width = width; // full target (color import and transient depth size)
            m_height = height;
            // Viewport sub-rect within the target (defaults to the whole target).
            m_viewportX = (settings.viewportWidth > 0) ? settings.viewportX : 0;
            m_viewportY = (settings.viewportWidth > 0) ? settings.viewportY : 0;
            m_viewportW = (settings.viewportWidth > 0) ? settings.viewportWidth : width;
            m_viewportH = (settings.viewportHeight > 0) ? settings.viewportHeight : height;
            m_outputWidth = width;
            m_outputHeight = height;
            m_outputViewportX = m_viewportX;
            m_outputViewportY = m_viewportY;
            m_outputViewportW = m_viewportW;
            m_outputViewportH = m_viewportH;
            // A scene size of its own: the chain runs at it, the whole of it the view's.
            const SceneSize& size = settings.scene;
            m_scaled = size.IsSet() &&
                       (size.width != m_viewportW || size.height != m_viewportH || size.IsCropped() ||
                        size.IsTurned());
            if (m_scaled)
            {
                m_width = size.width;
                m_height = size.height;
                m_viewportX = 0;
                m_viewportY = 0;
                m_viewportW = size.width;
                m_viewportH = size.height;
            }
            m_drawList.Clear();
        }

        // Build the per-view draw list from the bound scene: assign each renderable a category and
        // a view-dependent sort key (state bits and view-space depth), then radix-sort. When `cull` is set,
        // reject renderables whose world bounding sphere falls entirely outside the camera frustum first.
        void BuildDrawList(Array<DrawItem>& sortScratch, bool cull = false)
        {
            m_drawList.Clear();
            m_sceneItemCount = 0;
            m_culledCount = 0;
            if (m_scene == nullptr)
            {
                return;
            }

            const Float4x4 viewMat = m_camera.view;
            const f32 invFar = (m_camera.farZ > 0.0f) ? (1.0f / m_camera.farZ) : 1.0f;
            // View frustum for culling (6 planes from the camera VP; D3D z in [0,1], same extraction the
            // shadow cull uses). Built once per view; skipped entirely when culling is off.
            BoundingFrustum frustum{};
            if (cull)
            {
                frustum = BoundingFrustum{m_camera.ViewProjection()};
            }

            for (RenderData* data : m_scene->Items())
            {
                if (data == nullptr)
                {
                    continue;
                }
                ++m_sceneItemCount;
                // View-frustum cull (category-generic): reject when the world bounding sphere is fully
                // outside the frustum. Every producer sets worldCenter and worldRadius on the RenderData base.
                if (cull &&
                    !Intersects(frustum, BoundingSphere{data->worldCenter, data->worldRadius}))
                {
                    ++m_culledCount;
                    continue;
                }
                // Category-generic: read the base sort fields (worldCenter and sortBatchKey, set by the
                // producer) and the category's sort mode from the dynamic registry, with no downcast,
                // so any renderer's data (mesh, sprite, particle) sorts through this one path.
                // view-space depth: forward is -z in RH view space, so distance ~ -z_view.
                const Float3 vc = TransformPoint(data->worldCenter, viewMat);
                const f32 depth01 = (-vc.z) * invFar;

                const bool backToFront =
                    (Categories().Sort(data->category) == SortMode::BackToFront);
                // Front-to-back categories cluster by the producer's batch key so identical draws stay
                // contiguous (fusable); back-to-front zeroes it so depth wins (blend order intact).
                const u32 stateBits = backToFront ? 0u : data->sortBatchKey;
                const u32 depthBits = QuantizeDepth(depth01, /*invert*/ backToFront);
                const u64 key = MakeSortKey(data->category, stateBits, depthBits);

                m_drawList.PushBack(DrawItem{key, data});
            }

            RadixSortDrawItems(m_drawList, sortScratch);
        }

        [[nodiscard]] const ExtractedScene* Scene() const noexcept { return m_scene; }
        [[nodiscard]] const ViewCamera& Camera() const noexcept { return m_camera; }
        // Apply a sub-pixel TAA jitter to the projection, so every downstream read of
        // ViewProjection() (prepass, forward, sky) uses the same jittered matrix: a constant NDC
        // shift in either projection (projection::ApplyJitter puts it in the row that does that;
        // passes read it back with projection::JitterOf).
        void ApplyProjectionJitter(f32 jx, f32 jy) noexcept
        {
            projection::ApplyJitter(m_camera.projection, jx, jy);
        }
        [[nodiscard]] const ViewSettings& Settings() const noexcept { return m_settings; }
        [[nodiscard]] rhi::TextureView* Target() const noexcept { return m_target; }
        [[nodiscard]] rhi::TextureFormat TargetFormat() const noexcept { return m_targetFormat; }
        // The size the scene chain runs at, which the transients are sized to: the full target, or
        // with a scene size of its own (IsScaled), that size.
        [[nodiscard]] u32 Width() const noexcept { return m_width; }
        [[nodiscard]] u32 Height() const noexcept { return m_height; }
        [[nodiscard]] i32 ViewportX() const noexcept { return m_viewportX; }
        [[nodiscard]] i32 ViewportY() const noexcept { return m_viewportY; }
        [[nodiscard]] u32 ViewportWidth() const noexcept { return m_viewportW; }
        [[nodiscard]] u32 ViewportHeight() const noexcept { return m_viewportH; }
        // The scene draws at its own size and is scaled into the output rectangle at the end.
        [[nodiscard]] bool IsScaled() const noexcept { return m_scaled; }
        // The real target and the view's rectangle in it: the same as the above, unless scaled.
        [[nodiscard]] u32 OutputWidth() const noexcept { return m_outputWidth; }
        [[nodiscard]] u32 OutputHeight() const noexcept { return m_outputHeight; }
        [[nodiscard]] i32 OutputViewportX() const noexcept { return m_outputViewportX; }
        [[nodiscard]] i32 OutputViewportY() const noexcept { return m_outputViewportY; }
        [[nodiscard]] u32 OutputViewportWidth() const noexcept { return m_outputViewportW; }
        [[nodiscard]] u32 OutputViewportHeight() const noexcept { return m_outputViewportH; }
        [[nodiscard]] Span<const DrawItem> DrawList() const noexcept
        {
            return Span<const DrawItem>{m_drawList.Data(), m_drawList.Size()};
        }
        // Cull stats for the last BuildDrawList (0/0 when culling was off: nothing tested).
        [[nodiscard]] u32 SceneItemCount() const noexcept { return m_sceneItemCount; }
        [[nodiscard]] u32 CulledCount() const noexcept { return m_culledCount; }
        // Opaque per-scene debug-draw list for this view (set by the subsystem; cast back in RenderFrame).
        // Stored as void* to keep Views decoupled from the :debug_draw partition.
        void SetDebugScene(const void* d) noexcept { m_debugScene = d; }
        [[nodiscard]] const void* DebugScene() const noexcept { return m_debugScene; }
        // Opaque per-view debug-draw list (editor chrome: grid and selection gizmos). Drawn only in
        // this view, never in other views of the same scene (such as a camera preview). Scene-level
        // debug (physics, nav) rides m_debugScene and draws in every view.
        void SetDebugView(const void* d) noexcept { m_debugView = d; }
        [[nodiscard]] const void* DebugViewList() const noexcept { return m_debugView; }
        // Opaque scene identity for this view (set by the subsystem), handed to ISceneOverlay
        // sources so per-scene overlay state matches views without the renderer knowing scenes.
        void SetSceneKey(const void* key) noexcept { m_sceneKey = key; }
        [[nodiscard]] const void* SceneKey() const noexcept { return m_sceneKey; }

    private:
        const ExtractedScene* m_scene = nullptr;
        ViewCamera m_camera;
        ViewSettings m_settings;
        rhi::TextureView* m_target = nullptr;
        rhi::TextureFormat m_targetFormat = rhi::TextureFormat::BGRA8Unorm;
        u32 m_width = 0; // the size the scene chain runs at (the full target, or the scene size)
        u32 m_height = 0;
        i32 m_viewportX = 0; // where in that the view draws
        i32 m_viewportY = 0;
        u32 m_viewportW = 0;
        u32 m_viewportH = 0;
        u32 m_outputWidth = 0; // the real target, and the view's rectangle in it
        u32 m_outputHeight = 0;
        i32 m_outputViewportX = 0;
        i32 m_outputViewportY = 0;
        u32 m_outputViewportW = 0;
        u32 m_outputViewportH = 0;
        bool m_scaled = false;
        const void* m_debugScene = nullptr; // opaque debug::DebugDraw* for this view's scene
        const void* m_debugView = nullptr;  // opaque debug::DebugDraw* for this view only
        const void* m_sceneKey = nullptr;   // opaque scene identity (overlay matching)
        Array<DrawItem> m_drawList;         // per-view, owned (pooled storage)
        u32 m_sceneItemCount = 0;           // items considered by the last BuildDrawList
        u32 m_culledCount = 0;              // of those, rejected by the view-frustum cull
    };

    // A per-frame pool of views. `Begin` rewinds it (keeping the RenderView storage and their
    // draw-list buffers); `Acquire` hands out the next view. Views stay valid until the next
    // Begin. Stable addresses: views live in UniquePtr slots so growing the pool never moves
    // an already-acquired view.
    class RenderViewPool
    {
    public:
        explicit RenderViewPool(IAllocator& allocator) noexcept
            : m_allocator(&allocator), m_views(allocator)
        {
        }

        void Begin() noexcept { m_count = 0; }

        [[nodiscard]] RenderView* Acquire()
        {
            if (m_count == m_views.Size())
            {
                m_views.PushBack(MakeUnique<RenderView>(*m_allocator));
            }
            return m_views[m_count++].Get();
        }

        [[nodiscard]] usize ActiveCount() const noexcept { return m_count; }
        [[nodiscard]] RenderView* At(usize i) const noexcept { return m_views[i].Get(); }

    private:
        IAllocator* m_allocator;
        Array<UniquePtr<RenderView>> m_views;
        usize m_count = 0;
    };

} // namespace foundation::render
