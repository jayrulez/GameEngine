// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A light's shadow controls on a real device (the shadow-controls spec):
//  - acne: a wall the sun meets at a grazing angle (PaperKid's house walls and sun, 2026-10-03)
//    reads uniformly lit across a sweep of normal offsets and at the default. (The mottling seen on
//    PaperKid's walls was the roof overhang's shadow under-resolved by the cascades, not acne: a
//    nearer camera far plane resolves it into a clean band.);
//  - strength: a cube's shadow on a plane darkens fully at strength 1, half as much at 0.5 and not
//    at all at 0, so the per-light value reaches the sampling.
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
    constexpr u32 kSize = 128;

    struct Item
    {
        RefPtr<geometry::StaticMesh> mesh;
        RefPtr<materials::Material> material;
        Float4x4 world = Float4x4::Identity();
        Float3 center = Float3{0, 0, 0};
        f32 radius = 1.0f;
    };

    struct ShadowScene
    {
        Array<Item> items;
        ViewCamera camera;
        Float3 sunDirection = Float3{0, -1, 0};
        DirectionalShadow shadow; // the light's biases and strength
    };

    // Render `spec` raw (no tonemap, TAA or AO) with a shadow-casting sun, and read it back.
    testsupport::CapturedImage Render(rhi::Device& device, const ShadowScene& spec)
    {
        testsupport::CapturedImage image;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, testsupport::DataFileSystem()));
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            ShadowSystem shadows(device, 2u);
            REQUIRE(shadows.Initialize().IsOk());
            RenderFrame frame(DefaultAllocator(), device, registry, 2, nullptr, nullptr, &shadows);

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{0.05f, 0.05f, 0.05f});
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
            GpuLight sun;
            sun.type = 0.0f;
            sun.directionWS = Normalized(spec.sunDirection);
            sun.intensity = 4.0f;
            sun.shadowIndex = 0.0f;
            sun.shadowStrength = spec.shadow.strength;
            scene.AddLight(sun);
            DirectionalShadow ds = spec.shadow;
            ds.direction = sun.directionWS;
            ds.valid = true;
            scene.SetDirectionalShadow(ds);

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"shadowprobe.target";
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
            image = testsupport::Readback(device, target, kSize, kSize);
            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return image;
    }

    // PaperKid's case (2026-10-03): a house wall beside the street, facing it (+X), the camera at eye
    // height looking down the street with its 300 m far plane (the cascades are fitted to it), and
    // the game's sun, which meets that wall at N.L ~ 0.17.
    ShadowScene GrazingWall(f32 normalBias)
    {
        ShadowScene spec;
        Item wall;
        wall.mesh = geometry::Primitives::Plane(DefaultAllocator(), 12.0f, 5.0f);
        wall.material = materials::CreatePBR(u8"probe.wall", Float4{0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.9f);
        // Plane faces +Y; a -90 degree turn about Z faces it +X. Its 12 m run along -Z.
        wall.world = Float4x4::RotationY(1.57079633f) * Float4x4::RotationZ(-1.57079633f) *
                     Float4x4::Translation(Float3{-3.0f, 2.5f, -9.0f});
        wall.center = Float3{-3.0f, 2.5f, -9.0f};
        wall.radius = 7.0f;
        spec.items.PushBack(wall);
        spec.sunDirection = Float3{-0.171f, -0.867f, -0.467f}; // PaperKid's SUN_ROT forward
        spec.camera.view =
            Float4x4::LookAtRH(Float3{0, 1.6f, 0}, Float3{0, 1.6f, -10.0f}, Float3{0, 1, 0});
        spec.camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 300.0f);
        spec.camera.farZ = 300.0f;
        spec.shadow.normalBias = normalBias;
        return spec;
    }

    // The share of the wall that reads darker than 85% of its median: false shadow. The wall is
    // whatever the left half of the image drew (the rest is the black clear).
    f64 AcneFraction(const testsupport::CapturedImage& image, u32& median)
    {
        Array<u32> lumas;
        for (u32 y = 8; y < kSize - 8; ++y)
        {
            for (u32 x = 2; x < kSize / 2 - 4; ++x)
            {
                const u32 v = image.Luma(x, y);
                if (v > 12)
                {
                    lumas.PushBack(v);
                }
            }
        }
        if (lumas.IsEmpty())
        {
            median = 0;
            return 0.0;
        }
        Array<u32> sorted = lumas;
        for (usize i = 1; i < sorted.Size(); ++i) // insertion sort: a few thousand values
        {
            const u32 v = sorted[i];
            usize j = i;
            while (j > 0 && sorted[j - 1] > v)
            {
                sorted[j] = sorted[j - 1];
                --j;
            }
            sorted[j] = v;
        }
        median = sorted[sorted.Size() / 2];
        usize dark = 0;
        for (u32 v : lumas)
        {
            dark += (v * 100 < median * 85) ? 1u : 0u;
        }
        return static_cast<f64>(dark) / static_cast<f64>(lumas.Size());
    }

    ShadowScene CubeOnPlane(f32 strength)
    {
        ShadowScene spec;
        Item plane;
        plane.mesh = geometry::Primitives::Plane(DefaultAllocator(), 16.0f, 16.0f);
        plane.material = materials::CreatePBR(u8"probe.plane", Float4{0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.9f);
        plane.radius = 12.0f;
        spec.items.PushBack(plane);
        Item cube;
        cube.mesh = geometry::Primitives::Cube(DefaultAllocator(), 2.0f);
        cube.material = materials::CreatePBR(u8"probe.cube", Float4{0.8f, 0.8f, 0.8f, 1}, 0.0f, 0.9f);
        cube.world = Float4x4::Translation(Float3{0, 1, 0});
        cube.center = Float3{0, 1, 0};
        cube.radius = 2.0f;
        spec.items.PushBack(cube);
        spec.sunDirection = Float3{1.0f, -1.0f, 0.0f}; // the shadow falls toward +X
        spec.camera.view = Float4x4::LookAtRH(Float3{0, 14, 0.01f}, Float3{0, 0, 0}, Float3{0, 0, -1});
        spec.camera.projection = Float4x4::PerspectiveFovRH(1.0f, 1.0f, 0.1f, 100.0f);
        spec.camera.farZ = 100.0f;
        spec.shadow.strength = strength;
        return spec;
    }

    // Average luma of a small box of the plane: in the cube's shadow (+X of the cube) or out of it (-Z).
    f64 BoxLuma(const testsupport::CapturedImage& image, u32 cx, u32 cy)
    {
        f64 sum = 0.0;
        for (u32 y = cy - 2; y <= cy + 2; ++y)
        {
            for (u32 x = cx - 2; x <= cx + 2; ++x)
            {
                sum += image.Luma(x, y);
            }
        }
        return sum / 25.0;
    }
}

TEST_CASE("shadow controls: a wall the sun grazes is clean at the default normal offset - Vulkan")
{
    rhi::Backend* backend = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, backend);
    rhi::Device* device = backend != nullptr ? testsupport::MakeTestDevice(backend) : nullptr;
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the shadow control probe skipped");
        if (backend != nullptr)
        {
            backend->Destroy();
        }
        return;
    }
    const f32 sweep[] = {0.02f, 0.25f, 0.5f, 1.0f, 2.0f};
    f64 acne[5] = {};
    for (usize i = 0; i < 5; ++i)
    {
        const testsupport::CapturedImage image = Render(*device, GrazingWall(sweep[i]));
        REQUIRE(image.valid);
        u32 median = 0;
        acne[i] = AcneFraction(image, median);
        MESSAGE("normal bias ", sweep[i], " texels: acne ", acne[i] * 100.0, "% (median luma ", median, ")");
        CHECK(median > 60); // the wall is lit (the sun reaches it)
    }
    for (usize i = 0; i < 5; ++i)
    {
        CHECK(acne[i] < 0.005); // no false shadow across the sweep
    }
    const testsupport::CapturedImage image =
        Render(*device, GrazingWall(ShadowBiasDefaults::kNormalBias));
    u32 median = 0;
    CHECK(AcneFraction(image, median) < 0.005); // the default leaves it clean
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("shadow controls: a light's strength sets how dark its shadow gets - Vulkan")
{
    rhi::Backend* backend = nullptr;
    (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, backend);
    rhi::Device* device = backend != nullptr ? testsupport::MakeTestDevice(backend) : nullptr;
    if (device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the shadow control probe skipped");
        if (backend != nullptr)
        {
            backend->Destroy();
        }
        return;
    }
    // Top-down: +X is to the right of the image, and the shadow falls there.
    const u32 shadowX = kSize / 2 + 17; // x ~ 2: the shadow spans 1..3 (cube top 2 high, sun at 45 degrees)
    const u32 openX = kSize / 2 - 30; // x ~ -3.6: open plane
    // The shadowed patch at each strength, against the same patch at strength 0 (no darkening) and
    // the open plane (which no strength touches).
    f64 shadowed[3] = {};
    f64 open[3] = {};
    const f32 strengths[] = {0.0f, 0.5f, 1.0f};
    for (usize i = 0; i < 3; ++i)
    {
        const testsupport::CapturedImage image = Render(*device, CubeOnPlane(strengths[i]));
        REQUIRE(image.valid);
        open[i] = BoxLuma(image, openX, kSize / 2);
        shadowed[i] = BoxLuma(image, shadowX, kSize / 2);
        MESSAGE("strength ", strengths[i], ": open ", open[i], " shadowed ", shadowed[i]);
    }
    const f64 full = shadowed[0] - shadowed[2];
    const f64 half = shadowed[0] - shadowed[1];
    CHECK(full > 60.0);                                      // strength 1: a full shadow
    CHECK(half == doctest::Approx(full * 0.5).epsilon(0.1)); // strength 0.5: half as dark
    CHECK(open[0] == doctest::Approx(open[2]).epsilon(0.01)); // the open plane is untouched
    device->Destroy();
    backend->Destroy();
}
