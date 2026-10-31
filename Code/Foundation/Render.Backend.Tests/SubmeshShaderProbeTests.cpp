// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Submesh shader probe on real devices: a card of two submeshes, its left half's slot a PBR
// material and its right half's slot the `unlit` shader in green, in a scene with no light. Each
// slot draws with its own material's program, so the left half stays dark (PBR, unlit by
// anything) and the right half shows its green. Drawing every slot with the first slot's
// program, over the other slot's bind group, would leave the right half dark or missing.
// Drawn alone, and as an instanced set (a crowd's per-slot materials). Vulkan and WebGPU.
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

    // A unit card in the XY plane facing +Z, its left half submesh 0 (slot 0) and its right half
    // submesh 1 (slot 1).
    RefPtr<geometry::StaticMesh> SplitCard()
    {
        RefPtr<geometry::StaticMesh> mesh = MakeRef<geometry::StaticMesh>(DefaultAllocator());
        const u32 white = 0xFFFFFFFFu;
        const Float3 n{0, 0, 1};
        const Float3 t{1, 0, 0};
        for (i32 half = 0; half < 2; ++half)
        {
            const f32 x0 = half == 0 ? -0.5f : 0.0f;
            const f32 x1 = x0 + 0.5f;
            mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{x0, -0.5f, 0}, n, Float2{0, 1}, white, t});
            mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{x1, -0.5f, 0}, n, Float2{1, 1}, white, t});
            mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{x1, 0.5f, 0}, n, Float2{1, 0}, white, t});
            mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{x0, 0.5f, 0}, n, Float2{0, 0}, white, t});
        }
        mesh->indices.Resize(12);
        const u32 quad[] = {0, 1, 2, 0, 2, 3};
        for (u32 half = 0; half < 2; ++half)
        {
            for (u32 i : quad)
            {
                mesh->indices.Add(half * 4u + i);
            }
        }
        mesh->GenerateTangents();
        mesh->bounds = AABB::FromCenterExtents(Float3{0, 0, 0}, Float3{0.5f, 0.5f, 0.01f});
        mesh->subMeshes.PushBack(geometry::SubMesh{0, 6, 0, geometry::PrimitiveType::Triangles});
        mesh->subMeshes.PushBack(geometry::SubMesh{6, 6, 1, geometry::PrimitiveType::Triangles});
        return mesh;
    }

    struct Halves
    {
        bool valid = false;
        u32 greenLeft = 0;  // green pixels left of centre
        u32 greenRight = 0; // and right of it
    };

    Halves Render(rhi::Device& device, RenderFrame& frame, ExtractedScene& scene)
    {
        ViewCamera camera;
        camera.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
        camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        camera.farZ = 100.0f;

        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = kSize;
        td.height = kSize;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        td.label = u8"submesh.target";
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
        Halves out;
        out.valid = img.valid;
        if (img.valid)
        {
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    if (p[1] > 80 && p[1] > p[0] + 40 && p[1] > p[2] + 40)
                    {
                        ++(x < kSize / 2 ? out.greenLeft : out.greenRight);
                    }
                }
            }
        }
        device.WaitIdle();
        device.DestroyFence(fence);
        device.DestroyCommandPool(pool);
        device.DestroyTextureView(targetView);
        device.DestroyTexture(target);
        return out;
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
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            RenderFrame frame(DefaultAllocator(), device, registry, 2);

            RefPtr<geometry::StaticMesh> card = SplitCard();
            RefPtr<materials::Material> lit =
                materials::CreatePBR(u8"submesh.lit", Float4{0.8f, 0.8f, 0.8f, 1}, 0, 0.9f);
            RefPtr<materials::Material> green = materials::CreateUnlit(u8"submesh.green", Float4{0.1f, 0.9f, 0.1f, 1});
            const RefPtr<materials::Material> slots[2] = {lit, green};

            // Drawn alone.
            {
                ExtractedScene scene{DefaultAllocator()};
                scene.SetAmbient(Float3{0.0f, 0.0f, 0.0f}); // no light: PBR is dark, unlit is its colour
                MeshRenderData* md = scene.Add<MeshRenderData>();
                REQUIRE(md != nullptr);
                md->world = Float4x4::Translation(Float3{0.0f, 0.0f, -1.5f});
                md->mesh = card.Get();
                md->material = lit.Get();
                md->submeshMaterials = slots;
                md->submeshMaterialCount = 2;
                md->rendererId = meshRenderer.RendererId();
                md->category = RenderCategories::Opaque;
                md->sortBatchKey = BatchKey(card.Get(), lit.Get());
                md->worldCenter = Float3{0.0f, 0.0f, -1.5f};
                md->worldRadius = 1.0f;
                md->entityId = 2;
                const Halves halves = Render(device, frame, scene);
                REQUIRE(halves.valid);
                INFO("alone: green left ", halves.greenLeft, ", right ", halves.greenRight);
                CHECK(halves.greenRight > (kSize * kSize) / 8u); // the right half is its own green
                CHECK(halves.greenLeft == 0u);                   // the left half is PBR, unlit
            }

            // As an instanced set of one.
            {
                const Float4x4 transforms[1] = {Float4x4::Translation(Float3{0.0f, 0.0f, -1.5f})};
                ExtractedScene scene{DefaultAllocator()};
                scene.SetAmbient(Float3{0.0f, 0.0f, 0.0f});
                MultiMeshRenderData* set = scene.Add<MultiMeshRenderData>();
                REQUIRE(set != nullptr);
                set->multiMesh = true;
                set->key = 0x5ab5;
                set->transforms = transforms;
                set->instanceCount = 1;
                set->uploadCount = 1;
                set->version = 1;
                set->mesh = card.Get();
                set->material = lit.Get();
                set->submeshMaterials = slots;
                set->submeshMaterialCount = 2;
                set->rendererId = meshRenderer.RendererId();
                set->category = RenderCategories::Opaque;
                set->sortBatchKey = BatchKey(card.Get(), lit.Get());
                set->worldCenter = Float3{0.0f, 0.0f, -1.5f};
                set->worldRadius = 1.0f;
                const Halves halves = Render(device, frame, scene);
                REQUIRE(halves.valid);
                INFO("instanced: green left ", halves.greenLeft, ", right ", halves.greenRight);
                CHECK(halves.greenRight > (kSize * kSize) / 8u);
                CHECK(halves.greenLeft == 0u);
            }
        }
        host.Shutdown();
    }
}

TEST_CASE("submesh shader probe: each slot draws with its own material's program, alone and instanced - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan submesh shader probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu submesh shader probe skipped");
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
