// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// INSTANCE FADE probe on REAL devices: three cards of one instanced set at 3, 6 and 12 m under a
// 4..8 m window. The near one (density 1) draws; the middle one sits at density 0.5 with rank 0.5
// and collapses; the far one is past the window. With no window all three draw. The dissolve is
// per instance, at the instance's own distance - the seam-free replacement for the per-chunk
// fade (instance_fade.hlsli). Vulkan + WebGPU.
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

    Lit RenderCards(rhi::Device& device, f32 fadeStart, f32 fadeEnd)
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
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            RenderFrame frame(DefaultAllocator(), device, registry, 2);

            RefPtr<geometry::StaticMesh> card = Card();
            RefPtr<materials::Material> material =
                materials::CreatePBR(u8"fade.card", Float4{0.9f, 0.9f, 0.9f, 1}, 0, 0.9f);
            // Instance order = rank order: the near card ranks 1/6, the middle 1/2, the far 5/6.
            const Float4x4 transforms[3] = {
                Float4x4::Translation(Float3{-1.6f, 0.0f, -3.0f}),
                Float4x4::Translation(Float3{0.0f, 0.0f, -6.0f}),
                Float4x4::Translation(Float3{3.2f, 0.0f, -12.0f}),
            };
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            MultiMeshRenderData* set = scene.Add<MultiMeshRenderData>();
            REQUIRE(set != nullptr);
            set->multiMesh = true;
            set->key = 0x7ade;
            set->transforms = transforms;
            set->instanceCount = 3;
            set->uploadCount = 3;
            set->version = 1;
            set->mesh = card.Get();
            set->material = material.Get();
            set->rendererId = meshRenderer.RendererId();
            set->category = RenderCategories::Opaque;
            set->sortBatchKey = BatchKey(card.Get(), material.Get()); // every producer stamps it at extraction
            set->worldCenter = Float3{0, 0, -7};
            set->worldRadius = 10.0f;
            set->fadeStart = fadeStart;
            set->fadeEnd = fadeEnd;

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
            td.label = u8"fade.target";
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
        const Lit all = RenderCards(device, 0.0f, 0.0f); // no window: every card draws
        REQUIRE(all.valid);
        CHECK(all.columns[0] > 0u);
        CHECK(all.columns[1] > 0u);
        CHECK(all.columns[2] > 0u);
        const Lit faded = RenderCards(device, 4.0f, 8.0f);
        REQUIRE(faded.valid);
        CHECK(faded.columns[0] == all.columns[0]); // inside the window: untouched
        CHECK(faded.columns[1] == 0u);             // density 0.5 at rank 0.5: collapsed
        CHECK(faded.columns[2] == 0u);             // past the window
    }
}

TEST_CASE("instance fade probe: a set's cards dissolve by their own distance and rank; no window draws all - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan fade probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu fade probe skipped");
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
