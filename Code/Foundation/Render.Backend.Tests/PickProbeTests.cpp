// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GPU picking proven at the texel on real Vulkan and WebGPU devices, through the FULL RenderFrame
// chain (the PickSystem's cropped id pass + graph copy + ring-retired readback):
//  - a near cube over a far cube at the centre: the near one answers (its own depth test), by
//    entity index AND generation;
//  - three identical cubes side by side (one instanced run): each column answers its own id -
//    the instance-stepped ids are per instance, not per run;
//  - a pixel where nothing is drawn answers with no hits (the clear is "nothing");
//  - a rect over the whole view answers every entity once (unique hits);
//  - a second request on the same view in the same frame is answered independently.
// Run it on the cooked-pack WGSL path too: OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1.
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
        u32 entityIndex = 0;
        u32 generation = 0;
    };

    struct Probe
    {
        PickRect rect;
        PickRequestId id = kInvalidPickRequest;
        PickResult result;
        bool answered = false;
    };

    Item Cube(f32 size, Float3 at, RefPtr<materials::Material> material, u32 index, u32 gen)
    {
        Item item;
        item.mesh = geometry::Primitives::Cube(DefaultAllocator(), size);
        item.material = Move(material);
        item.world = Float4x4::Translation(at);
        item.center = at;
        item.radius = size;
        item.entityIndex = index;
        item.generation = gen;
        return item;
    }

    ViewCamera Camera()
    {
        ViewCamera camera;
        camera.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
        camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        camera.position = Float3{0, 0, 0};
        camera.farZ = 100.0f;
        return camera;
    }

    // The view pixel (y down) a world point lands on.
    PickRect PixelRect(const ViewCamera& camera, Float3 world)
    {
        const Float4 clip = Float4{world.x, world.y, world.z, 1.0f} * camera.ViewProjection();
        const f32 ndcX = clip.x / clip.w;
        const f32 ndcY = clip.y / clip.w;
        const f32 px = Clamp((ndcX * 0.5f + 0.5f) * static_cast<f32>(kSize), 0.0f,
                             static_cast<f32>(kSize - 1));
        const f32 py = Clamp((0.5f - ndcY * 0.5f) * static_cast<f32>(kSize), 0.0f,
                             static_cast<f32>(kSize - 1));
        return PickRect{static_cast<i32>(px), static_cast<i32>(py), 1, 1};
    }

    // Render the scene for `frames` frames with the probes requested before the first, and
    // collect each probe's answer as it lands (the ring retire: request frame + 2).
    void RenderAndPick(rhi::Device& device, const Array<Item>& items, const ViewCamera& camera,
                       Array<Probe>& probes)
    {
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem,
                                      /*framesInFlight*/ 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);

            // Declared BEFORE the frame: destroyed after it (readback buffers outlive the graph).
            PickSystem pick(DefaultAllocator(), device, /*framesInFlight*/ 2);

            RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2);
            frame.SetPick(&pick);

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            for (const Item& item : items)
            {
                MeshRenderData* data = scene.Add<MeshRenderData>();
                data->world = item.world;
                data->worldCenter = item.center;
                data->worldRadius = item.radius;
                data->mesh = item.mesh.Get();
                data->material = item.material.Get();
                data->category = RenderCategories::Opaque;
                data->entityId = EntityTag::Pack(item.entityIndex, item.generation);
            }

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"pick.target";
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

            int viewportKey = 0;
            ViewSettings settings;
            settings.clear = rhi::ClearColor::Black();
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.taaEnabled = false;
            settings.post.bloomEnabled = false;
            settings.viewportKey = &viewportKey;

            for (Probe& probe : probes)
            {
                probe.id = pick.Request(&viewportKey, probe.rect);
                REQUIRE(probe.id != kInvalidPickRequest);
            }

            // Frame 0 declares the passes; frame 1 is the other ring slot; frame 2's Begin
            // retires them. One spare frame proves nothing answers twice.
            for (u32 i = 0; i < 4; ++i)
            {
                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState =
                    (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.Begin(*encoder, i % 2);
                for (Probe& probe : probes)
                {
                    if (!probe.answered && pick.TryTakeResult(probe.id, probe.result))
                    {
                        probe.answered = true;
                    }
                }
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

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
    }

    bool HitIs(const PickResult& r, u32 index, u32 gen)
    {
        return r.hits.Size() == 1 && r.hits[0].entityIndex == index && r.hits[0].generation == gen;
    }

    bool Contains(const PickResult& r, u32 index, u32 gen)
    {
        for (const PickHit& h : r.hits)
        {
            if (h.entityIndex == index && h.generation == gen)
            {
                return true;
            }
        }
        return false;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        // Near red cube (entity 7 gen 3) over a far big cube (entity 200 gen 1) at the centre;
        // three identical grey cubes (one instanced run: same mesh + material) in a row below.
        RefPtr<materials::Material> red = materials::CreatePBR(u8"pick.red", Float4{1, 0, 0, 1}, 0, 0.9f);
        RefPtr<materials::Material> blue = materials::CreatePBR(u8"pick.blue", Float4{0, 0, 1, 1}, 0, 0.9f);
        RefPtr<materials::Material> grey = materials::CreatePBR(u8"pick.grey", Float4{0.5f, 0.5f, 0.5f, 1}, 0, 0.9f);
        RefPtr<geometry::StaticMesh> rowMesh = geometry::Primitives::Cube(DefaultAllocator(), 1.2f);

        Array<Item> items;
        items.PushBack(Cube(0.6f, Float3{0, 0, -3.0f}, red, 7, 3));
        items.PushBack(Cube(3.0f, Float3{0, 0, -8.0f}, blue, 200, 1));
        const Float3 rowAt[3] = {Float3{-2.4f, -3.2f, -7.0f}, Float3{0.0f, -3.2f, -7.0f},
                                 Float3{2.4f, -3.2f, -7.0f}};
        for (u32 k = 0; k < 3; ++k)
        {
            Item cube;
            cube.mesh = rowMesh;
            cube.material = grey;
            cube.world = Float4x4::Translation(rowAt[k]);
            cube.center = rowAt[k];
            cube.radius = 1.2f;
            cube.entityIndex = 30 + k;
            cube.generation = 2;
            items.PushBack(cube);
        }
        const ViewCamera camera = Camera();

        Array<Probe> probes;
        const auto add = [&probes](PickRect rect)
        {
            Probe probe;
            probe.rect = rect;
            probes.PushBack(Move(probe));
        };
        add(PickRect{kSize / 2, kSize / 2, 1, 1});      // 0: centre
        add(PickRect{kSize / 2, kSize / 2 - 20, 1, 1}); // 1: far cube only
        add(PixelRect(camera, rowAt[0]));               // 2: row left
        add(PixelRect(camera, rowAt[1]));               // 3: row middle
        add(PixelRect(camera, rowAt[2]));               // 4: row right
        add(PickRect{1, 1, 1, 1});                      // 5: nothing
        add(PickRect{0, 0, kSize, kSize});              // 6: everything
        add(PickRect{kSize / 2, kSize / 2, 1, 1});      // 7: centre again

        RenderAndPick(device, items, camera, probes);

        for (usize i = 0; i < probes.Size(); ++i)
        {
            INFO(doctest::String(name) << " probe " << i);
            REQUIRE(probes[i].answered);
            CHECK(probes[i].result.rendered);
        }
        INFO(doctest::String(name));
        CHECK(HitIs(probes[0].result, 7, 3));    // the NEAR cube, with its generation
        CHECK(HitIs(probes[1].result, 200, 1));  // the far cube shows around the near one
        CHECK(HitIs(probes[2].result, 30, 2));   // each instance of the run answers itself
        CHECK(HitIs(probes[3].result, 31, 2));
        CHECK(HitIs(probes[4].result, 32, 2));
        CHECK(probes[5].result.hits.IsEmpty()); // a corner nothing reached: no entity
        CHECK(probes[6].result.hits.Size() == 5); // every entity once
        CHECK(Contains(probes[6].result, 7, 3));
        CHECK(Contains(probes[6].result, 200, 1));
        CHECK(Contains(probes[6].result, 30, 2));
        CHECK(Contains(probes[6].result, 31, 2));
        CHECK(Contains(probes[6].result, 32, 2));
        CHECK(HitIs(probes[7].result, 7, 3));    // two requests, one frame, both answered
    }
}

TEST_CASE("gpu-pick: entity ids under a pixel and a rect, near wins, instances distinct - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan pick probes skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu pick probes skipped");
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
