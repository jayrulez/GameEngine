// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A view drawn at a scene size of its own and scaled into a rectangle of a target of another
// shape (Sedulous ebbf7a3b): a fixed render resolution letterboxed into a wider window. The whole
// chain runs at the scene size and one present pass scales the image into the rectangle,
// clearing the rest of the target black.
//
// The orientation probe's scene (a bright cube above, a dim plane below) at 32 x 32, into the
// middle 64 x 64 of a 128 x 64 target, on a real device: the bars stay black, the rectangle
// holds the scene, and the present keeps it upright.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
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
    constexpr u32 kTargetWidth = 128;
    constexpr u32 kTargetHeight = 64;
    constexpr u32 kScene = 32;
    // The letterbox: the square scene fits the target's height, centred.
    constexpr i32 kRectX = 32;
    constexpr u32 kRectSize = 64;
}

TEST_CASE("scaled-view: a view drawn at its own size fills its rectangle upright, the bars black")
{
    rhi::Backend* backend = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, backend);
    rhi::Device* device = backend != nullptr ? testsupport::MakeTestDevice(backend) : nullptr;
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the scaled view probe skipped");
        if (backend != nullptr)
        {
            backend->Destroy(); // backends self-free, created even when no device came up
        }
        return;
    }
    {
        shaders::ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(*device, testsupport::DataFileSystem()));
        shaders::ShaderSystem& shaderSystem = *host.System();
        materials::PipelineStateCache psoCache(shaderSystem, *device);
        materials::MaterialSystem materialSystem;
        REQUIRE(materialSystem.Initialize(*device).IsOk());
        MeshRenderer meshRenderer(DefaultAllocator(), *device, shaderSystem, psoCache, materialSystem, /*framesInFlight*/ 2);
        REQUIRE(meshRenderer.Initialize().IsOk());
        RendererRegistry registry;
        registry.Register(&meshRenderer);
        TonemapPass tonemap(*device, shaderSystem, /*framesInFlight*/ 2);
        REQUIRE(tonemap.Initialize().IsOk());
        RenderFrame frame(DefaultAllocator(), *device, registry, /*framesInFlight*/ 2, nullptr, &tonemap,
                          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

        RefPtr<geometry::StaticMesh> cubeMesh = geometry::Primitives::Cube(DefaultAllocator(), 1.6f);
        RefPtr<geometry::StaticMesh> planeMesh = geometry::Primitives::Plane(DefaultAllocator(), 24.0f, 24.0f);
        RefPtr<materials::Material> cubeMat = materials::CreatePBR(u8"probe.cube", Float4{1, 1, 1, 1}, 0.0f, 0.6f);
        RefPtr<materials::Material> planeMat =
            // 18% grey (linear 0.18), written as the sRGB colour it is entered as.
            materials::CreatePBR(u8"probe.plane", Float4{0.461f, 0.461f, 0.461f, 1}, 0.0f, 0.8f);
        ExtractedScene scene{DefaultAllocator()};
        scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
        MeshRenderData* cube = scene.Add<MeshRenderData>();
        cube->world = Float4x4::Translation(Float3{0.0f, 2.2f, 0.0f});
        cube->worldCenter = Float3{0.0f, 2.2f, 0.0f};
        cube->mesh = cubeMesh.Get();
        cube->material = cubeMat.Get();
        cube->category = RenderCategories::Opaque;
        MeshRenderData* plane = scene.Add<MeshRenderData>();
        plane->world = Float4x4::Translation(Float3{0.0f, -1.0f, 0.0f});
        plane->worldCenter = Float3{0.0f, -1.0f, 0.0f};
        plane->mesh = planeMesh.Get();
        plane->material = planeMat.Get();
        plane->category = RenderCategories::Opaque;

        ViewCamera camera;
        camera.view = Float4x4::LookAtRH(Float3{0, 0.5f, 7}, Float3{0, 0.5f, 0}, Float3{0, 1, 0});
        camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f); // the square scene

        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = kTargetWidth;
        td.height = kTargetHeight;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        td.label = u8"probe.target";
        rhi::Texture* target = nullptr;
        REQUIRE(device->CreateTexture(td, target).IsOk());
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA8Unorm;
        rhi::TextureView* targetView = nullptr;
        REQUIRE(device->CreateTextureView(target, vd, targetView).IsOk());
        rhi::CommandPool* pool = nullptr;
        REQUIRE(device->CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        rhi::Fence* fence = nullptr;
        REQUIRE(device->CreateFence(0, fence).IsOk());
        rhi::Queue* queue = device->GetQueue(rhi::QueueType::Graphics);
        REQUIRE(queue != nullptr);

        ViewSettings settings;
        settings.clear = rhi::ClearColor::Black();
        settings.targetTexture = target;
        settings.targetFinalState = rhi::ResourceState::CopySrc;
        settings.post.bloomEnabled = false;
        settings.viewportX = kRectX;
        settings.viewportY = 0;
        settings.viewportWidth = kRectSize;
        settings.viewportHeight = kRectSize;
        settings.scene = SceneSize(kScene, kScene);
        for (u32 i = 0; i < 2; ++i)
        {
            rhi::CommandEncoder* encoder = nullptr;
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            settings.targetCurrentState = (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
            frame.Begin(*encoder, i % 2);
            frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm, kTargetWidth,
                          kTargetHeight);
            frame.End();
            rhi::CommandBuffer* commandBuffer = encoder->Finish();
            REQUIRE(commandBuffer != nullptr);
            rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
            queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
            REQUIRE(fence->Wait(i + 1, ~0ull));
            pool->DestroyEncoder(encoder);
        }

        // The chain ran at the scene size: its image and its depth are 32 x 32, not the rectangle.
        Array<DebugResourceInfo> resources;
        frame.CollectDebugResources(resources);
        bool sceneImage = false;
        bool sceneDepth = false;
        for (const DebugResourceInfo& resource : resources)
        {
            sceneImage = sceneImage || (resource.name == StringView(u8"view.scene") && resource.width == kScene &&
                                        resource.height == kScene);
            sceneDepth = sceneDepth || (resource.name == StringView(u8"forward.depth") &&
                                        resource.width == kScene && resource.height == kScene);
        }
        CHECK(sceneImage);
        CHECK(sceneDepth);

        const testsupport::CapturedImage image = testsupport::Readback(*device, target, kTargetWidth, kTargetHeight);
        REQUIRE(image.valid);
        f64 bars = 0.0;
        f64 top = 0.0;
        f64 bottom = 0.0;
        for (u32 y = 0; y < kTargetHeight; ++y)
        {
            for (u32 x = 0; x < kTargetWidth; ++x)
            {
                const u8* p = image.At(x, y);
                const f64 luma = p[0] + p[1] + p[2];
                const bool inside = static_cast<i32>(x) >= kRectX && x < kRectX + kRectSize;
                if (!inside)
                {
                    bars += luma;
                }
                else
                {
                    (y < kTargetHeight / 2 ? top : bottom) += luma;
                }
            }
        }
        INFO("bars=", bars, " top=", top, " bottom=", bottom);
        CHECK(bars == doctest::Approx(0.0)); // the letterbox's bars stay black
        CHECK(top > 10000.0);                // the cube lights the rectangle's top half: upright
        CHECK(bottom > top * 1.3);           // and the plane, the larger, the bottom

        device->WaitIdle();
        device->DestroyFence(fence);
        device->DestroyCommandPool(pool);
        device->DestroyTextureView(targetView);
        device->DestroyTexture(target);
        host.Shutdown();
    }
    device->Destroy();
    backend->Destroy();
}
