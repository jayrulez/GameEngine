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

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :debug_font;
import :debug_draw;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    Status DebugDrawPass::Initialize()
    {

        // Geometry pipeline layout: just the ViewProj push (no bind groups).
        rhi::PushConstantRange gpc{};
        gpc.stages = rhi::ShaderStage::Vertex;
        gpc.offset = 0;
        gpc.size = sizeof(Float4x4);
        // No bind groups -> the push block lives at space0 (debug_geom.vs.hlsl), so the
        // browser push-constant emulation must synthesize its uniform at group 0 too. The
        // range default (1) matches every OTHER shader's space1 convention; here it must be 0.
        gpc.bindGroupIndex = 0;
        rhi::PipelineLayoutDesc gpld{};
        gpld.pushConstantRanges = Span<const rhi::PushConstantRange>{&gpc, 1};
        if (!m_device->CreatePipelineLayout(gpld, m_geomLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        // Screen pipeline: font atlas (t0) + sampler (s0) + InvSize push.
        rhi::BindGroupLayoutEntry se[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        rhi::BindGroupLayoutDesc sld{};
        sld.entries = Span<const rhi::BindGroupLayoutEntry>{se, 2};
        if (!m_device->CreateBindGroupLayout(sld, m_screenBgLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BindGroupLayout* sbl[] = {m_screenBgLayout};
        rhi::PushConstantRange spc{};
        spc.stages = rhi::ShaderStage::Vertex;
        spc.offset = 0;
        spc.size = sizeof(f32) * 4;
        rhi::PipelineLayoutDesc spld{};
        spld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{sbl, 1};
        spld.pushConstantRanges = Span<const rhi::PushConstantRange>{&spc, 1};
        if (!m_device->CreatePipelineLayout(spld, m_screenLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Nearest;
        ss.magFilter = rhi::FilterMode::Nearest;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"debug.fontSampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        return CreateFontAtlas();
    }

    void DebugDrawPass::DeclareGeometry(rendergraph::RenderGraph& graph,
                                        rendergraph::RGHandle color, rendergraph::RGHandle depth,
                                        const Float4x4& viewProj, const debug::DebugDraw* global,
                                        const debug::DebugDraw* scene, rhi::TextureFormat colorFmt,
                                        rhi::TextureFormat depthFmt, i32 vpX, i32 vpY, u32 vpW,
                                        u32 vpH, u32 frameIndex, u32 viewIndex)
    {
        // Pack the 4 streams (depth-lines, overlay-lines, depth-tris, overlay-tris) into one buffer.
        Array<debug::DebugVertex> verts;
        const u32 dl0 = 0;
        AppendVerts(verts, global ? &global->LineVertices() : nullptr,
                    scene ? &scene->LineVertices() : nullptr);
        const u32 ol0 = static_cast<u32>(verts.Size());
        AppendVerts(verts, global ? &global->OverlayLineVertices() : nullptr,
                    scene ? &scene->OverlayLineVertices() : nullptr);
        const u32 dt0 = static_cast<u32>(verts.Size());
        AppendVerts(verts, global ? &global->TriVertices() : nullptr,
                    scene ? &scene->TriVertices() : nullptr);
        const u32 ot0 = static_cast<u32>(verts.Size());
        AppendVerts(verts, global ? &global->OverlayTriVertices() : nullptr,
                    scene ? &scene->OverlayTriVertices() : nullptr);
        const u32 total = static_cast<u32>(verts.Size());
        if (total == 0)
        {
            return;
        }

        const u32 slot =
            (viewIndex % kMaxViews) * m_framesInFlight + (frameIndex % m_framesInFlight);
        rhi::Buffer* vb = UploadGeom(slot, verts);
        if (vb == nullptr)
        {
            return;
        }
        Pipelines* p = EnsurePipelines(colorFmt, depthFmt);
        if (p == nullptr)
        {
            return;
        }

        const u32 dlN = ol0 - dl0, olN = dt0 - ol0, dtN = ot0 - dt0, otN = total - ot0;
        const Float4x4 vpMat = viewProj;
        graph.AddRenderPass(
            u8"debug.geom",
            [color, depth, vb, p, vpX, vpY, vpW, vpH, vpMat, dlN, ol0, olN, dt0, dtN, ot0,
             otN](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, color, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.SetReadOnlyDepthTarget(
                    depth); // depth-test only (no sampling) -> no ReadTexture (layout conflict)
                b.SetViewport(vpX, vpY, vpW, vpH);
                b.NeverCull();
                b.SetExecute(
                    [vb, p, vpMat, dlN, ol0, olN, dt0, dtN, ot0, otN](rhi::RenderPassEncoder& rp)
                    {
                        // SetPipeline before SetPushConstants (push needs a bound layout); all 4 pipelines
                        // share the geom layout so re-pushing the ViewProj per stream is fine.
                        rp.SetVertexBuffer(0, vb, 0);
                        const auto draw = [&](rhi::RenderPipeline* pipe, u32 count, u32 first)
                        {
                            rp.SetPipeline(pipe);
                            rp.SetPushConstants(rhi::ShaderStage::Vertex, 0, sizeof(Float4x4),
                                                &vpMat);
                            rp.Draw(count, 1, first, 0);
                        };
                        if (dlN > 0)
                        {
                            draw(p->lineDepth, dlN, 0);
                        }
                        if (olN > 0)
                        {
                            draw(p->lineOverlay, olN, ol0);
                        }
                        if (dtN > 0)
                        {
                            draw(p->triDepth, dtN, dt0);
                        }
                        if (otN > 0)
                        {
                            draw(p->triOverlay, otN, ot0);
                        }
                    });
            });
    }

    void DebugDrawPass::DeclareScreen(rendergraph::RenderGraph& graph, rendergraph::RGHandle color,
                                      const Float4x4& viewProj, const debug::DebugDraw* global,
                                      const debug::DebugDraw* scene, rhi::TextureFormat colorFmt,
                                      i32 vpX, i32 vpY, u32 vpW, u32 vpH, u32 frameIndex,
                                      u32 viewIndex)
    {
        Array<debug::DebugTextVertex> verts;
        BuildScreenQuads(verts, global, viewProj, vpW, vpH);
        BuildScreenQuads(verts, scene, viewProj, vpW, vpH);
        if (verts.IsEmpty())
        {
            return;
        }

        const u32 slot =
            (viewIndex % kMaxViews) * m_framesInFlight + (frameIndex % m_framesInFlight);
        rhi::Buffer* vb = UploadScreen(slot, verts);
        if (vb == nullptr)
        {
            return;
        }
        rhi::RenderPipeline* pipe = EnsureScreenPipeline(colorFmt);
        if (pipe == nullptr)
        {
            return;
        }
        const u32 count = static_cast<u32>(verts.Size());
        const f32 push[4] = {1.0f / static_cast<f32>(vpW), 1.0f / static_cast<f32>(vpH), 0.0f,
                             0.0f};
        rhi::BindGroup* bg = m_fontBg;
        graph.AddRenderPass(
            u8"debug.screen",
            [color, vb, pipe, bg, vpX, vpY, vpW, vpH, count, push](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, color, rhi::LoadOp::Load, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.SetViewport(vpX, vpY, vpW, vpH);
                b.NeverCull();
                b.SetExecute(
                    [vb, pipe, bg, count, push](rhi::RenderPassEncoder& rp)
                    {
                        rp.SetPipeline(pipe);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetVertexBuffer(0, vb, 0);
                        rp.SetPushConstants(rhi::ShaderStage::Vertex, 0, sizeof(push), push);
                        rp.Draw(count, 1, 0, 0);
                    });
            });
    }

    void DebugDrawPass::AppendVerts(Array<debug::DebugVertex>& dst,
                                    const Array<debug::DebugVertex>* a,
                                    const Array<debug::DebugVertex>* b)
    {
        if (a)
        {
            for (const auto& v : *a)
            {
                dst.PushBack(v);
            }
        }
        if (b)
        {
            for (const auto& v : *b)
            {
                dst.PushBack(v);
            }
        }
    }

    void DebugDrawPass::BuildScreenQuads(Array<debug::DebugTextVertex>& out,
                                         const debug::DebugDraw* d, const Float4x4& viewProj,
                                         u32 vpW, u32 vpH)
    {
        if (d == nullptr)
        {
            return;
        }
        const Array<u8>& chars = d->TextChars();
        // 2D commands (pixel space).
        for (const debug::Debug2DCommand& cmd : d->Commands2D())
        {
            const u32 col = debug::PackColor(cmd.color);
            if (cmd.kind == debug::Debug2DKind::Rectangle)
            {
                f32 u0, v0, u1, v1;
                debug::GetSolidBlockUV(u0, v0, u1, v1);
                EmitQuad(out, cmd.position.x, cmd.position.y, cmd.size.x, cmd.size.y, u0, v0, u1,
                         v1, col);
            }
            else
            {
                const f32 cw = static_cast<f32>(debug::kCharWidth) * cmd.scale,
                          ch = static_cast<f32>(debug::kCharHeight) * cmd.scale;
                f32 x = cmd.position.x;
                const f32 y = cmd.position.y;
                for (i32 i = 0; i < cmd.textLength; ++i)
                {
                    f32 u0, v0, u1, v1;
                    if (debug::GetCharUV(chars[static_cast<usize>(cmd.textStart + i)], u0, v0, u1,
                                         v1))
                    {
                        EmitQuad(out, x, y, cw, ch, u0, v0, u1, v1, col);
                    }
                    x += cw;
                }
            }
        }
        // 3D text: project world -> pixel, then emit glyph quads.
        for (const debug::Debug3DTextCommand& cmd : d->TextCommands3D())
        {
            const f32 cx = cmd.worldPos.x * viewProj(0, 0) + cmd.worldPos.y * viewProj(1, 0) +
                           cmd.worldPos.z * viewProj(2, 0) + viewProj(3, 0);
            const f32 cyy = cmd.worldPos.x * viewProj(0, 1) + cmd.worldPos.y * viewProj(1, 1) +
                            cmd.worldPos.z * viewProj(2, 1) + viewProj(3, 1);
            const f32 cw4 = cmd.worldPos.x * viewProj(0, 3) + cmd.worldPos.y * viewProj(1, 3) +
                            cmd.worldPos.z * viewProj(2, 3) + viewProj(3, 3);
            if (cw4 <= 0.0f)
            {
                continue;
            } // behind camera
            const f32 ndcX = cx / cw4, ndcY = cyy / cw4;
            f32 px = (ndcX * 0.5f + 0.5f) * static_cast<f32>(vpW);
            const f32 py = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<f32>(vpH);
            const u32 col = debug::PackColor(cmd.color);
            const f32 cw = static_cast<f32>(debug::kCharWidth),
                      chh = static_cast<f32>(debug::kCharHeight);
            for (i32 i = 0; i < cmd.textLength; ++i)
            {
                f32 u0, v0, u1, v1;
                if (debug::GetCharUV(chars[static_cast<usize>(cmd.textStart + i)], u0, v0, u1, v1))
                {
                    EmitQuad(out, px, py, cw, chh, u0, v0, u1, v1, col);
                }
                px += cw;
            }
        }
    }

    void DebugDrawPass::EmitQuad(Array<debug::DebugTextVertex>& out, f32 x, f32 y, f32 w, f32 h,
                                 f32 u0, f32 v0, f32 u1, f32 v1, u32 col)
    {
        const debug::DebugTextVertex tl{Float3{x, y, 0}, Float2{u0, v0}, col};
        const debug::DebugTextVertex tr{Float3{x + w, y, 0}, Float2{u1, v0}, col};
        const debug::DebugTextVertex br{Float3{x + w, y + h, 0}, Float2{u1, v1}, col};
        const debug::DebugTextVertex bl{Float3{x, y + h, 0}, Float2{u0, v1}, col};
        out.PushBack(tl);
        out.PushBack(tr);
        out.PushBack(br);
        out.PushBack(tl);
        out.PushBack(br);
        out.PushBack(bl);
    }

    rhi::Buffer* DebugDrawPass::UploadGeom(u32 slot, const Array<debug::DebugVertex>& verts)
    {
        if (slot >= kMaxSlots)
        {
            return nullptr;
        }
        const u64 bytes = verts.Size() * sizeof(debug::DebugVertex);
        if (!EnsureBuffer(m_geomBuf[slot], m_geomCap[slot], bytes))
        {
            return nullptr;
        }
        if (void* p = m_geomBuf[slot]->Map())
        {
            MemCopy(p, verts.Data(), bytes);
            m_geomBuf[slot]->Unmap();
        }
        return m_geomBuf[slot];
    }

    rhi::Buffer* DebugDrawPass::UploadScreen(u32 slot, const Array<debug::DebugTextVertex>& verts)
    {
        if (slot >= kMaxSlots)
        {
            return nullptr;
        }
        const u64 bytes = verts.Size() * sizeof(debug::DebugTextVertex);
        if (!EnsureBuffer(m_screenBuf[slot], m_screenCap[slot], bytes))
        {
            return nullptr;
        }
        if (void* p = m_screenBuf[slot]->Map())
        {
            MemCopy(p, verts.Data(), bytes);
            m_screenBuf[slot]->Unmap();
        }
        return m_screenBuf[slot];
    }

    bool DebugDrawPass::EnsureBuffer(rhi::Buffer*& buf, u64& cap, u64 bytes)
    {
        if (buf != nullptr && cap >= bytes)
        {
            return true;
        }
        if (buf != nullptr)
        {
            m_device->DestroyBuffer(buf);
            buf = nullptr;
            cap = 0;
        }
        u64 newCap = cap > 0 ? cap : 4096;
        while (newCap < bytes)
        {
            newCap *= 2;
        }
        rhi::BufferDesc bd{};
        bd.size = newCap;
        bd.usage = rhi::BufferUsage::Vertex;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        bd.label = u8"debug.vtx";
        if (!m_device->CreateBuffer(bd, buf).IsOk())
        {
            buf = nullptr;
            cap = 0;
            return false;
        }
        cap = newCap;
        return true;
    }

    DebugDrawPass::Pipelines* DebugDrawPass::EnsurePipelines(rhi::TextureFormat colorFmt,
                                                             rhi::TextureFormat depthFmt)
    {
        const u64 shaderVersion = m_shaders->Version(u8"debug_geom"); // hot reload rebuilds
        if (m_geom.color == colorFmt && m_geom.depth == depthFmt && m_geom.lineDepth != nullptr &&
            m_geom.shaderVersion == shaderVersion)
        {
            return &m_geom;
        }
        DestroyGeomPipelines();
        m_geom.lineDepth = MakeGeomPipeline(colorFmt, depthFmt, rhi::PrimitiveTopology::LineList,
                                            rhi::CompareFunction::LessEqual);
        m_geom.lineOverlay = MakeGeomPipeline(colorFmt, depthFmt, rhi::PrimitiveTopology::LineList,
                                              rhi::CompareFunction::Always);
        m_geom.triDepth = MakeGeomPipeline(colorFmt, depthFmt, rhi::PrimitiveTopology::TriangleList,
                                           rhi::CompareFunction::LessEqual);
        m_geom.triOverlay = MakeGeomPipeline(
            colorFmt, depthFmt, rhi::PrimitiveTopology::TriangleList, rhi::CompareFunction::Always);
        if (m_geom.lineDepth == nullptr || m_geom.lineOverlay == nullptr ||
            m_geom.triDepth == nullptr || m_geom.triOverlay == nullptr)
        {
            return nullptr;
        }
        m_geom.color = colorFmt;
        m_geom.depth = depthFmt;
        m_geom.shaderVersion = shaderVersion;
        return &m_geom;
    }

    rhi::RenderPipeline* DebugDrawPass::MakeGeomPipeline(rhi::TextureFormat colorFmt,
                                                         rhi::TextureFormat depthFmt,
                                                         rhi::PrimitiveTopology topo,
                                                         rhi::CompareFunction cmp)
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"debug_geom", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(
            u8"debug_geom", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        rhi::VertexAttribute attrs[] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                        {rhi::VertexFormat::Unorm8x4, 12, 1}};
        rhi::VertexBufferLayout vbl{};
        vbl.stride = sizeof(debug::DebugVertex);
        vbl.stepMode = rhi::VertexStepMode::Vertex;
        vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 2};
        rhi::ColorTargetState color{};
        color.format = colorFmt;
        color.blend = rhi::BlendState::AlphaBlend();
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::DepthStencilState ds{};
        ds.format = depthFmt;
        ds.depthWriteEnabled = false;
        ds.depthCompare = cmp;
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_geomLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
        pd.fragment = frag;
        pd.primitive.topology = topo;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.depthStencil = ds;
        pd.label = u8"debug.geom";
        rhi::RenderPipeline* p = nullptr;
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return nullptr;
        }
        return p;
    }

    rhi::RenderPipeline* DebugDrawPass::EnsureScreenPipeline(rhi::TextureFormat colorFmt)
    {
        const u64 shaderVersion = m_shaders->Version(u8"debug_screen"); // hot reload rebuilds
        if (m_screenPipe != nullptr && m_screenFmt == colorFmt &&
            m_screenShaderVersion == shaderVersion)
        {
            return m_screenPipe;
        }
        if (m_screenPipe != nullptr)
        {
            m_device->DestroyRenderPipeline(m_screenPipe);
            m_screenPipe = nullptr;
        }
        rhi::ShaderModule* vs = m_shaders->GetVariant(
            u8"debug_screen", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(
            u8"debug_screen", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        rhi::VertexAttribute attrs[] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                        {rhi::VertexFormat::Float32x2, 12, 1},
                                        {rhi::VertexFormat::Unorm8x4, 20, 2}};
        rhi::VertexBufferLayout vbl{};
        vbl.stride = sizeof(debug::DebugTextVertex);
        vbl.stepMode = rhi::VertexStepMode::Vertex;
        vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 3};
        rhi::ColorTargetState color{};
        color.format = colorFmt;
        color.blend = rhi::BlendState::AlphaBlend();
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_screenLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"debug.screen";
        if (!m_device->CreateRenderPipeline(pd, m_screenPipe).IsOk())
        {
            m_screenPipe = nullptr;
            return nullptr;
        }
        m_screenFmt = colorFmt;
        m_screenShaderVersion = shaderVersion;
        return m_screenPipe;
    }

    Status DebugDrawPass::CreateFontAtlas()
    {
        Array<u8> pixels = debug::GenerateTextureData();
        const u32 w = static_cast<u32>(debug::kTextureWidth),
                  h = static_cast<u32>(debug::kTextureHeight);
        rhi::TextureDesc td{};
        td.dimension = rhi::TextureDimension::Texture2D;
        td.format = rhi::TextureFormat::R8Unorm;
        td.width = w;
        td.height = h;
        td.depth = 1;
        td.arrayLayerCount = 1;
        td.mipLevelCount = 1;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"debug.font";
        if (!m_device->CreateTexture(td, m_fontTex).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::R8Unorm;
        vd.dimension = rhi::TextureViewDimension::Texture2D;
        vd.mipLevelCount = 1;
        vd.arrayLayerCount = 1;
        if (!m_device->CreateTextureView(m_fontTex, vd, m_fontView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (rhi::Queue* q = m_device->GetQueue(rhi::QueueType::Graphics, 0))
        {
            rhi::TransferBatch* batch = nullptr;
            if (q->CreateTransferBatch(batch).IsOk() && batch != nullptr)
            {
                rhi::TextureDataLayout layout{};
                layout.bytesPerRow = w;
                layout.rowsPerImage = h;
                batch->WriteTexture(m_fontTex, Span<const u8>{pixels.Data(), pixels.Size()}, layout,
                                    rhi::Extent3D{w, h, 1});
                (void)batch->Submit();
                q->DestroyTransferBatch(batch);
            }
        }
        rhi::BindGroupEntry entries[] = {rhi::BindGroupEntry::TextureEntry(m_fontView),
                                         rhi::BindGroupEntry::SamplerEntry(m_sampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_screenBgLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
        if (!m_device->CreateBindGroup(bgd, m_fontBg).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void DebugDrawPass::DestroyGeomPipelines()
    {
        if (m_geom.lineDepth != nullptr)
        {
            m_device->DestroyRenderPipeline(m_geom.lineDepth);
            m_geom.lineDepth = nullptr;
        }
        if (m_geom.lineOverlay != nullptr)
        {
            m_device->DestroyRenderPipeline(m_geom.lineOverlay);
            m_geom.lineOverlay = nullptr;
        }
        if (m_geom.triDepth != nullptr)
        {
            m_device->DestroyRenderPipeline(m_geom.triDepth);
            m_geom.triDepth = nullptr;
        }
        if (m_geom.triOverlay != nullptr)
        {
            m_device->DestroyRenderPipeline(m_geom.triOverlay);
            m_geom.triOverlay = nullptr;
        }
    }

    void DebugDrawPass::Shutdown()
    {
        for (u32 i = 0; i < kMaxSlots; ++i)
        {
            if (m_geomBuf[i] != nullptr)
            {
                m_device->DestroyBuffer(m_geomBuf[i]);
                m_geomBuf[i] = nullptr;
            }
            if (m_screenBuf[i] != nullptr)
            {
                m_device->DestroyBuffer(m_screenBuf[i]);
                m_screenBuf[i] = nullptr;
            }
        }
        DestroyGeomPipelines();
        if (m_screenPipe != nullptr)
        {
            m_device->DestroyRenderPipeline(m_screenPipe);
            m_screenPipe = nullptr;
        }
        if (m_fontBg != nullptr)
        {
            m_device->DestroyBindGroup(m_fontBg);
            m_fontBg = nullptr;
        }
        if (m_fontView != nullptr)
        {
            m_device->DestroyTextureView(m_fontView);
            m_fontView = nullptr;
        }
        if (m_fontTex != nullptr)
        {
            m_device->DestroyTexture(m_fontTex);
            m_fontTex = nullptr;
        }
        if (m_sampler != nullptr)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
        if (m_geomLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_geomLayout);
            m_geomLayout = nullptr;
        }
        if (m_screenLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_screenLayout);
            m_screenLayout = nullptr;
        }
        if (m_screenBgLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_screenBgLayout);
            m_screenBgLayout = nullptr;
        }
    }
}
