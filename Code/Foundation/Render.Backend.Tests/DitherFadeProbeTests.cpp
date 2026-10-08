// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// DITHER FADE probe on REAL devices: a mesh's fade (MeshRenderData::fade, the cutaway's knob)
// thins its pixels by a screen-door pattern. Three cards side by side: solid, faded 0.5 and faded
// out, with a solid card behind the faded-out one. The solid card is untouched, the half-faded one
// keeps about half its pixels, and the faded-out one draws none and hides none of the card behind
// it, so it stayed out of the depth prepass too. The two faded cards share one instanced draw,
// each with its own fade; drawn apart (each its own material), each is a lone draw by the single
// path, which thins them the same. Vulkan + WebGPU.
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

    struct Lit
    {
        bool valid = false;
        u32 columns[3] = {0, 0, 0}; // lit pixels in the left / middle / right third
    };

    // The cards, at 3 m: solid on the left, `middleFade` in the middle, `rightFade` on the right
    // (none when negative), and a wider solid card 6 m away behind the right one. `apart`: the
    // middle and right cards each have a material of their own, so neither batches.
    Lit RenderCards(rhi::Device& device, f32 middleFade, f32 rightFade, bool apart = false)
    {
        Lit lit;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return lit;
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
            RefPtr<materials::Material> material =
                materials::CreatePBR(u8"dither.card", Float4{0.9f, 0.9f, 0.9f, 1}, 0, 0.9f);
            RefPtr<materials::Material> middleMaterial =
                materials::CreatePBR(u8"dither.middle", Float4{0.9f, 0.9f, 0.9f, 1}, 0, 0.9f);
            RefPtr<materials::Material> rightMaterial =
                materials::CreatePBR(u8"dither.right", Float4{0.9f, 0.9f, 0.9f, 1}, 0, 0.9f);
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            u32 nextEntity = 1;
            const auto add = [&](const Float3& at, f32 scale, f32 fade, materials::Material* mat)
            {
                MeshRenderData* md = scene.Add<MeshRenderData>();
                REQUIRE(md != nullptr);
                md->world = Float4x4::Scale(Float3{scale, scale, scale});
                md->world.m[3][0] = at.x; // the translation row (row vectors: scale, then move)
                md->world.m[3][1] = at.y;
                md->world.m[3][2] = at.z;
                md->mesh = card.Get();
                md->material = mat;
                md->fade = fade;
                md->rendererId = meshRenderer.RendererId();
                // As extraction routes it: a faded opaque mesh draws Masked (no prepass).
                md->category = fade > 0.0f ? RenderCategories::Masked : RenderCategories::Opaque;
                md->sortBatchKey = BatchKey(card.Get(), mat);
                md->worldCenter = at;
                md->worldRadius = scale;
                md->entityId = nextEntity++;
            };
            add(Float3{-1.15f, 0.0f, -3.0f}, 1.0f, 0.0f, material.Get());
            add(Float3{0.0f, 0.0f, -3.0f}, 1.0f, middleFade, apart ? middleMaterial.Get() : material.Get());
            if (rightFade >= 0.0f)
            {
                add(Float3{1.15f, 0.0f, -3.0f}, 1.0f, rightFade, apart ? rightMaterial.Get() : material.Get());
            }
            add(Float3{2.3f, 0.0f, -6.0f}, 1.6f, 0.0f, material.Get());

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
            camera.farZ = 100.0f;
            camera.position = Float3{0, 0, 0};

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"dither.target";
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
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    const u32 luma = static_cast<u32>(p[0]) + p[1] + p[2];
                    if (luma > 30)
                    {
                        ++lit.columns[Min(x * 3u / kSize, 2u)];
                    }
                }
            }
            lit.valid = true;
            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return lit;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        INFO(doctest::String(name));
        const Lit solid = RenderCards(device, 0.0f, -1.0f); // every card solid, none in front on the right
        REQUIRE(solid.valid);
        REQUIRE(solid.columns[0] > 0u);
        REQUIRE(solid.columns[1] > 0u);
        REQUIRE(solid.columns[2] > 0u);
        const Lit faded = RenderCards(device, 0.5f, 1.0f);
        REQUIRE(faded.valid);
        CHECK(faded.columns[0] == solid.columns[0]); // the solid card: untouched
        // Half faded: half of every 4x4 cell, so very nearly half the card.
        CHECK(faded.columns[1] * 10u >= solid.columns[1] * 4u);
        CHECK(faded.columns[1] * 10u <= solid.columns[1] * 6u);
        // Faded out: none of its pixels, and the card behind shows whole (no prepass depth).
        CHECK(faded.columns[2] == solid.columns[2]);
        // Each a lone draw (the single path): thinned exactly as in the shared instanced draw.
        const Lit apart = RenderCards(device, 0.5f, 1.0f, /*apart*/ true);
        REQUIRE(apart.valid);
        CHECK(apart.columns[0] == faded.columns[0]);
        CHECK(apart.columns[1] == faded.columns[1]);
        CHECK(apart.columns[2] == faded.columns[2]);
    }
}

TEST_CASE("dither fade probe: a faded mesh thins by its fade and hides nothing when faded out - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan dither fade probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu dither fade probe skipped");
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
