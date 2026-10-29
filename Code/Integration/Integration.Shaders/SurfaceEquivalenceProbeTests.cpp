// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A surface shader renders as the engine's own does: the built-in PBR written as a lit surface
// shader (Data/Tests/Shaders/surface_pbr), cooked by the shader pipeline into its forward, depth
// and pick programs, draws a lit, windy sphere casting its shadow on a ground the same as
// `forward` draws them, on REAL GPU backends. Measured identical on Vulkan and WebGPU (2026-10-12);
// the test allows a rounding step, since the two programs compute the same lighting in a different
// order (the surface samples every texture before the cut-out test) and another driver may round
// the last bit differently, nothing more.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.rhi;
#ifdef OPTION_HAS_VULKAN
import foundation.rhi.vulkan;
#endif
#ifdef OPTION_HAS_WEBGPU
import foundation.rhi.webgpu;
#endif
#ifdef OPTION_HAS_DX12
import foundation.rhi.dx12;
#endif
import foundation.rhi.testsupport;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.shaders.resource;
import foundation.render;
import shaders.pipeline;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    constexpr u32 kSize = 160;
    constexpr StringView kSurfaceName = u8"surface_pbr";

    // The surface PBR's programs, cooked once (every variant, as the shader cook proves them).
    struct SurfacePrograms
    {
        bool ok = false;
        Array<pipeline::ShaderCheckProgram> programs;
        Array<shaders::ShaderProperty> properties; // its properties block's table: its materials' layout
    };

    // `timedAlpha`: the cut-out also reads Time() (the depth and pick views' clock), through a
    // factor that is always 1 but that the compiler cannot fold, as a dissolve does; otherwise the
    // depth pixel stage reads nothing of the view's buffer and its layout goes untested.
    SurfacePrograms CookSurfacePbr(bool timedAlpha = false)
    {
        SurfacePrograms out;
        const String dataRoot = foundation::vfs::FindDataRoot();
        const String library = foundation::vfs::DataPath(dataRoot.AsView(), u8"Engine/Shaders");
        const Result<Array<byte>> text = ReadFile(
            foundation::vfs::DataPath(dataRoot.AsView(), u8"Tests/Shaders/surface_pbr/surface_pbr.hlsl")
                .AsView());
        REQUIRE(text.HasValue());
        String source(StringView(reinterpret_cast<const char8_t*>(text.Value().Data()), text.Value().Size()));
        if (timedAlpha)
        {
            const StringView alphaLine = u8"surface.alpha  = input.color.a * BaseColor.a * albedoTex.a;";
            String edited;
            const StringView all = source.AsView();
            usize at = 0;
            for (; at + alphaLine.Size() <= all.Size(); ++at)
            {
                if (all.SubStr(at, alphaLine.Size()) == alphaLine)
                {
                    break;
                }
            }
            REQUIRE(at + alphaLine.Size() <= all.Size());
            edited.Append(all.SubStr(0, at));
            edited.Append(StringView(u8"surface.alpha  = input.color.a * BaseColor.a * albedoTex.a * "
                                     u8"step(-1.0, Time());"));
            edited.Append(all.SubStr(at + alphaLine.Size(), all.Size() - at - alphaLine.Size()));
            source = Move(edited);
        }
        pipeline::ShaderCheckOptions options;
        options.shaderLibrary = library.AsView();
        pipeline::ShaderCheck check =
            pipeline::CheckShaderSource(source.AsView(), u8"surface_pbr.hlsl", options);
        for (const pipeline::ShaderDiagnostic& d : check.errors)
        {
            INFO(doctest::String(reinterpret_cast<const char*>(
                pipeline::FormatShaderDiagnostic(d).CStr())));
            CHECK(false);
        }
        REQUIRE(check.IsOk());
        REQUIRE(check.hasVertex); // forward's wind
        // The engine's vertex stages sway every surface shader by its material's Wind group, so its
        // programs declare WIND (Specs/wind-for-every-shader.md, rule 4).
        REQUIRE_FALSE(check.programs.IsEmpty());
        CHECK(shaders::HasFlag(check.programs[0].vertexMask, shaders::ShaderFlags::Wind));
        out.properties = check.block.properties;
        out.programs = Move(check.programs);
        out.ok = true;
        return out;
    }

    // The forward PBR material the scene uses, swaying by its Wind group, and the same values on
    // the surface PBR, laid out by its properties block (forward's, in forward's layout, with its
    // own wind properties in the lanes forward's buffer leaves as padding), swaying by its own
    // Vertex(). The surface material carries the Wind group too (its programs declare WIND), off:
    // so it sways once, by its Vertex(), and its own WindStrength does not collide with the
    // effect's Strength.
    RefPtr<materials::Material> PbrMaterial(StringView name, Float4 color, f32 metallic,
                                            f32 roughness, bool surface, const SurfacePrograms& cooked,
                                            const shaders::ShaderEffect& wind, f32 windStrength)
    {
        RefPtr<materials::Material> m;
        if (!surface)
        {
            m = materials::CreatePBR(name, color, metallic, roughness);
            m->AddEffectGroup(wind);
            m->SetDefaultFloat(u8"Wind.Strength", windStrength);
            m->SetDefaultFloat(u8"Wind.Speed", 1.0f);
            m->SetDefaultFloat(u8"Wind.Height", 0.5f);
            return m;
        }
        materials::MaterialBuilder builder(name);
        builder.Shader(kSurfaceName).VertexLayout(materials::VertexLayoutType::Mesh);
        for (const shaders::ShaderProperty& p : cooked.properties)
        {
            switch (p.type)
            {
            case shaders::ShaderPropertyType::Float:
                builder.Float(p.name.AsView(), p.value.x);
                break;
            case shaders::ShaderPropertyType::Color:
                builder.Color(p.name.AsView(), p.value);
                break;
            case shaders::ShaderPropertyType::ColorHdr:
                builder.ColorHdr(p.name.AsView(), p.value);
                break;
            case shaders::ShaderPropertyType::Texture:
                builder.Texture(p.name.AsView());
                break;
            case shaders::ShaderPropertyType::Sampler:
                builder.Sampler(p.name.AsView());
                break;
            default:
                FAIL("surface_pbr declares a property kind this test does not lay out");
                break;
            }
        }
        builder.Effect(wind);
        m = builder.Build();
        m->SetDefaultColor(u8"BaseColor", color);
        m->SetDefaultFloat(u8"Metallic", metallic);
        m->SetDefaultFloat(u8"Roughness", roughness);
        m->SetDefaultFloat(u8"WindStrength", windStrength);
        m->SetDefaultFloat(u8"WindSpeed", 1.0f);
        m->SetDefaultFloat(u8"WindHeight", 0.5f);
        // Laid out as forward's: the values mean the same on both.
        REQUIRE(m->FindProperty(u8"EmissiveColor") != nullptr);
        CHECK(m->FindProperty(u8"EmissiveColor")->offset == 32u);
        CHECK(m->FindProperty(u8"AlphaCutoff")->offset == 56u);
        m->depthShaderName = String(kSurfaceName);
        m->depthShaderName += shaders::kDepthProgramSuffix;
        m->pickShaderName = String(kSurfaceName);
        m->pickShaderName += shaders::kPickProgramSuffix;
        return m;
    }

    // `cutOut` makes the ball a masked material whose alpha is under its cut-off everywhere, so
    // its depth and shadow draws run the surface's own pixel stage and cut the whole ball away.
    Array<u8> RenderScene(rhi::Device& device, const SurfacePrograms& cooked, bool surface,
                          bool cutOut = false)
    {
        Array<u8> pixels;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return pixels;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            for (const pipeline::ShaderCheckProgram& p : cooked.programs)
            {
                String name(kSurfaceName);
                name += p.suffix;
                shaderSystem.RegisterSource(name.AsView(), shaders::ShaderStage::Vertex,
                                            p.text.AsView(), p.vertexEntryPoint.AsView(),
                                            p.vertexMask);
                shaderSystem.RegisterSource(name.AsView(), shaders::ShaderStage::Fragment,
                                            p.text.AsView(), p.fragmentEntryPoint.AsView(),
                                            p.fragmentMask);
            }
            if (surface)
            {
                // The surface programs resolve: the draws below are the surface shader's, not a
                // fallback (a missing program would skip the draw, and the images would differ).
                CHECK(shaderSystem.GetVariant(kSurfaceName, shaders::ShaderStage::Fragment,
                                              shaders::ShaderFlags::GBuffer) != nullptr);
                CHECK(shaderSystem.GetVariant(u8"surface_pbr.depth", shaders::ShaderStage::Vertex,
                                              shaders::ShaderFlags::None) != nullptr);
                if (cutOut)
                {
                    // The cut-out's own depth pixel stage: what the shadow and the prepass run.
                    CHECK(shaderSystem.GetVariant(u8"surface_pbr.depth", shaders::ShaderStage::Fragment,
                                                  shaders::ShaderFlags::AlphaTest) != nullptr);
                }
            }
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache,
                                      materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            // The shadow maps: without them the frame records no shadow pass at all.
            ShadowSystem shadows(device, 2u);
            REQUIRE(shadows.Initialize().IsOk());
            RenderFrame frame(DefaultAllocator(), device, registry, 2, nullptr, nullptr, &shadows);

            RefPtr<geometry::StaticMesh> sphere = geometry::Primitives::Sphere(DefaultAllocator(), 1.0f, 48, 24);
            RefPtr<geometry::StaticMesh> ground = geometry::Primitives::Plane(DefaultAllocator(), 8.0f, 8.0f);
            const shaders::ShaderEffect* wind = shaderSystem.EffectForFlag(shaders::ShaderFlags::Wind);
            REQUIRE(wind != nullptr);
            RefPtr<materials::Material> ball = PbrMaterial(u8"ball", Float4{0.8f, 0.3f, 0.2f, 1.0f}, 0.2f,
                                                           0.4f, surface, cooked, *wind, 0.15f);
            RefPtr<materials::Material> floor = PbrMaterial(u8"floor", Float4{0.7f, 0.7f, 0.7f, 1.0f}, 0.0f,
                                                            0.8f, surface, cooked, *wind, 0.0f); // stays put
            ball->SetDefaultFloat4(u8"EmissiveColor", Float4{0.05f, 0.02f, 0.0f, 1.0f});
            if (cutOut)
            {
                ball->SetDefaultFloat4(u8"BaseColor", Float4{0.8f, 0.3f, 0.2f, 0.1f});
                ball->pipeline.blendMode = materials::BlendMode::Masked;
            }

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{0.15f, 0.15f, 0.18f});
            MeshRenderData* b = scene.Add<MeshRenderData>();
            b->world = Float4x4::Translation(Float3{0.0f, 1.3f, -6.0f});
            b->worldCenter = Float3{0.0f, 1.3f, -6.0f};
            b->worldRadius = 1.5f;
            b->mesh = sphere.Get();
            b->material = ball.Get();
            b->sortBatchKey = BatchKey(sphere.Get(), ball.Get()); // as extraction sets it
            b->category = cutOut ? RenderCategories::Masked : RenderCategories::Opaque;
            MeshRenderData* g = scene.Add<MeshRenderData>();
            g->world = Float4x4::Translation(Float3{0.0f, 0.0f, -6.0f});
            g->worldCenter = Float3{0.0f, 0.0f, -6.0f};
            g->worldRadius = 6.0f;
            g->mesh = ground.Get();
            g->material = floor.Get();
            g->sortBatchKey = BatchKey(ground.Get(), floor.Get());
            g->category = RenderCategories::Opaque;
            GpuLight sun;
            sun.type = 0.0f;
            sun.directionWS = Normalized(Float3{0.4f, -1.0f, -0.3f});
            sun.intensity = 3.0f;
            sun.shadowIndex = 0.0f;
            sun.shadowStrength = 1.0f;
            scene.AddLight(sun);
            DirectionalShadow ds;
            ds.direction = sun.directionWS;
            ds.valid = true;
            scene.SetDirectionalShadow(ds);

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(Float3{0, 2.5f, 0}, Float3{0, 0.8f, -6}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
            camera.farZ = 100.0f;

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"surface.target";
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(td, target).IsOk());
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, vd, targetView).IsOk());
            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
            REQUIRE(queue != nullptr);

            ViewSettings settings;
            settings.clear = rhi::ClearColor::Black();
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.taaEnabled = false;
            settings.post.bloomEnabled = false;
            for (u32 i = 0; i < 2; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.SetTime(1.25f); // mid-sway: the sphere's top has moved
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm,
                              kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder);
            }

            const testsupport::CapturedImage img = testsupport::Readback(device, target, kSize, kSize);
            REQUIRE(img.valid);
            pixels.Resize(kSize * kSize * 4u);
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    const usize at = (static_cast<usize>(y) * kSize + x) * 4u;
                    for (u32 c = 0; c < 4; ++c)
                    {
                        pixels[at + c] = p[c];
                    }
                }
            }
            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return pixels;
    }

    const SurfacePrograms* g_cooked = nullptr;

    void Probe(rhi::Device& device, const char* name)
    {
        INFO(doctest::String(name));
        const Array<u8> forward = RenderScene(device, *g_cooked, false);
        const Array<u8> surface = RenderScene(device, *g_cooked, true);
        REQUIRE(forward.Size() == kSize * kSize * 4u);
        REQUIRE(surface.Size() == forward.Size());

        u32 lit = 0;         // pixels the scene covers (not the black clear)
        u32 worst = 0;       // the largest channel difference
        u32 differing = 0;   // pixels off by more than rounding
        for (usize i = 0; i < forward.Size(); i += 4)
        {
            const u32 luma = static_cast<u32>(forward[i]) + forward[i + 1] + forward[i + 2];
            lit += luma > 15 ? 1u : 0u;
            u32 d = 0;
            for (u32 c = 0; c < 3; ++c)
            {
                const i32 diff = static_cast<i32>(forward[i + c]) - static_cast<i32>(surface[i + c]);
                d = Max(d, static_cast<u32>(diff < 0 ? -diff : diff));
            }
            worst = Max(worst, d);
            differing += d > 2u ? 1u : 0u;
        }
        MESSAGE(doctest::String(name), ": lit pixels ", lit, ", worst channel difference ", worst,
                ", pixels off by more than 2: ", differing);
        CHECK(lit > kSize * kSize / 4u);   // the scene fills the frame
        CHECK(worst <= 3u);                // rounding, not a different picture
        CHECK(differing <= kSize * kSize / 200u);
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
#ifdef OPTION_HAS_VULKAN
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        if (rhi::Device* device = testsupport::MakeTestDevice(vulkan))
        {
            probe(*device, "vulkan");
            device->Destroy();
        }
        else
        {
            MESSAGE("Vulkan unavailable - skipped");
        }
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
#endif
#ifdef OPTION_HAS_WEBGPU
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
        if (rhi::Device* device = testsupport::MakeTestDevice(webgpu))
        {
            probe(*device, "webgpu");
            device->Destroy();
        }
        else
        {
            MESSAGE("WebGPU unavailable - skipped");
        }
        if (webgpu != nullptr)
        {
            webgpu->Destroy();
        }
#endif
#ifdef OPTION_HAS_DX12
        rhi::Backend* dx12 = nullptr;
        (void)rhi::dx12::CreateDxBackend(rhi::dx12::DxBackendDesc{}, dx12);
        if (rhi::Device* device = testsupport::MakeTestDevice(dx12))
        {
            probe(*device, "dx12");
            device->Destroy();
        }
        else
        {
            MESSAGE("DX12 unavailable - skipped");
        }
        if (dx12 != nullptr)
        {
            dx12->Destroy();
        }
#endif
    }
}

TEST_CASE("surface shader: the built-in PBR as a surface renders as forward does, wind and shadow included")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    compiler->Destroy();
    const SurfacePrograms cooked = CookSurfacePbr();
    REQUIRE(cooked.ok);
    g_cooked = &cooked;
    ForEachBackend(&Probe);
    g_cooked = nullptr;
}

// A masked surface's depth and pick pixel stages run its Surface() at the view's camera and time,
// read from the shadow view's buffer: the layout must give that buffer to the pixel stage too.
// Vulkan with validation on, as the gallery's dissolve found it (a validation error, tolerated by
// the driver; WebGPU refuses such a pipeline). The cut ball is gone, and so is its shadow.
TEST_CASE("surface shader: a masked surface's shadow runs its cut-out, under validation")
{
#ifdef OPTION_HAS_VULKAN
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    compiler->Destroy();
    const SurfacePrograms cooked = CookSurfacePbr(true);
    REQUIRE(cooked.ok);
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{.enableValidation = true}, vulkan);
    if (rhi::Device* device = vulkan != nullptr ? testsupport::MakeTestDevice(vulkan) : nullptr)
    {
        const u32 errorsBefore = rhi::vk::ValidationErrorCount();
        const Array<u8> whole = RenderScene(*device, cooked, true);
        const Array<u8> cut = RenderScene(*device, cooked, true, true);
        device->Destroy();
        CHECK(rhi::vk::ValidationErrorCount() == errorsBefore);
        REQUIRE(whole.Size() == kSize * kSize * 4u);
        REQUIRE(cut.Size() == whole.Size());
        // The ball covers a good part of the frame; cut away, the ground shows there instead.
        u32 changed = 0;
        for (usize i = 0; i < whole.Size(); i += 4)
        {
            i32 d = 0;
            for (u32 c = 0; c < 3; ++c)
            {
                const i32 diff = static_cast<i32>(whole[i + c]) - static_cast<i32>(cut[i + c]);
                d = Max(d, diff < 0 ? -diff : diff);
            }
            changed += d > 8 ? 1u : 0u;
        }
        MESSAGE("pixels the cut changes: ", changed);
        CHECK(changed > kSize * kSize / 20u);
    }
    else
    {
        MESSAGE("Vulkan unavailable - skipped");
    }
    if (vulkan != nullptr)
    {
        vulkan->Destroy();
    }
#else
    MESSAGE("no Vulkan backend in this build - skipped");
#endif
}

