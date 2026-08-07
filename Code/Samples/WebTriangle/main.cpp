// WebTriangle - the first Draconic app that runs in a browser.
//
// It uses the RUNTIME framework (DRACONIC_APP_MAIN -> IApplication driven by an ApplicationHost),
// NOT the Vulkan/SDL3 SampleApp framework: a WebShell hands the host an HTML <canvas>, the WebGPU
// graphics factory brings a device up on that canvas, and the web runner drives frames through
// requestAnimationFrame. The app records a single WGSL triangle each frame. Shaders are WGSL
// directly (browsers ingest WGSL; there is no runtime HLSL compiler on web), and the triangle is
// generated from the vertex index, so no vertex buffer or bind groups are needed.
//
// Build (wasm preset) emits WebTriangle.html + .js + .wasm; open the .html in a WebGPU browser.

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.runtime;
import draconic.runtime.client;
import draconic.shell;
import draconic.runtime.web;  // RunApplication (browser runner) - required by DRACONIC_APP_MAIN
import draconic.shell.web;    // WebShell - required by DRACONIC_APP_MAIN
import draconic.graphics;     // GraphicsDevice + FrameContext
import draconic.graphics.gpu; // CreateGraphicsDevice
import draconic.rhi;

#include "Draconic.Runtime.Client/AppMain.h"

namespace foundation = draconic::foundation;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace rhi = draconic::rhi;

namespace
{
    // Position + color per vertex baked into the shader, selected by vertex_index -> no vertex
    // buffer. Color target format is the swapchain default (BGRA8UnormSrgb, see RenderWindowDesc).
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
                foundation::ConsoleWrite(u8"WebTriangle: no graphics device - nothing to draw.\n");
                return;
            }
            m_device = gpu->Raw();

            rhi::ShaderModuleDesc moduleDesc;
            moduleDesc.code = foundation::Span<const foundation::u8>(
                reinterpret_cast<const foundation::u8*>(kTriangleWgsl),
                foundation::StringView(kTriangleWgsl).Size());
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
            plDesc.bindGroupLayouts = foundation::Span<rhi::BindGroupLayout* const>(sets, 1);
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
                foundation::Span<const rhi::ColorTargetState>(&colorTarget, 1);
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
                pass->SetViewport(0.0f, 0.0f, static_cast<foundation::f32>(frame.width),
                                  static_cast<foundation::f32>(frame.height), 0.0f, 1.0f);
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

DRACONIC_APP_MAIN(TriangleApp)
