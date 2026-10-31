// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:debug_pass` partition (Debug layer).
///
/// The GPU side of debug draw, in one owner on the render graph. Owns the font atlas, the
/// per-(view, frame) dynamic vertex buffers and the pipelines, and exposes DeclareGeometry and
/// DeclareScreen. Both are declared per view in the frame: they merge the global and the per-scene
/// DebugDraw and project through that view's ViewProj into the LDR target's sub-rect, so
/// side-by-side scenes and views don't bleed. Geometry has depth-tested (LessEqual, read-only depth)
/// and overlay (Always) buckets; screen text and rects are always on top.

module;
#include "Core/Prelude.h"

export module foundation.render:debug_pass;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :debug_font;
import :debug_draw;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // The debug draw's push blocks (debug_geom.vs.hlsl GeomPush, debug_grid.hlsli GridPush,
    // debug_screen.vs.hlsl ScreenPush); Integration.ShaderBlocks checks each against the compiled
    // shader, member by member.
    struct GpuDebugGeomPush
    {
        Float4x4 viewProj;
    };
    static_assert(sizeof(GpuDebugGeomPush) == 64, "push GeomPush layout drift");
    struct GpuDebugGridPush
    {
        Float4x4 viewProj;
        Float4 originExtent; // xyz: the quad's centre on the plane; w: metres each way
        Float4 axisUSpacing; // xyz: the plane's first axis; w: the finest line spacing, metres
        Float4 axisVBlend;   // xyz: the plane's second axis; w: 0..1 toward the next decade
        Float4 cameraFade;   // xyz: the camera; w: the distance the grid has faded out by
    };
    static_assert(sizeof(GpuDebugGridPush) == 128, "push GridPush layout drift");
    struct GpuDebugScreenPush
    {
        Float2 invSize{}; // 1 / the view's size in pixels
        Float2 pad{};
    };
    static_assert(sizeof(GpuDebugScreenPush) == 16, "push ScreenPush layout drift");

    class DebugDrawPass
    {
    public:
        DebugDrawPass(rhi::Device& device, shaders::ShaderSystem& shaders,
                      u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight)
        {
        }
        ~DebugDrawPass() { Shutdown(); }
        DebugDrawPass(const DebugDrawPass&) = delete;
        DebugDrawPass& operator=(const DebugDrawPass&) = delete;

        Status Initialize();

        // Declare the geometry pass for one view: draw the world-space lines and triangles of `global`,
        // `scene` and `view` (depth-tested and overlay) through `viewProj`, into `color` (Load),
        // read-only against `depth`, in the view sub-rect. `scene` is drawn in every view of the scene;
        // `view` is this view's private list (editor chrome).
        void DeclareGeometry(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                             rendergraph::RGHandle depth, const Float4x4& viewProj,
                             const debug::DebugDraw* global, const debug::DebugDraw* scene,
                             const debug::DebugDraw* view, rhi::TextureFormat colorFmt,
                             rhi::TextureFormat depthFmt, i32 vpX, i32 vpY, u32 vpW, u32 vpH,
                             u32 frameIndex, u32 viewIndex);

        // Declare the screen pass for one view: build glyph and rect quads from the 2D and 3D-text commands
        // of `global` and `scene` (3D projected through `viewProj`), and draw them on top into `color` in
        // the view sub-rect.
        void DeclareScreen(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                           const Float4x4& viewProj, const debug::DebugDraw* global,
                           const debug::DebugDraw* scene, const debug::DebugDraw* view,
                           rhi::TextureFormat colorFmt, i32 vpX,
                           i32 vpY, u32 vpW, u32 vpH, u32 frameIndex, u32 viewIndex);

    private:
        static constexpr u32 kMaxViews = 8;
        static constexpr u32 kMaxSlots = kMaxViews * 8;

        struct Pipelines
        {
            rhi::TextureFormat color = rhi::TextureFormat::Undefined;
            rhi::TextureFormat depth = rhi::TextureFormat::Undefined;
            rhi::RenderPipeline* lineDepth = nullptr;
            rhi::RenderPipeline* lineOverlay = nullptr;
            rhi::RenderPipeline* triDepth = nullptr;
            rhi::RenderPipeline* triOverlay = nullptr;
            rhi::RenderPipeline* grid = nullptr; // the shader grid; null when it did not build
            u64 shaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        };

        static void AppendVerts(Array<debug::DebugVertex>& dst, const Array<debug::DebugVertex>* a,
                                const Array<debug::DebugVertex>* b,
                                const Array<debug::DebugVertex>* c = nullptr);

        // Emit pixel-space glyph and rect quads (two triangles each) for a list's 2D commands and projected
        // 3D text.
        void BuildScreenQuads(Array<debug::DebugTextVertex>& out, const debug::DebugDraw* d,
                              const Float4x4& viewProj, u32 vpW, u32 vpH);

        static void EmitQuad(Array<debug::DebugTextVertex>& out, f32 x, f32 y, f32 w, f32 h, f32 u0,
                             f32 v0, f32 u1, f32 v1, u32 col);

        rhi::Buffer* UploadGeom(u32 slot, const Array<debug::DebugVertex>& verts);
        rhi::Buffer* UploadScreen(u32 slot, const Array<debug::DebugTextVertex>& verts);
        bool EnsureBuffer(rhi::Buffer*& buf, u64& cap, u64 bytes);

        Pipelines* EnsurePipelines(rhi::TextureFormat colorFmt, rhi::TextureFormat depthFmt);

        rhi::RenderPipeline* MakeGeomPipeline(rhi::TextureFormat colorFmt,
                                              rhi::TextureFormat depthFmt,
                                              rhi::PrimitiveTopology topo,
                                              rhi::CompareFunction cmp);

        rhi::RenderPipeline* EnsureScreenPipeline(rhi::TextureFormat colorFmt);
        rhi::RenderPipeline* MakeGridPipeline(rhi::TextureFormat colorFmt, rhi::TextureFormat depthFmt);

        Status CreateFontAtlas();

        void DestroyGeomPipelines(Pipelines& entry);
        void DestroyAllGeomPipelines();
        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;
        rhi::PipelineLayout* m_geomLayout = nullptr;
        rhi::PipelineLayout* m_gridLayout = nullptr; // the grid's push block (both stages)
        rhi::PipelineLayout* m_screenLayout = nullptr;
        rhi::BindGroupLayout* m_screenBgLayout = nullptr;
        rhi::Sampler* m_sampler = nullptr;
        rhi::Texture* m_fontTex = nullptr;
        rhi::TextureView* m_fontView = nullptr;
        rhi::BindGroup* m_fontBg = nullptr;
        // Per-(color, depth) entries: a frame can hold views with different target formats,
        // and destroying on mismatch would free pipelines an earlier view's recorded commands
        // still reference.
        static constexpr usize kMaxGeomFormats = 4;
        Pipelines m_geom[kMaxGeomFormats] = {};
        rhi::RenderPipeline* m_screenPipe = nullptr;
        rhi::TextureFormat m_screenFmt = rhi::TextureFormat::Undefined;
        u64 m_screenShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Buffer* m_geomBuf[kMaxSlots] = {};
        u64 m_geomCap[kMaxSlots] = {};
        rhi::Buffer* m_screenBuf[kMaxSlots] = {};
        u64 m_screenCap[kMaxSlots] = {};
    };

} // namespace foundation::render
