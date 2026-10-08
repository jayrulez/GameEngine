// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Vegetation pixel-level ground truth on REAL devices: a scene with a flat terrain, a splat painted
// on one half and a grass layer following it (on the terrain's vegetation component) goes through the manager (scatter + cache + snapshot)
// and the shared MeshRenderer's instanced path; the pixels prove grass draws on the painted half
// and not on the other, and that a far view origin thins it (the fade prefix). Vulkan is the
// reference, WebGPU must match; skips with no GPU.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
#ifdef OPTION_HAS_DX12
import foundation.rhi.dx12;
#endif
import foundation.rhi.testsupport;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;
import foundation.rendergraph;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.heightfield;
import foundation.terrain;
import foundation.terrain.resource;
import foundation.vegetation;
import foundation.resource; // Ref (a layer's materials)
import engine.terrain;
import engine.vegetation;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace shaders = foundation::shaders;
namespace scene = foundation::scene;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace hf = foundation::heightfield;
namespace tmodel = foundation::terrain;
namespace veg = foundation::vegetation;

namespace
{
    constexpr u32 kSize = 256;
    constexpr i32 kGrid = 129;     // 2 x 2 chunks
    constexpr f32 kWorld = 128.0f; // centred on the origin

    RefPtr<hf::Heightfield> MakeFlat()
    {
        auto grid = MakeRef<hf::Heightfield>(DefaultAllocator(), kGrid, Float2{kWorld, kWorld},
                                             0.0f, 10.0f);
        const hf::Height sample = grid->WorldYToSample(0.0f);
        for (i32 z = 0; z < kGrid; ++z)
        {
            for (i32 x = 0; x < kGrid; ++x)
            {
                grid->SetSample(x, z, sample);
            }
        }
        return grid;
    }

    // Palette layer 0 one-hot where x < 0, base where x > 0.
    RefPtr<tmodel::SplatWeights> MakeHalfSplat()
    {
        constexpr i32 n = 64;
        auto sw = MakeRef<tmodel::SplatWeights>(DefaultAllocator(), n, n);
        Span<u8> idx = sw->Indices();
        Span<u8> wts = sw->Weights();
        for (i32 y = 0; y < n; ++y)
        {
            for (i32 x = 0; x < n / 2; ++x)
            {
                const usize at = sw->TexelOffset(x, y);
                idx[at + 0] = 0;
                wts[at + 0] = 255;
            }
        }
        sw->BumpVersion();
        return sw;
    }

    ViewCamera TopDown()
    {
        ViewCamera camera;
        camera.position = Float3{0.0f, 90.0f, 0.0f};
        camera.view = Float4x4::LookAtRH(camera.position, Float3{0.0f, 0.0f, 0.0f},
                                         Float3{0.0f, 0.0f, 1.0f});
        camera.projection = Float4x4::PerspectiveFovRH(1.2f, 1.0f, 1.0f, 1000.0f);
        camera.farZ = 1000.0f;
        return camera;
    }

    // The view pixel (y down) a world point lands on.
    void PixelOf(const ViewCamera& camera, Float3 world, u32& px, u32& py)
    {
        const Float4 clip = Float4{world.x, world.y, world.z, 1.0f} * camera.ViewProjection();
        const f32 ndcX = clip.x / clip.w;
        const f32 ndcY = clip.y / clip.w;
        px = static_cast<u32>(Clamp((ndcX * 0.5f + 0.5f) * static_cast<f32>(kSize), 0.0f,
                                    static_cast<f32>(kSize - 1)));
        py = static_cast<u32>(Clamp((0.5f - ndcY * 0.5f) * static_cast<f32>(kSize), 0.0f,
                                    static_cast<f32>(kSize - 1)));
    }

    struct Probe
    {
        bool valid = false;
        u32 paintedGreen = 0;   // green texels in a window around the painted half's centre
        u32 unpaintedGreen = 0; // ... around the unpainted half's centre
        u32 totalGreen = 0;
        u32 setsEmitted = 0;
    };

    // Render the grass layer with the manager's fade evaluated from `viewOrigin`.
    Probe RenderGrass(rhi::Device& device, Float3 viewOrigin, f32 fadeStart, f32 fadeEnd)
    {
        Probe probe;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return probe;
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
            RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2);

            // The scene: a flat terrain with the half splat and one grass layer under it.
            scene::Scene world{DefaultAllocator()};
            engine::terrain::AddTerrainSceneManagers(world);
            engine::vegetation::AddVegetationSceneManagers(world);
            auto* mgr = world.GetSystem<engine::vegetation::TerrainVegetationComponentManager>();
            REQUIRE(mgr != nullptr);
            mgr->SetBuildBudget(100);
            RefPtr<hf::Heightfield> grid = MakeFlat();
            RefPtr<tmodel::SplatWeights> splat = MakeHalfSplat();
            auto resource = MakeRef<tmodel::TerrainResource>(DefaultAllocator());
            resource->heightfield = grid.Get();
            resource->weights = splat.Get();
            const scene::EntityHandle terrain = world.CreateEntity(u8"terrain");
            world.GetSystem<engine::terrain::TerrainComponentManager>()->Add(terrain).terrain =
                resource.Get();
            RefPtr<geometry::StaticMesh> tuft = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
            RefPtr<materials::Material> green =
                materials::CreatePBR(u8"grass", Float4{0.1f, 0.9f, 0.1f, 1.0f}, 0.0f, 0.9f);
            engine::vegetation::TerrainVegetationComponent& vegetation = mgr->Add(terrain);
            engine::vegetation::ProceduralVegetationLayer layer;
            layer.name = String(u8"Grass");
            layer.mesh = tuft.Get();
            layer.materials.PushBack(foundation::resource::Ref<materials::Material>(green));
            layer.placement = veg::VegetationPlacement::Splat;
            layer.splatLayer = 0;
            layer.density = 0.5f;
            layer.maxSlopeDegrees = 90.0f;
            layer.fadeStart = fadeStart;
            layer.fadeEnd = fadeEnd;
            vegetation.proceduralLayers.PushBack(layer);
            world.Start();

            ExtractedScene snapshot{DefaultAllocator()};
            snapshot.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            snapshot.SetViewOrigin(viewOrigin);
            mgr->ExtractRenderData(snapshot);
            probe.setsEmitted = static_cast<u32>(snapshot.Size());

            const ViewCamera camera = TopDown();

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"vegetation.probe.target";
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
                frame.AddView(snapshot, camera, settings, targetView,
                              rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder);
            }

            const testsupport::CapturedImage img =
                testsupport::Readback(device, target, kSize, kSize);
            REQUIRE(img.valid);
            u32 paintedX = 0, paintedY = 0, unpaintedX = 0, unpaintedY = 0;
            PixelOf(camera, Float3{-32.0f, 0.5f, 0.0f}, paintedX, paintedY);
            PixelOf(camera, Float3{32.0f, 0.5f, 0.0f}, unpaintedX, unpaintedY);
            constexpr u32 kHalfWindow = 24;
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    const bool isGreen = p[1] > 60 && p[1] > static_cast<u32>(p[0]) + p[2];
                    if (!isGreen)
                    {
                        continue;
                    }
                    ++probe.totalGreen;
                    const auto inWindow = [&](u32 cx, u32 cy)
                    {
                        return x + kHalfWindow >= cx && x < cx + kHalfWindow &&
                               y + kHalfWindow >= cy && y < cy + kHalfWindow;
                    };
                    if (inWindow(paintedX, paintedY))
                    {
                        ++probe.paintedGreen;
                    }
                    if (inWindow(unpaintedX, unpaintedY))
                    {
                        ++probe.unpaintedGreen;
                    }
                }
            }
            probe.valid = true;

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return probe;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        INFO(doctest::String(name));
        // Near: the view origin sits over the terrain; every chunk is inside fadeStart.
        const Probe near = RenderGrass(device, Float3{0.0f, 20.0f, 0.0f}, 100.0f, 200.0f);
        REQUIRE(near.valid);
        CHECK(near.setsEmitted == 2u); // the two painted chunks; the unpainted two scatter nothing
        CHECK(near.paintedGreen > 200u);  // dense grass on the painted half
        CHECK(near.unpaintedGreen == 0u); // none on the other
        CHECK(near.totalGreen > 2000u);

        // Far: the origin is ~250 m from the chunks, inside the fade (100..400): the prefix
        // thins the sets, so fewer green texels - but not none.
        const Probe far = RenderGrass(device, Float3{0.0f, 300.0f, 0.0f}, 100.0f, 400.0f);
        REQUIRE(far.valid);
        CHECK(far.setsEmitted == 2u);
        CHECK(far.totalGreen > 0u);
        CHECK(far.totalGreen < near.totalGreen * 3 / 4);
        CHECK(far.unpaintedGreen == 0u);

        // Beyond fadeEnd: nothing is emitted and nothing draws.
        const Probe gone = RenderGrass(device, Float3{0.0f, 300.0f, 0.0f}, 40.0f, 80.0f);
        REQUIRE(gone.valid);
        CHECK(gone.setsEmitted == 0u);
        CHECK(gone.totalGreen == 0u);
    }
}

TEST_CASE("vegetation probe: splat-driven grass draws on the painted half only and thins with distance - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan vegetation probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu vegetation probe skipped");
    }
#ifdef OPTION_HAS_DX12
    rhi::Backend* dx12 = nullptr;
    (void)rhi::dx12::CreateDxBackend(rhi::dx12::DxBackendDesc{}, dx12);
    if (rhi::Device* device = dx12 != nullptr ? testsupport::MakeTestDevice(dx12) : nullptr)
    {
        ProbeBackend(*device, "dx12");
        device->Destroy();
    }
    if (dx12 != nullptr)
    {
        dx12->Destroy();
    }
#endif
    if (vulkan != nullptr)
    {
        vulkan->Destroy();
    }
    if (webgpu != nullptr)
    {
        webgpu->Destroy();
    }
}
