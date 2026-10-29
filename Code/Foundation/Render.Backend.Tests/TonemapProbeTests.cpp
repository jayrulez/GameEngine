// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The tone map's bind groups on a real device: a group is kept only while every input is the same
// view of the same texture. Auto exposure hands the tone map one of two ping-pong textures each
// frame; the group was once kept on the transients' views and one generation number folded from
// all the inputs, so two exposure textures whose numbers folded alike shared a group, and every
// other frame showed the other texture's exposure (PaperKid's Block 1 flickered, 2026-10-29).
// Here two exposure textures carry the SAME generation and different levels: the second frame
// must show its own.
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
namespace rendergraph = foundation::rendergraph;

namespace
{
    constexpr u32 kSize = 4;

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

    struct Target
    {
        rhi::Texture* texture = nullptr;
        rhi::TextureView* view = nullptr;
    };

    Target MakeTarget(rhi::Device& device, rhi::TextureFormat format, u32 size, rhi::TextureUsage usage)
    {
        Target t;
        rhi::TextureDesc td{};
        td.format = format;
        td.width = size;
        td.height = size;
        td.usage = usage;
        td.label = u8"tonemap.probe";
        REQUIRE(device.CreateTexture(td, t.texture).IsOk());
        rhi::TextureViewDesc vd{};
        vd.format = format;
        REQUIRE(device.CreateTextureView(t.texture, vd, t.view).IsOk());
        return t;
    }

    void Destroy(rhi::Device& device, Target& t)
    {
        device.DestroyTextureView(t.view);
        device.DestroyTexture(t.texture);
    }
}

TEST_CASE("tonemap probe: an exposure texture with another's generation still gets its own bind group")
{
    ProbeDevice probe;
    if (probe.device == nullptr)
    {
        MESSAGE("Vulkan unavailable - the tonemap probe skipped");
        return;
    }
    rhi::Device& device = *probe.device;
    {
        shaders::ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, testsupport::DataFileSystem()));
        TonemapPass tonemap(device, *host.System(), /*framesInFlight*/ 2);
        REQUIRE(tonemap.Initialize().IsOk());
        rendergraph::RenderGraph graph(DefaultAllocator(), &device);

        const rhi::TextureUsage drawn = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
        // Two adapted-luminance textures (the exposure pass's format): a dim scene and a bright one.
        Target dim = MakeTarget(device, rhi::TextureFormat::R16Float, 1, drawn);
        Target bright = MakeTarget(device, rhi::TextureFormat::R16Float, 1, drawn);
        Target ldr = MakeTarget(device, rhi::TextureFormat::RGBA8Unorm, kSize,
                                rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc);
        rhi::CommandPool* pool = nullptr;
        REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        rhi::Fence* fence = nullptr;
        REQUIRE(device.CreateFence(0, fence).IsOk());
        rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);

        // One frame: a grey scene tone-mapped under `exposureTex` (holding `level`), read back. Both
        // frames use the same frame slot, the same transients and the same exposure generation.
        u64 submitted = 0;
        const auto frame = [&](Target& exposureTex, f32 level) -> u32
        {
            rhi::CommandEncoder* encoder = nullptr;
            REQUIRE(pool->CreateEncoder(encoder).IsOk());
            graph.SetOutputSize(kSize, kSize);
            graph.BeginFrame(0);
            const rendergraph::RGHandle hdr = graph.CreateTransient(
                u8"hdr", rendergraph::RGTextureDesc(rhi::TextureFormat::RGBA16Float, kSize, kSize));
            const rendergraph::RGHandle bloom = graph.CreateTransient(
                u8"bloom", rendergraph::RGTextureDesc(rhi::TextureFormat::RGBA16Float, kSize, kSize));
            const rendergraph::RGHandle ao = graph.CreateTransient(
                u8"ao", rendergraph::RGTextureDesc(rhi::TextureFormat::RGBA8Unorm, kSize, kSize));
            const rendergraph::RGHandle lum =
                graph.ImportTarget(u8"lum", exposureTex.texture, exposureTex.view, rhi::ResourceState::ShaderRead);
            const rendergraph::RGHandle out =
                graph.ImportTarget(u8"ldr", ldr.texture, ldr.view, rhi::ResourceState::CopySrc);
            const auto fill = [&](StringView name, rendergraph::RGHandle h, rhi::ClearColor c)
            {
                graph.AddRenderPass(name,
                                    [&, h, c](rendergraph::PassBuilder& b)
                                    {
                                        b.SetColorTarget(0, h, rhi::LoadOp::Clear, rhi::StoreOp::Store, c);
                                        b.NeverCull();
                                        b.SetExecute([](rhi::RenderPassEncoder&) {});
                                    });
            };
            fill(u8"fill.hdr", hdr, rhi::ClearColor{0.18f, 0.18f, 0.18f, 1.0f});
            fill(u8"fill.bloom", bloom, rhi::ClearColor::Black());
            fill(u8"fill.ao", ao, rhi::ClearColor::White());
            fill(u8"fill.lum", lum, rhi::ClearColor{level, level, level, 1.0f});
            TonemapAutoExposure autoExposure;
            autoExposure.enabled = true;
            autoExposure.handle = lum;
            autoExposure.view = exposureTex.view;
            autoExposure.generation = 7; // the same for both textures
            tonemap.DeclareTonemap(graph, hdr, bloom, ao, out, /*clearColor*/ true, rhi::ClearColor::Black(),
                                   rhi::TextureFormat::RGBA8Unorm, 0, 0, kSize, kSize, /*frameIndex*/ 0,
                                   /*viewIndex*/ 0, /*exposure*/ 1.0f, /*bloom*/ 0.0f, Float2{1, 1},
                                   Float2{0, 0}, /*ao*/ 0.0f, false, /*agx*/ false, false, autoExposure);
            REQUIRE(graph.Execute(encoder).IsOk());
            graph.EndFrame();
            rhi::CommandBuffer* commandBuffer = encoder->Finish();
            REQUIRE(commandBuffer != nullptr);
            rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
            ++submitted;
            queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, submitted);
            REQUIRE(fence->Wait(submitted, ~0ull));
            pool->DestroyEncoder(encoder);
            const testsupport::CapturedImage image = testsupport::Readback(device, ldr.texture, kSize, kSize);
            REQUIRE(image.valid);
            return image.At(kSize / 2, kSize / 2)[1];
        };

        // A dim average makes the grey bright; a bright average, dark. The second frame must not
        // keep the first frame's group, which binds the dim texture.
        const u32 underDim = frame(dim, 0.05f);
        const u32 underBright = frame(bright, 0.8f);
        INFO("under the dim level ", underDim, ", under the bright level ", underBright);
        CHECK(underDim > underBright + 40u);
        // And back: the first texture's level again.
        CHECK(frame(dim, 0.05f) == underDim);

        device.WaitIdle();
        device.DestroyFence(fence);
        device.DestroyCommandPool(pool);
        Destroy(device, dim);
        Destroy(device, bright);
        Destroy(device, ldr);
        host.Shutdown();
    }
}
