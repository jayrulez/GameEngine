// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The editor's shader grid (DebugDraw::DrawGridPlane) on a real device: a camera 5 m in front of a
// grid on the XY plane, looking straight at it, sees a line at every metre (about 11 pixels apart
// in a 64-pixel view), dark between them, and the line through the origin along Y in Y's green.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.testsupport;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
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


TEST_CASE("grid probe: the shader grid draws its lines on world metres, dark between them")
{
    ProbeDevice probe;
    if (probe.device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the grid probe skipped");
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

        // A metre grid on the XY plane (u = X, v = Y), the camera on +Z looking at the origin.
        debug::DebugDraw global;
        debug::GridPlaneDesc grid;
        grid.origin = Float3{0, 0, 0};
        grid.extent = 50.0f;
        grid.axisU = Float3{1, 0, 0};
        grid.axisV = Float3{0, 1, 0};
        grid.spacing = 1.0f;
        grid.blend = 0.0f;
        grid.cameraPosition = Float3{0, 0, 5};
        grid.fadeDistance = 200.0f;
        global.DrawGridPlane(grid);
        frame.SetDebug(&debugPass, &global, nullptr);

        ExtractedScene scene{DefaultAllocator()};
        const testsupport::CapturedImage image = RenderProbe(*probe.device, frame, scene, FrontCamera());
        REQUIRE(image.valid);

        // A metre is this many pixels at 5 m with a 60-degree view across 64 pixels.
        const f32 pixelsPerMetre = static_cast<f32>(kSize) / (2.0f * 5.0f * Tan(0.5f * 1.0472f));
        const auto columnOf = [&](f32 x) { return static_cast<i32>(0.5f * kSize + x * pixelsPerMetre); };
        const auto brightest = [&](i32 column, u32 row)
        {
            u32 best = 0;
            for (i32 c = column - 1; c <= column + 1; ++c)
            {
                const u8* p = image.At(static_cast<u32>(c), row);
                best = Max(best, static_cast<u32>(p[0]) + p[1] + p[2]);
            }
            return best;
        };
        // A row half a metre above the origin, between the lines along X.
        const u32 row = static_cast<u32>(0.5f * kSize - 0.5f * pixelsPerMetre);
        const u32 onLine = brightest(columnOf(1.0f), row);
        const u8* between = image.At(static_cast<u32>(columnOf(0.5f)), row);
        const u32 betweenSum = static_cast<u32>(between[0]) + between[1] + between[2];
        INFO("on the x = 1 line: ", onLine, ", half way to it: ", betweenSum);
        CHECK(onLine > 60u);
        CHECK(betweenSum < 20u);

        // The line through the origin along Y (x = 0) is Y's green.
        u32 bestColumn = static_cast<u32>(columnOf(0.0f));
        for (i32 c = columnOf(0.0f) - 1; c <= columnOf(0.0f) + 1; ++c)
        {
            if (image.At(static_cast<u32>(c), row)[1] > image.At(bestColumn, row)[1])
            {
                bestColumn = static_cast<u32>(c);
            }
        }
        const u8* axis = image.At(bestColumn, row);
        INFO("the Y axis line: ", static_cast<u32>(axis[0]), ", ", static_cast<u32>(axis[1]), ", ",
             static_cast<u32>(axis[2]));
        CHECK(axis[1] > axis[0] + 40);
        CHECK(axis[1] > axis[2] + 40);
        host.Shutdown();
    }
}
