// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// MsaaResolvePass implementation. Fullscreen sample-0
// resolve of the MSAA opaque depth + G-buffer aux into 1x targets. See MsaaResolvePass.cppm.

module;
#include "Core/Prelude.h"

module foundation.render;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;

using namespace foundation::core;

namespace foundation::render
{
    namespace rendergraph = foundation::rendergraph;
    namespace shaders = foundation::shaders;
    namespace rhi = foundation::rhi;

    Status MsaaResolvePass::Initialize()
    {
        // 5 MULTISAMPLED sampled textures (no sampler - the shader uses .Load(coord, 0)): normal (t0),
        // velocity (t1), material (t2) are RG float aux; depth (t3) is read as unfilterable float;
        // albedo (t4) is the RGBA8 diffuse albedo SSGI reads.
        rhi::BindGroupLayoutEntry e[] = {
            rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(2, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(3, rhi::ShaderStage::Fragment),
            rhi::BindGroupLayoutEntry::SampledTexture(4, rhi::ShaderStage::Fragment),
        };
        for (u32 i = 0; i < 5; ++i)
        {
            e[i].textureMultisampled = true; // Texture2DMS<...> inputs
            // WebGPU forbids a filterable (Float) sample type on a multisampled texture binding; all
            // four are read via .Load(coord, 0) (no sampler), so UnfilterableFloat is both required
            // and correct. (Vulkan tolerated Float here; WebGPU strictly rejects it.)
            e[i].textureSampleType = rhi::TextureSampleType::UnfilterableFloat;
        }
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{e, 5};
        if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BindGroupLayout* layouts[] = {m_layout};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
        if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        // The pipeline is (re)built lazily in DeclareResolve, keyed on the scene depth format (so a
        // host with a different depth format still gets a valid pipeline) + the shader version.
        return Status{};
    }

    rhi::RenderPipeline* MsaaResolvePass::MakePipeline(rhi::TextureFormat depthFormat)
    {
        rhi::ShaderModule* vs = m_shaders->GetVariant(u8"msaa_resolve", shaders::ShaderStage::Vertex,
                                                      shaders::ShaderFlags::None);
        rhi::ShaderModule* ps = m_shaders->GetVariant(
            u8"msaa_resolve", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        if (vs == nullptr || ps == nullptr)
        {
            return nullptr;
        }
        rhi::ColorTargetState targets[4] = {};
        targets[0].format = kGNormalFormat;   // SV_Target0 = normal
        targets[1].format = kGVelocityFormat; // SV_Target1 = velocity
        targets[2].format = kGMaterialFormat; // SV_Target2 = material
        targets[3].format = kGAlbedoFormat;   // SV_Target3 = albedo
        rhi::FragmentState frag{};
        frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
        frag.targets = Span<const rhi::ColorTargetState>{targets, 4};

        // Depth written via SV_Depth: depth test must be enabled to permit the write (Vulkan gates
        // depth writes on depthTestEnabled), so use Always (undefined initial depth is irrelevant).
        rhi::DepthStencilState ds{};
        ds.format = depthFormat;
        ds.depthTestEnabled = true;
        ds.depthWriteEnabled = true;
        ds.depthCompare = rhi::CompareFunction::Always;

        rhi::RenderPipelineDesc pd{};
        pd.layout = m_pipelineLayout;
        pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
        pd.fragment = frag;
        pd.depthStencil = ds;
        pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
        pd.primitive.cullMode = rhi::CullMode::None;
        pd.multisample.count = 1; // the RESOLVE OUTPUT is single-sample
        pd.label = u8"msaa_resolve";
        rhi::RenderPipeline* p = nullptr;
        if (!m_device->CreateRenderPipeline(pd, p).IsOk())
        {
            return nullptr;
        }
        return p;
    }

    rhi::BindGroup* MsaaResolvePass::EnsureBindGroup(const BindInputs& inputs)
    {
        if (!inputs.Complete())
        {
            return nullptr;
        }
        rhi::TextureView* depth = inputs.views[3];
        rhi::BindGroup* stale = nullptr;
        if (rhi::BindGroup* cached = m_bindGroups.Find(depth, inputs, stale))
        {
            return cached;
        }
        if (stale != nullptr)
        {
            m_device->DestroyBindGroup(stale);
        }
        rhi::BindGroupEntry ent[] = {
            rhi::BindGroupEntry::TextureEntry(inputs.views[0]),
            rhi::BindGroupEntry::TextureEntry(inputs.views[1]),
            rhi::BindGroupEntry::TextureEntry(inputs.views[2]),
            rhi::BindGroupEntry::TextureEntry(depth),
            rhi::BindGroupEntry::TextureEntry(inputs.views[4]),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{ent, 5};
        rhi::BindGroup* bg = nullptr;
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            return nullptr;
        }
        m_bindGroups.Store(depth, inputs, bg);
        return bg;
    }

    MsaaResolveOutputs MsaaResolvePass::DeclareResolve(
        rendergraph::RenderGraph& graph, rendergraph::RGHandle msaaDepth,
        rendergraph::RGHandle msaaNormal, rendergraph::RGHandle msaaVelocity,
        rendergraph::RGHandle msaaMaterial, rendergraph::RGHandle msaaAlbedo, rhi::TextureFormat depthFormat,
        u32 w, u32 h)
    {
        MsaaResolveOutputs out{};
        if (w == 0 || h == 0)
        {
            return out;
        }
        // Hot reload / first use: (re)build the pipeline when the shader changed or the depth format
        // differs from the cached one (GPU idled on shader reload by the caller).
        const u64 shaderVersion = m_shaders->Version(u8"msaa_resolve");
        if (m_pipeline == nullptr || shaderVersion != m_pipelineShaderVersion ||
            depthFormat != m_pipelineDepthFormat)
        {
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
            }
            m_pipeline = MakePipeline(depthFormat);
            m_pipelineShaderVersion = shaderVersion;
            m_pipelineDepthFormat = depthFormat;
        }
        if (m_pipeline == nullptr)
        {
            return out;
        }

        out.normal = graph.CreateTransient(u8"msaa.resolvedNormal",
                                           rendergraph::RGTextureDesc(kGNormalFormat, w, h));
        out.velocity = graph.CreateTransient(u8"msaa.resolvedVelocity",
                                             rendergraph::RGTextureDesc(kGVelocityFormat, w, h));
        out.material = graph.CreateTransient(u8"msaa.resolvedMaterial",
                                             rendergraph::RGTextureDesc(kGMaterialFormat, w, h));
        out.albedo = graph.CreateTransient(u8"msaa.resolvedAlbedo",
                                           rendergraph::RGTextureDesc(kGAlbedoFormat, w, h));
        out.depth =
            graph.CreateTransient(u8"msaa.resolvedDepth", rendergraph::RGTextureDesc(depthFormat, w, h));

        graph.AddRenderPass(
            u8"msaa.resolve",
            [this, &graph, msaaDepth, msaaNormal, msaaVelocity, msaaMaterial, msaaAlbedo, out,
             w, h](rendergraph::PassBuilder& b)
            {
                b.SetColorTarget(0, out.normal, rhi::LoadOp::DontCare, rhi::StoreOp::Store);
                b.SetColorTarget(1, out.velocity, rhi::LoadOp::DontCare, rhi::StoreOp::Store);
                b.SetColorTarget(2, out.material, rhi::LoadOp::DontCare, rhi::StoreOp::Store);
                b.SetColorTarget(3, out.albedo, rhi::LoadOp::DontCare, rhi::StoreOp::Store);
                // Depth is fully overwritten via SV_Depth (fullscreen), so it need not be loaded.
                b.SetDepthTarget(out.depth, rhi::LoadOp::DontCare, rhi::StoreOp::Store);
                b.ReadTexture(msaaNormal);
                b.ReadTexture(msaaVelocity);
                b.ReadTexture(msaaMaterial);
                b.ReadTexture(msaaDepth);
                b.ReadTexture(msaaAlbedo);
                b.SetViewport(0, 0, w, h);
                b.NeverCull();
                b.SetExecute(
                    [this, &graph, msaaDepth, msaaNormal, msaaVelocity, msaaMaterial,
                     msaaAlbedo](rhi::RenderPassEncoder& rp)
                    {
                        BindInputs inputs;
                        inputs.Set(0, graph.GetTextureView(msaaNormal), graph.GetTextureGeneration(msaaNormal));
                        inputs.Set(1, graph.GetTextureView(msaaVelocity), graph.GetTextureGeneration(msaaVelocity));
                        inputs.Set(2, graph.GetTextureView(msaaMaterial), graph.GetTextureGeneration(msaaMaterial));
                        inputs.Set(3, graph.GetTextureView(msaaDepth), graph.GetTextureGeneration(msaaDepth));
                        inputs.Set(4, graph.GetTextureView(msaaAlbedo), graph.GetTextureGeneration(msaaAlbedo));
                        rhi::BindGroup* bg = EnsureBindGroup(inputs);
                        if (bg == nullptr)
                        {
                            return;
                        }
                        rp.SetPipeline(m_pipeline);
                        rp.SetBindGroup(0, bg, Span<const u32>{});
                        rp.Draw(3, 1, 0, 0);
                    });
            });
        return out;
    }

    void MsaaResolvePass::Shutdown()
    {
        m_bindGroups.Release([this](rhi::BindGroup* bg) { m_device->DestroyBindGroup(bg); });
        if (m_pipeline != nullptr)
        {
            m_device->DestroyRenderPipeline(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_pipelineLayout != nullptr)
        {
            m_device->DestroyPipelineLayout(m_pipelineLayout);
            m_pipelineLayout = nullptr;
        }
        if (m_layout != nullptr)
        {
            m_device->DestroyBindGroupLayout(m_layout);
            m_layout = nullptr;
        }
    }
}
