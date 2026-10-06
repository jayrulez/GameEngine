// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Screen-space reflections and GI, proven at the pixel on real Vulkan and WebGPU devices through the
// full RenderFrame chain (structural assertions, no golden images).
//  - SSR reach: a mirror floor seen from low above it reflects the WHOLE of a tall pillar twenty
//    metres away, down to where it stands. A ray length tied to the pixel's own depth cut the
//    reflection off near the pillar's base (the reflected ray climbs only as steeply as the view
//    ray came down).
// Cooked-pack WGSL path too (the browser's shaders, on wgpu-native):
//   OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 ./Render.Backend.Tests
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
import foundation.rhi.testsupport;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    constexpr u32 kSize = 128;

    struct Item
    {
        RefPtr<geometry::StaticMesh> mesh;
        RefPtr<materials::Material> material;
        Float4x4 world = Float4x4::Identity();
        Float3 center = Float3{0, 0, 0};
        f32 radius = 1.0f;
    };

    struct ProbeScene
    {
        Array<Item> items;
        ViewCamera camera;
        Float3 ambient = Float3{1.0f, 1.0f, 1.0f};
        bool ssr = false;
    };

    // Render `spec` through the frame chain (forward + tonemap, plus SSR when asked; no TAA, no
    // bloom) and read the LDR pixels back.
    testsupport::CapturedImage Render(rhi::Device& device, const ProbeScene& spec)
    {
        testsupport::CapturedImage out;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return out;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);

            TonemapPass tonemap(device, shaderSystem, 2);
            REQUIRE(tonemap.Initialize().IsOk());
            SsrPass ssr(device, shaderSystem);
            REQUIRE(ssr.Initialize().IsOk());

            RenderFrame frame(DefaultAllocator(), device, registry, 2, /*clusters*/ nullptr, &tonemap,
                              /*shadows*/ nullptr, /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              /*taa*/ nullptr, /*ao*/ nullptr, /*fxaa*/ nullptr);
            if (spec.ssr)
            {
                SsrPass::Params params;
                params.temporal = false; // one frame's trace, not an accumulation still settling
                frame.SetSsr(&ssr);
                frame.SetSsrParams(true, params);
            }

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(spec.ambient);
            for (const Item& item : spec.items)
            {
                MeshRenderData* data = scene.Add<MeshRenderData>();
                data->world = item.world;
                data->worldCenter = item.center;
                data->worldRadius = item.radius;
                data->mesh = item.mesh.Get();
                data->material = item.material.Get();
                data->category = RenderCategories::Opaque;
            }

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"screenspace.probe.target";
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
            settings.post.bloomEnabled = false;
            settings.post.taaEnabled = false;
            settings.post.ssrEnabled = spec.ssr;

            for (u32 i = 0; i < 2; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, spec.camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm,
                              kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder);
            }

            out = testsupport::Readback(device, target, kSize, kSize);

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return out;
    }

    // The pixel column a world point lands on (x only: the y convention differs per backend, which
    // the orientation probe owns).
    u32 PixelX(const ViewCamera& camera, Float3 world)
    {
        const Float4 clip = Float4{world.x, world.y, world.z, 1.0f} * (camera.view * camera.projection);
        const f32 ndcX = clip.x / clip.w;
        return static_cast<u32>(Clamp((ndcX * 0.5f + 0.5f) * static_cast<f32>(kSize), 0.0f,
                                      static_cast<f32>(kSize - 1)));
    }

    bool Reddish(const u8* p) { return p[0] > 60 && p[0] > p[1] * 2 && p[0] > p[2] * 2; }

    // Red pixels down one column.
    u32 RedInColumn(const testsupport::CapturedImage& image, u32 x)
    {
        u32 count = 0;
        for (u32 y = 0; y < image.height; ++y)
        {
            count += Reddish(image.At(x, y)) ? 1u : 0u;
        }
        return count;
    }

    // A mirror floor, a camera half a metre above it looking along it, and a red pillar 8 m tall
    // twenty metres away. The floor is dark, so what reads red below the pillar is its reflection.
    ProbeScene PillarOverMirror(bool ssr)
    {
        ProbeScene spec;
        spec.ssr = ssr;
        Item floor;
        floor.mesh = geometry::Primitives::Plane(DefaultAllocator(), 200.0f, 200.0f);
        floor.material = materials::CreatePBR(u8"probe.mirror", Float4{0.02f, 0.02f, 0.02f, 1.0f}, 0.0f, 0.02f);
        floor.radius = 150.0f;
        spec.items.PushBack(floor);
        Item pillar;
        pillar.mesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        pillar.material = materials::CreatePBR(u8"probe.pillar", Float4{1.0f, 0.05f, 0.05f, 1.0f}, 0.0f, 0.9f);
        pillar.world = Float4x4::Scale(Float3{2.0f, 8.0f, 2.0f}) * Float4x4::Translation(Float3{0.0f, 4.0f, -20.0f});
        pillar.center = Float3{0.0f, 4.0f, -20.0f};
        pillar.radius = 5.0f;
        spec.items.PushBack(pillar);
        spec.camera.view = Float4x4::LookAtRH(Float3{0, 0.5f, 0}, Float3{0, 0.5f, -1}, Float3{0, 1, 0});
        spec.camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 500.0f);
        spec.camera.position = Float3{0, 0.5f, 0};
        return spec;
    }

    void ProbeSsrReach(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        const testsupport::CapturedImage off = Render(device, PillarOverMirror(false));
        const testsupport::CapturedImage on = Render(device, PillarOverMirror(true));
        REQUIRE(off.valid);
        REQUIRE(on.valid);
        const u32 x = PixelX(PillarOverMirror(false).camera, Float3{0.0f, 4.0f, -20.0f});
        const u32 pillar = RedInColumn(off, x);
        const u32 withReflection = RedInColumn(on, x);
        MESSAGE(doctest::String(backend) << ": red in the pillar's column, SSR off " << pillar << ", on "
                                         << withReflection);
        REQUIRE(pillar > 10u); // the pillar itself is on screen
        // The reflection mirrors the pillar about the floor: as tall as the pillar on screen, so
        // the column holds about twice as much red (less the last rows Fresnel fades). A ray cut at
        // the pixel's depth added none; a fixed hit band let the far reflection break into a dither;
        // the start pixel read as a hit left a gap where the reflection meets the pillar.
        CHECK(withReflection >= pillar * 18 / 10);
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
        bool any = false;
        if (rhi::Device* device = testsupport::MakeTestDevice(vulkan))
        {
            probe(*device, "vulkan");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("Vulkan unavailable - skipped");
        }
        if (rhi::Device* device = testsupport::MakeTestDevice(webgpu))
        {
            probe(*device, "webgpu");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("WebGPU unavailable - skipped");
        }
        if (!any)
        {
            MESSAGE("no GPU backend available - screen-space probe skipped entirely");
        }
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
        if (webgpu != nullptr)
        {
            webgpu->Destroy();
        }
    }
}

TEST_CASE("ssr: a mirror floor seen from low above it reflects the whole of a tall pillar")
{
    ForEachBackend(&ProbeSsrReach);
}
