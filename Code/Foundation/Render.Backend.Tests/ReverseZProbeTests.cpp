// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The depth convention (reverse-Z: core::projection / rhi::depth / depth.hlsli) proven at the
// pixel: three scenes through the FULL RenderFrame chain on real Vulkan and WebGPU devices, read
// back and asserted.
//  - ordering: a near cube over a far cube - the nearer wins the centre, drawn in either order, and
//    a pixel nothing touched keeps the clear (the far plane stays the far plane);
//  - precision: a red face 0.02 units in front of a grey wall NINE HUNDRED units away, with a 0.1
//    near plane. Under standard-Z that gap is ~1/25 of a float ulp at depth ~1.0 and the two
//    z-fight into speckle; under reverse-Z it is ~2500 ulps and the face is solid red. This is
//    the whole reason for the convention, so it is the test that must never go green by accident;
//  - shadows: a sun over a cube on a plane - the cascade's ortho projection, the caster-side
//    hardware bias (sign flipped with the convention), the sampler compare and the receiver-side
//    bias all have to agree for the plane to read lit beside the cube and dark in its shadow.
// Run it on the cooked-pack WGSL path too (the browser's shaders, on wgpu-native):
//   OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 ./Render.Backend.Tests
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <cstdio>

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

    struct SceneSpec
    {
        Array<Item> items;
        ViewCamera camera;
        bool sun = false; // a shadow-casting directional light (needs the ShadowSystem)
        Float3 sunDirection = Float3{0, -1, 0};
    };

    // Render `spec` raw (no tonemap, no TAA, no AO) and read the LDR pixels back.
    testsupport::CapturedImage RenderScene(rhi::Device& device, const SceneSpec& spec)
    {
        testsupport::CapturedImage image;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return image;
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

            UniquePtr<ShadowSystem> shadows;
            if (spec.sun)
            {
                shadows = MakeUnique<ShadowSystem>(DefaultAllocator(), device, /*framesInFlight*/ 2u);
                REQUIRE(shadows->Initialize().IsOk());
            }

            RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2,
                              /*clusters*/ nullptr, /*tonemap*/ nullptr, shadows.Get(),
                              /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr, /*taa*/ nullptr,
                              /*ao*/ nullptr, /*fxaa*/ nullptr);

            ExtractedScene scene{DefaultAllocator()};
            // Flat white ambient lights the unlit scenes; the sun scene keeps it dim so the lit
            // plane does not saturate and the shadow is readable as a ratio.
            scene.SetAmbient(spec.sun ? Float3{0.05f, 0.05f, 0.05f} : Float3{1.0f, 1.0f, 1.0f});
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
            if (spec.sun)
            {
                GpuLight sun;
                sun.type = 0.0f; // directional
                sun.directionWS = Normalized(spec.sunDirection);
                sun.color = Float3{1, 1, 1};
                sun.intensity = 1.5f;
                sun.shadowIndex = 0.0f; // shadowed (what the extraction marks)
                scene.AddLight(sun);
                DirectionalShadow ds;
                ds.direction = sun.directionWS;
                ds.valid = true;
                scene.SetDirectionalShadow(ds);
            }

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"reversez.target";
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
                frame.AddView(scene, spec.camera, settings, targetView,
                              rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
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

    Item Cube(f32 size, Float3 at, StringView name, Float4 color)
    {
        Item item;
        item.mesh = geometry::Primitives::Cube(DefaultAllocator(), size);
        item.material = materials::CreatePBR(name, color, 0.0f, 0.9f);
        item.world = Float4x4::Translation(at);
        item.center = at;
        item.radius = size;
        return item;
    }

    // A plane (built in XZ, facing +Y) rotated to face +Z (toward a camera looking down -Z).
    Item WallFacingCamera(f32 extent, Float3 at, StringView name, Float4 color)
    {
        Item item;
        item.mesh = geometry::Primitives::Plane(DefaultAllocator(), extent, extent);
        item.material = materials::CreatePBR(name, color, 0.0f, 0.9f);
        item.world = Float4x4::RotationX(1.57079633f) * Float4x4::Translation(at);
        item.center = at;
        item.radius = extent;
        return item;
    }

    Item Ground(f32 extent, f32 y, StringView name, Float4 color)
    {
        Item item;
        item.mesh = geometry::Primitives::Plane(DefaultAllocator(), extent, extent);
        item.material = materials::CreatePBR(name, color, 0.0f, 0.9f);
        item.world = Float4x4::Translation(Float3{0.0f, y, 0.0f});
        item.center = Float3{0.0f, y, 0.0f};
        item.radius = extent;
        return item;
    }

    // The pixel a world point lands on, through the camera (x only: the y convention differs per
    // backend, which the orientation probe owns; every probe here samples along one screen row).
    u32 PixelX(const ViewCamera& camera, Float3 world)
    {
        const Float4 clip = Float4{world.x, world.y, world.z, 1.0f} * (camera.view * camera.projection);
        const f32 ndcX = clip.x / clip.w;
        return static_cast<u32>(Clamp((ndcX * 0.5f + 0.5f) * static_cast<f32>(kSize), 0.0f,
                                      static_cast<f32>(kSize - 1)));
    }

    bool RedDominant(const u8* p) { return p[0] > 100 && p[0] > p[1] * 2 && p[0] > p[2] * 2; }
    bool BlueDominant(const u8* p) { return p[2] > 100 && p[2] > p[0] * 2 && p[2] > p[1] * 2; }

    // Fraction of the pixels in the centred square of `half` half-extent that satisfy `pred`.
    template <typename Pred>
    f64 CentreFraction(const testsupport::CapturedImage& image, u32 half, Pred pred)
    {
        u32 hits = 0, total = 0;
        for (u32 y = kSize / 2 - half; y < kSize / 2 + half; ++y)
        {
            for (u32 x = kSize / 2 - half; x < kSize / 2 + half; ++x)
            {
                ++total;
                hits += pred(image.At(x, y)) ? 1u : 0u;
            }
        }
        return static_cast<f64>(hits) / static_cast<f64>(total);
    }

    // ---- the three scenes ----

    // Near red cube, far blue cube, the far one ADDED after the near one (a renderer that lost its
    // depth test - or compares the wrong way - shows blue at the centre).
    SceneSpec OrderingScene()
    {
        SceneSpec spec;
        // Near cube 0.6 wide, front face 2.7 out: ~19% of the half-height around the centre. Far
        // cube 3 wide, front face 6.5 out: ~40%, so a row 20 pixels off centre is far-cube-only and
        // the corners (100%) see nothing.
        spec.items.PushBack(Cube(0.6f, Float3{0, 0, -3.0f}, u8"rz.near", Float4{1.0f, 0.05f, 0.05f, 1}));
        spec.items.PushBack(Cube(3.0f, Float3{0, 0, -8.0f}, u8"rz.far", Float4{0.05f, 0.05f, 1.0f, 1}));
        spec.camera.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
        spec.camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        spec.camera.position = Float3{0, 0, 0};
        spec.camera.farZ = 100.0f;
        return spec;
    }

    // A grey wall 900 units out, a red cube whose FRONT face sits 0.02 units in front of it (the
    // rest of the cube is behind the wall). The centre of the view is that face.
    SceneSpec PrecisionScene()
    {
        SceneSpec spec;
        constexpr f32 kWallZ = -900.0f;
        constexpr f32 kGap = 0.02f;
        constexpr f32 kCube = 400.0f;
        spec.items.PushBack(WallFacingCamera(4000.0f, Float3{0, 0, kWallZ}, u8"rz.wall",
                                             Float4{0.6f, 0.6f, 0.6f, 1}));
        spec.items.PushBack(Cube(kCube, Float3{0, 0, kWallZ + kGap - kCube * 0.5f}, u8"rz.face",
                                 Float4{1.0f, 0.05f, 0.05f, 1}));
        spec.camera.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
        spec.camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 2000.0f);
        spec.camera.position = Float3{0, 0, 0};
        spec.camera.farZ = 2000.0f;
        return spec;
    }

    // A cube floating over a ground plane, lit by a sun slanting toward +X: the shadow falls on the
    // plane beside the cube (x in [0.4, 3.6]); the plane at -x is lit.
    SceneSpec ShadowScene()
    {
        SceneSpec spec;
        spec.items.PushBack(Ground(40.0f, -1.0f, u8"rz.ground", Float4{0.8f, 0.8f, 0.8f, 1}));
        spec.items.PushBack(Cube(1.6f, Float3{0, 1.0f, 0}, u8"rz.caster", Float4{0.8f, 0.8f, 0.8f, 1}));
        spec.sun = true;
        spec.sunDirection = Float3{1.0f, -1.0f, 0.0f};
        // Straight down from 12 units up; screen x follows world x.
        spec.camera.view = Float4x4::LookAtRH(Float3{0, 12.0f, 0.001f}, Float3{0, 0, 0}, Float3{0, 0, -1});
        spec.camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        spec.camera.position = Float3{0, 12.0f, 0.001f};
        spec.camera.farZ = 100.0f;
        return spec;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        // Ordering.
        {
            const testsupport::CapturedImage image = RenderScene(device, OrderingScene());
            REQUIRE(image.valid);
            const f64 red = CentreFraction(image, 6, RedDominant);
            const f64 blue = CentreFraction(image, 6, BlueDominant);
            INFO(doctest::String(name) << " ordering: red " << red << " blue " << blue);
            CHECK(red > 0.99);
            CHECK(blue == 0.0);
            // The far cube is bigger: it shows around the near one.
            CHECK(BlueDominant(image.At(kSize / 2, kSize / 2 - 20)));
            CHECK(BlueDominant(image.At(kSize / 2, kSize / 2 + 20)));
            // A corner nothing reached keeps the clear: the far plane is still the far plane.
            const u8* corner = image.At(1, 1);
            CHECK((corner[0] + corner[1] + corner[2]) == 0);
        }
        // Precision at distance: solid red, no wall bleeding through the 0.02 gap.
        {
            const testsupport::CapturedImage image = RenderScene(device, PrecisionScene());
            REQUIRE(image.valid);
            const f64 red = CentreFraction(image, 16, RedDominant);
            INFO(doctest::String(name) << " precision: red fraction " << red);
            CHECK(red > 0.999);
        }
        // Shadows.
        {
            const SceneSpec spec = ShadowScene();
            const testsupport::CapturedImage image = RenderScene(device, spec);
            REQUIRE(image.valid);
            const u32 row = kSize / 2;
            const u32 litX = PixelX(spec.camera, Float3{-2.4f, -1.0f, 0.0f});
            const u32 shadowX = PixelX(spec.camera, Float3{2.4f, -1.0f, 0.0f});
            const u32 lit = image.Luma(litX, row);
            const u32 shadowed = image.Luma(shadowX, row);
            INFO(doctest::String(name) << " shadows: lit " << lit << " at x " << litX << ", shadowed " << shadowed
                      << " at x " << shadowX);
            CHECK(lit > 150);                // the sun reaches the open plane
            CHECK(lit < 765);                // and the probe is not saturated (the ratio means something)
            CHECK(shadowed * 2 < lit);       // the plane in the cube's shadow gets ambient only
            // No acne: the lit side is uniformly lit along the row (a wrong bias sign speckles it).
            u32 darkest = 765;
            for (u32 x = litX - 6; x <= litX + 6; ++x)
            {
                darkest = Min(darkest, image.Luma(x, row));
            }
            CHECK(darkest * 10 > lit * 8);
        }
    }
}

TEST_CASE("reverse-z: nearer wins, distance keeps its precision, shadows agree - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan probes skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu probes skipped");
    }
    // Backends self-free via Destroy() (created even when no device came up).
    if (vulkan != nullptr)
    {
        vulkan->Destroy();
    }
    if (webgpu != nullptr)
    {
        webgpu->Destroy();
    }
}
