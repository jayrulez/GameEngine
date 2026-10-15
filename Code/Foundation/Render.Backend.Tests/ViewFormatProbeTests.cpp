// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// VIEW FORMAT probe on REAL devices: an sRGB render target that declares its UNORM twin
// (TextureDesc::alternateViewFormat) is cleared to half grey through each view and read back. The
// UNORM view stores the value as given (128), the sRGB view encodes it (188): the same texture,
// written with or without the sRGB transfer, which is what lets VG blend encoded values into the
// targets everything else draws linear light into. Vulkan + WebGPU.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
import foundation.rhi.testsupport;
using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace
{
    constexpr u32 kSize = 4;

    // The texel the target holds after a half-grey clear through a view in `viewFormat`.
    u32 ClearThrough(rhi::Device& device, rhi::TextureFormat viewFormat)
    {
        rhi::TextureDesc td = rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA8UnormSrgb, kSize, kSize);
        td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        td.alternateViewFormat = rhi::TextureFormat::RGBA8Unorm;
        td.label = u8"viewformat.target";
        rhi::Texture* target = nullptr;
        REQUIRE(device.CreateTexture(td, target).IsOk());
        rhi::TextureViewDesc vd{};
        vd.format = viewFormat;
        rhi::TextureView* view = nullptr;
        REQUIRE(device.CreateTextureView(target, vd, view).IsOk());
        rhi::CommandPool* pool = nullptr;
        REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        rhi::Fence* fence = nullptr;
        REQUIRE(device.CreateFence(0, fence).IsOk());
        rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
        REQUIRE(queue != nullptr);

        rhi::CommandEncoder* encoder = nullptr;
        REQUIRE(pool->CreateEncoder(encoder).IsOk());
        encoder->TransitionTexture(target, rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget);
        rhi::RenderPassDesc rp{};
        rhi::ColorAttachment color{};
        color.view = view;
        color.loadOp = rhi::LoadOp::Clear;
        color.storeOp = rhi::StoreOp::Store;
        color.clearValue = rhi::ClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        rp.colorAttachments.Add(color);
        rhi::RenderPassEncoder* pass = encoder->BeginRenderPass(rp);
        REQUIRE(pass != nullptr);
        pass->End();
        encoder->TransitionTexture(target, rhi::ResourceState::RenderTarget, rhi::ResourceState::CopySrc);
        rhi::CommandBuffer* commands = encoder->Finish();
        REQUIRE(commands != nullptr);
        rhi::CommandBuffer* list[] = {commands};
        queue->Submit(Span<rhi::CommandBuffer* const>(list, 1), fence, 1);
        REQUIRE(fence->Wait(1, ~0ull));
        pool->DestroyEncoder(encoder);

        const testsupport::CapturedImage image = testsupport::Readback(device, target, kSize, kSize);
        REQUIRE(image.valid);
        const u32 red = image.At(1, 1)[0];
        device.WaitIdle();
        device.DestroyFence(fence);
        device.DestroyCommandPool(pool);
        device.DestroyTextureView(view);
        device.DestroyTexture(target);
        return red;
    }

    void ProbeBackend(rhi::Device& device, const char* name)
    {
        INFO(doctest::String(name));
        const u32 unorm = ClearThrough(device, rhi::TextureFormat::RGBA8Unorm);
        const u32 srgb = ClearThrough(device, rhi::TextureFormat::RGBA8UnormSrgb);
        INFO("UNORM view stored ", unorm, ", sRGB view ", srgb);
        CHECK(unorm >= 127u);
        CHECK(unorm <= 128u); // the value as given
        CHECK(srgb >= 187u);
        CHECK(srgb <= 189u); // encoded: 0.5 linear is 0.735 sRGB
    }
}

TEST_CASE("view format probe: an sRGB target written through its UNORM view stores the value as given - Vulkan + WebGPU")
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
        MESSAGE("Vulkan unavailable - vulkan view format probe skipped");
    }
    if (rhi::Device* device = webgpu != nullptr ? testsupport::MakeTestDevice(webgpu) : nullptr)
    {
        ProbeBackend(*device, "webgpu");
        device->Destroy();
    }
    else
    {
        MESSAGE("WebGPU unavailable - webgpu view format probe skipped");
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
