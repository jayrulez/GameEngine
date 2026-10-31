// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Vulkan backend: what the profiler needs from queries. A queue reads its
// timestamp and core::GetTicks' clock together (calibrated timestamps), and a pipeline-statistics
// query resolves to a record as wide as the device says, decoded into the RHI's counters. The
// cases run on a real adapter and skip without one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "RHI.Vulkan/VkIncludes.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;

using namespace foundation::core;
using namespace foundation::rhi;

namespace
{
    // Destroys the test's backend on every way out, after the device the test destroys itself.
    struct BackendRelease
    {
        Backend*& backend;
        ~BackendRelease()
        {
            if (backend != nullptr)
                backend->Destroy();
        }
    };

    Device* MakeDevice(Backend*& outBackend)
    {
        outBackend = nullptr;
        (void)vk::CreateBackend(vk::VkBackendDesc{}, outBackend);
        if (outBackend == nullptr || outBackend->EnumerateAdapters().IsEmpty())
        {
            return nullptr;
        }
        Device* device = nullptr;
        if (!outBackend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk())
        {
            return nullptr;
        }
        return device;
    }

    void Wait(u64 milliseconds)
    {
        const u64 until = GetTicks() + milliseconds * GetTickFrequency() / 1000u;
        while (GetTicks() < until)
        {
        }
    }
}

TEST_CASE("vk.queries: a calibration reads the GPU's timestamp beside core::GetTicks")
{
    Backend* backend = nullptr;
    const BackendRelease release{backend};
    Device* device = MakeDevice(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    Queue* queue = device->GetQueue(QueueType::Graphics);
    REQUIRE(queue != nullptr);
    if (!device->features.timestampCalibration)
    {
        u64 gpu = 7, cpu = 9;
        CHECK_FALSE(queue->CalibrateTimestamps(gpu, cpu));
        CHECK(gpu == 7u); // left alone
        MESSAGE("no calibrated timestamps on this adapter - only the refusal checked");
        device->Destroy();
        return;
    }

    const u64 before = GetTicks();
    u64 gpu0 = 0, cpu0 = 0;
    REQUIRE(queue->CalibrateTimestamps(gpu0, cpu0));
    const u64 after = GetTicks();
    // The CPU side is core::GetTicks' own clock: it lies between two reads of it.
    CHECK(cpu0 >= before);
    CHECK(cpu0 <= after);

    // Over a while, the GPU's ticks times its period track the CPU's time.
    Wait(30);
    u64 gpu1 = 0, cpu1 = 0;
    REQUIRE(queue->CalibrateTimestamps(gpu1, cpu1));
    const f64 cpuMs = TicksToMilliseconds(cpu1 - cpu0);
    const f64 gpuMs = static_cast<f64>(gpu1 - gpu0) * queue->TimestampPeriod() / 1.0e6;
    MESSAGE("calibration: CPU ", cpuMs, " ms, GPU ", gpuMs, " ms");
    CHECK(cpuMs >= 29.0);
    CHECK(gpuMs == doctest::Approx(cpuMs).epsilon(0.01));
    device->Destroy();
}

TEST_CASE("vk.queries: a statistics query resolves to a record the device decodes")
{
    Backend* backend = nullptr;
    const BackendRelease release{backend};
    Device* device = MakeDevice(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    CHECK(device->QueryResultSize(QueryType::Timestamp) == 8u);
    if (!device->features.pipelineStatisticsQueries)
    {
        CHECK(device->QueryResultSize(QueryType::PipelineStatistics) == 0u);
        MESSAGE("no pipeline statistics on this adapter - skipped");
        device->Destroy();
        return;
    }
    const u32 recordSize = device->QueryResultSize(QueryType::PipelineStatistics);
    REQUIRE(recordSize >= sizeof(u64) * 6u);

    QuerySetDesc qd{};
    qd.type = QueryType::PipelineStatistics;
    qd.count = 2;
    qd.label = u8"vk.queries.statistics";
    QuerySet* set = nullptr;
    REQUIRE(device->CreateQuerySet(qd, set).IsOk());
    BufferDesc bd{};
    bd.size = static_cast<u64>(recordSize) * 2u;
    bd.usage = BufferUsage::CopyDst;
    bd.memory = MemoryLocation::GpuToCpu;
    bd.label = u8"vk.queries.readback";
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(bd, readback).IsOk());
    // Fill the readback with a pattern, so a resolve that writes nothing would show.
    if (u8* bytes = static_cast<u8*>(readback->Map()))
    {
        for (u64 i = 0; i < bd.size; ++i)
        {
            bytes[i] = 0xAB;
        }
        readback->Unmap();
    }

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());
    enc->ResetQuerySet(set, 0, 2);
    // Two queries around no work at all: every counter reads zero, each in its own record.
    enc->BeginPipelineStatisticsQuery(set, 0);
    enc->EndPipelineStatisticsQuery(set, 0);
    enc->BeginPipelineStatisticsQuery(set, 1);
    enc->EndPipelineStatisticsQuery(set, 1);
    enc->ResolveQuerySet(set, 0, 2, readback, 0);
    CommandBuffer* cb = enc->Finish();
    REQUIRE(cb != nullptr);
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);
    CommandBuffer* cbs[] = {cb};
    queue->Submit(Span<CommandBuffer* const>(cbs, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    const u8* bytes = static_cast<const u8*>(readback->Map());
    REQUIRE(bytes != nullptr);
    for (u32 q = 0; q < 2; ++q)
    {
        const PipelineStatistics stats = device->DecodePipelineStatistics(bytes + q * recordSize);
        CHECK(stats.inputVertices == 0u);
        CHECK(stats.inputPrimitives == 0u);
        CHECK(stats.vertexInvocations == 0u);
        CHECK(stats.clippingPrimitives == 0u);
        CHECK(stats.fragmentInvocations == 0u);
        CHECK(stats.computeInvocations == 0u);
    }
    readback->Unmap();

    pool->DestroyEncoder(enc);
    device->DestroyFence(fence);
    device->DestroyCommandPool(pool);
    device->DestroyBuffer(readback);
    device->DestroyQuerySet(set);
    device->Destroy();
}

TEST_CASE("vk.counters: memory held follows real allocations, and the driver reports its budget")
{
    Backend* backend = nullptr;
    const BackendRelease release{backend};
    Device* device = MakeDevice(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    const DeviceCounters before = device->Counters();
    BufferDesc bd{};
    bd.size = 1u << 20;
    bd.usage = BufferUsage::Storage;
    bd.label = u8"vk.counters.buffer";
    Buffer* buffer = nullptr;
    REQUIRE(device->CreateBuffer(bd, buffer).IsOk());
    Texture* target = nullptr;
    REQUIRE(device->CreateTexture(TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 256, 256), target).IsOk());
    const DeviceCounters held = device->Counters();
    // What the driver allocated: at least what was asked (it may round up).
    CHECK(held.bufferBytes - before.bufferBytes >= bd.size);
    CHECK(held.renderTargetBytes - before.renderTargetBytes >= 256u * 256u * 4u);
    CHECK(held.buffers == before.buffers + 1u);
    CHECK(held.textures == before.textures + 1u);
    device->DestroyTexture(target);
    device->DestroyBuffer(buffer);
    const DeviceCounters after = device->Counters();
    CHECK(after.bufferBytes == before.bufferBytes);
    CHECK(after.renderTargetBytes == before.renderTargetBytes);

    u64 budget = 0, used = 0;
    if (device->features.memoryBudget)
    {
        REQUIRE(device->MemoryBudget(budget, used));
        MESSAGE("memory budget ", budget >> 20, " MiB, used ", used >> 20, " MiB");
        CHECK(budget > 0u);
        CHECK(used <= budget * 2u);
    }
    else
    {
        CHECK_FALSE(device->MemoryBudget(budget, used));
        MESSAGE("no VK_EXT_memory_budget on this adapter");
    }
    device->Destroy();
}

TEST_CASE("vk.queries: a bundle runs inside a statistics query without a validation error")
{
    // The profiler wraps each pass in a statistics query, and a pass may run render bundles:
    // Vulkan requires a secondary command buffer to inherit the active query's statistics flags,
    // or vkCmdExecuteCommands is invalid (the profiler armed in any game that uses bundles).
    Backend* backend = nullptr;
    const BackendRelease release{backend};
    (void)vk::CreateBackend(vk::VkBackendDesc{.enableValidation = true}, backend);
    if (backend == nullptr || backend->EnumerateAdapters().IsEmpty())
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    if (!device->features.pipelineStatisticsQueries)
    {
        MESSAGE("no pipeline statistics on this adapter - skipped");
        device->Destroy();
        return;
    }
    const u32 errorsBefore = vk::ValidationErrorCount();

    QuerySetDesc qd{};
    qd.type = QueryType::PipelineStatistics;
    qd.count = 1;
    qd.label = u8"vk.queries.bundle_statistics";
    QuerySet* set = nullptr;
    REQUIRE(device->CreateQuerySet(qd, set).IsOk());
    Texture* target = nullptr;
    REQUIRE(device->CreateTexture(TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 64, 64), target).IsOk());
    TextureViewDesc vd{};
    vd.format = TextureFormat::RGBA8Unorm;
    TextureView* view = nullptr;
    REQUIRE(device->CreateTextureView(target, vd, view).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());
    RenderBundleDesc bd{};
    bd.colorFormats[0] = TextureFormat::RGBA8Unorm;
    bd.colorFormatCount = 1;
    bd.width = 64;
    bd.height = 64;
    bd.label = u8"vk.queries.bundle";
    RenderBundleEncoder* bundleEncoder = enc->CreateRenderBundleEncoder(bd);
    REQUIRE(bundleEncoder != nullptr);
    RenderBundle* bundle = bundleEncoder->Finish();
    REQUIRE(bundle != nullptr);

    enc->ResetQuerySet(set, 0, 1);
    enc->TransitionTexture(target, ResourceState::Undefined, ResourceState::RenderTarget);
    enc->BeginPipelineStatisticsQuery(set, 0);
    ColorAttachment ca{};
    ca.view = view;
    RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);
    rpd.contents = RenderPassContents::SecondaryCommandBuffers;
    RenderPassEncoder* pass = enc->BeginRenderPass(rpd);
    RenderBundle* bundles[] = {bundle};
    pass->ExecuteBundles(Span<RenderBundle* const>(bundles, 1));
    pass->End();
    enc->EndPipelineStatisticsQuery(set, 0);
    CommandBuffer* cb = enc->Finish();
    REQUIRE(cb != nullptr);
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* cbs[] = {cb};
    device->GetQueue(QueueType::Graphics)->Submit(Span<CommandBuffer* const>(cbs, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));
    CHECK(vk::ValidationErrorCount() == errorsBefore);

    pool->DestroyEncoder(enc);
    device->DestroyFence(fence);
    device->DestroyCommandPool(pool);
    device->DestroyTextureView(view);
    device->DestroyTexture(target);
    device->DestroyQuerySet(set);
    device->Destroy();
}
