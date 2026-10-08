// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The colour pipeline on a real device: what is entered is what is seen. Authored colours are
// sRGB, the same encoding as an sRGB image, and every path decodes them once on the way to the
// GPU. Two probes:
//
// - A debug colour drawn into an sRGB target reads back as the bytes entered (the debug shaders
//   decode their vertex colours; the target encodes on write).
// - A material colour matches a texture of the same value: an unlit quad whose BaseColor is sRGB
//   0.5 over a white texture, beside one whose texture is sRGB 128 under a white BaseColor, render
//   the same pixel (the material upload decodes its colours as the sampler decodes the texture).
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
    constexpr u32 kSize = 64;

    // Everything one probe frame needs, on a Vulkan device (null when there is none).
    struct ProbeDevice
    {
        rhi::Backend* backend = nullptr;
        rhi::Device* device = nullptr;

        ProbeDevice()
        {
            (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, backend);
            device = backend != nullptr ? testsupport::MakeTestDevice(backend) : nullptr;
        }
        ~ProbeDevice()
        {
            if (device != nullptr)
            {
                device->Destroy();
            }
            if (backend != nullptr)
            {
                backend->Destroy();
            }
        }
    };

    // An sRGB 2x2 texture of one grey byte value.
    rhi::Texture* MakeGreyTexture(rhi::Device& device, u8 value, rhi::TextureView*& view)
    {
        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8UnormSrgb;
        td.width = 2;
        td.height = 2;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.label = u8"probe.grey";
        rhi::Texture* texture = nullptr;
        REQUIRE(device.CreateTexture(td, texture).IsOk());
        u8 pixels[2 * 2 * 4];
        for (u32 i = 0; i < 4; ++i)
        {
            pixels[i * 4 + 0] = value;
            pixels[i * 4 + 1] = value;
            pixels[i * 4 + 2] = value;
            pixels[i * 4 + 3] = 255;
        }
        rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
        rhi::TransferBatch* batch = nullptr;
        REQUIRE(queue->CreateTransferBatch(batch).IsOk());
        rhi::TextureDataLayout layout{};
        layout.bytesPerRow = 2 * 4;
        layout.rowsPerImage = 2;
        batch->WriteTexture(texture, Span<const u8>{pixels, sizeof(pixels)}, layout,
                            rhi::Extent3D{2, 2, 1});
        REQUIRE(batch->Submit().IsOk());
        queue->DestroyTransferBatch(batch);
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA8UnormSrgb;
        REQUIRE(device.CreateTextureView(texture, vd, view).IsOk());
        return texture;
    }

    // Renders `scene` (with `debug` as the global debug list) into a fresh sRGB target and reads
    // it back. Tonemapping runs as in any view; the probes compare like with like, or use the
    // debug overlay, which draws after it.
    testsupport::CapturedImage RenderProbe(rhi::Device& device, RenderFrame& frame,
                                           const ExtractedScene& scene, const ViewCamera& camera)
    {
        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::RGBA8UnormSrgb;
        td.width = kSize;
        td.height = kSize;
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        td.label = u8"probe.target";
        rhi::Texture* target = nullptr;
        REQUIRE(device.CreateTexture(td, target).IsOk());
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::RGBA8UnormSrgb;
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
        settings.post.bloomEnabled = false;
        for (u32 i = 0; i < 2; ++i)
        {
            rhi::CommandEncoder* encoder = nullptr;
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            settings.targetCurrentState =
                (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
            frame.Begin(*encoder, i % 2);
            frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8UnormSrgb,
                          kSize, kSize);
            frame.End();
            rhi::CommandBuffer* commandBuffer = encoder->Finish();
            REQUIRE(commandBuffer != nullptr);
            rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
            queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
            REQUIRE(fence->Wait(i + 1, ~0ull));
            pool->DestroyEncoder(encoder);
        }
        testsupport::CapturedImage image = testsupport::Readback(device, target, kSize, kSize);
        device.WaitIdle();
        device.DestroyFence(fence);
        device.DestroyCommandPool(pool);
        device.DestroyTextureView(targetView);
        device.DestroyTexture(target);
        return image;
    }

    ViewCamera FrontCamera()
    {
        ViewCamera camera;
        camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        return camera;
    }
}

TEST_CASE("colour probe: a debug colour reads back from an sRGB target as the bytes entered")
{
    ProbeDevice probe;
    if (probe.device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the colour probe skipped");
        return;
    }
    {
        shaders::ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(*probe.device, testsupport::DataFileSystem()));
        shaders::ShaderSystem& shaderSystem = *host.System();
        RendererRegistry registry;
        TonemapPass tonemap(*probe.device, shaderSystem, /*framesInFlight*/ 2);
        REQUIRE(tonemap.Initialize().IsOk());
        DebugDrawPass debugPass(*probe.device, shaderSystem, /*framesInFlight*/ 2);
        REQUIRE(debugPass.Initialize().IsOk());
        RenderFrame frame(DefaultAllocator(), *probe.device, registry, /*framesInFlight*/ 2, nullptr,
                          &tonemap);

        // A filled overlay quad across the whole view, in an authored colour.
        debug::DebugDraw global;
        global.DrawQuad(Float3{-5, -5, 0}, Float3{5, -5, 0}, Float3{5, 5, 0}, Float3{-5, 5, 0},
                        Color{0.5f, 0.25f, 0.75f, 1.0f}, /*overlay*/ true);
        frame.SetDebug(&debugPass, &global, nullptr);

        ExtractedScene scene{DefaultAllocator()};
        const testsupport::CapturedImage image = RenderProbe(*probe.device, frame, scene, FrontCamera());
        REQUIRE(image.valid);
        const u8* p = image.At(kSize / 2, kSize / 2);
        INFO("centre = ", static_cast<u32>(p[0]), ", ", static_cast<u32>(p[1]), ", ",
             static_cast<u32>(p[2]));
        // 0.5, 0.25, 0.75 as bytes: 128, 64, 191 (not 188, 137, 225, the value read as linear).
        CHECK(Abs(static_cast<i32>(p[0]) - 128) <= 2);
        CHECK(Abs(static_cast<i32>(p[1]) - 64) <= 2);
        CHECK(Abs(static_cast<i32>(p[2]) - 191) <= 2);
        host.Shutdown();
    }
}

TEST_CASE("colour probe: a material colour renders like a texture of the same value")
{
    ProbeDevice probe;
    if (probe.device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the colour probe skipped");
        return;
    }
    {
        rhi::Device& device = *probe.device;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, testsupport::DataFileSystem()));
        shaders::ShaderSystem& shaderSystem = *host.System();
        materials::PipelineStateCache psoCache(shaderSystem, device);
        materials::MaterialSystem materialSystem;
        REQUIRE(materialSystem.Initialize(device).IsOk());
        MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, /*framesInFlight*/ 2);
        REQUIRE(meshRenderer.Initialize().IsOk());
        RendererRegistry registry;
        registry.Register(&meshRenderer);
        TonemapPass tonemap(device, shaderSystem, /*framesInFlight*/ 2);
        REQUIRE(tonemap.Initialize().IsOk());
        RenderFrame frame(DefaultAllocator(), device, registry, /*framesInFlight*/ 2, nullptr, &tonemap);

        rhi::TextureView* greyView = nullptr;
        rhi::Texture* grey = MakeGreyTexture(device, 128, greyView);
        rhi::TextureView* whiteView = nullptr;
        rhi::Texture* white = MakeGreyTexture(device, 255, whiteView);

        // Left: the colour entered in the material (sRGB 0.5), over white. Right: white, over a
        // texture of byte 128 (sRGB 0.502). Unlit, so nothing but the colour reaches the pixel.
        RefPtr<materials::Material> tinted =
            materials::CreateUnlit(u8"probe.tinted", Float4{128.0f / 255.0f, 128.0f / 255.0f,
                                                            128.0f / 255.0f, 1.0f});
        tinted->SetDefaultTexture(u8"AlbedoMap", whiteView);
        RefPtr<materials::Material> textured =
            materials::CreateUnlit(u8"probe.textured", Float4{1, 1, 1, 1});
        textured->SetDefaultTexture(u8"AlbedoMap", greyView);

        RefPtr<geometry::StaticMesh> quad = geometry::Primitives::Quad(DefaultAllocator(), 2.0f, 2.0f);
        ExtractedScene scene{DefaultAllocator()};
        const auto add = [&](f32 x, materials::Material* material)
        {
            MeshRenderData* rd = scene.Add<MeshRenderData>();
            rd->world = Float4x4::Translation(Float3{x, 0.0f, 0.0f});
            rd->worldCenter = Float3{x, 0.0f, 0.0f};
            rd->mesh = quad.Get();
            rd->material = material;
            rd->category = RenderCategories::Opaque;
        };
        add(-1.2f, tinted.Get());
        add(1.2f, textured.Get());

        const testsupport::CapturedImage image = RenderProbe(device, frame, scene, FrontCamera());
        REQUIRE(image.valid);
        const u8* left = image.At(kSize / 2 - 10, kSize / 2);
        const u8* right = image.At(kSize / 2 + 10, kSize / 2);
        INFO("left = ", static_cast<u32>(left[0]), ", right = ", static_cast<u32>(right[0]));
        CHECK(left[0] > 10); // both quads drew
        CHECK(right[0] > 10);
        CHECK(Abs(static_cast<i32>(left[0]) - static_cast<i32>(right[0])) <= 2);
        CHECK(Abs(static_cast<i32>(left[1]) - static_cast<i32>(right[1])) <= 2);

        device.WaitIdle();
        device.DestroyTextureView(greyView);
        device.DestroyTexture(grey);
        device.DestroyTextureView(whiteView);
        device.DestroyTexture(white);
        host.Shutdown();
    }
}
