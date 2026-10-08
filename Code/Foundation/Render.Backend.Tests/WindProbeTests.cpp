// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WIND vertex probe on REAL devices: a tall card with a windy material renders at two frame times;
// its top rows (height mask 1) shift sideways between them while its bottom rows (below the
// root, mask 0) do not, and the same card with a material whose WindStrength is 0 renders
// byte-identical at both times (the variant is never selected). Vulkan + WebGPU.
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

    // A 1 x 4 m card in the XY plane with `rows` vertex rows (local y from -2 to +2): the height
    // mask is per VERTEX, so the rows below y = 0 carry a zero mask and stay put while the rows
    // above sway - a two-triangle quad would interpolate the top vertices' sway down every row.
    RefPtr<geometry::StaticMesh> SegmentedCard(u32 rows)
    {
        RefPtr<geometry::StaticMesh> mesh = MakeRef<geometry::StaticMesh>(DefaultAllocator());
        const u32 white = 0xFFFFFFFFu;
        for (u32 r = 0; r <= rows; ++r)
        {
            const f32 t = static_cast<f32>(r) / static_cast<f32>(rows);
            const f32 y = -2.0f + 4.0f * t;
            mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{-0.5f, y, 0.0f}, Float3{0, 0, 1},
                                                               Float2{0.0f, 1.0f - t}, white,
                                                               Float3{1, 0, 0}});
            mesh->vertices.PushBack(geometry::StaticMeshVertex{Float3{0.5f, y, 0.0f}, Float3{0, 0, 1},
                                                               Float2{1.0f, 1.0f - t}, white,
                                                               Float3{1, 0, 0}});
        }
        mesh->indices.Resize(rows * 6); // Add writes within the resized count
        for (u32 r = 0; r < rows; ++r)
        {
            const u32 a = r * 2, b = a + 1, c = a + 2, d = a + 3;
            mesh->indices.Add(a);
            mesh->indices.Add(b);
            mesh->indices.Add(d);
            mesh->indices.Add(a);
            mesh->indices.Add(d);
            mesh->indices.Add(c);
        }
        mesh->GenerateTangents();
        mesh->bounds = AABB::FromCenterExtents(Float3{0, 0, 0}, Float3{0.5f, 2.0f, 0.01f});
        mesh->subMeshes.PushBack(geometry::SubMesh{0, static_cast<i32>(mesh->IndexCount()), 0,
                                                  geometry::PrimitiveType::Triangles});
        return mesh;
    }

    struct Capture
    {
        bool valid = false;
        Array<u8> pixels; // RGBA8, kSize x kSize
        // The leftmost lit column per row (-1 = the row is dark).
        i32 leftmost[kSize];
    };

    Capture RenderCard(rhi::Device& device, materials::Material* material, f32 time)
    {
        Capture cap;
        for (u32 y = 0; y < kSize; ++y)
        {
            cap.leftmost[y] = -1;
        }
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return cap;
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

            // The segmented card 6 m in front of the camera: its centre at local y = 0, so the
            // top half carries the height mask and the bottom half none.
            RefPtr<geometry::StaticMesh> card = SegmentedCard(16);
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            MeshRenderData* data = scene.Add<MeshRenderData>();
            data->world = Float4x4::Translation(Float3{0.0f, 0.0f, -6.0f});
            data->worldCenter = Float3{0.0f, 0.0f, -6.0f};
            data->worldRadius = 3.0f;
            data->mesh = card.Get();
            data->material = material;
            data->category = RenderCategories::Opaque;

            ViewCamera camera;
            camera.view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
            camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
            camera.farZ = 100.0f;

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"wind.target";
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
                frame.SetTime(time); // the same clock both frames (prev = cur: no motion)
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
            cap.pixels.Resize(kSize * kSize * 4u);
            for (u32 y = 0; y < kSize; ++y)
            {
                for (u32 x = 0; x < kSize; ++x)
                {
                    const u8* p = img.At(x, y);
                    const usize at = (static_cast<usize>(y) * kSize + x) * 4u;
                    cap.pixels[at + 0] = p[0];
                    cap.pixels[at + 1] = p[1];
                    cap.pixels[at + 2] = p[2];
                    cap.pixels[at + 3] = p[3];
                    const u32 luma = static_cast<u32>(p[0]) + p[1] + p[2];
                    if (luma > 30 && cap.leftmost[y] < 0)
                    {
                        cap.leftmost[y] = static_cast<i32>(x);
                    }
                }
            }
            cap.valid = true;

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return cap;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        INFO(doctest::String(name));
        RefPtr<materials::Material> windy =
            materials::CreatePBR(u8"wind.card", Float4{0.9f, 0.9f, 0.9f, 1}, 0, 0.9f);
        windy->SetDefaultFloat(u8"WindStrength", 1.0f); // a metre of sway: ~18 texels
        windy->SetDefaultFloat(u8"WindSpeed", 1.0f);
        windy->SetDefaultFloat(u8"WindHeight", 2.0f); // full at the card's top edge
        RefPtr<materials::Material> still =
            materials::CreatePBR(u8"still.card", Float4{0.9f, 0.9f, 0.9f, 1}, 0, 0.9f);

        const Capture a = RenderCard(device, windy.Get(), 0.0f);
        const Capture b = RenderCard(device, windy.Get(), 1.5f);
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        // Find the card's rows: y down, the card spans the middle rows; the top quarter of its
        // lit rows are tips, the bottom quarter are roots.
        i32 first = -1, last = -1;
        for (u32 y = 0; y < kSize; ++y)
        {
            if (a.leftmost[y] >= 0 && b.leftmost[y] >= 0)
            {
                first = first < 0 ? static_cast<i32>(y) : first;
                last = static_cast<i32>(y);
            }
        }
        REQUIRE(first >= 0);
        REQUIRE(last - first > 40); // the 4 m card fills most of the frame height
        const i32 span = last - first;
        bool tipsMoved = false;
        bool rootsMoved = false;
        for (i32 y = first; y <= first + span / 4; ++y) // tips (screen top = local +y)
        {
            tipsMoved |= a.leftmost[y] != b.leftmost[y];
        }
        for (i32 y = last - span / 4; y <= last; ++y) // roots (below local y = 0: mask 0)
        {
            rootsMoved |= a.leftmost[y] != b.leftmost[y];
        }
        CHECK(tipsMoved);
        CHECK(!rootsMoved);

        // A material whose WindStrength is 0 never selects the variant: byte-identical frames.
        const Capture c = RenderCard(device, still.Get(), 0.0f);
        const Capture d = RenderCard(device, still.Get(), 1.5f);
        REQUIRE(c.valid);
        REQUIRE(d.valid);
        CHECK(MemCompare(c.pixels.Data(), d.pixels.Data(), c.pixels.Size()) == 0);
        // ...and its roots sit where the windy card's roots sit (the root rows share the geometry).
        CHECK(c.leftmost[last] == a.leftmost[last]);
    }
}

TEST_CASE("wind probe: a windy card's tips sway with the frame time and its roots stay; a still material is byte-identical - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan wind probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu wind probe skipped");
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
