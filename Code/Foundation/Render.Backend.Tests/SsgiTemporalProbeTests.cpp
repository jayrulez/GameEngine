// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SSGI under a small, bright, moving light, measured at the pixel on real Vulkan and WebGPU devices
// through the full RenderFrame chain: a lantern-sized glowing cube walks along a grey wall over a
// grey floor under a dim ambient, as a guard carries a lantern past a hedge. The few GI rays that
// hit so small and bright a source flash far above the bounce around them; a history that keeps
// those flashes and fades them slowly on still pixels leaves a speckled smear trailing the
// lantern. Measured: the specks around the light, the floor just behind it against just ahead of
// it, the frame-to-frame steadiness once it stops, and how mottled its glow on the wall is then
// and how much its pixels flicker.
// Cooked-pack WGSL path too (the browser's shaders, on wgpu-native):
//   OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 ./Render.Backend.Tests
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
    constexpr u32 kSize = 192;
    constexpr u32 kSettle = 30;   // frames before the light sets off
    constexpr u32 kWalk = 90;     // frames it walks
    constexpr u32 kStill = 40;    // frames it then stands
    constexpr f32 kStart = -1.4f;
    constexpr f32 kStep = 0.03f;  // metres a frame (about the guard's walk at 40 fps)
    constexpr f32 kLaneZ = -1.2f; // the light's line, 0.4 m off the wall's face

    struct Measure
    {
        f32 specks = 0.0f;    // mean speck pixels a frame around the moving light
        f32 behind = 0.0f;    // mean floor luma 0.6 m behind it while it walks
        f32 ahead = 0.0f;     // and 0.6 m ahead
        f32 wobble = 0.0f;    // mean frame-to-frame change near it once it stands
        f32 nearStill = 0.0f; // the settled floor luma beside it once it stands
        f32 blotch = 0.0f;    // how mottled the lit area is once it stands (Blotchiness)
        f32 shimmer = 0.0f;   // mean per-pixel frame-to-frame change on the wall once it stands
        u64 groupsStanding = 0; // bind groups SSGI built over the standing frames (inputs repeat)
    };

    ViewCamera Camera()
    {
        ViewCamera camera;
        camera.position = Float3{0.0f, 2.0f, 2.2f};
        camera.view = Float4x4::LookAtRH(camera.position, Float3{0.0f, 0.2f, -1.0f}, Float3{0, 1, 0});
        camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        return camera;
    }

    // The pixel a world point lands on (rows from the top, as the readback is laid out).
    void PixelOf(const ViewCamera& camera, Float3 world, i32& px, i32& py)
    {
        const Float4 clip = Float4{world.x, world.y, world.z, 1.0f} * (camera.view * camera.projection);
        px = static_cast<i32>((clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(kSize));
        py = static_cast<i32>((0.5f - clip.y / clip.w * 0.5f) * static_cast<f32>(kSize));
    }

    f32 Luma(const testsupport::CapturedImage& image, i32 x, i32 y)
    {
        x = Clamp(x, 0, static_cast<i32>(kSize) - 1);
        y = Clamp(y, 0, static_cast<i32>(kSize) - 1);
        const u8* p = image.At(static_cast<u32>(x), static_cast<u32>(y));
        return static_cast<f32>(p[0] + p[1] + p[2]);
    }

    f32 PatchLuma(const testsupport::CapturedImage& image, i32 cx, i32 cy, i32 half)
    {
        f64 sum = 0.0;
        u32 n = 0;
        for (i32 y = cy - half; y <= cy + half; ++y)
        {
            for (i32 x = cx - half; x <= cx + half; ++x)
            {
                sum += Luma(image, x, y);
                ++n;
            }
        }
        return static_cast<f32>(sum / n);
    }

    // Pixels in a box that stand well above the mean of their 5x5 neighbourhood: the flashes.
    u32 Specks(const testsupport::CapturedImage& image, i32 x0, i32 y0, i32 x1, i32 y1)
    {
        u32 count = 0;
        for (i32 y = y0; y <= y1; ++y)
        {
            for (i32 x = x0; x <= x1; ++x)
            {
                f32 around = 0.0f;
                for (i32 dy = -2; dy <= 2; ++dy)
                {
                    for (i32 dx = -2; dx <= 2; ++dx)
                    {
                        around += Luma(image, x + dx, y + dy);
                    }
                }
                around /= 25.0f;
                count += (Luma(image, x, y) > around + 30.0f) ? 1u : 0u;
            }
        }
        return count;
    }

    // Mottling at the scale of a few to twenty pixels: the mean gap between a 5x5 and a 21x21 box
    // average of the luma over a box. Clouds that a history held to a noisy neighbourhood leaves
    // read here; fine pixel noise and the smooth falloff of a glow mostly do not.
    f32 Blotchiness(const testsupport::CapturedImage& image, i32 x0, i32 y0, i32 x1, i32 y1)
    {
        const auto box = [&](i32 cx, i32 cy, i32 half)
        {
            f32 sum = 0.0f;
            for (i32 y = cy - half; y <= cy + half; ++y)
            {
                for (i32 x = cx - half; x <= cx + half; ++x)
                {
                    sum += Luma(image, x, y);
                }
            }
            return sum / static_cast<f32>((2 * half + 1) * (2 * half + 1));
        };
        f64 sum = 0.0;
        u32 n = 0;
        for (i32 y = y0; y <= y1; y += 2)
        {
            for (i32 x = x0; x <= x1; x += 2)
            {
                sum += Abs(box(x, y, 2) - box(x, y, 10));
                ++n;
            }
        }
        return n > 0 ? static_cast<f32>(sum / n) : 0.0f;
    }

    Measure Run(rhi::Device& device, bool ssgiOn)
    {
        Measure out;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return out;
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
            TonemapPass tonemap(device, shaderSystem, 2);
            REQUIRE(tonemap.Initialize().IsOk());
            SsgiPass ssgi(device, shaderSystem);
            REQUIRE(ssgi.Initialize().IsOk());
            RenderFrame frame(DefaultAllocator(), device, registry, 2, /*clusters*/ nullptr, &tonemap,
                              /*shadows*/ nullptr, /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              /*taa*/ nullptr, /*ao*/ nullptr, /*fxaa*/ nullptr);
            frame.SetSsgi(&ssgi);

            RefPtr<geometry::StaticMesh> plane = geometry::Primitives::Plane(DefaultAllocator(), 20.0f, 20.0f);
            RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
            RefPtr<materials::Material> grey =
                materials::CreatePBR(u8"ssgi.grey", Float4{0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.95f);
            RefPtr<materials::Material> glass =
                materials::CreatePBR(u8"ssgi.lantern", Float4{1.0f, 0.75f, 0.4f, 1.0f}, 0.0f, 0.9f);
            glass->SetDefaultColor(u8"EmissiveColor", Float4{5.0f, 3.75f, 2.0f, 1.0f}); // the guard's glass

            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{0.05f, 0.05f, 0.06f});
            MeshRenderData* floor = scene.Add<MeshRenderData>();
            // Each with an id of its own: the renderer keys last frame's transform (the motion
            // vectors) by it, so a shared id would make every mesh look moved.
            floor->entityId = 1;
            floor->world = Float4x4::Identity();
            floor->worldRadius = 15.0f;
            floor->mesh = plane.Get();
            floor->material = grey.Get();
            floor->category = RenderCategories::Opaque;
            MeshRenderData* wall = scene.Add<MeshRenderData>();
            wall->entityId = 2;
            wall->world = Float4x4::Scale(Float3{20.0f, 4.0f, 0.2f}) * Float4x4::Translation(Float3{0.0f, 2.0f, -1.7f});
            wall->worldCenter = Float3{0.0f, 2.0f, -1.7f};
            wall->worldRadius = 11.0f;
            wall->mesh = cube.Get();
            wall->material = grey.Get();
            wall->category = RenderCategories::Opaque;
            MeshRenderData* lantern = scene.Add<MeshRenderData>();
            lantern->entityId = 3;
            const auto place = [lantern](f32 x)
            {
                lantern->world = Float4x4::Scale(Float3{0.14f, 0.2f, 0.14f}) *
                                 Float4x4::Translation(Float3{x, 0.45f, kLaneZ});
                lantern->worldCenter = Float3{x, 0.45f, kLaneZ};
            };
            place(kStart);
            lantern->worldRadius = 0.2f;
            lantern->mesh = cube.Get();
            lantern->material = glass.Get();
            lantern->category = RenderCategories::Opaque;

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"ssgi.lantern.target";
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
            settings.post.bloomEnabled = false;
            settings.post.taaEnabled = false;
            settings.post.ssgiEnabled = ssgiOn;
            settings.post.needsMotion = true;

            const ViewCamera camera = Camera();
            f64 specks = 0.0, behind = 0.0, ahead = 0.0, wobble = 0.0, still = 0.0, shimmer = 0.0;
            u32 shimmerSamples = 0;
            u64 groupsAtStand = 0;
            Array<f32> lastWall{DefaultAllocator()};
            u32 walkSamples = 0, stillSamples = 0;
            f32 previous = -1.0f;
            f32 x = kStart;
            for (u32 i = 0; i < kSettle + kWalk + kStill; ++i)
            {
                if (i >= kSettle && i < kSettle + kWalk)
                {
                    x += kStep;
                    place(x);
                }
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

                const bool walking = i >= kSettle + 30 && i < kSettle + kWalk; // past the start-up
                const bool standing = i >= kSettle + kWalk + 20;
                if (i == kSettle + kWalk + 20)
                {
                    groupsAtStand = ssgi.BindGroupsBuilt();
                }
                if (!walking && !standing)
                {
                    continue;
                }
                const testsupport::CapturedImage image = testsupport::Readback(device, target, kSize, kSize);
                i32 lx = 0, ly = 0, bx = 0, by = 0, ax = 0, ay = 0;
                PixelOf(camera, Float3{x, 0.0f, kLaneZ + 0.3f}, lx, ly);        // the floor in front of it
                PixelOf(camera, Float3{x - 0.6f, 0.0f, kLaneZ + 0.1f}, bx, by); // behind
                PixelOf(camera, Float3{x + 0.6f, 0.0f, kLaneZ + 0.1f}, ax, ay); // ahead
                if (walking)
                {
                    specks += Specks(image, lx - 30, ly - 30, lx + 30, ly + 12);
                    behind += PatchLuma(image, bx, by, 3);
                    ahead += PatchLuma(image, ax, ay, 3);
                    ++walkSamples;
                }
                else
                {
                    const f32 near = PatchLuma(image, lx, ly, 3);
                    // Each wall pixel around it against the last frame: a flicker that a patch
                    // average hides.
                    i32 cx = 0, cy = 0;
                    PixelOf(camera, Float3{x, 0.45f, kLaneZ}, cx, cy);
                    usize index = 0;
                    const bool compare = lastWall.Size() > 0;
                    for (i32 py = cy - 50; py <= cy + 10; ++py)
                    {
                        for (i32 px = cx - 50; px <= cx + 50; ++px)
                        {
                            if (Abs(px - cx) < 12 && py > cy - 14)
                            {
                                continue; // the glowing cube itself
                            }
                            const f32 luma = Luma(image, px, py);
                            if (compare)
                            {
                                shimmer += Abs(luma - lastWall[index]);
                                lastWall[index] = luma;
                                ++shimmerSamples;
                            }
                            else
                            {
                                lastWall.PushBack(luma);
                            }
                            ++index;
                        }
                    }
                    if (i + 1 == kSettle + kWalk + kStill)
                    {
                        // The wall and floor around it, the glowing cube itself left out.
                        i32 wx = 0, wy = 0;
                        PixelOf(camera, Float3{x, 0.45f, kLaneZ}, wx, wy);
                        out.blotch = 0.5f * (Blotchiness(image, wx - 50, wy - 50, wx - 12, wy + 30) +
                                             Blotchiness(image, wx + 12, wy - 50, wx + 50, wy + 30));
                    }
                    if (previous >= 0.0f)
                    {
                        wobble += Abs(near - previous);
                    }
                    previous = near;
                    still += near;
                    ++stillSamples;
                }
            }
            out.specks = static_cast<f32>(specks / Max(walkSamples, 1u));
            out.behind = static_cast<f32>(behind / Max(walkSamples, 1u));
            out.ahead = static_cast<f32>(ahead / Max(walkSamples, 1u));
            out.wobble = static_cast<f32>(wobble / Max(stillSamples - 1u, 1u));
            out.nearStill = static_cast<f32>(still / Max(stillSamples, 1u));
            out.shimmer = static_cast<f32>(shimmer / Max(shimmerSamples, 1u));
            out.groupsStanding = ssgi.BindGroupsBuilt() - groupsAtStand;

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return out;
    }

    void ProbeLantern(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        const Measure off = Run(device, false); // the scene alone: what the speck count reads as edges
        const Measure on = Run(device, true);
        MESSAGE(doctest::String(backend) << ": specks a frame " << on.specks << " (" << off.specks
                                         << " without SSGI); floor behind " << on.behind << " vs ahead "
                                         << on.ahead << "; standing: floor " << on.nearStill << ", wobble "
                                         << on.wobble << ", mottling " << on.blotch << " (" << off.blotch
                                         << " without SSGI), shimmer a pixel " << on.shimmer
                                         << ", bind groups built standing " << on.groupsStanding);
        REQUIRE(on.nearStill > off.nearStill + 10.0f); // the bounce is there to see
        // No trail: behind the light no brighter than ahead of it. No flashes on top of the scene's
        // own edges, and steady once it stands.
        CHECK(on.behind <= on.ahead * 1.15f + 3.0f);
        CHECK(on.specks <= off.specks + 5.0f);
        CHECK(on.wobble <= 1.5f);
        // No clouds: a tight clip alone holds the history to a noisy neighbourhood and leaves the
        // glow mottled; the a-trous denoise after the accumulate smooths it.
        CHECK(on.blotch <= 2.8f);
        // No flicker: with the history that close to the frame, two rays a pixel leave the wall
        // beside the glow changing visibly once it stands; four rays of half the steps, with the
        // same taps, keep it under the bound.
        CHECK(on.shimmer <= 0.7f);
        // A still view's inputs all come back, so its bind groups do too. The composite and the
        // next frame's accumulate must not share one cache keyed by the same history view, or they
        // throw each other's group away and build two a frame.
        CHECK(on.groupsStanding == 0u);
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
        bool any = false;
        if (rhi::Device* device = testsupport::MakeTestDevice(vulkan))
        {
            probe(*device, "vulkan");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("Vulkan unavailable - skipped");
        }
        if (rhi::Device* device = testsupport::MakeTestDevice(webgpu))
        {
            probe(*device, "webgpu");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("WebGPU unavailable - skipped");
        }
        if (!any)
        {
            MESSAGE("no GPU backend available - SSGI lantern probe skipped entirely");
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
}

TEST_CASE("ssgi: a small bright light walking by a wall leaves no specks and no trail")
{
    ForEachBackend(&ProbeLantern);
}
