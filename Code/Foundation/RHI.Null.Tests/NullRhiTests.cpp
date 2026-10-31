// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;

using namespace foundation::core;
using namespace foundation::rhi;

TEST_CASE("rhi.null: backend enumerates an adapter and creates a device")
{
    Backend* backend = nullptr;
    REQUIRE(null::CreateNullBackend(backend).IsOk());
    REQUIRE(backend != nullptr);
    CHECK(backend->isInitialized);

    auto adapters = backend->EnumerateAdapters();
    REQUIRE(adapters.Size() == 1u);

    const AdapterInfo info = adapters[0]->Info();
    CHECK(info.name == u8"Null Device");
    CHECK(info.type == AdapterType::Cpu);

    Device* device = nullptr;
    REQUIRE(adapters[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    REQUIRE(device != nullptr);

    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.null: device creates resources and a mappable buffer")
{
    Backend* backend = nullptr;
    REQUIRE(null::CreateNullBackend(backend).IsOk());
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    BufferDesc bufferDesc{};
    bufferDesc.size = 256;
    Buffer* buffer = nullptr;
    REQUIRE(device->CreateBuffer(bufferDesc, buffer).IsOk());
    REQUIRE(buffer != nullptr);
    CHECK(buffer->Map() != nullptr); // null backend backs it with host memory
    buffer->Unmap();

    Texture* texture = nullptr;
    CHECK(device->CreateTexture(TextureDesc{}, texture).IsOk());
    CHECK(texture != nullptr);

    // Unsupported extensions degrade; they do not crash.
    MeshPipeline* mesh = nullptr;
    CHECK(device->CreateMeshPipeline(MeshPipelineDesc{}, mesh).Code() == ErrorCode::NotSupported);

    device->DestroyTexture(texture);
    device->DestroyBuffer(buffer);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi: texture views carry unique monotonic ids (address-reuse guard)")
{
    // Caches keyed by TextureView* validate uniqueId on every hit: a destroyed view's
    // address can be reused by the very next allocation, and a stale cached descriptor
    // then samples a dead image view and loses the device. Ids must be unique across
    // create, destroy and create cycles; an address is never enough.
    Backend* backend = nullptr;
    REQUIRE(null::CreateNullBackend(backend).IsOk());
    REQUIRE(backend != nullptr);
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    Texture* texture = nullptr;
    REQUIRE(device->CreateTexture(TextureDesc{}, texture).IsOk());

    TextureView* a = nullptr;
    REQUIRE(device->CreateTextureView(texture, TextureViewDesc{}, a).IsOk());
    const foundation::core::u64 idA = a->uniqueId;
    CHECK(idA != 0u);

    TextureView* b = nullptr;
    REQUIRE(device->CreateTextureView(texture, TextureViewDesc{}, b).IsOk());
    CHECK(b->uniqueId != idA);

    // Destroy and recreate: even if the allocator reuses the address, the id is fresh.
    device->DestroyTextureView(a);
    TextureView* c = nullptr;
    REQUIRE(device->CreateTextureView(texture, TextureViewDesc{}, c).IsOk());
    CHECK(c->uniqueId != idA);
    CHECK(c->uniqueId != b->uniqueId);

    device->DestroyTextureView(b);
    device->DestroyTextureView(c);
    device->DestroyTexture(texture);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.format: every uncompressed format has a byte size, every compressed one a block size")
{
    // A format missing from BytesPerPixel's table sizes its uploads at zero bytes. Walk the
    // whole enum: a format is either per-texel sized or block sized, never neither, so a new
    // enumerator missing from the table fails here.
    const u32 first = static_cast<u32>(TextureFormat::R8Unorm);
    const u32 last = static_cast<u32>(TextureFormat::ASTC8x8UnormSrgb);
    for (u32 i = first; i <= last; ++i)
    {
        const TextureFormat f = static_cast<TextureFormat>(i);
        INFO("format ", i);
        if (IsCompressed(f))
        {
            CHECK(BytesPerPixel(f) == 0u);
            CHECK((BlockBytes(f) == 8u || BlockBytes(f) == 16u));
        }
        else
        {
            const u32 bpp = BytesPerPixel(f);
            CHECK((bpp == 1u || bpp == 2u || bpp == 4u || bpp == 8u || bpp == 16u));
        }
    }
    // Ordinary integer, signed and packed formats that are easy to leave out of the table.
    CHECK(BytesPerPixel(TextureFormat::R8Uint) == 1u);
    CHECK(BytesPerPixel(TextureFormat::R8Snorm) == 1u);
    CHECK(BytesPerPixel(TextureFormat::RG8Sint) == 2u);
    CHECK(BytesPerPixel(TextureFormat::RG16Uint) == 4u);
    CHECK(BytesPerPixel(TextureFormat::RGBA8Snorm) == 4u);
    CHECK(BytesPerPixel(TextureFormat::RGB10A2Uint) == 4u);
    CHECK(BytesPerPixel(TextureFormat::RGB9E5Float) == 4u);
    CHECK(BytesPerPixel(TextureFormat::RG32Sint) == 8u);
    CHECK(BytesPerPixel(TextureFormat::RGBA16Unorm) == 8u);
    CHECK(BytesPerPixel(TextureFormat::RGBA16Snorm) == 8u);
}


TEST_CASE("rhi.counters: a frame of known work counts what it did, a bundle each time it runs")
{
    // The device's counts for the profiler: totals since it was made.
    null::NullDevice device{DefaultAllocator()};
    CommandPool* pool = nullptr;
    REQUIRE(device.CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());

    // A bundle of two draws and a bind, recorded once.
    RenderBundleEncoder* bundleEncoder = enc->CreateRenderBundleEncoder(RenderBundleDesc{});
    REQUIRE(bundleEncoder != nullptr);
    bundleEncoder->SetBindGroup(0, nullptr, {});
    bundleEncoder->Draw(3);
    bundleEncoder->DrawIndexed(6);
    RenderBundle* bundle = bundleEncoder->Finish();
    CHECK(device.Counters().draws == 0u); // recording a bundle draws nothing yet

    RenderPassEncoder* pass = enc->BeginRenderPass(RenderPassDesc{});
    pass->SetPipeline(nullptr);
    pass->SetBindGroup(0, nullptr, {});
    pass->SetBindGroup(1, nullptr, {});
    pass->Draw(3);
    pass->DrawIndexed(36);
    pass->DrawIndirect(nullptr, 0, 4, 16);
    RenderBundle* bundles[] = {bundle, bundle};
    pass->ExecuteBundles(Span<RenderBundle* const>(bundles, 2)); // the bundle runs twice
    pass->End();
    ComputePassEncoder* compute = enc->BeginComputePass();
    compute->SetPipeline(nullptr);
    compute->Dispatch(8, 8, 1);
    compute->DispatchIndirect(nullptr, 0);
    compute->End();
    TransferBatch* batch = nullptr;
    REQUIRE(device.GetQueue(QueueType::Graphics, 0)->CreateTransferBatch(batch).IsOk());
    const u8 bytes[100] = {};
    batch->WriteBuffer(nullptr, 0, Span<const u8>(bytes, 100));

    const DeviceCounters c = device.Counters();
    CHECK(c.draws == 2u + 4u + 2u * 2u); // two direct, four indirect, the bundle's two twice
    CHECK(c.renderPasses == 1u);
    CHECK(c.computePasses == 1u);
    CHECK(c.dispatches == 2u);
    CHECK(c.pipelineBinds == 2u);       // render and compute
    CHECK(c.bindGroupBinds == 2u + 2u); // the pass's two, the bundle's one twice
    CHECK(c.transferBytes == 100u);
    device.DestroyCommandPool(pool);
}

TEST_CASE("rhi.counters: memory held rises and falls with buffers and textures, render targets apart")
{
    null::NullDevice device{DefaultAllocator()};
    BufferDesc bd{};
    bd.size = 1024;
    Buffer* buffer = nullptr;
    REQUIRE(device.CreateBuffer(bd, buffer).IsOk());
    TextureDesc td{};
    td.format = TextureFormat::RGBA8Unorm;
    td.width = 64;
    td.height = 64;
    td.usage = TextureUsage::Sampled;
    Texture* texture = nullptr;
    REQUIRE(device.CreateTexture(td, texture).IsOk());
    Texture* target = nullptr;
    REQUIRE(device.CreateTexture(TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 32, 32), target).IsOk());

    DeviceCounters c = device.Counters();
    CHECK(c.bufferBytes == 1024u);
    CHECK(c.buffers == 1u);
    CHECK(c.textureBytes == 64u * 64u * 4u);
    CHECK(c.renderTargetBytes == 32u * 32u * 4u);
    CHECK(c.textures == 2u);

    device.DestroyTexture(target);
    device.DestroyBuffer(buffer);
    c = device.Counters();
    CHECK(c.bufferBytes == 0u);
    CHECK(c.renderTargetBytes == 0u);
    CHECK(c.textureBytes == 64u * 64u * 4u);
    CHECK(c.textures == 1u);
    device.DestroyTexture(texture);
    CHECK(device.Counters().textures == 0u);

    // Every mip, every layer: a 64x64 RGBA8 with a full chain of 7 levels, 2 layers.
    td.mipLevelCount = 7;
    td.arrayLayerCount = 2;
    CHECK(EstimateTextureBytes(td) == 2u * 4u * (4096u + 1024u + 256u + 64u + 16u + 4u + 1u));
    u64 budget = 5, used = 6;
    CHECK_FALSE(device.MemoryBudget(budget, used)); // no driver to ask
    CHECK(budget == 5u);
}

TEST_CASE("rhi.null: a swapchain honours an asked-for sRGB or UNORM twin view, and only that")
{
    Backend* backend = nullptr;
    REQUIRE(null::CreateNullBackend(backend).IsOk());
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    SwapChainDesc desc{};
    desc.width = 8;
    desc.height = 8;
    desc.format = TextureFormat::BGRA8UnormSrgb;
    SwapChain* plain = nullptr;
    REQUIRE(device->CreateSwapChain(nullptr, desc, plain).IsOk());
    CHECK(plain->AlternateViewFormat() == TextureFormat::Undefined); // not asked for
    CHECK(plain->CurrentAlternateTextureView() == nullptr);
    device->DestroySwapChain(plain);

    desc.alternateViewFormat = TextureFormat::BGRA8Unorm;
    SwapChain* twin = nullptr;
    REQUIRE(device->CreateSwapChain(nullptr, desc, twin).IsOk());
    REQUIRE(twin->AcquireNextImage().IsOk());
    CHECK(twin->AlternateViewFormat() == TextureFormat::BGRA8Unorm);
    REQUIRE(twin->CurrentAlternateTextureView() != nullptr);
    CHECK(twin->CurrentAlternateTextureView()->desc.format == TextureFormat::BGRA8Unorm);
    CHECK(twin->CurrentTextureView()->desc.format == TextureFormat::BGRA8UnormSrgb);
    CHECK(twin->CurrentTexture()->desc.alternateViewFormat == TextureFormat::BGRA8Unorm);
    device->DestroySwapChain(twin);

    desc.alternateViewFormat = TextureFormat::RGBA16Float; // not the twin: refused quietly
    SwapChain* wrong = nullptr;
    REQUIRE(device->CreateSwapChain(nullptr, desc, wrong).IsOk());
    CHECK(wrong->AlternateViewFormat() == TextureFormat::Undefined);
    device->DestroySwapChain(wrong);

    device->Destroy();
    backend->Destroy();
}
