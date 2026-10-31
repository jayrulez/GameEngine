// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of Queue.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

export module foundation.rhi.vulkan:queue;

import foundation.core;
import foundation.rhi;
import :command_buffer;
import :fence;
import :transfer_batch;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    class VkDeviceImpl; // forward

    class VkQueueImpl : public Queue
    {
    public:
        VkQueueImpl(VkQueue queue, QueueType type, u32 familyIndex, f32 tsPeriod,
                    VkDeviceImpl* device, VkDevice vkDevice, VkPhysicalDevice physDevice,
                    IAllocator& allocator)
            : m_queue(queue), m_familyIndex(familyIndex), m_tsPeriod(tsPeriod), m_device(device),
              m_vkDevice(vkDevice), m_physDevice(physDevice), m_allocator(allocator)
        {
            queueType = type;
        }

        // ---- Queue interface ----
        // All Submit overloads are defined out-of-line (VkDevice.cppm): they report
        // VK_ERROR_DEVICE_LOST to the owning device, which needs its complete type.

        void Submit(Span<CommandBuffer* const> cmdBufs) override;

        void Submit(Span<CommandBuffer* const> cmdBufs, Fence* signalFence,
                    u64 signalValue) override;

        void Submit(Span<CommandBuffer* const> cmdBufs, Span<Fence* const> waitFences,
                    Span<const u64> waitValues, Fence* signalFence, u64 signalValue) override;

        void WaitIdle() override { vkQueueWaitIdle(m_queue); }

        Status CreateTransferBatch(TransferBatch*& out) override
        {
            out = m_allocator.New<VkTransferBatchImpl>(m_vkDevice, m_queue, m_familyIndex,
                                                       m_physDevice, counters);
            return ErrorCode::Ok;
        }
        void DestroyTransferBatch(TransferBatch*& batch) override
        {
            if (batch)
            {
                static_cast<VkTransferBatchImpl*>(batch)->Destroy();
                m_allocator.Delete(batch);
                batch = nullptr;
            }
        }

        f32 TimestampPeriod() const override { return m_tsPeriod; }
        // Out-of-line (VkDevice.cppm): the device holds the calibration entry point.
        bool CalibrateTimestamps(u64& gpuTimestamp, u64& cpuTicks) override;

        // ---- Internal ----
        [[nodiscard]] VkQueue handle() const { return m_queue; }
        [[nodiscard]] u32 familyIndex() const { return m_familyIndex; }
        [[nodiscard]] VkDeviceImpl* owner() const { return m_device; }
        /// The device's counts its transfer batches bump (Device::Counters).
        DeviceCounterBlock* counters = nullptr;

    private:
        VkQueue m_queue = VK_NULL_HANDLE;
        u32 m_familyIndex = 0;
        f32 m_tsPeriod = 0.0f;
        VkDeviceImpl* m_device = nullptr;
        VkDevice m_vkDevice = VK_NULL_HANDLE;
        VkPhysicalDevice m_physDevice = VK_NULL_HANDLE;
        IAllocator& m_allocator;
    };

} // namespace foundation::rhi::vk
