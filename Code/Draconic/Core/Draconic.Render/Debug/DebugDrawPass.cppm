/// Draconic::Render - the `:debug_pass` partition (Debug layer).
///
/// The GPU side of debug draw (SedulousEngine's DebugDrawSystem + DebugGeometryPass + DebugScreenPass,
/// folded into one owner adapted to the render graph). Owns the font atlas + per-(view,frame) dynamic
/// vertex buffers + the pipelines, and exposes DeclareGeometry / DeclareScreen - each declared PER VIEW
/// in the frame, merging a GLOBAL + a per-SCENE DebugDraw and projecting through that view's ViewProj
/// into the LDR target's sub-rect (so side-by-side scenes/views don't bleed). Geometry has depth-tested
/// (LessEqual, read-only depth) + overlay (Always) buckets; screen text/rects are always-on-top.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:debug_pass;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :debug_font;
import :debug_draw;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

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

        // Declare the geometry pass for one view: draw `global`+`scene` world-space lines/triangles (depth
        // + overlay) through `viewProj`, into `color` (Load), read-only against `depth`, in the view sub-rect.
        void DeclareGeometry(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                             rendergraph::RGHandle depth, const Float4x4& viewProj,
                             const debug::DebugDraw* global, const debug::DebugDraw* scene,
                             rhi::TextureFormat colorFmt, rhi::TextureFormat depthFmt, i32 vpX,
                             i32 vpY, u32 vpW, u32 vpH, u32 frameIndex, u32 viewIndex);

        // Declare the screen pass for one view: build glyph/rect quads from `global`+`scene` 2D + 3D-text
        // commands (3D projected through `viewProj`), draw always-on-top into `color` in the view sub-rect.
        void DeclareScreen(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                           const Float4x4& viewProj, const debug::DebugDraw* global,
                           const debug::DebugDraw* scene, rhi::TextureFormat colorFmt, i32 vpX,
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
            u64 shaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        };

        static void AppendVerts(Array<debug::DebugVertex>& dst, const Array<debug::DebugVertex>* a,
                                const Array<debug::DebugVertex>* b);

        // Emit pixel-space glyph/rect quads (2 tris each) for a list's 2D commands + projected 3D text.
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

        Status CreateFontAtlas();

        void DestroyGeomPipelines();
        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;
        rhi::PipelineLayout* m_geomLayout = nullptr;
        rhi::PipelineLayout* m_screenLayout = nullptr;
        rhi::BindGroupLayout* m_screenBgLayout = nullptr;
        rhi::Sampler* m_sampler = nullptr;
        rhi::Texture* m_fontTex = nullptr;
        rhi::TextureView* m_fontView = nullptr;
        rhi::BindGroup* m_fontBg = nullptr;
        Pipelines m_geom{};
        rhi::RenderPipeline* m_screenPipe = nullptr;
        rhi::TextureFormat m_screenFmt = rhi::TextureFormat::Undefined;
        u64 m_screenShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Buffer* m_geomBuf[kMaxSlots] = {};
        u64 m_geomCap[kMaxSlots] = {};
        rhi::Buffer* m_screenBuf[kMaxSlots] = {};
        u64 m_screenCap[kMaxSlots] = {};
    };

} // namespace draconic::render
