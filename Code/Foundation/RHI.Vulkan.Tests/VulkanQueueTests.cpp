// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Vulkan backend: queue-family rules. Barriers must not name graphics stages on a compute-only
// family, a requested compute queue is granted when the adapter has a compute family, and the
// device-latched swap-chain semaphores belong to the graphics queue's submit whichever queue
// submits first. The stage rule is pure and pinned here; the device cases run on a real adapter and
// skip without one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "RHI.Vulkan/VkIncludes.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;

using namespace foundation::core;
using namespace foundation::rhi;

TEST_CASE("vk.barrier: stage masks are cut to what the recording queue family can execute")
{
    using vk::maskStagesForQueue;
    const u64 shaderAccess = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // Graphics executes everything: untouched.
    CHECK(maskStagesForQueue(shaderAccess, QueueType::Graphics) == shaderAccess);
    // Compute-only: the graphics half goes, the compute half stays.
    CHECK(maskStagesForQueue(shaderAccess, QueueType::Compute) == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    // Transfer-only: no shader stage at all; a transfer stage survives.
    CHECK(maskStagesForQueue(shaderAccess | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, QueueType::Transfer) ==
          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    // A mask the cut would empty (a render-target state on a compute queue) becomes ALL_COMMANDS,
    // valid on every family and merely stronger, never zero, which is invalid.
    CHECK(maskStagesForQueue(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, QueueType::Compute) ==
          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    // Zero in, zero out (the caller decides what an empty mask means).
    CHECK(maskStagesForQueue(0, QueueType::Compute) == 0u);
    // Ray-tracing and indirect stages are legal on compute families and survive.
    CHECK(maskStagesForQueue(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, QueueType::Compute) ==
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
}

TEST_CASE("vk.barrier: the access half is cut with the stage half, as one pair")
{
    // Cutting stages alone leaves VERTEX_ATTRIBUTE_READ on a compute queue under ALL_COMMANDS,
    // which is invalid because ALL_COMMANDS expands per family and the compute expansion has no
    // vertex-input stage. Stage and access must move together.
    using vk::maskForQueue;
    using vk::getStageAccess;
    const vk::StageAccess vertex = getStageAccess(ResourceState::VertexBuffer);
    REQUIRE((vertex.accessMask & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) != 0);
    const vk::StageAccess onCompute = maskForQueue(vertex, QueueType::Compute);
    CHECK(onCompute.stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(onCompute.accessMask == 0u); // execution-only: valid; a dangling graphics access is not
    // Shader access on compute keeps its shader bits; on transfer it keeps none.
    const vk::StageAccess write = getStageAccess(ResourceState::ShaderWrite);
    CHECK((maskForQueue(write, QueueType::Compute).accessMask & VK_ACCESS_2_SHADER_WRITE_BIT) != 0);
    CHECK((maskForQueue(write, QueueType::Transfer).accessMask & VK_ACCESS_2_SHADER_WRITE_BIT) == 0);
    // Graphics is untouched on both halves.
    const vk::StageAccess g = maskForQueue(vertex, QueueType::Graphics);
    CHECK(g.stageMask == vertex.stageMask);
    CHECK(g.accessMask == vertex.accessMask);
    // Transfer access survives everywhere.
    const vk::StageAccess copy = getStageAccess(ResourceState::CopyDst);
    CHECK(maskForQueue(copy, QueueType::Transfer).accessMask == copy.accessMask);
}

namespace
{
    // A device on the first adapter, with a compute queue requested; null when there is no
    // Vulkan adapter (CI) so the device cases skip rather than fail.
    Device* MakeDeviceWithComputeQueue(Backend*& outBackend)
    {
        outBackend = nullptr;
        (void)vk::CreateBackend(vk::VkBackendDesc{}, outBackend);
        if (outBackend == nullptr || outBackend->EnumerateAdapters().IsEmpty())
        {
            return nullptr;
        }
        DeviceDesc dd{};
        dd.computeQueueCount = 1;
        Device* device = nullptr;
        if (!outBackend->EnumerateAdapters()[0]->CreateDevice(dd, device).IsOk())
        {
            return nullptr;
        }
        return device;
    }
}

TEST_CASE("vk.queues: a requested compute queue is granted exactly when the adapter has a compute-only family")
{
    Backend* backend = nullptr;
    Device* device = MakeDeviceWithComputeQueue(backend);
    if (device == nullptr)
    {
        MESSAGE("no Vulkan adapter - skipped");
        return;
    }
    auto* adapter = static_cast<vk::VkAdapterImpl*>(backend->EnumerateAdapters()[0]);
    const u32 expected = adapter->findQueueFamily(QueueType::Compute) >= 0 ? 1u : 0u;
    CHECK(device->GetQueueCount(QueueType::Compute) == expected);
    CHECK(device->GetQueueCount(QueueType::Graphics) >= 1u);
    MESSAGE("compute queues: ", device->GetQueueCount(QueueType::Compute));

    // A barrier with shader access recorded on the compute family: the masked stages are what
    // reach the driver. Records and finishes cleanly (validation layers, when present, would
    // flag a graphics stage here).
    if (expected == 1u)
    {
        CommandPool* pool = nullptr;
        REQUIRE(device->CreateCommandPool(QueueType::Compute, pool).IsOk());
        CommandEncoder* enc = nullptr;
        REQUIRE(pool->CreateEncoder(enc).IsOk());
        REQUIRE(enc != nullptr);
        MemoryBarrier mb{};
        mb.oldState = ResourceState::ShaderWrite;
        mb.newState = ResourceState::ShaderRead;
        // The MultiQueue sample's hand-over: a vertex buffer handed to compute as storage, whose
        // barrier would carry VERTEX_ATTRIBUTE_READ onto the compute family unmasked.
        BufferDesc bd{};
        bd.size = 256;
        bd.usage = BufferUsage::Vertex | BufferUsage::Storage;
        bd.label = u8"vk.queues.vertex";
        Buffer* buffer = nullptr;
        REQUIRE(device->CreateBuffer(bd, buffer).IsOk());
        BufferBarrier bb{};
        bb.buffer = buffer;
        bb.oldState = ResourceState::VertexBuffer;
        bb.newState = ResourceState::ShaderWrite;
        BarrierGroup group{};
        group.memoryBarriers = Span<const MemoryBarrier>(&mb, 1);
        group.bufferBarriers = Span<const BufferBarrier>(&bb, 1);
        enc->Barrier(group);
        CommandBuffer* cb = enc->Finish();
        CHECK(cb != nullptr);
        pool->DestroyEncoder(enc);
        device->DestroyCommandPool(pool);
        device->DestroyBuffer(buffer);
    }
    device->Destroy();
    backend->Destroy();
}
