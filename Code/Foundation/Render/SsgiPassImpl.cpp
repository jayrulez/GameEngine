// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:ssgi` partition implementation (GI tier 1).
///
/// Structure mirrors :ssr exactly (trace + temporal resolve, per-view ping-pong history,
/// generation-keyed bind-group caches with deferred frees). See SsgiPass.cppm for the design
/// notes and the composite (the bounce replaces the sky it hides, tinted by the albedo G-buffer).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module foundation.render;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

namespace foundation::render
{
    Status SsgiPass::Initialize()
    {
        // Trace bind-group layout: scene(t0) + depth(t1) + normal(t2) + point(s0) + linear(s1).
        rhi::BindGroupLayoutEntry ge[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(1, rhi::ShaderStage::Fragment),
        };
        // WebGPU annotations (Vulkan/DX12 ignore): t1 receives the DEPTH view, sampled only
        // through the point sampler s0 - UnfilterableFloat + non-filtering (:ssr rule).
        ge[1].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        ge[3].samplerNonFiltering = true;
        rhi::BindGroupLayoutDesc gld{};
        gld.entries = Span<const rhi::BindGroupLayoutEntry>{ge, 5};
        if (!m_device->CreateBindGroupLayout(gld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayout* gl[] = {m_layout};
        rhi::PushConstantRange pcr{};
        pcr.stages = rhi::ShaderStage::Fragment;
        pcr.offset = 0;
        pcr.size = sizeof(SsgiPushC);
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{gl, 1};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pcr, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Nearest;
        ss.magFilter = rhi::FilterMode::Nearest;
        ss.mipmapFilter = rhi::MipmapFilterMode::Nearest;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"ssgi.sampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ls{};
        ls.minFilter = rhi::FilterMode::Linear;
        ls.magFilter = rhi::FilterMode::Linear;
        ls.addressU = rhi::AddressMode::ClampToEdge;
        ls.addressV = rhi::AddressMode::ClampToEdge;
        ls.addressW = rhi::AddressMode::ClampToEdge;
        ls.label = u8"ssgi.linear";
        if (!m_device->CreateSampler(ls, m_linearSampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        if (!CreateTracePipeline())
        {
            return Status{ErrorCode::Unknown};
        }

        // Radiance prefilter: full-res hdr(t0) + linear sampler(s0) -> quarter-res box average.
        rhi::BindGroupLayoutEntry de[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        rhi::BindGroupLayoutDesc dld{};
        dld.entries = Span<const rhi::BindGroupLayoutEntry>{de, 2};
        if (!m_device->CreateBindGroupLayout(dld, m_downLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BindGroupLayout* dgl[] = {m_downLayout};
        rhi::PushConstantRange dpc{};
        dpc.stages = rhi::ShaderStage::Fragment;
        dpc.offset = 0;
        dpc.size = sizeof(SsgiDownPushC);
        rhi::PipelineLayoutDesc dpld{};
        dpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{dgl, 1};
        dpld.pushConstantRanges = Span<const rhi::PushConstantRange>{&dpc, 1};
        if (!m_device->CreatePipelineLayout(dpld, m_downPipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreateDownPipeline())
        {
            return Status{ErrorCode::Unknown};
        }

        // Spatial denoise: raw gi(t0) + depth(t1) + point sampler(s0).
        rhi::BindGroupLayoutEntry be[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
        };
        be[1].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        be[2].samplerNonFiltering = true;
        rhi::BindGroupLayoutDesc bld{};
        bld.entries = Span<const rhi::BindGroupLayoutEntry>{be, 3};
        if (!m_device->CreateBindGroupLayout(bld, m_blurLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BindGroupLayout* bgl[] = {m_blurLayout};
        rhi::PushConstantRange bpc{};
        bpc.stages = rhi::ShaderStage::Fragment;
        bpc.offset = 0;
        bpc.size = sizeof(SsgiBlurPushC);
        rhi::PipelineLayoutDesc bpld{};
        bpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{bgl, 1};
        bpld.pushConstantRanges = Span<const rhi::PushConstantRange>{&bpc, 1};
        if (!m_device->CreatePipelineLayout(bpld, m_blurPipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreateBlurPipeline())
        {
            return Status{ErrorCode::Unknown};
        }

        // Resolve: gi(t0) history(t1) velocity(t2) hdr(t3) albedo(t4) normal(t5) sky SH(t6) +
        // point(s0) linear(s1). MRT out = composited HDR + next-frame GI history.
        rhi::BindGroupLayoutEntry re[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(3, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(4, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(5, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::StorageBuffer(6, rhi::ShaderStage::Fragment, /*readOnly*/ true,
                                                     /*stride*/ 16), // StructuredBuffer<float4> IblSH
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::Sampler(1, rhi::ShaderStage::Fragment),
        };
        rhi::BindGroupLayoutDesc rld{};
        rld.entries = Span<const rhi::BindGroupLayoutEntry>{re, 9};
        if (!m_device->CreateBindGroupLayout(rld, m_resolveLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BindGroupLayout* rgl[] = {m_resolveLayout};
        rhi::PushConstantRange rpc{};
        rpc.stages = rhi::ShaderStage::Fragment;
        rpc.offset = 0;
        rpc.size = sizeof(SsgiResolvePushC);
        rhi::PipelineLayoutDesc rpld{};
        rpld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{rgl, 1};
        rpld.pushConstantRanges = Span<const rhi::PushConstantRange>{&rpc, 1};
        if (!m_device->CreatePipelineLayout(rpld, m_resolvePipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreateResolvePipeline())
        {
            return Status{ErrorCode::Unknown};
        }
        // Bound in the SH slot when a scene has no SH sky: the composite then never reads it (its
        // dimmer is 0), so its contents do not matter.
        rhi::BufferDesc shd{};
        shd.size = 9 * sizeof(Float4);
        shd.usage = rhi::BufferUsage::Storage;
        shd.memory = rhi::MemoryLocation::GpuOnly;
        shd.label = u8"ssgi.dummySH";
        if (!m_device->CreateBuffer(shd, m_dummyShBuffer).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    bool SsgiPass::CreateTracePipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ssgi", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"ssgi", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return false;
        }
        rhi::ColorTargetState color{};
        color.format = kHdrFormat;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"ssgi";
        if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
        {
            m_pipeline = nullptr;
            return false;
        }
        return true;
    }

    bool SsgiPass::CreateDownPipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ssgi_down", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"ssgi_down", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return false;
        }
        rhi::ColorTargetState color{};
        color.format = kHdrFormat;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_downPipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"ssgi_down";
        if (!m_device->CreateRenderPipeline(pd, m_downPipeline).IsOk())
        {
            m_downPipeline = nullptr;
            return false;
        }
        return true;
    }

    bool SsgiPass::CreateBlurPipeline()
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"ssgi_blur", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"ssgi_blur", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return false;
        }
        rhi::ColorTargetState color{};
        color.format = kHdrFormat;
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
        rhi::RenderPipelineDesc pd{};
        pd.layout = m_blurPipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"ssgi_blur";
        if (!m_device->CreateRenderPipeline(pd, m_blurPipeline).IsOk())
        {
            m_blurPipeline = nullptr;
            return false;
        }
        return true;
    }

    bool SsgiPass::CreateResolvePipeline()
    {
        rhi::ShaderModule* rvs = m_shaders->GetVariant(
            u8"ssgi_resolve", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
        rhi::ShaderModule* rps = m_shaders->GetVariant(
            u8"ssgi_resolve", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (rvs == nullptr || rps == nullptr)
        {
            return false;
        }
        rhi::ColorTargetState rtargets[2] = {};
        rtargets[0].format = kHdrFormat; // composited HDR
        rtargets[1].format = kHdrFormat; // GI history
        rhi::FragmentState rfrag{};
        rfrag.shader = rhi::ProgrammableStage{rps, u8"main", rhi::ShaderStage::Fragment};
        rfrag.targets = Span<const rhi::ColorTargetState>{rtargets, 2};
        rhi::RenderPipelineDesc rpd{};
        rpd.layout = m_resolvePipelineLayout;
        rpd.vertex.shader = rhi::ProgrammableStage{rvs, u8"main", rhi::ShaderStage::Vertex};
        rpd.fragment = rfrag;
        rpd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        rpd.primitive.cullMode = rhi::CullMode::None;
        rpd.label = u8"ssgi_resolve";
        if (!m_device->CreateRenderPipeline(rpd, m_resolvePipeline).IsOk())
        {
            m_resolvePipeline = nullptr;
            return false;
        }
        return true;
    }

    rendergraph::RGHandle
    SsgiPass::DeclareSsgi(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                          rendergraph::RGHandle depth, rendergraph::RGHandle normal,
                          rendergraph::RGHandle velocity, rendergraph::RGHandle albedo,
                          const Sky& sky, u32 w, u32 h, i32 vx, i32 vy, u32 vw, u32 vh,
                          const Float4x4& invProj, const Float4x4& proj, const Params& p,
                          u32 viewIndex, u32 frameIndex)
    {
        if (w == 0 || h == 0 || viewIndex >= kMaxViews)
        {
            return hdr;
        }
        // Hot reload: rebuild the pipelines when the shaders changed.
        const u64 shaderVersion = m_shaders->Version(u8"ssgi") +
                                  m_shaders->Version(u8"ssgi_down") +
                                  m_shaders->Version(u8"ssgi_blur") +
                                  m_shaders->Version(u8"ssgi_resolve");
        if (shaderVersion != m_pipelineShaderVersion)
        {
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }
            if (m_downPipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_downPipeline);
                m_downPipeline = nullptr;
            }
            if (m_blurPipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_blurPipeline);
                m_blurPipeline = nullptr;
            }
            if (m_resolvePipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_resolvePipeline);
                m_resolvePipeline = nullptr;
            }
            (void)CreateTracePipeline();
            (void)CreateDownPipeline();
            (void)CreateResolvePipeline();
            (void)CreateBlurPipeline();
            m_pipelineShaderVersion = shaderVersion;
            if (m_pipeline == nullptr || m_downPipeline == nullptr ||
                m_blurPipeline == nullptr || m_resolvePipeline == nullptr)
            {
                // A hot-reloaded shader that no longer matches this binary (e.g. its push
                // block grew) fails pipeline creation and the pass silently skips - say so
                // once per version instead of leaving only the validation spam.
                LOG_WARNING(u8"Render",
                            u8"SSGI pipelines failed to rebuild after shader reload - the "
                            u8"pass is OFF (shader/binary mismatch? restart with a rebuilt "
                            u8"editor)");
            }
        }
        if (m_pipeline == nullptr || m_downPipeline == nullptr || m_blurPipeline == nullptr ||
            m_resolvePipeline == nullptr)
        {
            return hdr;
        }
        Tick(frameIndex);
        const f32 fw = static_cast<f32>(w), fh = static_cast<f32>(h);
        const Float2 vpMin{static_cast<f32>(vx) / fw, static_cast<f32>(vy) / fh};
        const Float2 vpSize{static_cast<f32>(vw) / fw, static_cast<f32>(vh) / fh};
        const bool temporalOn = p.temporal;

        // --- Radiance prefilter: quarter-res box average of the lit HDR, the trace's gather
        //     source. THE structural firefly fix (Godot SSIL gathers from mip 5): every tap
        //     the trace takes is already a ~16-pixel mean, so variance dies at the source. ---
        const u32 qw = (w / 4u > 0u) ? w / 4u : 1u;
        const u32 qh = (h / 4u > 0u) ? h / 4u : 1u;
        const rendergraph::RGHandle sceneQuarter = graph.CreateTransient(
            u8"ssgi.scene.quarter", rendergraph::RGTextureDesc(kHdrFormat, qw, qh));
        SsgiDownPushC dpc2{};
        dpc2.srcTexelSize = Float2{1.0f / fw, 1.0f / fh};
        graph.AddRenderPass(
            u8"ssgi.down",
            [this, &graph, hdr, sceneQuarter, dpc2](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, sceneQuarter, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(hdr);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, hdr, dpc2](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureDownBindGroup(
                            graph.GetTextureView(hdr), graph.GetTextureGeneration(hdr));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_downPipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(SsgiDownPushC),
                                            &dpc2);
                        rp.Draw(3, 1, 0, 0);
                    });
            });

        // --- Trace: GI buffer (rgb one-bounce radiance, a = hit fraction) ---
        const rendergraph::RGHandle gi =
            graph.CreateTransient(u8"ssgi.raw", rendergraph::RGTextureDesc(kHdrFormat, w, h));
        SsgiPushC pc{};
        pc.invProj = invProj;
        pc.vpMin = vpMin;
        pc.vpSize = vpSize;
        pc.jitter = Float2{proj(2, 0), proj(2, 1)};
        pc.projXX = proj(0, 0);
        pc.projYY = proj(1, 1);
        pc.thickness = (p.thickness > 1e-3f) ? p.thickness : 1e-3f;
        pc.radius = (p.radius > 1e-2f) ? p.radius : 1e-2f;
        pc.maxSteps = (p.maxSteps > 1) ? p.maxSteps : 1;
        pc.rayCount = (p.rayCount > 1) ? ((p.rayCount < 4) ? p.rayCount : 4) : 1;
        pc.ySign = m_device->NeedsClipSpaceYFlip() ? 1.0f : -1.0f;
        pc.frameIndex = frameIndex;
        pc.maxRadiance = (p.maxRadiance > 0.1f) ? p.maxRadiance : 0.1f;
        graph.AddRenderPass(
            u8"ssgi.trace",
            [this, &graph, sceneQuarter, depth, normal, gi, pc](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, gi, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(sceneQuarter);
                b.ReadTexture(depth);
                b.ReadTexture(normal);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, sceneQuarter, depth, normal, pc](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBindGroup(
                            graph.GetTextureView(sceneQuarter), graph.GetTextureView(depth),
                            graph.GetTextureView(normal),
                            Combine(graph, sceneQuarter, depth, normal));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(SsgiPushC), &pc);
                        rp.Draw(3, 1, 0, 0);
                    });
            });

        // --- Spatial denoise: neighbors share their hits before the temporal accumulate
        //     (depth-aware; at 1-4 rays the raw estimate alone cannot converge) ---
        const rendergraph::RGHandle filtered =
            graph.CreateTransient(u8"ssgi.filtered", rendergraph::RGTextureDesc(kHdrFormat, w, h));
        SsgiBlurPushC bpc2{};
        bpc2.texelSize = Float2{1.0f / fw, 1.0f / fh};
        bpc2.depthSigma = (p.depthSigma > 1e-3f) ? p.depthSigma : 1e-3f;
        bpc2.invProj = invProj;
        bpc2.vpMin = vpMin;
        bpc2.vpSize = vpSize;
        bpc2.ySign = m_device->NeedsClipSpaceYFlip() ? 1.0f : -1.0f;
        graph.AddRenderPass(
            u8"ssgi.blur",
            [this, &graph, gi, depth, filtered, bpc2](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, filtered, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(gi);
                b.ReadTexture(depth);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, gi, depth, bpc2](rhi::RenderPassEncoder& rp)
                    {
                        rhi::BindGroup* bg = EnsureBlurBindGroup(
                            graph.GetTextureView(gi), graph.GetTextureView(depth),
                            graph.GetTextureGeneration(gi) ^
                                (graph.GetTextureGeneration(depth) * 1099511628211ull));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_blurPipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(SsgiBlurPushC),
                                            &bpc2);
                        rp.Draw(3, 1, 0, 0);
                    });
            });

        // --- Resolve: reproject + variance-clip + accumulate, then ADD into the HDR ---
        ViewHistory& hist = m_views[viewIndex];
        if (!EnsureHistory(hist, w, h))
        {
            return hdr;
        }
        const u32 cur = hist.cur, prev = cur ^ 1u;
        const rendergraph::RGHandle out =
            graph.CreateTransient(u8"ssgi.scene", rendergraph::RGTextureDesc(kHdrFormat, w, h));
        const rendergraph::RGHandle histPrev =
            graph.ImportTarget(u8"ssgi.histPrev", hist.tex[prev], hist.view[prev],
                               rhi::ResourceState::ShaderRead, hist.state[prev]);
        hist.state[prev] = rhi::ResourceState::ShaderRead;
        const rendergraph::RGHandle histCur =
            graph.ImportTarget(u8"ssgi.histCur", hist.tex[cur], hist.view[cur],
                               rhi::ResourceState::RenderTarget, hist.state[cur]);
        hist.state[cur] = rhi::ResourceState::RenderTarget;

        SsgiResolvePushC rpc2{};
        rpc2.vpMin = vpMin;
        rpc2.vpSize = vpSize;
        rpc2.texelSize = Float2{1.0f / fw, 1.0f / fh};
        rpc2.blendFactor = p.historyBlend;
        rpc2.historyValid = hist.valid ? 1.0f : 0.0f;
        rpc2.varianceGamma = p.varianceGamma;
        rpc2.motionScale = p.motionScale;
        rpc2.temporalOn = temporalOn ? 1 : 0;
        rpc2.debug = p.debug;
        rpc2.ghostReject = p.ghostReject;
        rpc2.intensity = (p.intensity >= 0.0f) ? p.intensity : 0.0f;
        // The ambient a hit replaces (see :ssgi): the camera's world rotation turns the G-buffer's
        // view-space normal to world, where the SH sky is; no SH sky = flat fill alone (as the forward).
        for (u32 r = 0; r < 3; ++r)
        {
            rpc2.viewToWorld[r] = Float4{sky.viewToWorld(r, 0), sky.viewToWorld(r, 1), sky.viewToWorld(r, 2), 0.0f};
        }
        const bool shSky = sky.shBuffer != nullptr;
        rpc2.skyAmbient = Float4{sky.ambient.x, sky.ambient.y, sky.ambient.z, shSky ? sky.iblDiffuse : 0.0f};
        rhi::Buffer* const shBuffer = shSky ? sky.shBuffer : m_dummyShBuffer;
        const u64 shGeneration = shSky ? sky.generation : 0;
        const rendergraph::RGHandle shHandle = shSky ? sky.shHandle : rendergraph::RGHandle{};

        rhi::TextureView* histPrevView = hist.view[prev];
        graph.AddRenderPass(
            u8"ssgi.resolve",
            [this, &graph, filtered, histPrev, velocity, hdr, albedo, normal, out, histCur,
             histPrevView, rpc2, shBuffer, shGeneration, shHandle](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, out, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.SetColorTarget(1, histCur, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                 rhi::ClearColor::Black());
                b.ReadTexture(filtered);
                b.ReadTexture(histPrev);
                b.ReadTexture(velocity);
                b.ReadTexture(hdr);
                b.ReadTexture(albedo);
                b.ReadTexture(normal);
                if (shHandle.IsValid())
                {
                    b.ReadBuffer(shHandle);
                }
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, filtered, velocity, hdr, albedo, normal, histPrevView, rpc2,
                     shBuffer, shGeneration](rhi::RenderPassEncoder& rp)
                    {
                        ResolveInputs inputs;
                        inputs.Set(0, graph.GetTextureView(filtered), graph.GetTextureGeneration(filtered));
                        inputs.Set(1, graph.GetTextureView(velocity), graph.GetTextureGeneration(velocity));
                        inputs.Set(2, graph.GetTextureView(hdr), graph.GetTextureGeneration(hdr));
                        inputs.Set(3, graph.GetTextureView(albedo), graph.GetTextureGeneration(albedo));
                        inputs.Set(4, graph.GetTextureView(normal), graph.GetTextureGeneration(normal));
                        inputs.SetBuffer(shBuffer, shGeneration);
                        rhi::BindGroup* bg = EnsureResolveBindGroup(histPrevView, inputs);
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_resolvePipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment,
                                            0, sizeof(SsgiResolvePushC), &rpc2);
                        rp.Draw(3, 1, 0, 0);
                    });
            });
        hist.cur = prev; // this frame's history output becomes next frame's read
        hist.valid = true;
        return out;
    }

    bool SsgiPass::EnsureHistory(ViewHistory& hist, u32 w, u32 h)
    {
        if (hist.tex[0] != nullptr && hist.w == w && hist.h == h)
        {
            return true;
        }
        DestroyHistory(hist);
        for (u32 i = 0; i < 2; ++i)
        {
            rhi::TextureDesc td{};
            td.format = kHdrFormat;
            td.width = w;
            td.height = h;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
            td.label = u8"ssgi.history";
            if (!m_device->CreateTexture(td, hist.tex[i]).IsOk())
            {
                hist.tex[i] = nullptr;
                DestroyHistory(hist);
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = kHdrFormat;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(hist.tex[i], vd, hist.view[i]).IsOk())
            {
                hist.view[i] = nullptr;
                DestroyHistory(hist);
                return false;
            }
            hist.state[i] = rhi::ResourceState::Undefined;
        }
        hist.w = w;
        hist.h = h;
        hist.cur = 0;
        hist.valid = false;
        return true;
    }

    void SsgiPass::DestroyHistory(ViewHistory& hist)
    {
        for (u32 i = 0; i < 2; ++i)
        {
            if (hist.view[i])
            {
                m_device->DestroyTextureView(hist.view[i]);
                hist.view[i] = nullptr;
            }
            if (hist.tex[i])
            {
                m_device->DestroyTexture(hist.tex[i]);
                hist.tex[i] = nullptr;
            }
        }
        hist.w = hist.h = 0;
        hist.valid = false;
    }

    u64 SsgiPass::Combine(rendergraph::RenderGraph& g, rendergraph::RGHandle a,
                          rendergraph::RGHandle b, rendergraph::RGHandle c)
    {
        u64 k = g.GetTextureGeneration(a);
        k = (k ^ g.GetTextureGeneration(b)) * 1099511628211ull;
        k = (k ^ g.GetTextureGeneration(c)) * 1099511628211ull;
        return k;
    }

    void SsgiPass::Tick(u32 frameIndex)
    {
        if (frameIndex == m_lastFrame)
        {
            return;
        }
        m_lastFrame = frameIndex;
        usize w = 0;
        for (usize i = 0; i < m_retired.Size(); ++i)
        {
            if (m_retired[i].left <= 1)
            {
                m_device->DestroyBindGroup(m_retired[i].bg);
            }
            else
            {
                m_retired[i].left -= 1;
                m_retired[w++] = m_retired[i];
            }
        }
        m_retired.Resize(w);
    }

    rhi::BindGroup* SsgiPass::EnsureBindGroup(rhi::TextureView* scene, rhi::TextureView* depth,
                                              rhi::TextureView* normal, u64 generation)
    {
        if (scene == nullptr || depth == nullptr || normal == nullptr)
        {
            return nullptr;
        }
        if (Entry* e = m_bindGroups.Find(scene))
        {
            if (e->gen == generation && e->depth == depth && e->normal == normal &&
                e->bg != nullptr)
            {
                return e->bg;
            }
            if (e->bg != nullptr)
            {
                m_retired.PushBack(Retired{e->bg, kRetireFrames});
                e->bg = nullptr;
            }
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(scene),
            rhi::BindGroupEntry::TextureEntry(depth),
            rhi::BindGroupEntry::TextureEntry(normal),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
            rhi::BindGroupEntry::SamplerEntry(m_linearSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 5};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_bindGroups.InsertOrAssign(scene, Entry{bg, depth, normal, generation});
        return bg;
    }

    rhi::BindGroup* SsgiPass::EnsureDownBindGroup(rhi::TextureView* hdr, u64 generation)
    {
        if (hdr == nullptr)
        {
            return nullptr;
        }
        if (DownEntry* e = m_downBindGroups.Find(hdr))
        {
            if (e->gen == generation && e->bg != nullptr)
            {
                return e->bg;
            }
            if (e->bg != nullptr)
            {
                m_retired.PushBack(Retired{e->bg, kRetireFrames});
                e->bg = nullptr;
            }
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(hdr),
            rhi::BindGroupEntry::SamplerEntry(m_linearSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_downLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 2};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_downBindGroups.InsertOrAssign(hdr, DownEntry{bg, generation});
        return bg;
    }

    rhi::BindGroup* SsgiPass::EnsureBlurBindGroup(rhi::TextureView* gi, rhi::TextureView* depth,
                                                  u64 generation)
    {
        if (gi == nullptr || depth == nullptr)
        {
            return nullptr;
        }
        if (BlurEntry* e = m_blurBindGroups.Find(gi))
        {
            if (e->gen == generation && e->depth == depth && e->bg != nullptr)
            {
                return e->bg;
            }
            if (e->bg != nullptr)
            {
                m_retired.PushBack(Retired{e->bg, kRetireFrames});
                e->bg = nullptr;
            }
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(gi),
            rhi::BindGroupEntry::TextureEntry(depth),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_blurLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 3};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_blurBindGroups.InsertOrAssign(gi, BlurEntry{bg, depth, generation});
        return bg;
    }

    rhi::BindGroup* SsgiPass::EnsureResolveBindGroup(rhi::TextureView* histPrev, const ResolveInputs& inputs)
    {
        if (histPrev == nullptr || !inputs.Complete())
        {
            return nullptr;
        }
        rhi::BindGroup* stale = nullptr;
        if (rhi::BindGroup* cached = m_resolveBindGroups.Find(histPrev, inputs, stale))
        {
            return cached;
        }
        if (stale != nullptr)
        {
            m_retired.PushBack(Retired{stale, kRetireFrames}); // a frame in flight may still use it
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(inputs.views[0]),
            rhi::BindGroupEntry::TextureEntry(histPrev),
            rhi::BindGroupEntry::TextureEntry(inputs.views[1]),
            rhi::BindGroupEntry::TextureEntry(inputs.views[2]),
            rhi::BindGroupEntry::TextureEntry(inputs.views[3]),
            rhi::BindGroupEntry::TextureEntry(inputs.views[4]),
            rhi::BindGroupEntry::BufferEntry(inputs.buffer, 0, 9 * sizeof(Float4)),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
            rhi::BindGroupEntry::SamplerEntry(m_linearSampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_resolveLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 9};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_resolveBindGroups.Store(histPrev, inputs, bg);
        return bg;
    }

    void SsgiPass::Shutdown()
    {
        for (auto& kv : m_bindGroups)
        {
            if (kv.value.bg != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_bindGroups.Clear();
        for (auto& kv : m_downBindGroups)
        {
            if (kv.value.bg != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_downBindGroups.Clear();
        for (auto& kv : m_blurBindGroups)
        {
            if (kv.value.bg != nullptr)
            {
                m_device->DestroyBindGroup(kv.value.bg);
            }
        }
        m_blurBindGroups.Clear();
        m_resolveBindGroups.Release([this](rhi::BindGroup* bg) { m_device->DestroyBindGroup(bg); });
        if (m_dummyShBuffer != nullptr)
        {
            m_device->DestroyBuffer(m_dummyShBuffer);
            m_dummyShBuffer = nullptr;
        }
        for (auto& r : m_retired)
        {
            m_device->DestroyBindGroup(r.bg);
        }
        m_retired.Clear();
        for (u32 v = 0; v < kMaxViews; ++v)
        {
            DestroyHistory(m_views[v]);
        }
        if (m_pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_downPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_downPipeline);
            m_downPipeline = nullptr;
        }
        if (m_blurPipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_blurPipeline);
            m_blurPipeline = nullptr;
        }
        if (m_resolvePipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_resolvePipeline);
            m_resolvePipeline = nullptr;
        }
        if (m_sampler != nullptr)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
        if (m_linearSampler != nullptr)
        {
            m_device->DestroySampler(m_linearSampler);
            m_linearSampler = nullptr;
        }
        if (m_pipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = nullptr;
        }
        if (m_downPipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_downPipelineLayout);
            m_downPipelineLayout = nullptr;
        }
        if (m_blurPipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_blurPipelineLayout);
            m_blurPipelineLayout = nullptr;
        }
        if (m_resolvePipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_resolvePipelineLayout);
            m_resolvePipelineLayout = nullptr;
        }
        if (m_layout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
        if (m_downLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_downLayout);
            m_downLayout = nullptr;
        }
        if (m_blurLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_blurLayout);
            m_blurLayout = nullptr;
        }
        if (m_resolveLayout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_resolveLayout);
            m_resolveLayout = nullptr;
        }
    }
}
