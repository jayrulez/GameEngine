// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// MATERIAL OVERRIDE probe on REAL devices: three cards share one grey material and the middle one
// overrides its BaseColor (MeshRenderData::overrides, a mesh's own material properties). The middle
// card turns red while its neighbours stay grey (the override is its own, not the material's);
// changing the override's value (its version) turns it green; clearing it brings it back to grey,
// like its neighbours. Over frames on one renderer, so the instance it keeps per mesh and slot is
// re-applied, not rebuilt. Vulkan + WebGPU.
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

    // The mean colour of each third of the image's lit pixels (left / middle / right card).
    struct Thirds
    {
        Float3 mean[3] = {};
        bool valid = false;
    };

    Thirds Measure(const testsupport::CapturedImage& img)
    {
        Thirds t;
        u32 count[3] = {0, 0, 0};
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
                t.mean[c] = t.mean[c] + Float3{static_cast<f32>(p[0]), static_cast<f32>(p[1]), static_cast<f32>(p[2])};
                ++count[c];
            }
        }
        for (u32 c = 0; c < 3; ++c)
        {
            if (count[c] > 0)
            {
                t.mean[c] = t.mean[c] * (1.0f / static_cast<f32>(count[c]));
            }
        }
        t.valid = count[0] > 0 && count[1] > 0 && count[2] > 0;
        return t;
    }

    // Grey: its channels within a few steps of each other.
    bool Grey(const Float3& c) { return Abs(c.x - c.y) < 8.0f && Abs(c.y - c.z) < 8.0f && c.x > 40.0f; }

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
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            RenderFrame frame(DefaultAllocator(), device, registry, 2);

            RefPtr<geometry::StaticMesh> card = Card();
            RefPtr<materials::Material> material =
                materials::CreatePBR(u8"override.card", Float4{0.6f, 0.6f, 0.6f, 1}, 0, 0.9f);
            Array<MaterialPropertyOverride> overrides;
            MaterialPropertyOverride tint;
            tint.slot = 0;
            tint.size = sizeof(Float4);
            tint.name = String(u8"BaseColor");
            overrides.PushBack(tint);

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            MeshRenderData* middle = nullptr;
            for (i32 i = -1; i <= 1; ++i)
            {
                MeshRenderData* md = scene.Add<MeshRenderData>();
                REQUIRE(md != nullptr);
                md->world = Float4x4::Translation(Float3{1.15f * static_cast<f32>(i), 0.0f, -3.0f});
                md->mesh = card.Get();
                md->material = material.Get();
                md->rendererId = meshRenderer.RendererId();
                md->category = RenderCategories::Opaque;
                md->sortBatchKey = BatchKey(card.Get(), material.Get()); // the three would batch
                md->worldCenter = Float3{1.15f * static_cast<f32>(i), 0.0f, -3.0f};
                md->worldRadius = 1.0f;
                md->entityId = static_cast<u64>(i + 2);
                if (i == 0)
                {
                    middle = md;
                }
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
            td.label = u8"override.target";
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
            u64 submitted = 0;
            // Two frames (both in-flight slots), then the image.
            const auto render = [&]()
            {
                for (u32 i = 0; i < 2; ++i)
                {
                    rhi::CommandEncoder* encoder = nullptr;
                    REQUIRE(pool->CreateEncoder(encoder).IsOk());
                    settings.targetCurrentState =
                        (submitted == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                    frame.Begin(*encoder, static_cast<u32>(submitted % 2));
                    frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
                    frame.End();
                    rhi::CommandBuffer* commandBuffer = encoder->Finish();
                    REQUIRE(commandBuffer != nullptr);
                    rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                    ++submitted;
                    queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, submitted);
                    REQUIRE(fence->Wait(submitted, ~0ull));
                    pool->DestroyEncoder(encoder);
                }
                const testsupport::CapturedImage img = testsupport::Readback(device, target, kSize, kSize);
                REQUIRE(img.valid);
                return Measure(img);
            };

            // Red, for the middle card alone.
            overrides[0].value = Float4{0.9f, 0.05f, 0.05f, 1.0f};
            middle->overrides = overrides.Data();
            middle->overrideCount = 1;
            middle->overrideVersion = 1;
            const Thirds red = render();
            REQUIRE(red.valid);
            CHECK(Grey(red.mean[0]));
            CHECK(Grey(red.mean[2]));
            CHECK(red.mean[1].x > red.mean[1].y * 3.0f); // red, not grey
            // A new value (a new version): green.
            overrides[0].value = Float4{0.05f, 0.9f, 0.05f, 1.0f};
            middle->overrideVersion = 2;
            const Thirds green = render();
            REQUIRE(green.valid);
            CHECK(green.mean[1].y > green.mean[1].x * 3.0f);
            CHECK(Grey(green.mean[0]));
            // Cleared: grey, as its neighbours.
            middle->overrides = nullptr;
            middle->overrideCount = 0;
            middle->overrideVersion = 3;
            const Thirds cleared = render();
            REQUIRE(cleared.valid);
            CHECK(Grey(cleared.mean[1]));
            CHECK(Abs(cleared.mean[1].x - cleared.mean[0].x) < 8.0f);

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
    }
}

TEST_CASE("material override probe: one mesh's own property changes it alone, follows its version, clears - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan material override probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu material override probe skipped");
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
