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

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data;        // SkySnapshot / SkyMode / ExtractedScene (context identity)

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    Status IBLSystem::Initialize()
    {
        // Cube/LUT fragment shaders share the fullscreen VS; the cube ones prepend IblCommon().

        if (!CreateSharedResources())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreatePipelines())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void IBLSystem::BeginFrame(rendergraph::RenderGraph& graph)
    {
        // Hot reload: any IBL shader change rebuilds the pipelines and re-runs the whole
        // precompute (contexts + BRDF) - the cached results are stale. Polled BEFORE the
        // ready gate: a failed rebuild (broken shader) clears m_ready; a later successful
        // one restores it. GPU idled by the subsystem on reload.
        const u64 shaderVersion =
            m_shaders->Version(u8"ibl_fs") + m_shaders->Version(u8"ibl_procenv") +
            m_shaders->Version(u8"ibl_analytic") + m_shaders->Version(u8"ibl_equirect") +
            m_shaders->Version(u8"ibl_cubemap") + m_shaders->Version(u8"ibl_downsample") +
            m_shaders->Version(u8"ibl_prefilter") + m_shaders->Version(u8"ibl_brdf") +
            m_shaders->Version(u8"ibl_sh");
        if (m_pipelineShaderVersion != shaderVersion)
        {
            m_pipelineShaderVersion = shaderVersion;
            if (m_envOnlyLayout != nullptr) // Initialize ran; layouts are live
            {
                m_ready = RebuildPipelinesForReload();
                if (m_ready)
                {
                    for (usize i = 0; i < m_contexts.Size(); ++i)
                    {
                        DestroyContext(*m_contexts[i]);
                    }
                    m_contexts.Clear();
                    m_brdfDone = false;
                }
            }
        }
        if (!m_ready)
        {
            return;
        }
        ++m_frame;
        m_brdfH = graph.ImportTarget(u8"ibl.brdf", m_brdfLut, m_brdfView,
                                     rhi::ResourceState::ShaderRead, m_brdfState);
        m_brdfState = rhi::ResourceState::ShaderRead;
        if (!m_brdfDone)
        {
            DeclareBrdf(graph, m_brdfH);
            m_brdfDone = true;
        }

        for (usize i = 0; i < m_contexts.Size(); /**/)
        {
            if (m_frame - m_contexts[i]->m_lastUsedFrame > kEvictAfterFrames)
            {
                DestroyContext(*m_contexts[i]);
                m_contexts.RemoveAtSwap(i);
            }
            else
            {
                ++i;
            }
        }
    }

    IBLSystem::IBLSystem::Context* IBLSystem::Prepare(const void* scene, const SkySnapshot& sky,
                                                      const Float3& sunDir,
                                                      rendergraph::RenderGraph& graph)
    {
        if (!m_ready)
        {
            return nullptr;
        }
        Context* ctx = nullptr;
        for (const UniquePtr<Context>& c : m_contexts)
        {
            if (c->m_scene == scene)
            {
                ctx = c.Get();
                break;
            }
        }
        if (ctx == nullptr)
        {
            UniquePtr<Context> fresh = MakeUnique<Context>(DefaultAllocator());
            fresh->m_scene = scene;
            fresh->m_uid = ++m_nextContextUid;
            if (!CreateContextResources(*fresh))
            {
                DestroyContext(*fresh);
                return nullptr;
            }
            m_contexts.PushBack(Move(fresh));
            ctx = m_contexts[m_contexts.Size() - 1].Get();
        }
        ctx->m_lastUsedFrame = m_frame;

        // Sky authoring: re-dirty only for fields baked into the env cube (sunAngularSize is
        // analytic-only - the sky pass draws the disc live). Sun direction feeds the procedural env.
        if (!PrecomputeEqual(sky, ctx->m_sky))
        {
            ctx->m_dirty = true;
        }
        if (sunDir.x != ctx->m_sunDir.x || sunDir.y != ctx->m_sunDir.y ||
            sunDir.z != ctx->m_sunDir.z)
        {
            ctx->m_sunDir = sunDir;
            ctx->m_dirty = true;
        }
        // A programmatic pixel source changed (SetEquirect/SetCubemap): textured modes re-bake.
        if (ctx->m_sourceStamp != m_sourceStamp &&
            (sky.mode == SkyMode::HDREquirect || sky.mode == SkyMode::Cubemap))
        {
            ctx->m_dirty = true;
        }
        ctx->m_sourceStamp = m_sourceStamp;
        // Asset-driven sky texture: (re)build the context's external bind group when the PRODUCT
        // changes - detected by uid, never the pointer (reloads reuse freed addresses; deferred
        // product destruction keeps the old view alive for in-flight frames).
        if (sky.textureUid != ctx->m_externalUid)
        {
            DestroyExternalBindGroups(*ctx);
            ctx->m_externalUid = sky.textureUid;
            if (sky.texture != nullptr)
            {
                if (sky.textureIsCube)
                {
                    if (EnsureCubemapPipeline())
                    {
                        rhi::BindGroupEntry be[] = {rhi::BindGroupEntry::TextureEntry(sky.texture),
                                                    rhi::BindGroupEntry::SamplerEntry(m_sampler)};
                        rhi::BindGroupDesc bgd{};
                        bgd.layout = m_envLayout;
                        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
                        if (!m_device->CreateBindGroup(bgd, ctx->m_externalCubeBG).IsOk())
                        {
                            ctx->m_externalCubeBG = nullptr;
                        }
                    }
                }
                else
                {
                    if (EnsureEquirectPipeline())
                    {
                        rhi::BindGroupEntry be[] = {
                            rhi::BindGroupEntry::TextureEntry(sky.texture),
                            rhi::BindGroupEntry::SamplerEntry(m_equirectSampler)};
                        rhi::BindGroupDesc bgd{};
                        bgd.layout = m_equirectLayout;
                        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
                        if (!m_device->CreateBindGroup(bgd, ctx->m_externalEquirectBG).IsOk())
                        {
                            ctx->m_externalEquirectBG = nullptr;
                        }
                    }
                }
            }
            ctx->m_dirty = true;
        }
        ctx->m_sky = sky; // always store the latest (the sky pass reads sun size/intensity live)

        ProcessContext(*ctx, graph);
        return ctx;
    }

    void IBLSystem::SetEquirect(u32 w, u32 h, Span<const f32> rgba)
    {
        if (!m_ready || w == 0 || h == 0 || rgba.Size() < static_cast<usize>(w) * h * 4u)
        {
            return;
        }
        DestroyEquirect();
        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA32Float;
        td.width = w;
        td.height = h;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"ibl.equirect";
        if (!m_device->CreateTexture(td, m_equirectTex).IsOk())
        {
            m_equirectTex = nullptr;
            return;
        }
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA32Float;
        vd.dimension = rhi::TextureViewDimension::Texture2D;
        if (!m_device->CreateTextureView(m_equirectTex, vd, m_equirectView).IsOk())
        {
            DestroyEquirect();
            return;
        }
        const u64 bytes = static_cast<u64>(w) * h * 4u * sizeof(f32);
        rhi::BufferDesc sd{};
        sd.size = bytes;
        sd.usage = rhi::BufferUsage::CopySrc;
        sd.memory = rhi::MemoryLocation::CpuToGpu;
        sd.label = u8"ibl.equirectStaging";
        if (!m_device->CreateBuffer(sd, m_equirectStaging).IsOk())
        {
            DestroyEquirect();
            return;
        }
        if (void* p = m_equirectStaging->Map())
        {
            MemCopy(p, rgba.Data(), bytes);
            m_equirectStaging->Unmap();
        }
        if (!EnsureEquirectPipeline())
        {
            DestroyEquirect();
            return;
        }
        rhi::BindGroupEntry be[] = {rhi::BindGroupEntry::TextureEntry(m_equirectView),
                                    rhi::BindGroupEntry::SamplerEntry(m_equirectSampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_equirectLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
        if (!m_device->CreateBindGroup(bgd, m_equirectBindGroup).IsOk())
        {
            m_equirectBindGroup = nullptr;
            DestroyEquirect();
            return;
        }
        m_equirectW = w;
        m_equirectH = h;
        m_equirectPending = true;
        ++m_sourceStamp;
    }

    void IBLSystem::SetCubemap(u32 faceSize, Span<const u8> sixFaces)
    {
        const u64 faceBytes = static_cast<u64>(faceSize) * faceSize * 4u;
        if (!m_ready || faceSize == 0 || sixFaces.Size() < faceBytes * 6u)
        {
            return;
        }
        DestroyCubemap();
        // sRGB format so the hardware decodes the (sRGB-encoded LDR) faces to linear on sample - the
        // env cube is a linear working-space texture. Without this the sky reads washed out.
        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8UnormSrgb;
        td.width = faceSize;
        td.height = faceSize;
        td.arrayLayerCount = 6;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"ibl.srcCube";
        if (!m_device->CreateTexture(td, m_srcCube).IsOk())
        {
            m_srcCube = nullptr;
            return;
        }
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA8UnormSrgb;
        vd.dimension = rhi::TextureViewDimension::TextureCube;
        vd.arrayLayerCount = 6;
        if (!m_device->CreateTextureView(m_srcCube, vd, m_srcCubeView).IsOk())
        {
            DestroyCubemap();
            return;
        }
        rhi::BufferDesc sd{};
        sd.size = faceBytes * 6u;
        sd.usage = rhi::BufferUsage::CopySrc;
        sd.memory = rhi::MemoryLocation::CpuToGpu;
        sd.label = u8"ibl.srcCubeStaging";
        if (!m_device->CreateBuffer(sd, m_cubemapStaging).IsOk())
        {
            DestroyCubemap();
            return;
        }
        if (void* p = m_cubemapStaging->Map())
        {
            MemCopy(p, sixFaces.Data(), faceBytes * 6u);
            m_cubemapStaging->Unmap();
        }
        if (!EnsureCubemapPipeline())
        {
            DestroyCubemap();
            return;
        }
        rhi::BindGroupEntry be[] = {rhi::BindGroupEntry::TextureEntry(m_srcCubeView),
                                    rhi::BindGroupEntry::SamplerEntry(m_sampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_envLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
        if (!m_device->CreateBindGroup(bgd, m_cubemapBindGroup).IsOk())
        {
            m_cubemapBindGroup = nullptr;
            DestroyCubemap();
            return;
        }
        m_cubemapFaceSize = faceSize;
        m_cubemapPending = true;
        ++m_sourceStamp;
    }

    void IBLSystem::Upload(rhi::CommandEncoder& enc)
    {
        if (m_equirectPending && m_equirectTex != nullptr && m_equirectStaging != nullptr)
        {
            enc.TransitionTexture(m_equirectTex, rhi::ResourceState::Undefined,
                                  rhi::ResourceState::CopyDst);
            rhi::BufferTextureCopyRegion r{};
            r.bytesPerRow = m_equirectW * 4u * static_cast<u32>(sizeof(f32));
            r.rowsPerImage = m_equirectH;
            r.textureExtent = rhi::Extent3D{m_equirectW, m_equirectH, 1};
            enc.CopyBufferToTexture(m_equirectStaging, m_equirectTex, r);
            enc.TransitionTexture(m_equirectTex, rhi::ResourceState::CopyDst,
                                  rhi::ResourceState::ShaderRead);
            m_equirectPending = false;
        }
        if (m_cubemapPending && m_srcCube != nullptr && m_cubemapStaging != nullptr)
        {
            enc.TransitionTexture(m_srcCube, rhi::ResourceState::Undefined,
                                  rhi::ResourceState::CopyDst);
            const u64 faceBytes = static_cast<u64>(m_cubemapFaceSize) * m_cubemapFaceSize * 4u;
            for (u32 f = 0; f < 6; ++f)
            {
                rhi::BufferTextureCopyRegion r{};
                r.bufferOffset = faceBytes * f;
                r.bytesPerRow = m_cubemapFaceSize * 4u;
                r.rowsPerImage = m_cubemapFaceSize;
                r.textureArrayLayer = f;
                r.textureExtent = rhi::Extent3D{m_cubemapFaceSize, m_cubemapFaceSize, 1};
                enc.CopyBufferToTexture(m_cubemapStaging, m_srcCube, r);
            }
            enc.TransitionTexture(m_srcCube, rhi::ResourceState::CopyDst,
                                  rhi::ResourceState::ShaderRead);
            m_cubemapPending = false;
        }
    }

    void IBLSystem::ProcessContext(Context& ctx, rendergraph::RenderGraph& graph)
    {
        ctx.m_prefilterH =
            graph.ImportTarget(u8"ibl.prefilter", ctx.m_prefilterCube, ctx.m_prefilterView,
                               rhi::ResourceState::ShaderRead, ctx.m_prefilterState);
        ctx.m_prefilterState = rhi::ResourceState::ShaderRead;
        ctx.m_shH = graph.ImportBuffer(u8"ibl.sh", ctx.m_shBuffer);
        // The env cube is imported every frame too (the sky pass reads it for the visible background).
        ctx.m_envH = graph.ImportTarget(u8"ibl.env", ctx.m_envCube, ctx.m_envSampleView,
                                        rhi::ResourceState::ShaderRead, ctx.m_envState);
        ctx.m_envState = rhi::ResourceState::ShaderRead;

        if (!ctx.m_dirty && ctx.m_bakeWarmup == 0)
        {
            return;
        }
        if (ctx.m_bakeWarmup > 0)
        {
            --ctx.m_bakeWarmup; // startup re-bake window (survives dropped-submit startup frames)
        }
        // Generation tracks PRODUCT IDENTITY (consumers key bind-group caches on it), so it moves
        // only on a real content change - not on a warmup re-bake, which re-renders the same
        // content into the same textures. Bumping it there would churn every downstream cache for
        // the whole warmup window and break the steady-state contract.
        if (ctx.m_dirty)
        {
            ctx.m_generation = ++m_nextGeneration; // system-wide: never collides across contexts
        }
        ctx.m_dirty = false;

        // (1) Source -> env cube: 6 faces. The scene-authored ASSET texture wins over the
        // programmatic pixel path (SetEquirect/SetCubemap) when both are present.
        const rendergraph::RGHandle envH = ctx.m_envH;
        rhi::BindGroup* equirectBG =
            (ctx.m_externalEquirectBG != nullptr) ? ctx.m_externalEquirectBG : m_equirectBindGroup;
        rhi::BindGroup* cubemapBG =
            (ctx.m_externalCubeBG != nullptr) ? ctx.m_externalCubeBG : m_cubemapBindGroup;
        const bool useEquirect = (ctx.m_sky.mode == SkyMode::HDREquirect) && equirectBG != nullptr;
        const bool useCubemap = (ctx.m_sky.mode == SkyMode::Cubemap) && cubemapBG != nullptr;
        const bool useAnalytic = (ctx.m_sky.mode == SkyMode::Analytic);
        rhi::RenderPipeline* envPipe = useEquirect   ? m_equirectPipeline
                                       : useCubemap  ? m_cubemapPipeline
                                       : useAnalytic ? m_analyticPipeline
                                                     : m_envPipeline;
        // Procedural/analytic have no env source; bind the dummy so the declared
        // group is always satisfied (WebGPU requirement, harmless elsewhere).
        rhi::BindGroup* envBG =
            useEquirect ? equirectBG : useCubemap ? cubemapBG : m_dummyEnvBindGroup;
        for (u32 face = 0; face < 6; ++face)
        {
            IblPush push = MakeSkyPush(ctx, static_cast<i32>(face));
            graph.AddRenderPass(u8"ibl.env.face",
                                [envH, face, push, envPipe, envBG](rendergraph::PassBuilder& b)
                                {
                                    b.SetColorTarget(
                                        0, envH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                        rhi::ClearColor::Black(),
                                        rendergraph::RGSubresourceRange{0, 1, face, 1});
                                    b.SetViewport(0, 0, kEnvResolution, kEnvResolution);
                                    b.NeverCull();
                                    b.SetExecute(
                                        [push, envPipe, envBG](rhi::RenderPassEncoder& rp)
                                        {
                                            rp.SetPipeline(envPipe);
                                            if (envBG != nullptr)
                                            {
                                                rp.SetBindGroup(0, envBG, Span<const u32>{});
                                            }
                                            rp.SetPushConstants(rhi::ShaderStage::Fragment, 0,
                                                                sizeof(IblPush), &push);
                                            rp.Draw(3, 1, 0, 0);
                                        });
                                });
        }

        // (2) Build the env mip pyramid: box-downsample each mip from the previous. Reads mip m-1 (a
        // single-mip view) and writes mip m - non-overlapping subresources, so the graph orders + barriers
        // it correctly. SH/prefilter (whole-resource reads) then run after the whole chain is written.
        DeclareEnvMips(ctx, graph, envH);

        // (3) env -> SH9 diffuse (compute), (4) env -> prefilter mips (PDF-samples the pyramid).
        DeclareShProjection(ctx, graph, envH, ctx.m_shH);
        DeclarePrefilter(ctx, graph, envH, ctx.m_prefilterH);
    }

    IBLSystem::IBLSystem::IblPush IBLSystem::MakeSkyPush(const Context& ctx, i32 face)
    {
        IblPush p{};
        p.faceIndex = face;
        p.mode = static_cast<i32>(ctx.m_sky.mode);
        p.skyIntensity = ctx.m_sky.intensity;
        p.sun = Float4{ctx.m_sunDir.x, ctx.m_sunDir.y, ctx.m_sunDir.z, ctx.m_sky.sunAngularSize};
        p.horizon = Float4{ctx.m_sky.horizon.x, ctx.m_sky.horizon.y, ctx.m_sky.horizon.z,
                           ctx.m_sky.sunIntensity};
        p.zenith =
            Float4{ctx.m_sky.zenith.x, ctx.m_sky.zenith.y, ctx.m_sky.zenith.z, ctx.m_sky.rotation};
        p.ground =
            Float4{ctx.m_sky.ground.x, ctx.m_sky.ground.y, ctx.m_sky.ground.z, ctx.m_sky.turbidity};
        return p;
    }

    void IBLSystem::DestroyExternalBindGroups(Context& ctx)
    {
        if (ctx.m_externalEquirectBG)
        {
            m_device->DestroyBindGroup(ctx.m_externalEquirectBG);
            ctx.m_externalEquirectBG = nullptr;
        }
        if (ctx.m_externalCubeBG)
        {
            m_device->DestroyBindGroup(ctx.m_externalCubeBG);
            ctx.m_externalCubeBG = nullptr;
        }
    }

    bool IBLSystem::PrecomputeEqual(const SkySnapshot& a, const SkySnapshot& b)
    {
        return a.mode == b.mode && a.intensity == b.intensity && a.rotation == b.rotation &&
               a.textureUid == b.textureUid && a.horizon.x == b.horizon.x &&
               a.horizon.y == b.horizon.y && a.horizon.z == b.horizon.z &&
               a.zenith.x == b.zenith.x && a.zenith.y == b.zenith.y && a.zenith.z == b.zenith.z &&
               a.ground.x == b.ground.x && a.ground.y == b.ground.y && a.ground.z == b.ground.z &&
               a.sunIntensity == b.sunIntensity && a.turbidity == b.turbidity;
    }

    void IBLSystem::DeclareShProjection(Context& ctx, rendergraph::RenderGraph& graph,
                                        rendergraph::RGHandle envH, rendergraph::RGHandle shH)
    {
        rhi::BindGroup* shBG = ctx.m_shBindGroup;
        graph.AddComputePass(u8"ibl.sh",
                             [this, envH, shH, shBG](rendergraph::PassBuilder& b)
                             {
                                 b.ReadTexture(envH);
                                 b.WriteStorage(shH);
                                 b.SetComputeExecute(
                                     [this, shBG](rhi::ComputePassEncoder& cp)
                                     {
                                         if (shBG == nullptr)
                                         {
                                             return;
                                         }
                                         cp.SetPipeline(m_shPipeline);
                                         cp.SetBindGroup(0, shBG, Span<const u32>{});
                                         cp.Dispatch(1, 1, 1);
                                     });
                             });
    }

    void IBLSystem::DeclareEnvMips(Context& ctx, rendergraph::RenderGraph& graph,
                                   rendergraph::RGHandle envH)
    {
        for (u32 mip = 1; mip < kEnvMips; ++mip)
        {
            const u32 res = kEnvResolution >> mip;
            rhi::BindGroup* srcBG = ctx.m_envMipBG[mip - 1];
            for (u32 face = 0; face < 6; ++face)
            {
                IblPush push{};
                push.faceIndex = static_cast<i32>(face);
                graph.AddRenderPass(
                    u8"ibl.env.mip",
                    [this, envH, mip, face, res, push, srcBG](rendergraph::PassBuilder& b)
                    {
                        b.ReadTexture(envH, rendergraph::RGSubresourceRange{mip - 1, 1, 0, 6});
                        b.SetColorTarget(0, envH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                         rhi::ClearColor::Black(),
                                         rendergraph::RGSubresourceRange{mip, 1, face, 1});
                        b.SetViewport(0, 0, res, res);
                        b.NeverCull();
                        b.SetExecute(
                            [this, push, srcBG](rhi::RenderPassEncoder& rp)
                            {
                                rp.SetPipeline(m_downsamplePipeline);
                                rp.SetBindGroup(0, srcBG, Span<const u32>{});
                                rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(IblPush),
                                                    &push);
                                rp.Draw(3, 1, 0, 0);
                            });
                    });
            }
        }
    }

    void IBLSystem::DeclarePrefilter(Context& ctx, rendergraph::RenderGraph& graph,
                                     rendergraph::RGHandle envH, rendergraph::RGHandle preH)
    {
        rhi::BindGroup* envBG = ctx.m_envBindGroup;
        for (u32 mip = 0; mip < kPrefilterMips; ++mip)
        {
            const u32 res = kPrefilterRes >> mip;
            const f32 roughness = (kPrefilterMips > 1)
                                      ? static_cast<f32>(mip) / static_cast<f32>(kPrefilterMips - 1)
                                      : 0.0f;
            for (u32 face = 0; face < 6; ++face)
            {
                IblPush push{};
                push.faceIndex = static_cast<i32>(face);
                push.roughness = roughness;
                graph.AddRenderPass(
                    u8"ibl.prefilter",
                    [this, envH, preH, mip, face, res, push, envBG](rendergraph::PassBuilder& b)
                    {
                        b.ReadTexture(envH);
                        b.SetColorTarget(0, preH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                         rhi::ClearColor::Black(),
                                         rendergraph::RGSubresourceRange{mip, 1, face, 1});
                        b.SetViewport(0, 0, res, res);
                        b.NeverCull();
                        b.SetExecute(
                            [this, push, envBG](rhi::RenderPassEncoder& rp)
                            {
                                rp.SetPipeline(m_prefilterPipeline);
                                rp.SetBindGroup(0, envBG, Span<const u32>{});
                                rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(IblPush),
                                                    &push);
                                rp.Draw(3, 1, 0, 0);
                            });
                    });
            }
        }
    }

    void IBLSystem::DeclareBrdf(rendergraph::RenderGraph& graph, rendergraph::RGHandle brdfH)
    {
        graph.AddRenderPass(u8"ibl.brdf",
                            [this, brdfH](rendergraph::PassBuilder& b)
                            {
                                b.SetColorTarget(0, brdfH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                                 rhi::ClearColor::Black());
                                b.SetViewport(0, 0, kBrdfResolution, kBrdfResolution);
                                b.NeverCull();
                                b.SetExecute(
                                    [this](rhi::RenderPassEncoder& rp)
                                    {
                                        rp.SetPipeline(m_brdfPipeline);
                                        rp.Draw(3, 1, 0, 0);
                                    });
                            });
    }

    bool IBLSystem::CreateSharedResources()
    {
        rhi::TextureDesc bd{};
        bd.format = kBrdfFormat;
        bd.width = kBrdfResolution;
        bd.height = kBrdfResolution;
        bd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
        bd.label = u8"ibl.brdf";
        if (!m_device->CreateTexture(bd, m_brdfLut).IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc bv{};
        bv.format = kBrdfFormat;
        bv.dimension = rhi::TextureViewDimension::Texture2D;
        if (!m_device->CreateTextureView(m_brdfLut, bv, m_brdfView).IsOk())
        {
            return false;
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Linear;
        ss.magFilter = rhi::FilterMode::Linear;
        ss.mipmapFilter = rhi::MipmapFilterMode::Linear;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"ibl.sampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return false;
        }
        return true;
    }

    bool IBLSystem::CreateContextResources(Context& ctx)
    {
        // Env cube (mip pyramid): mip 0 holds the full-res source radiance; mips 1..N are box-downsampled
        // so the prefilter can PDF-sample a pre-averaged mip per GGX sample (firefly suppression).
        rhi::TextureDesc ed{};
        ed.format = kCubeFormat;
        ed.width = kEnvResolution;
        ed.height = kEnvResolution;
        ed.arrayLayerCount = 6;
        ed.mipLevelCount = kEnvMips;
        ed.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
        ed.label = u8"ibl.env";
        if (!m_device->CreateTexture(ed, ctx.m_envCube).IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc ev{};
        ev.format = kCubeFormat;
        ev.dimension = rhi::TextureViewDimension::TextureCube;
        ev.arrayLayerCount = 6;
        ev.mipLevelCount = kEnvMips;
        if (!m_device->CreateTextureView(ctx.m_envCube, ev, ctx.m_envSampleView).IsOk())
        {
            return false;
        }
        // Single-mip cube views of each env mip - bound as the source when downsampling the NEXT mip, so
        // the read descriptor covers only mip m (never the mip m+1 being rendered -> no read/write hazard).
        for (u32 m = 0; m < kEnvMips; ++m)
        {
            rhi::TextureViewDesc mv{};
            mv.format = kCubeFormat;
            mv.dimension = rhi::TextureViewDimension::TextureCube;
            mv.baseMipLevel = m;
            mv.mipLevelCount = 1;
            mv.arrayLayerCount = 6;
            if (!m_device->CreateTextureView(ctx.m_envCube, mv, ctx.m_envMipView[m]).IsOk())
            {
                return false;
            }
        }

        // Prefilter cube (mip chain).
        rhi::TextureDesc pd{};
        pd.format = kCubeFormat;
        pd.width = kPrefilterRes;
        pd.height = kPrefilterRes;
        pd.arrayLayerCount = 6;
        pd.mipLevelCount = kPrefilterMips;
        pd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
        pd.label = u8"ibl.prefilter";
        if (!m_device->CreateTexture(pd, ctx.m_prefilterCube).IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc pv{};
        pv.format = kCubeFormat;
        pv.dimension = rhi::TextureViewDimension::TextureCube;
        pv.arrayLayerCount = 6;
        pv.mipLevelCount = kPrefilterMips;
        if (!m_device->CreateTextureView(ctx.m_prefilterCube, pv, ctx.m_prefilterView).IsOk())
        {
            return false;
        }

        // SH9 coefficient buffer (RW for the compute write, read-only in forward).
        rhi::BufferDesc sd{};
        sd.size = ShBytes();
        sd.usage = rhi::BufferUsage::Storage;
        sd.memory = rhi::MemoryLocation::GpuOnly;
        sd.label = u8"ibl.sh";
        if (!m_device->CreateBuffer(sd, ctx.m_shBuffer).IsOk())
        {
            return false;
        }

        // env sample bind group (for prefilter: full mip chain) + per-mip downsample sources.
        rhi::BindGroupEntry be[] = {rhi::BindGroupEntry::TextureEntry(ctx.m_envSampleView),
                                    rhi::BindGroupEntry::SamplerEntry(m_sampler)};
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_envLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
        if (!m_device->CreateBindGroup(bgd, ctx.m_envBindGroup).IsOk())
        {
            return false;
        }
        for (u32 m = 0; m < kEnvMips; ++m)
        {
            rhi::BindGroupEntry me[] = {rhi::BindGroupEntry::TextureEntry(ctx.m_envMipView[m]),
                                        rhi::BindGroupEntry::SamplerEntry(m_sampler)};
            rhi::BindGroupDesc md{};
            md.layout = m_envLayout;
            md.entries = Span<const rhi::BindGroupEntry>{me, 2};
            if (!m_device->CreateBindGroup(md, ctx.m_envMipBG[m]).IsOk())
            {
                return false;
            }
        }

        // SH compute bind group (t0 env cube + s0 sampler + u0 SH buffer).
        rhi::BindGroupEntry she[] = {
            rhi::BindGroupEntry::TextureEntry(ctx.m_envSampleView),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
            rhi::BindGroupEntry::BufferEntry(ctx.m_shBuffer, 0, ShBytes()),
        };
        rhi::BindGroupDesc shBgd{};
        shBgd.layout = m_shLayout;
        shBgd.entries = Span<const rhi::BindGroupEntry>{she, 3};
        if (!m_device->CreateBindGroup(shBgd, ctx.m_shBindGroup).IsOk())
        {
            return false;
        }

        ctx.m_dirty = true; // build the products on the first Prepare
        return true;
    }

    void IBLSystem::DestroyContext(Context& ctx)
    {
        DestroyExternalBindGroups(ctx);
        if (ctx.m_shBindGroup)
        {
            m_device->DestroyBindGroup(ctx.m_shBindGroup);
            ctx.m_shBindGroup = nullptr;
        }
        if (ctx.m_envBindGroup)
        {
            m_device->DestroyBindGroup(ctx.m_envBindGroup);
            ctx.m_envBindGroup = nullptr;
        }
        for (u32 m = 0; m < kEnvMips; ++m)
        {
            if (ctx.m_envMipBG[m])
            {
                m_device->DestroyBindGroup(ctx.m_envMipBG[m]);
                ctx.m_envMipBG[m] = nullptr;
            }
        }
        if (ctx.m_shBuffer)
        {
            m_device->DestroyBuffer(ctx.m_shBuffer);
            ctx.m_shBuffer = nullptr;
        }
        if (ctx.m_prefilterView)
        {
            m_device->DestroyTextureView(ctx.m_prefilterView);
            ctx.m_prefilterView = nullptr;
        }
        if (ctx.m_prefilterCube)
        {
            m_device->DestroyTexture(ctx.m_prefilterCube);
            ctx.m_prefilterCube = nullptr;
        }
        for (u32 m = 0; m < kEnvMips; ++m)
        {
            if (ctx.m_envMipView[m])
            {
                m_device->DestroyTextureView(ctx.m_envMipView[m]);
                ctx.m_envMipView[m] = nullptr;
            }
        }
        if (ctx.m_envSampleView)
        {
            m_device->DestroyTextureView(ctx.m_envSampleView);
            ctx.m_envSampleView = nullptr;
        }
        if (ctx.m_envCube)
        {
            m_device->DestroyTexture(ctx.m_envCube);
            ctx.m_envCube = nullptr;
        }
    }

    bool IBLSystem::CreatePipelines()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ibl_fs", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr)
        {
            return false;
        }

        // --- dummy env source (see IBLSystem.cppm member comment) ---
        {
            rhi::TextureDesc dummyDesc;
            dummyDesc.format = rhi::TextureFormat::RGBA8Unorm;
            dummyDesc.width = 1;
            dummyDesc.height = 1;
            dummyDesc.arrayLayerCount = 6;
            dummyDesc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            dummyDesc.label = u8"ibl.dummy_env";
            if (!m_device->CreateTexture(dummyDesc, m_dummyEnvCube).IsOk())
            {
                return false;
            }
            rhi::TextureViewDesc dummyView;
            dummyView.format = rhi::TextureFormat::RGBA8Unorm;
            dummyView.dimension = rhi::TextureViewDimension::TextureCube;
            dummyView.arrayLayerCount = 6;
            if (!m_device->CreateTextureView(m_dummyEnvCube, dummyView, m_dummyEnvView).IsOk())
            {
                return false;
            }
        }

        // --- env sample bind group layout (t0 cube + s0 sampler), shared by prefilter ---
        rhi::BindGroupLayoutEntry envTex = rhi::BindGroupLayoutEntry::SampledTexture(
            0, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::TextureCube);
        rhi::BindGroupLayoutEntry envSamp =
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry envEntries[] = {envTex, envSamp};
        rhi::BindGroupLayoutDesc envLd{};
        envLd.entries = Span<const rhi::BindGroupLayoutEntry>{envEntries, 2};
        if (!m_device->CreateBindGroupLayout(envLd, m_envLayout).IsOk())
        {
            return false;
        }
        {
            rhi::BindGroupEntry dummyEntries[] = {
                rhi::BindGroupEntry::TextureEntry(m_dummyEnvView),
                rhi::BindGroupEntry::SamplerEntry(m_sampler)};
            rhi::BindGroupDesc dummyDesc;
            dummyDesc.layout = m_envLayout;
            dummyDesc.entries = Span<const rhi::BindGroupEntry>{dummyEntries, 2};
            if (!m_device->CreateBindGroup(dummyDesc, m_dummyEnvBindGroup).IsOk())
            {
                return false;
            }
        }

        // --- pipeline layouts ---
        rhi::PushConstantRange pcRange{};
        pcRange.stages = rhi::ShaderStage::Fragment;
        pcRange.offset = 0;
        pcRange.size = sizeof(IblPush);
        // procedural env: push constants + the env bind group (unused but included so
        // push constants land at space1, matching the prefilter/downsample/equirect layouts).
        rhi::BindGroupLayout* envLayouts[] = {m_envLayout};
        rhi::PipelineLayoutDesc envPld{};
        envPld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{envLayouts, 1};
        envPld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pcRange, 1};
        if (!m_device->CreatePipelineLayout(envPld, m_envOnlyLayout).IsOk())
        {
            return false;
        }
        // prefilter: env bind group + push constants.
        rhi::BindGroupLayout* preLayouts[] = {m_envLayout};
        rhi::PipelineLayoutDesc prePld{};
        prePld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{preLayouts, 1};
        prePld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pcRange, 1};
        if (!m_device->CreatePipelineLayout(prePld, m_prefilterLayout).IsOk())
        {
            return false;
        }
        // brdf: no inputs.
        rhi::PipelineLayoutDesc brdfPld{};
        if (!m_device->CreatePipelineLayout(brdfPld, m_brdfPipelineLayout).IsOk())
        {
            return false;
        }

        m_envPipeline = MakeFullscreenPipeline(vs, u8"ibl_procenv", m_envOnlyLayout, kCubeFormat);
        m_analyticPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_analytic", m_envOnlyLayout, kCubeFormat);
        m_downsamplePipeline =
            MakeFullscreenPipeline(vs, u8"ibl_downsample", m_prefilterLayout, kCubeFormat);
        m_prefilterPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_prefilter", m_prefilterLayout, kCubeFormat);
        m_brdfPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_brdf", m_brdfPipelineLayout, kBrdfFormat);
        if (m_envPipeline == nullptr || m_analyticPipeline == nullptr ||
            m_downsamplePipeline == nullptr || m_prefilterPipeline == nullptr ||
            m_brdfPipeline == nullptr)
        {
            return false;
        }

        // --- SH compute pipeline (t0 cube + s0 sampler + u0 SH buffer; bind groups are per context) ---
        rhi::ShaderModule* cs = m_shaders->GetVariant(u8"ibl_sh", shaders::ShaderStage::Compute,
                                                      shaders::ShaderFlags::None);
        if (cs == nullptr)
        {
            return false;
        }
        rhi::BindGroupLayoutEntry shTex = rhi::BindGroupLayoutEntry::SampledTexture(
            0, rhi::ShaderStage::Compute, rhi::TextureViewDimension::TextureCube);
        rhi::BindGroupLayoutEntry shSamp =
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Compute);
        rhi::BindGroupLayoutEntry shOut = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Compute, /*readOnly*/ false,
            /*stride*/ 16); // RWStructuredBuffer<float4> ShOut
        rhi::BindGroupLayoutEntry shEntries[] = {shTex, shSamp, shOut};
        rhi::BindGroupLayoutDesc shLd{};
        shLd.entries = Span<const rhi::BindGroupLayoutEntry>{shEntries, 3};
        if (!m_device->CreateBindGroupLayout(shLd, m_shLayout).IsOk())
        {
            return false;
        }
        rhi::BindGroupLayout* shLayouts[] = {m_shLayout};
        rhi::PipelineLayoutDesc shPld{};
        shPld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{shLayouts, 1};
        if (!m_device->CreatePipelineLayout(shPld, m_shPipelineLayout).IsOk())
        {
            return false;
        }
        rhi::ComputePipelineDesc cpd{};
        cpd.layout = m_shPipelineLayout;
        cpd.compute = rhi::ProgrammableStage{cs, u8"main", rhi::ShaderStage::Compute};
        cpd.label = u8"ibl.sh";
        if (!m_device->CreateComputePipeline(cpd, m_shPipeline).IsOk())
        {
            return false;
        }

        m_ready = true;
        return true;
    }

    bool IBLSystem::RebuildPipelinesForReload()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ibl_fs", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* cs = m_shaders->GetVariant(u8"ibl_sh", shaders::ShaderStage::Compute,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || cs == nullptr)
        {
            return false;
        }
        rhi::RenderPipeline** stale[] = {&m_envPipeline,        &m_analyticPipeline,
                                         &m_downsamplePipeline, &m_prefilterPipeline,
                                         &m_brdfPipeline,       &m_equirectPipeline,
                                         &m_cubemapPipeline};
        for (rhi::RenderPipeline** p : stale)
        {
            if (*p != nullptr)
            {
                m_device->DestroyRenderPipeline(*p);
                *p = nullptr;
            }
        }
        if (m_shPipeline != nullptr)
        {
            m_device->DestroyComputePipeline(m_shPipeline);
            m_shPipeline = nullptr;
        }

        m_envPipeline = MakeFullscreenPipeline(vs, u8"ibl_procenv", m_envOnlyLayout, kCubeFormat);
        m_analyticPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_analytic", m_envOnlyLayout, kCubeFormat);
        m_downsamplePipeline =
            MakeFullscreenPipeline(vs, u8"ibl_downsample", m_prefilterLayout, kCubeFormat);
        m_prefilterPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_prefilter", m_prefilterLayout, kCubeFormat);
        m_brdfPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_brdf", m_brdfPipelineLayout, kBrdfFormat);

        rhi::ComputePipelineDesc cpd{};
        cpd.layout = m_shPipelineLayout;
        cpd.compute = rhi::ProgrammableStage{cs, u8"main", rhi::ShaderStage::Compute};
        cpd.label = u8"ibl.sh";
        if (!m_device->CreateComputePipeline(cpd, m_shPipeline).IsOk())
        {
            m_shPipeline = nullptr;
        }

        // equirect/cubemap are rebuilt lazily by their Ensure*Pipeline on next use.
        return m_envPipeline != nullptr && m_analyticPipeline != nullptr &&
               m_downsamplePipeline != nullptr && m_prefilterPipeline != nullptr &&
               m_brdfPipeline != nullptr && m_shPipeline != nullptr;
    }

    rhi::RenderPipeline* IBLSystem::MakeFullscreenPipeline(rhi::ShaderModule* vs, StringView psName,
                                                           rhi::PipelineLayout* layout,
                                                           rhi::TextureFormat fmt)
    {
        rhi::ShaderModule* ps = m_shaders->GetVariant(psName, shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (ps == nullptr)
        {
            return nullptr;
        }
        rhi::ColorTargetState color{};
        color.format = fmt;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = layout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = psName;
        rhi::RenderPipeline* p = nullptr;
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return nullptr;
        }
        return p;
    }

    bool IBLSystem::EnsureEquirectPipeline()
    {
        if (m_equirectPipeline != nullptr)
        {
            return true;
        }
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ibl_fs", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr)
        {
            return false;
        }
        if (m_equirectLayout == nullptr) // layouts survive a shader hot reload
        {
            rhi::BindGroupLayoutEntry tex = rhi::BindGroupLayoutEntry::SampledTexture(
                0, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2D);
            rhi::BindGroupLayoutEntry samp =
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry e[] = {tex, samp};
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{e, 2};
            if (!m_device->CreateBindGroupLayout(ld, m_equirectLayout).IsOk())
            {
                return false;
            }
            rhi::PushConstantRange pc{};
            pc.stages = rhi::ShaderStage::Fragment;
            pc.offset = 0;
            pc.size = sizeof(IblPush);
            rhi::BindGroupLayout* layouts[] = {m_equirectLayout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
            pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
            if (!m_device->CreatePipelineLayout(pld, m_equirectPipelineLayout).IsOk())
            {
                return false;
            }
        }
        m_equirectPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_equirect", m_equirectPipelineLayout, kCubeFormat);
        if (m_equirectPipeline == nullptr)
        {
            return false;
        }
        if (m_equirectSampler == nullptr)
        {
            rhi::SamplerDesc ss{};
            ss.minFilter = rhi::FilterMode::Linear;
            ss.magFilter = rhi::FilterMode::Linear;
            ss.mipmapFilter = rhi::MipmapFilterMode::Linear;
            ss.addressU = rhi::AddressMode::Repeat;
            ss.addressV = rhi::AddressMode::ClampToEdge;
            ss.addressW = rhi::AddressMode::ClampToEdge;
            ss.label = u8"ibl.equirectSampler";
            if (!m_device->CreateSampler(ss, m_equirectSampler).IsOk())
            {
                return false;
            }
        }
        return true;
    }

    bool IBLSystem::EnsureCubemapPipeline()
    {
        if (m_cubemapPipeline != nullptr)
        {
            return true;
        }
        if (m_prefilterLayout == nullptr || m_envLayout == nullptr)
        {
            return false;
        }
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ibl_fs", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr)
        {
            return false;
        }
        m_cubemapPipeline =
            MakeFullscreenPipeline(vs, u8"ibl_cubemap", m_prefilterLayout, kCubeFormat);
        return m_cubemapPipeline != nullptr;
    }

    void IBLSystem::DestroyCubemap()
    {
        if (m_cubemapBindGroup)
        {
            m_device->DestroyBindGroup(m_cubemapBindGroup);
            m_cubemapBindGroup = nullptr;
        }
        if (m_cubemapStaging)
        {
            m_device->DestroyBuffer(m_cubemapStaging);
            m_cubemapStaging = nullptr;
        }
        if (m_srcCubeView)
        {
            m_device->DestroyTextureView(m_srcCubeView);
            m_srcCubeView = nullptr;
        }
        if (m_srcCube)
        {
            m_device->DestroyTexture(m_srcCube);
            m_srcCube = nullptr;
        }
        m_cubemapPending = false;
    }

    void IBLSystem::DestroyEquirect()
    {
        if (m_equirectBindGroup)
        {
            m_device->DestroyBindGroup(m_equirectBindGroup);
            m_equirectBindGroup = nullptr;
        }
        if (m_equirectStaging)
        {
            m_device->DestroyBuffer(m_equirectStaging);
            m_equirectStaging = nullptr;
        }
        if (m_equirectView)
        {
            m_device->DestroyTextureView(m_equirectView);
            m_equirectView = nullptr;
        }
        if (m_equirectTex)
        {
            m_device->DestroyTexture(m_equirectTex);
            m_equirectTex = nullptr;
        }
        m_equirectPending = false;
    }

    void IBLSystem::Shutdown()
    {
        for (const UniquePtr<Context>& ctx : m_contexts)
        {
            DestroyContext(*ctx);
        }
        m_contexts.Clear();
        DestroyCubemap();
        if (m_cubemapPipeline)
        {
            m_device->DestroyRenderPipeline(m_cubemapPipeline);
            m_cubemapPipeline = nullptr;
        }
        DestroyEquirect();
        if (m_equirectPipeline)
        {
            m_device->DestroyRenderPipeline(m_equirectPipeline);
            m_equirectPipeline = nullptr;
        }
        if (m_equirectPipelineLayout)
        {
            m_device->DestroyPipelineLayout(m_equirectPipelineLayout);
            m_equirectPipelineLayout = nullptr;
        }
        if (m_equirectLayout)
        {
            m_device->DestroyBindGroupLayout(m_equirectLayout);
            m_equirectLayout = nullptr;
        }
        if (m_equirectSampler)
        {
            m_device->DestroySampler(m_equirectSampler);
            m_equirectSampler = nullptr;
        }
        if (m_shPipeline)
        {
            m_device->DestroyComputePipeline(m_shPipeline);
            m_shPipeline = nullptr;
        }
        if (m_dummyEnvBindGroup != nullptr)
        {
            m_device->DestroyBindGroup(m_dummyEnvBindGroup);
            m_dummyEnvBindGroup = nullptr;
        }
        if (m_dummyEnvView != nullptr)
        {
            m_device->DestroyTextureView(m_dummyEnvView);
            m_dummyEnvView = nullptr;
        }
        if (m_dummyEnvCube != nullptr)
        {
            m_device->DestroyTexture(m_dummyEnvCube);
            m_dummyEnvCube = nullptr;
        }
        if (m_envPipeline)
        {
            m_device->DestroyRenderPipeline(m_envPipeline);
            m_envPipeline = nullptr;
        }
        if (m_analyticPipeline)
        {
            m_device->DestroyRenderPipeline(m_analyticPipeline);
            m_analyticPipeline = nullptr;
        }
        if (m_downsamplePipeline)
        {
            m_device->DestroyRenderPipeline(m_downsamplePipeline);
            m_downsamplePipeline = nullptr;
        }
        if (m_prefilterPipeline)
        {
            m_device->DestroyRenderPipeline(m_prefilterPipeline);
            m_prefilterPipeline = nullptr;
        }
        if (m_brdfPipeline)
        {
            m_device->DestroyRenderPipeline(m_brdfPipeline);
            m_brdfPipeline = nullptr;
        }
        if (m_shPipelineLayout)
        {
            m_device->DestroyPipelineLayout(m_shPipelineLayout);
            m_shPipelineLayout = nullptr;
        }
        if (m_envOnlyLayout)
        {
            m_device->DestroyPipelineLayout(m_envOnlyLayout);
            m_envOnlyLayout = nullptr;
        }
        if (m_prefilterLayout)
        {
            m_device->DestroyPipelineLayout(m_prefilterLayout);
            m_prefilterLayout = nullptr;
        }
        if (m_brdfPipelineLayout)
        {
            m_device->DestroyPipelineLayout(m_brdfPipelineLayout);
            m_brdfPipelineLayout = nullptr;
        }
        if (m_shLayout)
        {
            m_device->DestroyBindGroupLayout(m_shLayout);
            m_shLayout = nullptr;
        }
        if (m_envLayout)
        {
            m_device->DestroyBindGroupLayout(m_envLayout);
            m_envLayout = nullptr;
        }
        if (m_sampler)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
        if (m_brdfView)
        {
            m_device->DestroyTextureView(m_brdfView);
            m_brdfView = nullptr;
        }
        if (m_brdfLut)
        {
            m_device->DestroyTexture(m_brdfLut);
            m_brdfLut = nullptr;
        }
    }
}
