/// Draconic::Render - the `:views` partition.
///
/// `RenderView` is the renderer's unit of work *and* its isolation boundary: render one
/// scene's extracted data, from one camera, into one target. A frame renders a SET of
/// views - primary cameras, plus (later) derived shadow/probe views - and `RenderView`
/// owning all per-frame-mutable state (the culled+sorted draw list, and later the view
/// UBO, cluster grid, shadow slots, HDR/depth targets) is what guarantees views don't
/// trash each other. The only state shared across views is the immutable `ExtractedScene`
/// and shared GPU resources. (§9 of docs/design/renderer.md.)
///
/// Views are pooled per frame from a `RenderViewPool` (reset, not freed, each frame).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:views;

// ViewCamera + ViewportRect moved to the light `draconic.render.api` module (with
// ISceneRenderer); re-exported here so draconic.render importers see them unchanged.
export import draconic.render.api;

import draconic.foundation;
import draconic.rhi;
import :data;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // Per-view settings (grows with post config, layer mask, etc. in later phases).
    struct ViewSettings
    {
        rhi::ClearColor clear = rhi::ClearColor::CornflowerBlue();
        // Viewport sub-rect within the target, in pixels (split-screen). Width 0 => the full target.
        i32 viewportX = 0, viewportY = 0;
        u32 viewportWidth = 0, viewportHeight = 0;
        // Target resource-state handling for the imported color target. `targetTexture` is the backing
        // texture the graph barriers (null => host-managed backbuffer; the graph touches no barrier).
        // `targetFinalState` is where the graph leaves it - RenderTarget for present, or ShaderRead /
        // CopySrc for an offscreen target the caller then samples / blits.
        rhi::Texture* targetTexture = nullptr;
        rhi::ResourceState targetCurrentState = rhi::ResourceState::RenderTarget;
        rhi::ResourceState targetFinalState = rhi::ResourceState::RenderTarget;
        // Resolved per-view post-processing (exposure/bloom/AO). The RenderSubsystem fills this from
        // the scene's authored PostProcessSettings; the compose passes read it per view.
        ViewPostConfig post{};
    };

    // A single view: what to draw (a shared ExtractedScene), from where (camera), into what
    // (target). Owns the per-view draw list. Pooled - `Reset` rebinds it for a new use without
    // freeing the draw-list storage.
    class RenderView
    {
    public:
        // Bind this view to a scene snapshot + camera + target for the current frame.
        void Bind(const ExtractedScene& scene, const ViewCamera& camera,
                  const ViewSettings& settings, rhi::TextureView* target,
                  rhi::TextureFormat targetFormat, u32 width, u32 height) noexcept
        {
            m_scene = &scene;
            m_camera = camera;
            m_settings = settings;
            m_target = target;
            m_targetFormat = targetFormat;
            m_width = width; // full target (color import + transient depth size)
            m_height = height;
            // Viewport sub-rect within the target (defaults to the whole target).
            m_viewportX = (settings.viewportWidth > 0) ? settings.viewportX : 0;
            m_viewportY = (settings.viewportWidth > 0) ? settings.viewportY : 0;
            m_viewportW = (settings.viewportWidth > 0) ? settings.viewportWidth : width;
            m_viewportH = (settings.viewportHeight > 0) ? settings.viewportHeight : height;
            m_drawList.Clear();
        }

        // Build the per-view draw list from the bound scene: assign each renderable a category +
        // a view-dependent sort key (state bits + view-space depth), then radix-sort. When `cull` is set,
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
                // outside the frustum. Every producer sets worldCenter/worldRadius on the RenderData base.
                if (cull &&
                    !Intersects(frustum, BoundingSphere{data->worldCenter, data->worldRadius}))
                {
                    ++m_culledCount;
                    continue;
                }
                // Category-generic: read the base sort fields (worldCenter/sortBatchKey the producer set)
                // + the category's sort mode from the dynamic registry - no downcast to a concrete type,
                // so any renderer's data (mesh, sprite, particle) sorts through this one path.
                // view-space depth: forward is -z in RH view space, so distance ~ -z_view.
                const Float3 vc = TransformPoint(data->worldCenter, viewMat);
                const f32 depth01 = (-vc.z) * invFar;

                const bool backToFront =
                    (Categories().Sort(data->category) == SortMode::BackToFront);
                // Front-to-back categories cluster by the producer's batch key so identical draws stay
                // contiguous (fusable); back-to-front zeroes it so depth dominates (blend order intact).
                const u32 stateBits = backToFront ? 0u : data->sortBatchKey;
                const u32 depthBits = QuantizeDepth(depth01, /*invert*/ backToFront);
                const u64 key = MakeSortKey(data->category, stateBits, depthBits);

                m_drawList.PushBack(DrawItem{key, data});
            }

            RadixSortDrawItems(m_drawList, sortScratch);
        }

        [[nodiscard]] const ExtractedScene* Scene() const noexcept { return m_scene; }
        [[nodiscard]] const ViewCamera& Camera() const noexcept { return m_camera; }
        // Apply a sub-pixel TAA jitter (clip-space offset) to the projection, so every downstream read of
        // ViewProjection() (prepass, forward, sky) uses the same jittered matrix. Row-vector convention:
        // offsetting proj(2,0)/(2,1) shifts clip.xy by jitter*clip.w -> a constant pixel offset.
        void ApplyProjectionJitter(f32 jx, f32 jy) noexcept
        {
            m_camera.projection(2, 0) += jx;
            m_camera.projection(2, 1) += jy;
        }
        [[nodiscard]] const ViewSettings& Settings() const noexcept { return m_settings; }
        [[nodiscard]] rhi::TextureView* Target() const noexcept { return m_target; }
        [[nodiscard]] rhi::TextureFormat TargetFormat() const noexcept { return m_targetFormat; }
        [[nodiscard]] u32 Width() const noexcept { return m_width; } // full target
        [[nodiscard]] u32 Height() const noexcept { return m_height; }
        [[nodiscard]] i32 ViewportX() const noexcept { return m_viewportX; }
        [[nodiscard]] i32 ViewportY() const noexcept { return m_viewportY; }
        [[nodiscard]] u32 ViewportWidth() const noexcept { return m_viewportW; }
        [[nodiscard]] u32 ViewportHeight() const noexcept { return m_viewportH; }
        [[nodiscard]] Span<const DrawItem> DrawList() const noexcept
        {
            return Span<const DrawItem>{m_drawList.Data(), m_drawList.Size()};
        }
        // Cull stats for the last BuildDrawList (0/0 when culling was off - nothing tested).
        [[nodiscard]] u32 SceneItemCount() const noexcept { return m_sceneItemCount; }
        [[nodiscard]] u32 CulledCount() const noexcept { return m_culledCount; }
        // Opaque per-scene debug-draw list for this view (set by the subsystem; cast back in RenderFrame).
        // Stored as void* to keep Views decoupled from the :debug_draw partition.
        void SetDebugScene(const void* d) noexcept { m_debugScene = d; }
        [[nodiscard]] const void* DebugScene() const noexcept { return m_debugScene; }
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
        u32 m_width = 0; // full target size
        u32 m_height = 0;
        i32 m_viewportX = 0; // viewport sub-rect within the target
        i32 m_viewportY = 0;
        u32 m_viewportW = 0;
        u32 m_viewportH = 0;
        const void* m_debugScene = nullptr; // opaque debug::DebugDraw* for this view's scene
        const void* m_sceneKey = nullptr;   // opaque scene identity (overlay matching)
        Array<DrawItem> m_drawList;         // per-view, owned (pooled storage)
        u32 m_sceneItemCount = 0;           // items considered by the last BuildDrawList
        u32 m_culledCount = 0;              // of those, rejected by the view-frustum cull
    };

    // A per-frame pool of views. `Begin` rewinds it (keeping the RenderView storage + their
    // draw-list buffers); `Acquire` hands out the next view. Views stay valid until the next
    // Begin. Stable addresses: views live in UniquePtr slots so growing the pool never moves
    // an already-acquired view.
    class RenderViewPool
    {
    public:
        void Begin() noexcept { m_count = 0; }

        [[nodiscard]] RenderView* Acquire()
        {
            if (m_count == m_views.Size())
            {
                m_views.PushBack(MakeUnique<RenderView>(DefaultAllocator()));
            }
            return m_views[m_count++].Get();
        }

        [[nodiscard]] usize ActiveCount() const noexcept { return m_count; }
        [[nodiscard]] RenderView* At(usize i) const noexcept { return m_views[i].Get(); }

    private:
        Array<UniquePtr<RenderView>> m_views;
        usize m_count = 0;
    };

} // namespace draconic::render
