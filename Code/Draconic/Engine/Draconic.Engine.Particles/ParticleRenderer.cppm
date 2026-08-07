// draconic.engine.particles:renderer - the dedicated billboard particle Renderer.
//
// Modeled on SpriteRenderer (SV_VertexID quad + hardware instancing + per-frame ring + blended
// forward pass), but with a richer per-instance record and VS: per-particle rotation and a
// velocity-stretched billboard mode. Each DrawItem is a ParticleBillboardRenderData BATCH (N
// instances), so a whole system draws as one instanced call - the particle count never reaches the
// draw-list sort. Registered with RenderSubsystem via the RegisterRenderer seam (rides Transparent).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.particles:renderer;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.shaders.system;
import draconic.render; // Renderer, RenderRecordContext, ResolvedDraw, DrawItem, DynamicUniformRing, categories
import draconic.particles; // ParticleBlendMode
import :renderdata;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;
namespace render = draconic::render;

export namespace draconic::particles
{
    class ParticleRenderer final : public render::Renderer
    {
    public:
        ParticleRenderer(rhi::Device& device, shaders::ShaderSystem& shaders,
                         u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight),
              m_instanceRing(device, framesInFlight, sizeof(ParticleBillboardInstance),
                             rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
                             u8"particle.instances"),
              m_trailRing(device, framesInFlight, sizeof(TrailVertex),
                          rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst,
                          u8"particle.trailverts"),
              m_viewRing(device, framesInFlight, kViewSlotSize,
                         rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"particle.view")
        {
        }
        ~ParticleRenderer() override { Shutdown(); }
        ParticleRenderer(const ParticleRenderer&) = delete;
        ParticleRenderer& operator=(const ParticleRenderer&) = delete;

        foundation::Status Initialize()
        {

            rhi::BindGroupLayoutEntry viewEntry = rhi::BindGroupLayoutEntry::UniformBuffer(
                0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
            viewEntry.hasDynamicOffset = true;
            rhi::BindGroupLayoutDesc vld{};
            vld.entries = Span<const rhi::BindGroupLayoutEntry>{&viewEntry, 1};
            if (!m_device->CreateBindGroupLayout(vld, m_viewLayout).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }

            rhi::BindGroupLayoutEntry texEntries[] = {
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            };
            rhi::BindGroupLayoutDesc tld{};
            tld.entries = Span<const rhi::BindGroupLayoutEntry>{texEntries, 2};
            if (!m_device->CreateBindGroupLayout(tld, m_texLayout).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }

            // set 2: the opaque scene depth (sampled via Load, no sampler) for soft particles.
            rhi::BindGroupLayoutEntry depthEntry =
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            // WebGPU annotation (Vulkan/DX12 ignore): the DEPTH view read as data.
            depthEntry.textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
            rhi::BindGroupLayoutDesc dld{};
            dld.entries = Span<const rhi::BindGroupLayoutEntry>{&depthEntry, 1};
            if (!m_device->CreateBindGroupLayout(dld, m_depthLayout).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }

            rhi::BindGroupLayout* layouts[] = {m_viewLayout, m_texLayout, m_depthLayout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 3};
            if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }

            rhi::SamplerDesc sd{};
            sd.minFilter = rhi::FilterMode::Linear;
            sd.magFilter = rhi::FilterMode::Linear;
            sd.addressU = rhi::AddressMode::ClampToEdge;
            sd.addressV = rhi::AddressMode::ClampToEdge;
            sd.addressW = rhi::AddressMode::ClampToEdge;
            if (!m_device->CreateSampler(sd, m_sampler).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }

            const u16 indices[6] = {0, 1, 2, 3, 4, 5};
            rhi::BufferDesc ibd{};
            ibd.size = sizeof(indices);
            ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
            ibd.memory = rhi::MemoryLocation::CpuToGpu;
            ibd.label = u8"particle.indices";
            if (!m_device->CreateBuffer(ibd, m_indexBuffer).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }
            if (void* p = m_indexBuffer->Map())
            {
                MemCopy(p, indices, sizeof(indices));
                m_indexBuffer->Unmap();
            }

            // Default texture: a soft radial dot (white RGB, smooth alpha falloff to the edge) so
            // untextured particles read as glowing sparks rather than hard squares. Generated on the CPU
            // and uploaded once. A component can still supply its own atlas to override this.
            constexpr u32 kDot = 64;
            u8 dot[kDot * kDot * 4];
            for (u32 y = 0; y < kDot; ++y)
            {
                for (u32 x = 0; x < kDot; ++x)
                {
                    const f32 fx = (static_cast<f32>(x) + 0.5f) / kDot * 2.0f - 1.0f;
                    const f32 fy = (static_cast<f32>(y) + 0.5f) / kDot * 2.0f - 1.0f;
                    const f32 d = Sqrt(fx * fx + fy * fy); // 0 centre .. 1 edge
                    f32 a = 1.0f - d;
                    a = (a < 0.0f) ? 0.0f : a * a; // squared falloff (soft)
                    const usize i = (static_cast<usize>(y) * kDot + x) * 4;
                    dot[i + 0] = 255;
                    dot[i + 1] = 255;
                    dot[i + 2] = 255;
                    dot[i + 3] = static_cast<u8>(a * 255.0f);
                }
            }
            rhi::TextureDesc wtd{};
            wtd.format = rhi::TextureFormat::RGBA8Unorm;
            wtd.width = kDot;
            wtd.height = kDot;
            wtd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            wtd.label = u8"particle.softdot";
            if (!m_device->CreateTexture(wtd, m_whiteTex).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }
            rhi::TextureViewDesc wvd{};
            wvd.format = rhi::TextureFormat::RGBA8Unorm;
            wvd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(m_whiteTex, wvd, m_whiteView).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }
            if (rhi::Queue* q = m_device->GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* tb = nullptr;
                if (q->CreateTransferBatch(tb).IsOk() && tb != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = kDot * 4;
                    layout.rowsPerImage = kDot;
                    tb->WriteTexture(m_whiteTex, Span<const u8>{dot, sizeof(dot)}, layout,
                                     rhi::Extent3D{kDot, kDot, 1});
                    (void)tb->Submit();
                    q->DestroyTransferBatch(tb);
                }
            }

            // Trails: a 2-set layout (view + texture, no depth) + a big identity index buffer so
            // the ribbon triangle-list draws through DrawIndexed (ResolvedDraw is always indexed).
            rhi::BindGroupLayout* trailLayouts[] = {m_viewLayout, m_texLayout};
            rhi::PipelineLayoutDesc tpld{};
            tpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{trailLayouts, 2};
            if (!m_device->CreatePipelineLayout(tpld, m_trailPipelineLayout).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }
            rhi::BufferDesc tibd{};
            tibd.size = static_cast<u64>(kTrailMaxIndices) * sizeof(u32);
            tibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
            tibd.memory = rhi::MemoryLocation::CpuToGpu;
            tibd.label = u8"particle.trailindices";
            if (!m_device->CreateBuffer(tibd, m_trailIndexBuffer).IsOk())
            {
                return foundation::Status{foundation::ErrorCode::Unknown};
            }
            if (void* p = m_trailIndexBuffer->Map())
            {
                u32* ix = static_cast<u32*>(p);
                for (u32 k = 0; k < kTrailMaxIndices; ++k)
                {
                    ix[k] = k;
                }
                m_trailIndexBuffer->Unmap();
            }
            return foundation::Status{};
        }

        [[nodiscard]] Span<const render::RenderCategory> SupportedCategories() const override
        {
            static const render::RenderCategory cats[] = {render::RenderCategories::Transparent};
            return Span<const render::RenderCategory>{cats, 1};
        }

        void PrepareFrame(u32 maxDraws, u32 frameIndex) override
        {
            // maxDraws is the frame's draw-ITEM count, but a particle batch is one item holding many
            // INSTANCES - and EVERY billboard system allocates from this one ring within a frame (the
            // cursor only resets at EndFrame). So size by the SUM of instances across all systems in a
            // frame (m_maxInstancesSeen, accumulated in Resolve, finalized in FinishFrame), not the largest
            // single system - else later systems in the sorted draw list fail to allocate and vanish.
            // Rounded up in 8k chunks; ramps one frame during growth, then holds (ring can't grow mid-frame).
            const u32 chunk = 8192u;
            const u32 want = Max(maxDraws, ((m_maxInstancesSeen + chunk - 1u) / chunk) * chunk);
            m_instanceRing.Reserve(want == 0 ? 1u : want);
            const u32 twant = ((m_maxTrailVertsSeen + chunk - 1u) / chunk) * chunk;
            m_trailRing.Reserve(twant == 0 ? 1u : twant);
            m_viewRing.Reserve(kMaxViews);
            m_instanceRing.BeginFrame(frameIndex);
            m_trailRing.BeginFrame(frameIndex);
            m_viewRing.BeginFrame(frameIndex);
            m_frameInstances = 0; // sum of instances allocated this frame (all systems/views)
            m_frameTrailVerts = 0;

            // Retire scene-depth bind groups created framesInFlight+ frames ago (no longer in flight).
            ++m_frameCounter;
            usize keep = 0;
            for (usize i = 0; i < m_depthPending.Size(); ++i)
            {
                if (m_frameCounter >= m_depthPending[i].frame + m_framesInFlight)
                {
                    m_device->DestroyBindGroup(m_depthPending[i].bg);
                }
                else
                {
                    m_depthPending[keep++] = m_depthPending[i];
                }
            }
            m_depthPending.Resize(keep);
        }

        void Resolve(const render::RenderRecordContext& ctx, Span<const render::DrawItem> items,
                     Array<render::ResolvedDraw>& out) override
        {
            if (items.IsEmpty())
            {
                return;
            }
            m_depthFormat = ctx.depthFormat;
            rhi::BindGroup* viewBg = EnsureViewBindGroup();
            if (viewBg == nullptr)
            {
                return;
            }

            const render::DynamicUniformRing::Range vr = m_viewRing.Allocate();
            if (!vr.ok)
            {
                return;
            }
            // DepthParams = the projection coeffs that reconstruct view-space depth from a sampled NDC
            // depth (Proj[2][2], Proj[3][2], Proj[2][3]) + the soft-particle fade distance.
            const Float4x4 proj =
                (ctx.view != nullptr) ? ctx.view->Camera().projection : Float4x4::Identity();
            struct ViewUBO
            {
                Float4x4 viewProj;
                Float4x4 view;
                Float4 depthParams;
            } ubo{ctx.viewProj, ctx.viewMatrix,
                  Float4{proj(2, 2), proj(3, 2), proj(2, 3),
                         0.0f}}; // .w unused (soft distance is per-instance)
            MemCopy(vr.ptr, &ubo, sizeof(ubo));

            // Scene-depth bind group (set 2) for soft particles. The transparent pass hands us the opaque
            // depth (already DepthStencilRead). Fresh per Resolve, retired after framesInFlight frames.
            rhi::BindGroup* depthBg = AcquireDepthBindGroup(
                ctx.sceneDepthView != nullptr ? ctx.sceneDepthView : m_whiteView);
            if (depthBg == nullptr)
            {
                return;
            }

            // Walk the items; dispatch each by kind (both ride the particle rendererId). Trails draw one at
            // a time; consecutive billboards sharing (texture, blend) fuse into one instanced draw.
            usize i = 0;
            while (i < items.Size())
            {
                const auto* base = static_cast<const ParticleRenderDataBase*>(items[i].data);
                if (base->particleKind == 1) // trail ribbon
                {
                    EmitTrailDraw(ctx, static_cast<const ParticleTrailRenderData*>(base), viewBg,
                                  vr, out);
                    ++i;
                    continue;
                }
                const auto* head = static_cast<const ParticleBillboardRenderData*>(base);
                usize j = i + 1;
                u32 total = head->count;
                while (j < items.Size())
                {
                    const auto* nb = static_cast<const ParticleRenderDataBase*>(items[j].data);
                    if (nb->particleKind != 0)
                    {
                        break;
                    }
                    const auto* nd = static_cast<const ParticleBillboardRenderData*>(nb);
                    if (nd->texture != head->texture || nd->blend != head->blend)
                    {
                        break;
                    }
                    total += nd->count;
                    ++j;
                }
                if (total > 0)
                {
                    m_frameInstances +=
                        total; // sum across all systems -> sizes the ring next frame
                    const render::DynamicUniformRing::Range ir =
                        m_instanceRing.AllocateRange(total);
                    if (ir.ok)
                    {
                        auto* dst = static_cast<ParticleBillboardInstance*>(ir.ptr);
                        u32 off = 0;
                        for (usize k = i; k < j; ++k)
                        {
                            const auto* b =
                                static_cast<const ParticleBillboardRenderData*>(items[k].data);
                            if (b->count > 0 && b->instances != nullptr)
                            {
                                MemCopy(dst + off, b->instances,
                                        static_cast<usize>(b->count) *
                                            sizeof(ParticleBillboardInstance));
                                off += b->count;
                            }
                        }
                        rhi::RenderPipeline* pso = EnsurePipeline(ctx.colorFormat, head->blend);
                        rhi::BindGroup* texBg = EnsureTextureBindGroup(
                            head->texture != nullptr ? head->texture : m_whiteView);
                        if (pso != nullptr && texBg != nullptr && off > 0)
                        {
                            render::ResolvedDraw d{};
                            d.pso = pso;
                            d.viewSet = viewBg;
                            d.viewDynamic = true;
                            d.viewOffset = vr.byteOffset;
                            d.drawSet = texBg;
                            d.materialSet = depthBg; // set 2: scene depth (soft particles)
                            d.vertexBuffer0 = m_instanceRing.Buffer();
                            d.vertexOffset0 = ir.byteOffset;
                            d.indexBuffer = m_indexBuffer;
                            d.indexFormat = rhi::IndexFormat::UInt16;
                            d.indexCount = 6;
                            d.instanceCount = off;
                            out.PushBack(d);
                        }
                    }
                }
                i = j;
            }
        }

        void FinishFrame() override
        {
            m_maxInstancesSeen =
                Max(m_maxInstancesSeen, m_frameInstances); // sized to the frame SUM
            m_maxTrailVertsSeen = Max(m_maxTrailVertsSeen, m_frameTrailVerts);
            m_instanceRing.EndFrame();
            m_trailRing.EndFrame();
            m_viewRing.EndFrame();
        }

        /// Wire the render subsystem's frames-in-flight retire queue: the rings then
        /// RETIRE their old buffer on grow instead of a mid-frame WaitIdle (which on web
        /// pumps the event loop, expires the canvas texture, and drops the frame's
        /// submit - the exact waiter the tripwire stack named in PrepareFrame).
        void SetRetireQueue(render::GpuRetireQueue* retire) noexcept
        {
            m_instanceRing.SetRetireQueue(retire);
            m_trailRing.SetRetireQueue(retire);
            m_viewRing.SetRetireQueue(retire);
        }

    private:
        static constexpr u32 kMaxViews = 8;
        static constexpr u64 kViewSlotSize =
            256; // 2x mat4 + a float4, padded to the dynamic-uniform alignment
        static constexpr u32 kTrailMaxIndices =
            262144u; // identity index buffer cap (largest single trail batch)

        rhi::BindGroup* EnsureViewBindGroup()
        {
            const u32 gen = m_viewRing.Generation();
            if (m_viewBg != nullptr && m_viewBgGen == gen)
            {
                return m_viewBg;
            }
            if (m_viewBg != nullptr)
            {
                m_device->DestroyBindGroup(m_viewBg);
                m_viewBg = nullptr;
            }
            if (m_viewRing.Buffer() == nullptr)
            {
                return nullptr;
            }
            rhi::BindGroupEntry e =
                rhi::BindGroupEntry::BufferEntry(m_viewRing.Buffer(), 0, kViewSlotSize);
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_viewLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
            if (!m_device->CreateBindGroup(bgd, m_viewBg).IsOk())
            {
                m_viewBg = nullptr;
                return nullptr;
            }
            m_viewBgGen = gen;
            return m_viewBg;
        }

        // A fresh scene-depth bind group over `depth` (set 2). Tracked for deferred destruction so an
        // in-flight frame never references a freed set. Cheap (one bind group per Resolve).
        rhi::BindGroup* AcquireDepthBindGroup(rhi::TextureView* depth)
        {
            if (depth == nullptr)
            {
                return nullptr;
            }
            rhi::BindGroupEntry e = rhi::BindGroupEntry::TextureEntry(depth);
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_depthLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{&e, 1};
            rhi::BindGroup* bg = nullptr;
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                return nullptr;
            }
            m_depthPending.PushBack(PendingBg{bg, m_frameCounter});
            return bg;
        }

        rhi::BindGroup* EnsureTextureBindGroup(rhi::TextureView* tex)
        {
            if (tex == nullptr)
            {
                return nullptr;
            }
            if (TexBindGroup* found = m_texBindGroups.Find(tex))
            {
                if (found->viewId == tex->uniqueId)
                {
                    return found->bindGroup;
                }
                // Address reuse: the cached group references a DESTROYED view - rebuild.
                if (found->bindGroup != nullptr)
                {
                    m_device->DestroyBindGroup(found->bindGroup);
                }
                m_texBindGroups.Remove(tex);
            }
            rhi::BindGroupEntry ent[] = {rhi::BindGroupEntry::TextureEntry(tex),
                                         rhi::BindGroupEntry::SamplerEntry(m_sampler)};
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_texLayout;
            bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
            rhi::BindGroup* bg = nullptr;
            if (!m_device->CreateBindGroup(bgd, bg).IsOk())
            {
                return nullptr;
            }
            m_texBindGroups.InsertOrAssign(tex, TexBindGroup{bg, tex->uniqueId});
            return bg;
        }

        // The blend state for each particle mode. Additive keeps the particle-appropriate {SrcAlpha, One}
        // (scales the source by its alpha before accumulating) rather than the RHI's plain {One, One}, so
        // an un-premultiplied soft dot still reads as a glow instead of blowing out.
        static rhi::BlendState BlendFor(ParticleBlendMode mode)
        {
            switch (mode)
            {
            case ParticleBlendMode::Additive:
                return {
                    {rhi::BlendFactor::SrcAlpha, rhi::BlendFactor::One, rhi::BlendOperation::Add},
                    {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOperation::Add}};
            case ParticleBlendMode::Premultiplied:
                return rhi::BlendState::PremultipliedAlpha();
            case ParticleBlendMode::Multiply:
                return rhi::BlendState::Multiply();
            case ParticleBlendMode::Alpha:
            default:
                return rhi::BlendState::AlphaBlend();
            }
        }

        static const char8_t* BlendLabel(ParticleBlendMode mode)
        {
            switch (mode)
            {
            case ParticleBlendMode::Additive:
                return u8"particle.additive";
            case ParticleBlendMode::Premultiplied:
                return u8"particle.premultiplied";
            case ParticleBlendMode::Multiply:
                return u8"particle.multiply";
            case ParticleBlendMode::Alpha:
            default:
                return u8"particle.alpha";
            }
        }

        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat colorFormat, ParticleBlendMode mode)
        {
            Pipelines& p = m_billboard[static_cast<u32>(mode)];
            const u64 shaderVersion = m_shaders->Version(u8"particle"); // hot reload rebuilds
            if (p.pso != nullptr && p.format == colorFormat && p.shaderVersion == shaderVersion)
            {
                return p.pso;
            }
            if (p.pso != nullptr)
            {
                m_device->DestroyRenderPipeline(p.pso);
                p.pso = nullptr;
            }

            rhi::ShaderModule* vs = m_shaders->GetVariant(
                u8"particle", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            rhi::ShaderModule* ps = m_shaders->GetVariant(
                u8"particle", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            if (vs == nullptr || ps == nullptr)
            {
                return nullptr;
            }

            const rhi::VertexAttribute attrs[] = {
                {rhi::VertexFormat::Float32x4, 0, 0},  {rhi::VertexFormat::Float32x4, 16, 1},
                {rhi::VertexFormat::Float32x4, 32, 2}, {rhi::VertexFormat::Float32x4, 48, 3},
                {rhi::VertexFormat::Float32x4, 64, 4},
            };
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(ParticleBillboardInstance);
            vbl.stepMode = rhi::VertexStepMode::Instance;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 5};

            rhi::ColorTargetState target{};
            target.format = colorFormat;
            target.blend = BlendFor(mode);

            rhi::FragmentState frag{};
            frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
            frag.targets = Span<const rhi::ColorTargetState>{&target, 1};

            rhi::DepthStencilState ds{};
            ds.format = m_depthFormat;
            ds.depthTestEnabled = true;
            ds.depthWriteEnabled = false;
            ds.depthCompare = rhi::CompareFunction::LessEqual;

            rhi::RenderPipelineDesc pd{};
            pd.layout = m_pipelineLayout;
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.depthStencil = ds;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = BlendLabel(mode);
            rhi::RenderPipeline* pso = nullptr;
            if (!m_device->CreateRenderPipeline(pd, pso).IsOk())
            {
                return nullptr;
            }
            p.pso = pso;
            p.format = colorFormat;
            return pso;
        }

        // Draw one system's trail ribbon: upload its (already camera-facing) vertices to the trail ring
        // and emit a triangle-list draw via the identity index buffer. Reuses the view UBO + a texture.
        void EmitTrailDraw(const render::RenderRecordContext& ctx, const ParticleTrailRenderData* b,
                           rhi::BindGroup* viewBg, const render::DynamicUniformRing::Range& vr,
                           Array<render::ResolvedDraw>& out)
        {
            if (b->vertexCount == 0 || b->vertices == nullptr)
            {
                return;
            }
            const u32 count = Min(b->vertexCount, kTrailMaxIndices);
            m_frameTrailVerts +=
                count; // sum across all trail systems -> sizes the trail ring next frame
            const render::DynamicUniformRing::Range tr = m_trailRing.AllocateRange(count);
            if (!tr.ok)
            {
                return;
            }
            MemCopy(tr.ptr, b->vertices, static_cast<usize>(count) * sizeof(TrailVertex));
            rhi::RenderPipeline* pso = EnsureTrailPipeline(ctx.colorFormat, b->blend);
            rhi::BindGroup* texBg =
                EnsureTextureBindGroup(b->texture != nullptr ? b->texture : m_whiteView);
            if (pso == nullptr || texBg == nullptr)
            {
                return;
            }
            render::ResolvedDraw d{};
            d.pso = pso;
            d.viewSet = viewBg;
            d.viewDynamic = true;
            d.viewOffset = vr.byteOffset;
            d.drawSet = texBg;
            d.vertexBuffer0 = m_trailRing.Buffer();
            d.vertexOffset0 = tr.byteOffset;
            d.indexBuffer = m_trailIndexBuffer;
            d.indexFormat = rhi::IndexFormat::UInt32;
            d.indexCount = count;
            d.instanceCount = 1;
            out.PushBack(d);
        }

        rhi::RenderPipeline* EnsureTrailPipeline(rhi::TextureFormat colorFormat,
                                                 ParticleBlendMode mode)
        {
            Pipelines& p = m_trail[static_cast<u32>(mode)];
            const u64 shaderVersion = m_shaders->Version(u8"particletrail"); // hot reload rebuilds
            if (p.pso != nullptr && p.format == colorFormat && p.shaderVersion == shaderVersion)
            {
                return p.pso;
            }
            if (p.pso != nullptr)
            {
                m_device->DestroyRenderPipeline(p.pso);
                p.pso = nullptr;
            }

            rhi::ShaderModule* vs = m_shaders->GetVariant(
                u8"particletrail", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            rhi::ShaderModule* ps = m_shaders->GetVariant(
                u8"particletrail", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            if (vs == nullptr || ps == nullptr)
            {
                return nullptr;
            }

            const rhi::VertexAttribute attrs[] = {
                {rhi::VertexFormat::Float32x3, 0, 0},  // position
                {rhi::VertexFormat::Float32x2, 12, 1}, // texcoord
                {rhi::VertexFormat::Float32x4, 20, 2}, // color
            };
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(TrailVertex);
            vbl.stepMode = rhi::VertexStepMode::Vertex;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 3};

            rhi::ColorTargetState target{};
            target.format = colorFormat;
            target.blend = BlendFor(mode);

            rhi::FragmentState frag{};
            frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
            frag.targets = Span<const rhi::ColorTargetState>{&target, 1};
            rhi::DepthStencilState ds{};
            ds.format = m_depthFormat;
            ds.depthTestEnabled = true;
            ds.depthWriteEnabled = false;
            ds.depthCompare = rhi::CompareFunction::LessEqual;

            rhi::RenderPipelineDesc pd{};
            pd.layout = m_trailPipelineLayout;
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.depthStencil = ds;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = BlendLabel(mode); // shared labels (particle.*) - fine for debug naming
            rhi::RenderPipeline* pso = nullptr;
            if (!m_device->CreateRenderPipeline(pd, pso).IsOk())
            {
                return nullptr;
            }
            p.pso = pso;
            p.format = colorFormat;
            return pso;
        }

        void Shutdown()
        {
            for (auto& kv : m_texBindGroups)
            {
                if (kv.value.bindGroup != nullptr)
                {
                    m_device->DestroyBindGroup(kv.value.bindGroup);
                }
            }
            m_texBindGroups.Clear();
            if (m_viewBg != nullptr)
            {
                m_device->DestroyBindGroup(m_viewBg);
                m_viewBg = nullptr;
            }
            for (Pipelines& p : m_billboard)
            {
                if (p.pso != nullptr)
                {
                    m_device->DestroyRenderPipeline(p.pso);
                    p.pso = nullptr;
                }
            }
            for (Pipelines& p : m_trail)
            {
                if (p.pso != nullptr)
                {
                    m_device->DestroyRenderPipeline(p.pso);
                    p.pso = nullptr;
                }
            }
            if (m_trailIndexBuffer != nullptr)
            {
                m_device->DestroyBuffer(m_trailIndexBuffer);
                m_trailIndexBuffer = nullptr;
            }
            if (m_trailPipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_trailPipelineLayout);
                m_trailPipelineLayout = nullptr;
            }
            if (m_indexBuffer != nullptr)
            {
                m_device->DestroyBuffer(m_indexBuffer);
                m_indexBuffer = nullptr;
            }
            if (m_whiteView != nullptr)
            {
                m_device->DestroyTextureView(m_whiteView);
                m_whiteView = nullptr;
            }
            if (m_whiteTex != nullptr)
            {
                m_device->DestroyTexture(m_whiteTex);
                m_whiteTex = nullptr;
            }
            if (m_sampler != nullptr)
            {
                m_device->DestroySampler(m_sampler);
                m_sampler = nullptr;
            }
            for (PendingBg& p : m_depthPending)
            {
                if (p.bg != nullptr)
                {
                    m_device->DestroyBindGroup(p.bg);
                }
            }
            m_depthPending.Clear();
            if (m_pipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
                m_pipelineLayout = nullptr;
            }
            if (m_depthLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_depthLayout);
                m_depthLayout = nullptr;
            }
            if (m_texLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_texLayout);
                m_texLayout = nullptr;
            }
            if (m_viewLayout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_viewLayout);
                m_viewLayout = nullptr;
            }
        }

        struct Pipelines
        {
            rhi::RenderPipeline* pso = nullptr;
            rhi::TextureFormat format = rhi::TextureFormat::Undefined;
            u64 shaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        };

        struct PendingBg
        {
            rhi::BindGroup* bg;
            u32 frame;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;
        u32 m_frameCounter = 0;
        render::DynamicUniformRing m_instanceRing;
        render::DynamicUniformRing m_trailRing; // trail ribbon vertices (per-frame)
        render::DynamicUniformRing m_viewRing;
        rhi::BindGroupLayout* m_viewLayout = nullptr;
        rhi::BindGroupLayout* m_texLayout = nullptr;
        rhi::BindGroupLayout* m_depthLayout = nullptr; // set 2: scene depth (soft particles)
        Array<PendingBg> m_depthPending; // depth bind groups awaiting deferred destroy
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::Sampler* m_sampler = nullptr;
        rhi::Buffer* m_indexBuffer = nullptr;
        rhi::Texture* m_whiteTex = nullptr; // 1x1 white default (untextured particles)
        rhi::TextureView* m_whiteView = nullptr;
        rhi::BindGroup* m_viewBg = nullptr;
        u32 m_viewBgGen = 0;
        // Keyed by view pointer, validated by TextureView::uniqueId on every hit (address
        // reuse of destroyed dynamic textures - see SpriteRenderer::TexBindGroup).
        struct TexBindGroup
        {
            rhi::BindGroup* bindGroup = nullptr;
            u64 viewId = 0;
        };
        HashMap<rhi::TextureView*, TexBindGroup> m_texBindGroups;
        Pipelines
            m_billboard[4]; // one per ParticleBlendMode (Alpha/Additive/Premultiplied/Multiply)
        rhi::Buffer* m_trailIndexBuffer = nullptr; // identity indices [0,1,2,...] for trail draws
        rhi::PipelineLayout* m_trailPipelineLayout = nullptr; // view + texture (no depth set)
        Pipelines m_trail[4]; // trail ribbon PSOs, one per blend mode
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Undefined;
        u32 m_maxInstancesSeen = 0;  // sizes the instance ring (see PrepareFrame)
        u32 m_frameInstances = 0;    // running sum of instances this frame
        u32 m_frameTrailVerts = 0;   // running sum of trail verts this frame
        u32 m_maxTrailVertsSeen = 0; // sizes the trail vertex ring
    };
}
