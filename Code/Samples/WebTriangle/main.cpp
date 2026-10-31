// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebTriangle: a minimal app that runs in a browser.
//
// It uses the runtime framework (APP_MAIN, with an IApplication driven by an ApplicationHost),
// not the Vulkan/SDL3 SampleApp framework: a WebShell hands the host an HTML <canvas>, the WebGPU
// graphics factory brings a device up on that canvas, and the web runner drives frames through
// requestAnimationFrame. The app records a single WGSL triangle each frame. Shaders are WGSL
// directly (browsers ingest WGSL; there is no runtime HLSL compiler on web), and the triangle is
// generated from the vertex index, so no vertex buffer or bind groups are needed.
//
// The wasm preset build emits WebTriangle.html, .js and .wasm; open the .html in a WebGPU browser.

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.web;  // RunApplication (the browser runner), required by APP_MAIN
import foundation.shell.web;    // WebShell, required by APP_MAIN
import foundation.graphics;     // GraphicsDevice and FrameContext
import foundation.graphics.gpu; // CreateGraphicsDevice
import foundation.rhi;

#include "Runtime.Client/AppMain.h"

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace rhi = foundation::rhi;

namespace
{
    // Position and color per vertex are baked into the shader and selected by vertex_index, so
    // there is no vertex buffer. The color target format is the swapchain default
    // (BGRA8UnormSrgb, see RenderWindowDesc).
    constexpr const char8_t kTriangleWgsl[] = u8R"(
        struct VSOut {
            @builtin(position) position : vec4f,
            @location(0) color : vec3f,
        };
        @vertex fn vs(@builtin(vertex_index) index : u32) -> VSOut {
            var positions = array<vec2f, 3>(
                vec2f( 0.0,  0.5),
                vec2f( 0.5, -0.5),
                vec2f(-0.5, -0.5));
            var colors = array<vec3f, 3>(
                vec3f(1.0, 0.0, 0.0),
                vec3f(0.0, 1.0, 0.0),
                vec3f(0.0, 0.0, 1.0));
            var out : VSOut;
            out.position = vec4f(positions[index], 0.0, 1.0);
            out.color = colors[index];
            return out;
        }
        @fragment fn fs(in : VSOut) -> @location(0) vec4f {
            return vec4f(in.color, 1.0);
        }
    )";

    class TriangleApp final : public runtime::IApplication
    {
    public:
        void OnStartup(runtime::IApplicationHost& host) override
        {
            graphics::GraphicsDevice* gpu = host.Graphics();
            if (gpu == nullptr)
            {
                core::ConsoleWrite(u8"WebTriangle: no graphics device - nothing to draw.\n");
                return;
            }
            m_device = gpu->Raw();

            rhi::ShaderModuleDesc moduleDesc;
            moduleDesc.code = core::Span<const core::u8>(
                reinterpret_cast<const core::u8*>(kTriangleWgsl),
                core::StringView(kTriangleWgsl).Size());
            moduleDesc.label = u8"TriangleWGSL";
            if (!m_device->CreateShaderModule(moduleDesc, m_shader).IsOk())
            {
                return;
            }

            rhi::BindGroupLayoutDesc bglDesc;
            bglDesc.label = u8"EmptyBGL";
            if (!m_device->CreateBindGroupLayout(bglDesc, m_bgl).IsOk())
            {
                return;
            }
            rhi::PipelineLayoutDesc plDesc;
            rhi::BindGroupLayout* sets[1] = {m_bgl};
            plDesc.bindGroupLayouts = core::Span<rhi::BindGroupLayout* const>(sets, 1);
            plDesc.label = u8"TrianglePL";
            if (!m_device->CreatePipelineLayout(plDesc, m_pipelineLayout).IsOk())
            {
                return;
            }

            rhi::ColorTargetState colorTarget;
            colorTarget.format = rhi::TextureFormat::BGRA8UnormSrgb; // the swapchain's default format
            colorTarget.writeMask = rhi::ColorWriteMask::All;

            rhi::RenderPipelineDesc pipelineDesc;
            pipelineDesc.layout = m_pipelineLayout;
            pipelineDesc.vertex.shader = {m_shader, u8"vs", rhi::ShaderStage::Vertex};
            pipelineDesc.fragment = rhi::FragmentState{};
            pipelineDesc.fragment->shader = {m_shader, u8"fs", rhi::ShaderStage::Fragment};
            pipelineDesc.fragment->targets =
                core::Span<const rhi::ColorTargetState>(&colorTarget, 1);
            pipelineDesc.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pipelineDesc.label = u8"TrianglePipeline";
            if (!m_device->CreateRenderPipeline(pipelineDesc, m_pipeline).IsOk())
            {
                m_pipeline = nullptr;
            }
        }

        void OnRenderWindow(runtime::IApplicationHost&, graphics::FrameContext& frame) override
        {
            rhi::ClearColor clear;
            clear.r = 0.05f;
            clear.g = 0.06f;
            clear.b = 0.10f;
            clear.a = 1.0f;
            rhi::RenderPassEncoder* pass = frame.BeginBackbufferPass(clear);
            if (pass != nullptr && m_pipeline != nullptr)
            {
                pass->SetPipeline(m_pipeline);
                pass->SetViewport(0.0f, 0.0f, static_cast<core::f32>(frame.width),
                                  static_cast<core::f32>(frame.height), 0.0f, 1.0f);
                pass->SetScissor(0, 0, frame.width, frame.height);
                pass->Draw(3);
            }
            frame.EndBackbufferPass();
        }

        void OnShutdown(runtime::IApplicationHost&) override
        {
            if (m_device == nullptr)
            {
                return;
            }
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
            }
            if (m_pipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
            }
            if (m_bgl != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_bgl);
            }
            if (m_shader != nullptr)
            {
                m_device->DestroyShaderModule(m_shader);
            }
        }

    private:
        rhi::Device* m_device = nullptr;
        rhi::ShaderModule* m_shader = nullptr;
        rhi::BindGroupLayout* m_bgl = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
    };
}

APP_MAIN(TriangleApp)
