// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:tonemap` partition.
///
/// The HDR resolve: the forward pass renders linear HDR into a transient (RGBA16F); this fullscreen
/// pass reads it, applies exposure + a tonemap operator + the display OETF, and writes the LDR
/// target. Keeps the renderer in a strict linear working space that the IBL/post stack is
/// designed against. The operator is selectable: a trivial clamp or AgX.

module;
#include "Core/Prelude.h"

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
    Status TonemapPass::Initialize()
    {

        // set 0: HDR (t0) + bloom (t1) + AO (t2), all sampled, + a linear sampler (s0).
        rhi::BindGroupLayoutEntry hdrEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry bloomEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry aoEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry autoLumEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(3, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry lutEntry =
            rhi::BindGroupLayoutEntry::SampledTexture(4, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry sampEntry =
            rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
        rhi::BindGroupLayoutEntry entries[] = {hdrEntry, bloomEntry, aoEntry,
                                               autoLumEntry, lutEntry, sampEntry};
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{entries, 6};
        if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayout* layouts[] = {m_layout};
        rhi::PushConstantRange pc{};
        pc.stages = rhi::ShaderStage::Fragment;
        pc.offset = 0;
        pc.size = sizeof(GpuTonemapPush);
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
        pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::SamplerDesc ss{};
        ss.minFilter = rhi::FilterMode::Linear;
        ss.magFilter = rhi::FilterMode::Linear;
        ss.addressU = rhi::AddressMode::ClampToEdge;
        ss.addressV = rhi::AddressMode::ClampToEdge;
        ss.addressW = rhi::AddressMode::ClampToEdge;
        ss.label = u8"tonemap.bloomSampler";
        if (!m_device->CreateSampler(ss, m_sampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    void TonemapPass::DeclareTonemap(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                                     rendergraph::RGHandle bloom, rendergraph::RGHandle ao,
                                     rendergraph::RGHandle ldr, bool clearColor,
                                     const rhi::ClearColor& clear, rhi::TextureFormat ldrFormat,
                                     i32 vpX, i32 vpY, u32 vpW, u32 vpH, u32 frameIndex,
                                     u32 viewIndex, f32 exposure, f32 bloomIntensity,
                                     Float2 uvScale, Float2 uvOffset, f32 aoStrength,
                                     bool debugShowAo, bool agx, bool sceneYFlipped,
                                     const TonemapAutoExposure& autoExposure,
                                     const TonemapGrading& grading, bool copyOnly, ContentRotation turn)
    {
        rhi::RenderPipeline* pipeline = EnsurePipeline(ldrFormat);
        if (pipeline == nullptr)
        {
            return;
        }
        // The present copy has slots of its own: the tone map's group for this view is still
        // referenced by commands recorded earlier in the frame.
        const u32 use = copyOnly ? 1u : 0u;
        const u32 slot = ((use * kMaxViews) + (viewIndex % kMaxViews)) * m_framesInFlight +
                         (frameIndex % m_framesInFlight);
        // The scene input is mirrored on Y-flip backends unless the TAA resolve un-mirrored
        // it upstream; tonemap compensates then (see tonemap.ps.hlsl FlipSceneY).
        const bool flipSceneY = sceneYFlipped && m_device->NeedsClipSpaceYFlip();
        // Auto-exposure / grading are optional: unbound slots fall back to the HDR view (any
        // valid texture - the shader gates on the flags, never the binding).
        const bool autoOn = autoExposure.view != nullptr && autoExposure.enabled;
        const bool gradeOn = grading.view != nullptr && grading.lutSize >= 2.0f;
        // Encode for display ONCE: an sRGB target encodes on write and a float target is linear
        // (encoded wherever it is shown), so only a plain UNORM target takes the display
        // encoding as the shader writes it. Writing it to an sRGB swapchain encoded it twice.
        const bool encodeOutput = !rhi::IsSrgb(ldrFormat) && !rhi::IsFloat(ldrFormat);
        GpuTonemapPush push{};
        push.exposure = exposure;
        push.bloomIntensity = bloomIntensity;
        push.uvScale = uvScale;
        push.uvOffset = uvOffset;
        push.aoStrength = aoStrength;
        push.debugShowAo = debugShowAo ? 1.0f : 0.0f;
        push.toneOperator = copyOnly ? 2.0f : (agx ? 1.0f : 0.0f);
        push.flipSceneY = flipSceneY ? 1.0f : 0.0f;
        push.autoExposure = autoOn ? 1.0f : 0.0f;
        push.autoKey = autoExposure.key;
        push.autoMin = autoExposure.minExposure;
        push.autoMax = autoExposure.maxExposure;
        push.gradeIntensity = gradeOn ? grading.intensity : 0.0f;
        push.lutSize = gradeOn ? grading.lutSize : 0.0f;
        push.encodeOutput = encodeOutput ? 1.0f : 0.0f;
        push.rotation = static_cast<f32>(static_cast<u32>(turn));
        push._pad0 = 0.0f;
        push._pad1 = 0.0f;

        const rhi::LoadOp load = clearColor ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
        rhi::TextureView* autoView = autoOn ? autoExposure.view : nullptr;
        const u64 autoGen = autoOn ? autoExposure.generation : 0;
        const rendergraph::RGHandle autoHandle = autoExposure.handle;
        rhi::TextureView* lutView = gradeOn ? grading.view : nullptr;
        const u64 lutUid = gradeOn ? grading.uid : 0;
        graph.AddRenderPass(
            u8"tonemap",
            [this, &graph, hdr, bloom, ao, ldr, load, clear, vpX, vpY, vpW, vpH, pipeline, slot,
             push, autoView, autoGen, autoHandle, lutView, lutUid](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, ldr, load, rhi::StoreOp::Store, clear);
                b.ReadTexture(hdr);
                b.ReadTexture(bloom);
                b.ReadTexture(ao);
                if (autoView != nullptr && autoHandle.IsValid())
                {
                    b.ReadTexture(autoHandle); // orders after the exposure measure pass
                }
                b.SetViewport(vpX, vpY, vpW, vpH);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, hdr, bloom, ao, pipeline, slot, push, autoView, autoGen,
                     lutView, lutUid](rhi::RenderPassEncoder& rp)
                    {
                        rhi::TextureView* hdrView = graph.GetTextureView(hdr);
                        rhi::BindGroup* bg = EnsureBindGroup(
                            slot, hdrView, graph.GetTextureView(bloom),
                            graph.GetTextureView(ao),
                            autoView != nullptr ? autoView : hdrView,
                            lutView != nullptr ? lutView : hdrView,
                            graph.GetTextureGeneration(hdr) ^
                                (graph.GetTextureGeneration(bloom) * 1099511628211ull) ^
                                (graph.GetTextureGeneration(ao) * 14695981039346656037ull) ^
                                (autoGen * 31ull) ^ (lutUid * 131071ull));
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(push), &push);
                        rp.Draw(3, 1, 0, 0);
                    });
            });
    }

    rhi::RenderPipeline* TonemapPass::EnsurePipeline(rhi::TextureFormat fmt)
    {
        const u64 shaderVersion = m_shaders->Version(u8"tonemap"); // hot reload rebuilds
        // Per-format cache: NEVER destroy on a format mismatch - a frame can hold views with
        // different target formats, and the earlier view's commands still reference their
        // pipeline. A stale-shader-version entry rebuilds in place (hot reload is a dev-loop
        // event, the pre-existing trade); an unknown format takes a free slot, or evicts slot
        // 0 when all are taken (more distinct formats per run than slots is not a real case).
        PipelineEntry* entry = nullptr;
        for (PipelineEntry& candidate : m_pipelines)
        {
            if (candidate.pipeline != nullptr && candidate.format == fmt)
            {
                entry = &candidate;
                break;
            }
        }
        if (entry != nullptr && entry->shaderVersion == shaderVersion)
        {
            return entry->pipeline;
        }
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"tonemap", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(u8"tonemap", shaders::ShaderStage::Fragment,
                                                      shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        if (entry == nullptr)
        {
            for (PipelineEntry& candidate : m_pipelines)
            {
                if (candidate.pipeline == nullptr)
                {
                    entry = &candidate;
                    break;
                }
            }
        }
        if (entry == nullptr)
        {
            entry = &m_pipelines[0];
        }
        if (entry->pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(entry->pipeline);
            entry->pipeline = nullptr;
        }

        rhi::ColorTargetState color{};
        color.format = fmt;
        rhi::FragmentState frag{};
        frag.shader = rhi::StageOf(ps, rhi::ShaderStage::Fragment);
        frag.targets = Span<const rhi::ColorTargetState>{&color, 1};

        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::StageOf(vs, rhi::ShaderStage::Vertex);
        pd.fragment = frag;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.label = u8"tonemap";
        if (!m_device->CreateRenderPipeline(pd, entry->pipeline).IsOk())
        {
            entry->pipeline = nullptr;
            return nullptr;
        }
        entry->format = fmt;
        entry->shaderVersion = shaderVersion;
        return entry->pipeline;
    }

    rhi::BindGroup* TonemapPass::EnsureBindGroup(u32 slot, rhi::TextureView* hdrView,
                                                 rhi::TextureView* bloomView,
                                                 rhi::TextureView* aoView,
                                                 rhi::TextureView* autoLumView,
                                                 rhi::TextureView* lutView, u64 generation)
    {
        if (slot >= kMaxSlots || hdrView == nullptr || bloomView == nullptr ||
            aoView == nullptr || autoLumView == nullptr || lutView == nullptr)
        {
            return nullptr;
        }
        if (m_bindGroups[slot] != nullptr && m_bgViews[slot] == hdrView &&
            m_bgBloom[slot] == bloomView && m_bgAo[slot] == aoView && m_bgGen[slot] == generation)
        {
            return m_bindGroups[slot];
        }
        if (m_bindGroups[slot] != nullptr)
        {
            m_device->DestroyBindGroup(m_bindGroups[slot]);
            m_bindGroups[slot] = nullptr;
        }
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::TextureEntry(hdrView),
            rhi::BindGroupEntry::TextureEntry(bloomView),
            rhi::BindGroupEntry::TextureEntry(aoView),
            rhi::BindGroupEntry::TextureEntry(autoLumView),
            rhi::BindGroupEntry::TextureEntry(lutView),
            rhi::BindGroupEntry::SamplerEntry(m_sampler),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 6};
        if (!m_device->CreateBindGroup(bgd, m_bindGroups[slot]).IsOk())
        {
            m_bindGroups[slot] = nullptr;
            return nullptr;
        }
        m_bgViews[slot] = hdrView;
        m_bgBloom[slot] = bloomView;
        m_bgAo[slot] = aoView;
        m_bgGen[slot] = generation;
        return m_bindGroups[slot];
    }

    void TonemapPass::Shutdown()
    {
        for (u32 i = 0; i < kMaxSlots; ++i)
        {
            if (m_bindGroups[i] != nullptr)
            {
                m_device->DestroyBindGroup(m_bindGroups[i]);
                m_bindGroups[i] = nullptr;
            }
        }
        for (PipelineEntry& entry : m_pipelines)
        {
            if (entry.pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(entry.pipeline);
                entry.pipeline = nullptr;
            }
        }
        if (m_pipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = nullptr;
        }
        if (m_sampler != nullptr)
        {
            m_device->DestroySampler(m_sampler);
            m_sampler = nullptr;
        }
        if (m_layout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
    }
}
