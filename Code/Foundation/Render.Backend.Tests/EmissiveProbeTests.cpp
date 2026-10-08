// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// EMISSIVE probe on REAL devices: in a scene with no light at all, a material whose EmissiveColor is
// set and that has no emissive texture glows its colour (glTF: emission = factor x texture, the
// texture white when absent), and one with the default black EmissiveColor stays black. Three cards:
// plain, glowing orange, glowing orange at twice the intensity. Vulkan + WebGPU.
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
    constexpr u32 kSize = 96;

    // A unit card in the XY plane, facing +Z (towards a camera looking down -Z).
    RefPtr<geometry::StaticMesh> Card()
    {
        RefPtr<geometry::StaticMesh> mesh = MakeRef<geometry::StaticMesh>(DefaultAllocator());
        const u32 white = 0xFFFFFFFFu;
        const Float3 n{0, 0, 1};
        const Float3 t{1, 0, 0};
        mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{-0.5f, -0.5f, 0}, n, Float2{0, 1}, white, t});
        mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{0.5f, -0.5f, 0}, n, Float2{1, 1}, white, t});
        mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{0.5f, 0.5f, 0}, n, Float2{1, 0}, white, t});
        mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{-0.5f, 0.5f, 0}, n, Float2{0, 0}, white, t});
        mesh->indices.Resize(6);
        mesh->indices.Add(0);
        mesh->indices.Add(1);
        mesh->indices.Add(2);
        mesh->indices.Add(0);
        mesh->indices.Add(2);
        mesh->indices.Add(3);
        mesh->GenerateTangents();
        mesh->bounds = AABB::FromCenterExtents(Float3{0, 0, 0}, Float3{0.5f, 0.5f, 0.01f});
        mesh->subMeshes.PushBack(geometry::SubMesh{0, 6, 0, geometry::PrimitiveType::Triangles});
        return mesh;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        INFO(doctest::String(name));
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            MESSAGE("shader system unavailable - skipped");
            return;
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
            RenderFrame frame(DefaultAllocator(), device, registry, 2);

            RefPtr<geometry::StaticMesh> card = Card();
            RefPtr<materials::Material> plain =
                materials::CreatePBR(u8"emissive.plain", Float4{0.6f, 0.6f, 0.6f, 1}, 0, 0.9f);
            RefPtr<materials::Material> glow =
                materials::CreatePBR(u8"emissive.glow", Float4{0.6f, 0.6f, 0.6f, 1}, 0, 0.9f);
            glow->SetDefaultColor(u8"EmissiveColor", Float4{1.0f, 0.5f, 0.2f, 0.5f}); // sRGB, intensity
            RefPtr<materials::Material> bright =
                materials::CreatePBR(u8"emissive.bright", Float4{0.6f, 0.6f, 0.6f, 1}, 0, 0.9f);
            bright->SetDefaultColor(u8"EmissiveColor", Float4{1.0f, 0.5f, 0.2f, 1.0f});

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{0.0f, 0.0f, 0.0f}); // no light: only what glows shows
            materials::Material* mats[] = {plain.Get(), glow.Get(), bright.Get()};
            for (i32 i = -1; i <= 1; ++i)
            {
                MeshRenderData* md = scene.Add<MeshRenderData>();
                REQUIRE(md != nullptr);
                md->world = Float4x4::Translation(Float3{1.15f * static_cast<f32>(i), 0.0f, -3.0f});
                md->mesh = card.Get();
                md->material = mats[i + 1];
                md->rendererId = meshRenderer.RendererId();
                md->category = RenderCategories::Opaque;
                md->sortBatchKey = BatchKey(card.Get(), mats[i + 1]);
                md->worldCenter = Float3{1.15f * static_cast<f32>(i), 0.0f, -3.0f};
                md->worldRadius = 1.0f;
                md->entityId = static_cast<u64>(i + 2);
            }

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
            camera.farZ = 100.0f;

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"emissive.target";
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
                settings.targetCurrentState = (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
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
            // Lit pixels per third: none for the plain card, the glowing ones orange.
            u32 lit[3] = {0, 0, 0};
            Float3 sum[3] = {};
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    if (static_cast<u32>(p[0]) + p[1] + p[2] <= 30)
                    {
                        continue;
                    }
                    const u32 c = Min(x * 3u / kSize, 2u);
                    ++lit[c];
                    sum[c] = sum[c] + Float3{static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2])};
                }
            }
            CHECK(lit[0] == 0u);  // a black EmissiveColor in the dark: nothing
            REQUIRE(lit[1] > 0u); // a glow authored as a colour alone shows
            REQUIRE(lit[2] > 0u);
            const Float3 glowMean = sum[1] * (1.0f / static_cast<f32>(lit[1]));
            const Float3 brightMean = sum[2] * (1.0f / static_cast<f32>(lit[2]));
            CHECK(glowMean.x > glowMean.z * 2.0f);   // its colour: orange
            CHECK(brightMean.x > glowMean.x);        // twice the intensity, brighter

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
    }
}

TEST_CASE("emissive probe: a colour alone glows without an emissive texture; black stays dark - Vulkan + WebGPU")
{
    rhi::Backend* vulkan = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
    rhi::Backend* webgpu = nullptr;
    (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
    if (rhi::Device* device = vulkan != nullptr ? testsupport::MakeTestDevice(vulkan) : nullptr)
    {
        ProbeBackend(*device, "vulkan");
        device->Destroy();
    }
    else
    {
        MESSAGE("Vulkan unavailable - vulkan emissive probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu emissive probe skipped");
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
