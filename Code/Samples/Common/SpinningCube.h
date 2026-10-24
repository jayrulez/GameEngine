// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#pragma once

// A spinning cube for the dev samples' viewports: drawn through raw RHI into a viewport's offscreen
// colour and depth targets, the content a viewport widget shows (foundation.ui's UISandbox, the
// GUI's GUISandbox). The including translation unit must have imported foundation.core,
// foundation.rhi and foundation.shaders.

namespace samples
{
    using namespace foundation::core;
    namespace rhi = foundation::rhi;
    namespace shaders = foundation::shaders;

    // A minimal, self-contained spinning cube rendered via raw RHI into a viewport's offscreen
    // RGBA16Float color + Depth32Float depth targets - the payload for the Viewport tab. It only
    // exercises the OnRender(encoder) contract; the real renderer would slot in here instead.
    struct SpinningCube
    {
        static constexpr const char8_t kShader[] = u8R"(
#pragma pack_matrix(row_major)
cbuffer Uniforms : register(b0) { float4x4 MVP; };
struct VSIn { float3 Position : TEXCOORD0; float3 Color : TEXCOORD1; };
struct PSIn { float4 Position : SV_POSITION; float3 Color : COLOR0; };
PSIn VSMain(VSIn i) { PSIn o; o.Position = mul(float4(i.Position, 1.0), MVP); o.Color = i.Color; return o; }
float4 PSMain(PSIn i) : SV_TARGET { return float4(i.Color, 1.0); }
)";
        // 8 corners, each a distinct color; stride = 6 floats (pos3 + color3).
        static constexpr float kVerts[] = {
            -1, -1, -1, 0, 0, 0, 1, -1, -1, 1, 0, 0, 1, 1, -1, 1, 1, 0, -1, 1, -1, 0, 1, 0,
            -1, -1, 1,  0, 0, 1, 1, -1, 1,  1, 0, 1, 1, 1, 1,  1, 1, 1, -1, 1, 1,  0, 1, 1,
        };
        static constexpr u16 kIdx[] = {
            0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
            3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
        };
        struct Uniforms
        {
            Float4x4 mvp;
        };

        void Init(rhi::Device* device, shaders::Compiler* compiler, i32 frameCount,
                  rhi::TextureFormat colorFmt, rhi::TextureFormat depthFmt)
        {
            m_device = device;
            rhi::Queue* queue = device->GetQueue(rhi::QueueType::Graphics, 0);

            CompileOne(compiler, shaders::ShaderStage::Vertex, u8"VSMain", m_vs);
            CompileOne(compiler, shaders::ShaderStage::Fragment, u8"PSMain", m_ps);

            rhi::BufferDesc vbd{};
            vbd.size = sizeof(kVerts);
            vbd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
            vbd.memory = rhi::MemoryLocation::GpuOnly;
            device->CreateBuffer(vbd, m_vb);
            rhi::BufferDesc ibd{};
            ibd.size = sizeof(kIdx);
            ibd.usage = rhi::BufferUsage::Index | rhi::BufferUsage::CopyDst;
            ibd.memory = rhi::MemoryLocation::GpuOnly;
            device->CreateBuffer(ibd, m_ib);
            rhi::TransferBatch* tb = nullptr;
            queue->CreateTransferBatch(tb);
            tb->WriteBuffer(m_vb, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(kVerts), sizeof(kVerts)));
            tb->WriteBuffer(m_ib, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(kIdx), sizeof(kIdx)));
            tb->Submit();
            queue->DestroyTransferBatch(tb);

            rhi::BindGroupLayoutEntry e =
                rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex);
            rhi::BindGroupLayoutDesc bgld{};
            bgld.entries = Span<const rhi::BindGroupLayoutEntry>(&e, 1);
            device->CreateBindGroupLayout(bgld, m_bgl);
            rhi::BindGroupLayout* layouts[1] = {m_bgl};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(layouts, 1);
            device->CreatePipelineLayout(pld, m_pl);

            // Per-frame uniform ring (frame N+1's CPU write mustn't clobber frame N's in-flight read).
            m_uniforms.Resize(static_cast<usize>(frameCount));
            m_bindGroups.Resize(static_cast<usize>(frameCount));
            for (i32 i = 0; i < frameCount; ++i)
            {
                rhi::BufferDesc ud{};
                ud.size = sizeof(Uniforms);
                ud.usage = rhi::BufferUsage::Uniform;
                ud.memory = rhi::MemoryLocation::CpuToGpu;
                device->CreateBuffer(ud, m_uniforms[static_cast<usize>(i)]);
                rhi::BindGroupEntry bge = rhi::BindGroupEntry::BufferEntry(
                    m_uniforms[static_cast<usize>(i)], 0, sizeof(Uniforms));
                rhi::BindGroupDesc bgd{};
                bgd.layout = m_bgl;
                bgd.entries = Span<const rhi::BindGroupEntry>(&bge, 1);
                device->CreateBindGroup(bgd, m_bindGroups[static_cast<usize>(i)]);
            }

            rhi::VertexAttribute attrs[2] = {{rhi::VertexFormat::Float32x3, 0, 0},
                                             {rhi::VertexFormat::Float32x3, 12, 1}};
            rhi::VertexBufferLayout vbl{};
            vbl.stride = 24;
            vbl.attributes = Span<const rhi::VertexAttribute>(attrs, 2);
            rhi::ColorTargetState ct{};
            ct.format = colorFmt; // matches the viewport's owned color format
            rhi::RenderPipelineDesc rpd{};
            rpd.layout = m_pl;
            rpd.vertex.shader = {m_vs, u8"VSMain", rhi::ShaderStage::Vertex};
            rpd.vertex.buffers = Span<const rhi::VertexBufferLayout>(&vbl, 1);
            rpd.fragment = rhi::FragmentState{};
            rpd.fragment->shader = {m_ps, u8"PSMain", rhi::ShaderStage::Fragment};
            rpd.fragment->targets = Span<const rhi::ColorTargetState>(&ct, 1);
            rpd.depthStencil = rhi::DepthStencilState{};
            rpd.depthStencil->format = depthFmt;
            rpd.depthStencil->depthWriteEnabled = true;
            rpd.depthStencil->depthCompare = rhi::depth::Nearer();
            rpd.primitive.cullMode = rhi::CullMode::None;
            device->CreateRenderPipeline(rpd, m_pipeline);
        }

        void Render(rhi::CommandEncoder& enc, rhi::TextureView* colorView,
                    rhi::TextureView* depthView, u32 w, u32 h, rhi::ClearColor clear,
                    const Float4x4& mvp, i32 frameIndex)
        {
            if (m_pipeline == nullptr)
            {
                return;
            }
            const usize fi = static_cast<usize>(frameIndex);
            Uniforms u{mvp};
            if (u8* p = static_cast<u8*>(m_uniforms[fi]->Map()))
            {
                MemCopy(p, &u, sizeof(u));
                m_uniforms[fi]->Unmap();
            }

            rhi::ColorAttachment ca{};
            ca.view = colorView;
            ca.loadOp = rhi::LoadOp::Clear;
            ca.storeOp = rhi::StoreOp::Store;
            ca.clearValue = clear;
            rhi::DepthStencilAttachment dsa{};
            dsa.view = depthView;
            dsa.depthLoadOp = rhi::LoadOp::Clear;
            dsa.depthStoreOp = rhi::StoreOp::Store;
            dsa.depthClearValue = rhi::depth::ClearValue();
            rhi::RenderPassDesc rp{};
            rp.colorAttachments.Add(ca);
            rp.depthStencilAttachment = dsa;
            rhi::RenderPassEncoder* pass = enc.BeginRenderPass(rp);
            if (pass == nullptr)
            {
                return;
            }
            pass->SetViewport(0.0f, 0.0f, static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f);
            pass->SetScissor(0, 0, w, h);
            pass->SetPipeline(m_pipeline);
            pass->SetBindGroup(0, m_bindGroups[fi]);
            pass->SetVertexBuffer(0, m_vb, 0);
            pass->SetIndexBuffer(m_ib, rhi::IndexFormat::UInt16, 0);
            pass->DrawIndexed(36);
            pass->End();
        }

        void Shutdown()
        {
            if (m_device == nullptr)
            {
                return;
            }
            if (m_pipeline)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
            }
            for (usize i = 0; i < m_bindGroups.Size(); ++i)
            {
                if (m_bindGroups[i])
                {
                    m_device->DestroyBindGroup(m_bindGroups[i]);
                }
            }
            for (usize i = 0; i < m_uniforms.Size(); ++i)
            {
                if (m_uniforms[i])
                {
                    m_device->DestroyBuffer(m_uniforms[i]);
                }
            }
            if (m_pl)
            {
                m_device->DestroyPipelineLayout(m_pl);
            }
            if (m_bgl)
            {
                m_device->DestroyBindGroupLayout(m_bgl);
            }
            if (m_ib)
            {
                m_device->DestroyBuffer(m_ib);
            }
            if (m_vb)
            {
                m_device->DestroyBuffer(m_vb);
            }
            if (m_ps)
            {
                m_device->DestroyShaderModule(m_ps);
            }
            if (m_vs)
            {
                m_device->DestroyShaderModule(m_vs);
            }
            m_device = nullptr;
        }

        void CompileOne(shaders::Compiler* compiler, shaders::ShaderStage stage, StringView entry,
                        rhi::ShaderModule*& out)
        {
            const bool isDX12 = (m_device->type == rhi::DeviceType::DX12);
            const shaders::ShaderTarget target =
                isDX12 ? shaders::ShaderTarget::DXIL : shaders::ShaderTarget::SPIRV;
            shaders::CompileOptions opts{};
            opts.shaderModel = u8"6_0";
            opts.optimizationLevel = 3;
            if (!isDX12)
            {
                opts.bindingShifts = shaders::BindingShifts::Standard();
                opts.bindingShiftSets = 4;
            }
            const StringView src(kShader);
            shaders::CompileResult cr{};
            if (compiler->compile(reinterpret_cast<const u8*>(src.Data()), src.Size(), stage, entry,
                                  target, opts, cr) == ErrorCode::Ok)
            {
                rhi::ShaderModuleDesc d{};
                d.code = Span<const u8>(cr.bytecode, cr.bytecodeSize);
                m_device->CreateShaderModule(d, out);
            }
            compiler->freeResult(cr);
        }

        rhi::Device* m_device = nullptr;
        rhi::ShaderModule* m_vs = nullptr;
        rhi::ShaderModule* m_ps = nullptr;
        rhi::Buffer* m_vb = nullptr;
        rhi::Buffer* m_ib = nullptr;
        rhi::BindGroupLayout* m_bgl = nullptr;
        rhi::PipelineLayout* m_pl = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        Array<rhi::Buffer*> m_uniforms;
        Array<rhi::BindGroup*> m_bindGroups;
    };
} // namespace samples
